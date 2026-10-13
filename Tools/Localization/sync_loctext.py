#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""刷新 loctext.json（源码里所有 LOCTEXT / NSLOCTEXT 的清单），并检查 translations/*.json 跟得上。

这是本地化链条的中间一环（从 DreamShader 的 sync_loctext.py 移植，译文由单个文件改成按负责范围分文件）：

    Source/**/*.{h,cpp}
        |  localization_lint.ps1 -Json     扫描 LOCTEXT / NSLOCTEXT（本脚本调它）
    Source/**/*.h
        |  scan_reflection.py              扫描 UCLASS / UPROPERTY 的元数据：项目设置页的显示名、提示、分类
        v
    loctext.json                            清单，本脚本写
    translations/*.json                     zh-Hans 译文，人写，本脚本只读
    language_neutral.json                   语言中立的条目登记，人写
        |  build_locres.py
        v
    Content/Localization/DreamTextureForUnreal/{DreamTextureForUnreal.locmeta, en/, zh-Hans/}

整条链不需要编辑器：扫描是正则，.locres 由 build_locres.py 自己写。

用法：
    python sync_loctext.py            刷新 loctext.json，打印差异与缺译文 / 孤儿 / 占位符问题（缺译文不算失败）
    python sync_loctext.py --check    只检查不写：清单过期、缺译文、孤儿译文、占位符不一致、重复译文、
                                      产物 .locres 不是最新——任何一条都退出 1。Tools/Test.ps1 默认先跑的就是它（-NoLint 跳过）

清单里的 src 必须是「运行时看到的字符串」（C 转义已解码），不是源码里写的样子：.locres 里存的是它的 CRC，
引擎拿 LOCTEXT 的实际字符串算同一个 CRC，对不上就静默丢掉译文。解码在 localization_lint.ps1 里做。
"""
import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import loc_common as common  # noqa: E402
import scan_reflection  # noqa: E402

# 这些规则一旦触发，清单本身就不可信（少了条目、条目互相冲突），不能拿去写 loctext.json。
INTEGRITY_RULES = {"R5", "R7", "R8", "R11", "R12"}


def find_pwsh():
    """lint 脚本要 PowerShell 7（#Requires -Version 7.0）。Windows 自带的 powershell.exe 是 5.1，不能退而求其次，
    否则报的是「版本不匹配」而不是真正的原因。"""
    import shutil
    candidates = [os.environ.get("PWSH"), shutil.which("pwsh")]
    program_files = os.environ.get("ProgramFiles")
    if program_files:
        candidates.append(os.path.join(program_files, "PowerShell", "7", "pwsh.exe"))
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise SystemExit("没找到 pwsh（PowerShell 7）。装一个，或用环境变量 PWSH 指向它。")


def run_lint():
    """跑 localization_lint.ps1 -Json，返回解析后的结果。

    经文件而不是标准输出传：清单里有中文、破折号、省略号，重定向的 PowerShell 标准输出按控制台代码页（本机 936）写，
    会写坏；让脚本直接写 UTF-8 文件。
    """
    exe = find_pwsh()
    fd, tmp = tempfile.mkstemp(suffix=".json", prefix="dtloctext-")
    os.close(fd)
    try:
        cmd = [exe, "-NoProfile", "-NonInteractive", "-File", common.LINT_PATH, "-Json", "-OutFile", tmp]
        proc = subprocess.run(cmd, capture_output=True, check=False)
        if proc.returncode != 0:
            raise SystemExit("localization_lint.ps1 没跑成功（退出码 %d）：\n%s" % (
                proc.returncode, (proc.stderr or proc.stdout).decode("utf-8", "replace")))
        return common.read_json(tmp)
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


def to_entries(lint):
    entries = []
    for row in lint["inventory"]:
        entries.append({
            "ns": row["namespace"],
            "key": row["key"],
            "src": row["text"],
            "file": row["file"],
        })
    entries.sort(key=lambda e: (e["ns"], e["key"]))
    return entries


def describe(finding):
    return "%s:%s [%s] %s" % (finding["file"], finding["line"], finding["rule"], finding["message"])


def merge_reflection(entries, reflection):
    """把属性面板的条目（UObjectDisplayNames / UObjectToolTips / UObjectCategory）并进 LOCTEXT 的清单。

    它们和 LOCTEXT 的条目走同一条路：有 ns、key、src，译文放 translations/*.json，进同一份 .locres。
    命名空间是引擎规定的，几乎不可能与 LOCTEXT_NAMESPACE 撞；撞了且源文不同就拒绝，免得两边悄悄互相覆盖。
    返回 (合并后的条目, 冲突说明列表)。
    """
    by_id = {common.entry_id(e): e for e in entries}
    conflicts = []
    merged = list(entries)
    for e in reflection.entries:
        ident = common.entry_id(e)
        if ident in by_id:
            if by_id[ident]["src"] != e["src"]:
                conflicts.append("%s 同时被 LOCTEXT 与反射元数据定义，源文不同：%r 与 %r" % (ident, by_id[ident]["src"], e["src"]))
            continue
        by_id[ident] = e
        merged.append(e)
    merged.sort(key=lambda e: (e["ns"], e["key"]))
    return merged, conflicts


def main(argv=None):
    ap = argparse.ArgumentParser(description="刷新 loctext.json 并检查译文")
    ap.add_argument("--check", action="store_true", help="不写文件，过期 / 缺译文 / 产物过期就退出 1")
    args = ap.parse_args(argv)
    common.setup_output()

    lint = run_lint()
    blocking = [f for f in lint["violations"] if f["rule"] in INTEGRITY_RULES]
    if blocking:
        print("扫描结果不可信（%d 处）——这些写法会让清单与引擎实际看到的对不上，先改掉：" % len(blocking))
        for f in blocking[:40]:
            print("  " + describe(f))
        if len(blocking) > 40:
            print("  …… 还有 %d 处" % (len(blocking) - 40))
        return 1

    reflection = scan_reflection.scan(common.PLUGIN)
    if reflection.problems:
        print("项目设置页等属性面板的元数据有问题（%d 处）——引擎拿 UPROPERTY 的 meta 去查本地化，写法不对就静默翻不了：" % len(reflection.problems))
        for f in reflection.problems[:40]:
            print("  " + str(f))
        if len(reflection.problems) > 40:
            print("  …… 还有 %d 处" % (len(reflection.problems) - 40))
        print("规则见 Tools/Localization/README.md 的「属性面板（UPROPERTY 元数据）」一节。")
        return 1

    entries, conflicts = merge_reflection(to_entries(lint), reflection)
    if conflicts:
        for line in conflicts:
            print("  " + line)
        return 1
    old = common.load_loctext()
    oldmap = {common.entry_id(e): e for e in old}
    newmap = {common.entry_id(e): e for e in entries}

    added = sorted(k for k in newmap if k not in oldmap)
    removed = sorted(k for k in oldmap if k not in newmap)
    changed = sorted(k for k in newmap if k in oldmap and newmap[k]["src"] != oldmap[k]["src"])
    moved = sorted(k for k in newmap if k in oldmap and newmap[k]["file"] != oldmap[k]["file"])

    print("loctext：%d -> %d 条（+%d，-%d，源文改了 %d，换了文件 %d）"
          % (len(old), len(entries), len(added), len(removed), len(changed), len(moved)))
    for k in added[:30]:
        print("  + " + k)
    if len(added) > 30:
        print("  …… 还有 %d 条新增" % (len(added) - 30))
    for k in removed[:30]:
        print("  - %s   （它的译文现在成了孤儿）" % k)
    for k in changed[:30]:
        print("  ~ %s   （源文改了，回头看看译文还对不对）" % k)

    translations = common.load_translations()
    report = common.validate(entries, translations)
    print("译文：%d 个文件（%s），%d 条译文，%d 条登记语言中立" % (
        len(translations.files), "、".join(translations.files) or "无", len(translations.values), len(translations.neutral)))
    common.print_report(report, entries)

    lint_errors = lint["summary"]["errorCount"]
    if lint_errors:
        print("lint 还有 %d 个错误（跑 localization_lint.ps1 看）——不影响生成清单。" % lint_errors)

    if args.check:
        stale = bool(added or removed or changed or moved)
        problems = []
        if stale:
            problems.append("loctext.json 过期：python Tools/Localization/sync_loctext.py")
        if report.error_count:
            problems.append("译文有 %d 个问题（缺译文 %d、孤儿 %d、占位符 %d、重复 %d、文件 %d、登记 %d）" % (
                report.error_count, len(report.missing), len(report.orphans), len(report.placeholders),
                len(report.duplicates), len(report.file_problems), len(report.neutral_problems)))
        if not stale and not report.error_count:
            # 清单与译文都对得上，才有意义去比产物（否则 build_locres 会因为缺译文拒绝生成）。
            import build_locres
            problems.extend(build_locres.check_outputs())
        for p in problems:
            print("STALE: " + p)
        return 1 if problems else 0

    common.write_json(common.LOCTEXT_PATH, entries)
    print("写 %s" % os.path.relpath(common.LOCTEXT_PATH, common.PLUGIN).replace(os.sep, "/"))
    if report.error_count:
        print("注意：译文还有 %d 个问题，补完再 python Tools/Localization/build_locres.py。" % report.error_count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
