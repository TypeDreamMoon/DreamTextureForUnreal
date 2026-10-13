#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地化工具链共用的东西：路径、译文合并与校验、占位符解析、JSON 读写。

链条（和 DreamShader 同一条，只是译文改成「按负责范围分文件」）：

    Source/**/*.{h,cpp}
        |  localization_lint.ps1 -Json     扫描 LOCTEXT / NSLOCTEXT（含 C 转义解码）
        v
    loctext.json                            源文清单（sync_loctext.py 写）
    translations/*.json                     zh-Hans 译文（人写，工具只读）
        |  build_locres.py                  校验 + 写 LocRes 容器（不需要编辑器）
        v
    Content/Localization/DreamTextureForUnreal/{.locmeta, en/, zh-Hans/}

这个模块只放「读与判」，不写 .locres，也不碰源码。
"""
import collections
import io
import json
import os
import re

BASE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(os.path.dirname(BASE))
LOCTEXT_PATH = os.path.join(BASE, "loctext.json")
TRANSLATIONS_DIR = os.path.join(BASE, "translations")
# 语言中立的条目（ComfyUI、DreamTexture、纯占位符这类翻不翻都一样的），值写明为什么。
# 不放在 translations/ 里：那个目录下的 JSON 一律当译文合并，值是原因而不是译文，混进去会被当成译文写进 .locres。
NEUTRAL_PATH = os.path.join(BASE, "language_neutral.json")
LINT_PATH = os.path.join(BASE, "localization_lint.ps1")

# 本地化目标。名字必须与 .uplugin 里 LocalizationTargets 的 Name、Content/Localization/ 下的目录名、
# 目录里 .locmeta / .locres 的文件名一致——引擎按 <目标目录>/<目标名>.locres 去找，差一个字母就静默加载不到。
TARGET = "DreamTextureForUnreal"
NATIVE_CULTURE = "en"
# 非原生的文化。新增一种语言就在这里加一项，并在 translations/ 里放它的译文（见 README）。
TRANSLATED_CULTURES = ["zh-Hans"]
LOCALIZATION_DIR = os.path.join(PLUGIN, "Content", "Localization", TARGET)

KEY_SEPARATOR = "|"
NEWLINE = chr(10)


def setup_output():
    """输出被管道接走时（Test.ps1、别的工具捕获）固定成 UTF-8，免得中文提示按本机代码页 936 写出去变乱码；
    直接打在终端上时保持终端自己的编码。"""
    import sys
    for stream in (sys.stdout, sys.stderr):
        try:
            if not stream.isatty():
                stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def read_json(path, pairs_hook=None):
    # utf-8-sig：PowerShell 5.1 的 Out-File -Encoding utf8 会带 BOM，读的时候要容忍。
    with io.open(path, encoding="utf-8-sig") as f:
        return json.load(f, object_pairs_hook=pairs_hook)


def write_json(path, data):
    """UTF-8、无 BOM、LF、一格缩进。仓库的 .gitattributes 是 eol=lf，写 CRLF 会让 git 报换行警告。"""
    text = json.dumps(data, ensure_ascii=False, indent=1)
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
        f.write("\n")


def entry_id(entry):
    return entry["ns"] + KEY_SEPARATOR + entry["key"]


def load_loctext():
    """loctext.json -> 条目列表。文件不存在时当作空，让第一次 sync 能跑起来。"""
    if not os.path.exists(LOCTEXT_PATH):
        return []
    return read_json(LOCTEXT_PATH)


# --------------------------------------------------------------------------------------- 译文合并

def _usable(src, tr):
    """译文能不能用：不能没有、不能是空串；源文本身有字的话，也不能只是一串空白
    （源文就是个空白分隔符的，译文写全角空格这种空白是对的）。"""
    if tr is None or tr == "":
        return False
    return not (tr.strip() == "" and src.strip() != "")


class TranslationSet(object):
    """translations/*.json 合并后的结果，连同「哪个文件写了哪条」，报错时能指到人。"""

    def __init__(self):
        self.values = {}           # "ns|key" -> 译文
        self.origin = {}           # "ns|key" -> 文件名
        self.duplicates = []       # (id, [文件名, ...], 说明)  同一条出现在多个文件，或同一文件里写了两遍
        self.file_problems = []    # (文件名, 说明)       文件本身读不了、格式不对
        self.files = []            # 读到的文件名，有序
        self.neutral = {}          # "ns|key" -> 为什么语言中立（language_neutral.json）

    def lookup(self, entry):
        """这条在 zh-Hans 里该显示什么；缺译文返回 None。语言中立的条目用源文本身。"""
        ident = entry_id(entry)
        if ident in self.neutral:
            return entry["src"]
        tr = self.values.get(ident)
        if not _usable(entry["src"], tr):
            return None
        return tr


def _same_or_conflict(first, second):
    # 文字相同的重复也算错（留着两份迟早改岔），但说明里区分一下，好决定删哪个。
    if first == second:
        return "文字相同，删掉多余的那份"
    return "文字冲突：%r 与 %r" % (first, second)


def load_translations(directory=None):
    directory = directory or TRANSLATIONS_DIR
    result = TranslationSet()
    if not os.path.isdir(directory):
        return result

    holders = collections.defaultdict(list)
    for name in sorted(os.listdir(directory)):
        if not name.lower().endswith(".json"):
            continue
        path = os.path.join(directory, name)
        result.files.append(name)

        in_file_duplicates = []

        def hook(pairs, _dups=in_file_duplicates):
            seen = {}
            for k, v in pairs:
                if k in seen:
                    _dups.append((k, seen[k], v))
                seen[k] = v
            return seen

        try:
            data = read_json(path, pairs_hook=hook)
        except (ValueError, OSError) as e:
            result.file_problems.append((name, "读不了或不是合法 JSON：%s" % e))
            continue
        if not isinstance(data, dict):
            result.file_problems.append((name, "顶层必须是对象 {\"<命名空间>|<key>\": \"译文\"}"))
            continue

        for k, first, second in in_file_duplicates:
            result.duplicates.append((k, [name, name], _same_or_conflict(first, second)))
        for k, v in data.items():
            if not isinstance(v, str):
                result.file_problems.append((name, "%s 的值不是字符串" % k))
                continue
            if KEY_SEPARATOR not in k or k.startswith(KEY_SEPARATOR) or k.endswith(KEY_SEPARATOR):
                result.file_problems.append((name, "键 %r 不是 \"<命名空间>|<key>\" 的形式" % k))
                continue
            holders[k].append((name, v))
            result.values[k] = v
            result.origin[k] = name

    for k, held in holders.items():
        if len(held) > 1:
            names = [h[0] for h in held]
            result.duplicates.append((k, names, _same_or_conflict(held[0][1], held[-1][1])))

    if directory == TRANSLATIONS_DIR and os.path.exists(NEUTRAL_PATH):
        try:
            neutral = read_json(NEUTRAL_PATH)
        except (ValueError, OSError) as e:
            result.file_problems.append(("language_neutral.json", "读不了或不是合法 JSON：%s" % e))
            neutral = {}
        if not isinstance(neutral, dict):
            result.file_problems.append(("language_neutral.json", "顶层必须是对象 {\"<命名空间>|<key>\": \"为什么语言中立\"}"))
            neutral = {}
        for k, reason in neutral.items():
            if not isinstance(reason, str) or reason.strip() == "":
                result.file_problems.append(("language_neutral.json", "%s 没写原因（值必须是一句说明为什么翻不翻都一样的话）" % k))
                continue
            result.neutral[k] = reason
    return result


# --------------------------------------------------------------------------------------- 占位符

# 引擎注册的参数修饰符（TextFormatter.cpp 的 TextArgumentModifiers）。hpi 5.8 里没有，为后续版本留着。
_MODIFIER = re.compile(r"\|(plural|ordinal|gender|hpp|hpi)\(")


def _skip_modifier(text, i):
    """text[i] 是 |，后面若是一个 |关键字(…) 修饰符，返回它结束后的下标；不是修饰符返回 -1。

    引擎找右括号的规矩（ParseArgumentModifier）：双引号里的 ) 不算，引号前有奇数个反斜杠则引号不闭合。
    """
    m = _MODIFIER.match(text, i)
    if not m:
        return -1
    j = m.end()
    quoted = False
    slashes = 0
    while j < len(text):
        c = text[j]
        if c == ")" and not quoted:
            return j + 1
        if c == '"' and slashes % 2 == 0:
            quoted = not quoted
        slashes = slashes + 1 if c == "\\" else 0
        j += 1
    return -1


def parse_format(text):
    """按引擎 FTextFormatter 的文法拆出占位符。

    引擎的规矩（TextFormatter.h / TextFormatter.cpp）：
      - 转义符是反引号：`` ` `` 后面跟 `` ` `` `{` `}` `|` 之一，表示那个字符本身；
      - 参数是 {名字}，名字是 { 到第一个 } 之间的全部字符（{0}、{Name}）；
      - 参数修饰符写在大括号**外面**：{Count}|plural(one=item,other=items)，还有 ordinal / gender / hpp。
        整个 |keyword(...) 连同括号里的字当作不透明的一段跳过——括号里的 { } 与单词都不算占位符。
        中文没有单复数，译文完全可以只写 {Count}，所以修饰符不参与比较；
      - 没有配上 } 的 {、孤立的 }，引擎都当普通字符，不报错——但中文译文里出现它们几乎一定是漏了个括号，
        所以把「孤立括号的个数」也算作校验项。

    返回 (参数名的计数, 孤立 { 个数, 孤立 } 个数)。
    """
    args = collections.Counter()
    stray_open = 0
    stray_close = 0
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "`" and i + 1 < n and text[i + 1] in "`{}|":
            i += 2
            continue
        if c == "|":
            end = _skip_modifier(text, i)
            if end >= 0:
                i = end
                continue
        if c == "{":
            end = text.find("}", i + 1)
            if end < 0:
                stray_open += 1
                i += 1
                continue
            name = text[i + 1:end]
            if name == "":
                # {} 引擎会把空名当作参数名 ""，格式化时多半失败。当孤立括号报出来。
                stray_open += 1
                stray_close += 1
            else:
                args[name] += 1
            i = end + 1
            continue
        if c == "}":
            stray_close += 1
        i += 1
    return args, stray_open, stray_close


_CJK = re.compile(u"[\u3000-\u303f\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uff00-\uffef]"
                  u"|[\ud840-\ud87f][\udc00-\udfff]")


def has_cjk(text):
    return bool(_CJK.search(text))


# --------------------------------------------------------------------------------------- 校验

class Report(object):
    """校验结果。error_count 非零就不该写 .locres；warnings 只提醒。"""

    def __init__(self):
        self.missing = []         # 清单里有、译文里没有（或译文是空的），也没登记语言中立的条目
        self.orphans = []         # 译文里有、清单里没有的键
        self.placeholders = []    # (条目, 说明)
        self.duplicates = []      # (键, [文件名], 说明)
        self.file_problems = []   # (文件名, 说明)
        self.neutral_problems = []  # (键, 说明)  language_neutral.json 里不对劲的登记
        self.warnings = []        # 字符串

    @property
    def error_count(self):
        return (len(self.missing) + len(self.orphans) + len(self.placeholders) + len(self.duplicates)
                + len(self.file_problems) + len(self.neutral_problems))


def validate(entries, translations):
    report = Report()
    report.file_problems = list(translations.file_problems)

    for ident, names, detail in translations.duplicates:
        report.duplicates.append((ident, names, detail))

    live = set()
    for e in entries:
        ident = entry_id(e)
        live.add(ident)

        if ident in translations.neutral:
            if ident in translations.values:
                report.neutral_problems.append((ident, "既登记了语言中立，又在 %s 里写了译文——只留一个" % translations.origin[ident]))
            continue

        tr = translations.values.get(ident)
        if not _usable(e["src"], tr):
            report.missing.append(e)
            continue

        src_args, src_open, src_close = parse_format(e["src"])
        tr_args, tr_open, tr_close = parse_format(tr)
        # 比的是「有哪些参数名」，不比出现几次：{0} {0}|plural(one=个,other=个) 里 {0} 写了两遍，
        # 中文译文只写一个 {0} 完全正确。缺了某个参数、或多出一个源文里没有的，才是真错误。
        if set(src_args) != set(tr_args):
            lost = sorted(set(src_args) - set(tr_args))
            extra = sorted(set(tr_args) - set(src_args))
            parts = []
            if lost:
                parts.append("译文里缺 " + "、".join("{%s}" % a for a in lost))
            if extra:
                parts.append("译文里多出源文没有的 " + "、".join("{%s}" % a for a in extra))
            report.placeholders.append((e, "占位符不一致：%s（源文 %s，译文 %s）" % (
                "；".join(parts), _fmt_args(src_args), _fmt_args(tr_args))))
        elif (src_open, src_close) != (tr_open, tr_close):
            report.placeholders.append((e, "孤立的大括号个数不一致：孤立 { 源文 %d、译文 %d；孤立 } 源文 %d、译文 %d" % (
                src_open, tr_open, src_close, tr_close)))

        # 只提醒的几样：不拦，但常是笔误。
        src_lines = e["src"].count(NEWLINE)
        tr_lines = tr.count(NEWLINE)
        if src_lines != tr_lines:
            report.warnings.append("%s：换行个数与源文不同（源文 %d，译文 %d）" % (ident, src_lines, tr_lines))
        if tr == e["src"] and len(re.findall(r"[A-Za-z]{3,}", e["src"])) >= 2 and not has_cjk(tr):
            report.warnings.append("%s：译文与源文完全相同，像是忘了翻——确实不用翻就登记进 language_neutral.json" % ident)

    for k in sorted(translations.values):
        if k not in live:
            report.orphans.append(k)
    for k in sorted(translations.neutral):
        if k not in live:
            report.neutral_problems.append((k, "登记了语言中立，但源码里已经没有这一条（key 改名或删了）"))
    return report


def _fmt_args(counter):
    if not counter:
        return "无"
    return ", ".join("{%s}×%d" % (k, v) if v > 1 else "{%s}" % k for k, v in sorted(counter.items()))


def print_report(report, entries, limit=60, out=None):
    """把校验结果打印出来；每一类最多列 limit 条，其余折叠成一句，免得几百条淹没别的输出。"""
    import sys
    out = out or sys.stdout

    def say(line):
        out.write(line + "\n")

    def listing(items, fmt):
        for i, item in enumerate(items):
            if i >= limit:
                say("  …… 还有 %d 条没列出" % (len(items) - limit))
                break
            say(fmt(item))

    if report.file_problems:
        say("译文文件有问题（%d）：" % len(report.file_problems))
        listing(report.file_problems, lambda p: "  FILE    %s：%s" % p)
    if report.duplicates:
        say("同一条译文写在多处（%d）——按负责范围分文件，一条只能有一个家：" % len(report.duplicates))
        listing(report.duplicates, lambda p: "  DUP     %s  在 %s（%s）" % (p[0], "、".join(p[1]), p[2]))
    if report.missing:
        say("缺 zh-Hans 译文（%d）：" % len(report.missing))
        listing(report.missing, lambda e: "  MISSING %s | %s :: %s    (%s)" % (e["ns"], e["key"], _one_line(e["src"]), e["file"]))
    if report.placeholders:
        say("占位符不一致（%d）：" % len(report.placeholders))
        listing(report.placeholders, lambda p: "  PLACEHOLDER %s | %s  %s" % (p[0]["ns"], p[0]["key"], p[1]))
    if report.neutral_problems:
        say("language_neutral.json 的登记有问题（%d）：" % len(report.neutral_problems))
        listing(report.neutral_problems, lambda p: "  NEUTRAL %s  %s" % p)
    if report.orphans:
        say("译文指向不存在的 key（%d）——key 改名了、或写错了：" % len(report.orphans))
        listing(report.orphans, lambda k: "  ORPHAN  %s" % k)
    if report.warnings:
        say("提醒（%d，不拦）：" % len(report.warnings))
        listing(report.warnings, lambda w: "  WARN    %s" % w)


def _one_line(text):
    return text.replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t")
