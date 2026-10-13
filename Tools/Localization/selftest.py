#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""本地化工具链的自测：python Tools/Localization/selftest.py

工具链里出错的方式都是「静默」的：转义解码错一个字符，.locres 里的源文 CRC 就对不上，引擎加载时悄悄丢掉那条译文，
界面回落到英文，没有任何报错。所以这里把能离线验证的都钉死：

  * 扫描器：C 转义、相邻字面量拼接、注释与字符串互相包含时的切分、各条 lint 规则、豁免；
  * 哈希：源文 CRC 用 zlib 独立复算；键哈希用从引擎自带 .locres 里摘出来的已知答案（覆盖 CityHash 的各个长度分支）；
  * LocRes / LocMeta：写出来再用独立的读取器读回，字段与引擎的布局逐项对；
  * 占位符：包括 ICU 风格的 {0}|plural(...)；
  * 译文校验：重复、孤儿、缺译文、语言中立登记；
  * 属性面板（项目设置页）：FName::NameToDisplayString 的移植用引擎自带的 PropertyNames 英文 locres 回归，
    引擎用的命名空间 / key / 源文约定用引擎自带的 locres 钉死，UCLASS / UPROPERTY 扫描器的各条规则（M1-M5）。

在引擎里的真验证是自动化测试 DreamTexture.Localization.*（Tools/Test.ps1）。
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_locres as b  # noqa: E402
import loc_common as c  # noqa: E402
import scan_reflection as r  # noqa: E402
import sync_loctext as s  # noqa: E402

LINT = c.LINT_PATH


def find_engine_root():
    """引擎目录，找不到返回 None（依赖引擎文件的测试会跳过）。

    顺序同 Tools/Common.ps1：环境变量 DREAMTEXTURE_ENGINE_ROOT -> Tools/LocalSettings.psd1 的 EngineRoot ->
    Windows 上 Launcher 版 5.8 登记在注册表里的 InstalledDirectory。"""
    root = os.environ.get("DREAMTEXTURE_ENGINE_ROOT", "").strip()
    if root:
        return root
    settings = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "LocalSettings.psd1")
    if os.path.exists(settings):
        import re
        with open(settings, encoding="utf-8-sig") as f:
            m = re.search(r"^\s*EngineRoot\s*=\s*'([^']+)'", f.read(), re.MULTILINE)
        if m:
            return m.group(1)
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\EpicGames\Unreal Engine\5.8") as k:
            return winreg.QueryValueEx(k, "InstalledDirectory")[0]
    except (ImportError, OSError):
        return None


ENGINE_ROOT = find_engine_root()


def run_lint(root):
    """对一个临时的「插件目录」跑 lint，返回 JSON 结果。"""
    out = os.path.join(root, "lint.json")
    cmd = [s.find_pwsh(), "-NoProfile", "-NonInteractive", "-File", LINT, "-Json", "-OutFile", out, "-PluginRoot", root]
    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        raise AssertionError("lint 脚本自己失败了：" + (proc.stderr or proc.stdout).decode("utf-8", "replace"))
    return c.read_json(out)


def write(root, rel, text):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def utf16_crc(text):
    """独立的 FCrc::StrCrc32<TCHAR>：标准 CRC-32，输入是每个 UTF-16 码元补成 4 字节。"""
    raw = text.encode("utf-16-le", "surrogatepass")
    data = b"".join(struct.pack("<I", u) for (u,) in struct.iter_unpack("<H", raw))
    return zlib.crc32(data) & 0xFFFFFFFF


class HashTests(unittest.TestCase):
    def test_source_crc_matches_zlib(self):
        for text in ["", "a", "Hello, world", "{0} items", "中文", "mixed 中 and ascii", "dot \u00b7 dash \u2014 ellipsis \u2026",
                     "emoji \U0001F600 pair", "line\nbreak\ttab", "x" * 500]:
            self.assertEqual(b.str_crc32(text), utf16_crc(text), repr(text))

    def test_key_hash_known_answers_from_engine_locres(self):
        # 这些是从引擎自带的 Engine.locres 里读出来的（命名空间 / key 与它们存着的哈希），
        # 长度（UTF-16 字节数）分别落在 CityHash 的 0-16、17-32、33-64、>64 几个分支。
        vectors = [
            ("X", 0xb5ae0139), ("Out", 0xaf81951b), ("A3Nodes", 0x274fe4e1), ("16:9DSLR", 0x96aec930),
            ("Super16mm", 0xd1a27171), ("16:9DigitalFilm", 0xf6a0b79c), ("SkelMeshSlotName", 0xbd73895f),
            ("ARKitSettingsName", 0x81fe6906), ("AddActionMappingToGroupToolTip", 0x6c636dcf),
            ("111212DB46349A8A1F45339CED4FCA8A", 0xbd378a5e), ("ALevelVariantSetsActorDisplayName", 0xdc8840dd),
            ("ShowMenu_TrajectoriesLabelTrajectoriesOrientations", 0x6a3cc176),
            ("AudioDashboardEditor_SaveCacheSnapshotButton_NotificationSubText", 0x47607a19),
            ("AssetDefinition_ResonanceAudioSpatializationSourceSettingsSubMenu", 0x2b3a5067),
            ("TokenMessage_WorldPartition_ContainsRuntimeDataLayersWithDifferentTypesOfLoadFilter", 0xffcf9430),
        ]
        for text, expected in vectors:
            self.assertEqual(b.CityHash.city_hash_64_utf16_to_uint32(text), expected, text)


class LocResTests(unittest.TestCase):
    ROWS = [
        ("NsA", "Plain", "Hello", "你好"),
        ("NsA", "Fmt", "{0} items", "{0} 项"),
        ("NsA", "Same", "ComfyUI", "ComfyUI"),
        ("NsB", "Emoji", "Ready \U0001F600", "就绪 \U0001F600"),
        ("NsB", "Escapes", "tab\there \"q\" \\ back", "制表\t引号\"反斜杠\\"),
        ("NsC", "Latin1", "caf\u00e9 \u00b7 dot", "咖啡馆 \u00b7 点"),
    ]

    def test_roundtrip(self):
        data = b.build_locres_bytes(self.ROWS)
        entries, strings = b.read_locres(data)
        self.assertEqual(len(entries), len(self.ROWS))
        got = {(ns, key): (crc, tr) for ns, key, _, _, crc, tr in entries}
        for ns, key, src, tr in self.ROWS:
            self.assertEqual(got[(ns, key)], (utf16_crc(src), tr))
        b.verify_locres(data, self.ROWS)

    def test_string_table_is_deduplicated_and_refcounted(self):
        data = b.build_locres_bytes(self.ROWS + [("NsD", "Dup", "x", "你好")])
        _, strings = b.read_locres(data)
        self.assertEqual(strings.count("你好"), 1)

    def test_output_is_deterministic_regardless_of_input_order(self):
        self.assertEqual(b.build_locres_bytes(self.ROWS), b.build_locres_bytes(list(reversed(self.ROWS))))

    def test_header_layout(self):
        data = b.build_locres_bytes(self.ROWS)
        self.assertEqual(data[:16], bytes.fromhex("0e147475674a03fc4a15909dc3377f1b"))
        self.assertEqual(data[16], 3)
        total, namespaces = struct.unpack_from("<II", data, 25)
        self.assertEqual((total, namespaces), (len(self.ROWS), 3))

    def test_fstring_ansi_and_utf16(self):
        self.assertEqual(b.fstring("ab"), struct.pack("<i", 3) + b"ab\x00")
        self.assertEqual(b.fstring("\u00e9"), struct.pack("<i", -2) + "\u00e9".encode("utf-16-le") + b"\x00\x00")
        self.assertEqual(b.fstring("\U0001F600"), struct.pack("<i", -3) + "\U0001F600".encode("utf-16-le") + b"\x00\x00")

    def test_locmeta_roundtrip(self):
        data = b.build_locmeta_bytes("en", "en/T.locres", ["en", "zh-Hans"])
        meta = b.read_locmeta(data)
        self.assertEqual(meta, {"version": 2, "native_culture": "en", "native_locres": "en/T.locres",
                                "compiled_cultures": ["en", "zh-Hans"], "is_ugc": False, "trailing": 0})

    def test_locmeta_layout_matches_engine_shipped_file(self):
        path = os.path.join(ENGINE_ROOT, "Engine/Content/Localization/Editor/Editor.locmeta") if ENGINE_ROOT else ""
        if not os.path.exists(path):
            self.skipTest("本机没有引擎的 Editor.locmeta")
        with open(path, "rb") as f:
            meta = b.read_locmeta(f.read())
        self.assertEqual((meta["version"], meta["native_culture"], meta["trailing"], meta["is_ugc"]), (2, "en", 0, False))
        # 我们写的同版本文件，字节前缀与引擎的一致到「魔数 + 版本 + 原生文化 + 原生 locres 路径」。
        ours = b.build_locmeta_bytes("en", "en/Editor.locres", meta["compiled_cultures"])
        with open(path, "rb") as f:
            self.assertEqual(ours, f.read())


class PlaceholderTests(unittest.TestCase):
    def names(self, text):
        return set(c.parse_format(text)[0])

    def test_plain(self):
        self.assertEqual(self.names("{0} and {Name}"), {"0", "Name"})

    def test_plural_modifier_is_not_extra_placeholders(self):
        text = "{0} {0}|plural(one=material slot,other=material slots) selected"
        self.assertEqual(self.names(text), {"0"})
        self.assertEqual(c.parse_format(text)[1:], (0, 0))

    def test_named_and_other_modifiers(self):
        self.assertEqual(self.names("{Count}|plural(one=file,other=files)"), {"Count"})
        self.assertEqual(self.names("{N}|ordinal(one=st,other=th)"), {"N"})
        self.assertEqual(self.names("{Who}|gender(masculine=he,feminine=she)"), {"Who"})

    def test_modifier_body_with_quotes_and_braces(self):
        self.assertEqual(self.names('{0}|plural(one="a)b",other={x})'), {"0"})

    def test_escapes(self):
        self.assertEqual(self.names("`{0`} {1}"), {"1"})

    def test_stray_braces_are_counted(self):
        self.assertEqual(c.parse_format("a } b { c")[1:], (1, 1))

    def test_pipe_alone_is_literal(self):
        self.assertEqual(c.parse_format("a | b"), (c.collections.Counter(), 0, 0))


class ValidateTests(unittest.TestCase):
    def entry(self, ns, key, src):
        return {"ns": ns, "key": key, "src": src, "file": "Source/X.cpp"}

    def translations(self, files, neutral=None):
        d = tempfile.mkdtemp(prefix="dtl10n-")
        for name, body in files.items():
            with open(os.path.join(d, name), "w", encoding="utf-8") as f:
                f.write(body if isinstance(body, str) else json.dumps(body, ensure_ascii=False))
        result = c.load_translations(d)
        if neutral:
            result.neutral.update(neutral)
        return result

    def test_complete_set_is_clean(self):
        entries = [self.entry("A", "K1", "Hello"), self.entry("A", "K2", "{0} items")]
        t = self.translations({"a.json": {"A|K1": "你好", "A|K2": "{0} 项"}})
        self.assertEqual(c.validate(entries, t).error_count, 0)

    def test_missing_orphan_and_empty(self):
        entries = [self.entry("A", "K1", "Hello"), self.entry("A", "K2", "World")]
        t = self.translations({"a.json": {"A|K1": "", "A|Gone": "x"}})
        r = c.validate(entries, t)
        self.assertEqual([e["key"] for e in r.missing], ["K1", "K2"])
        self.assertEqual(r.orphans, ["A|Gone"])

    def test_duplicate_across_files_is_an_error_even_when_identical(self):
        entries = [self.entry("A", "K1", "Hello")]
        t = self.translations({"a.json": {"A|K1": "你好"}, "b.json": {"A|K1": "你好"}})
        r = c.validate(entries, t)
        self.assertEqual(len(r.duplicates), 1)
        self.assertIn("相同", r.duplicates[0][2])
        t = self.translations({"a.json": {"A|K1": "你好"}, "b.json": {"A|K1": "您好"}})
        self.assertIn("冲突", c.validate(entries, t).duplicates[0][2])

    def test_duplicate_inside_one_file(self):
        t = self.translations({"a.json": '{"A|K1": "x", "A|K1": "y"}'})
        self.assertEqual(len(t.duplicates), 1)

    def test_placeholder_mismatch(self):
        entries = [self.entry("A", "K1", "{0} of {1}")]
        t = self.translations({"a.json": {"A|K1": "共 {0}"}})
        self.assertEqual(len(c.validate(entries, t).placeholders), 1)
        t = self.translations({"a.json": {"A|K1": "{0} / {1} / {2}"}})
        self.assertEqual(len(c.validate(entries, t).placeholders), 1)

    def test_plural_source_with_plain_translation_passes(self):
        entries = [self.entry("A", "K1", "{0} {0}|plural(one=slot,other=slots)")]
        t = self.translations({"a.json": {"A|K1": "{0} 个槽位"}})
        self.assertEqual(c.validate(entries, t).error_count, 0)

    def test_neutral_entries_need_no_translation_and_use_the_source(self):
        entries = [self.entry("A", "Brand", "ComfyUI")]
        t = self.translations({}, neutral={"A|Brand": "product name"})
        self.assertEqual(c.validate(entries, t).error_count, 0)
        self.assertEqual(t.lookup(entries[0]), "ComfyUI")

    def test_neutral_conflicts_and_stale_registrations_are_errors(self):
        entries = [self.entry("A", "Brand", "ComfyUI")]
        t = self.translations({"a.json": {"A|Brand": "ComfyUI"}}, neutral={"A|Brand": "x", "A|Old": "y"})
        r = c.validate(entries, t)
        self.assertEqual(len(r.neutral_problems), 2)

    def test_whitespace_translation_allowed_only_for_whitespace_source(self):
        entries = [self.entry("A", "Sep", "  "), self.entry("A", "Word", "Hello")]
        t = self.translations({"a.json": {"A|Sep": "\u3000", "A|Word": "  "}})
        self.assertEqual([e["key"] for e in c.validate(entries, t).missing], ["Word"])


class ScannerTests(unittest.TestCase):
    """用临时的「插件目录」跑真正的 localization_lint.ps1。"""

    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="dtlint-")
        write(cls.root, "Source/Mod/Private/Escapes.cpp", r'''// Copyright
#define LOCTEXT_NAMESPACE "Esc"
static void F()
{
	auto A = LOCTEXT("Tab", "tab\there \"q\" \\ back");
	auto B = LOCTEXT("Hex", "\x41\x42 \101 \u00b7 \u4e2d \U0001F600 end");
	auto C = LOCTEXT("Concat",
		"first part, " // comment between pieces is fine
		"second part, "
		/* block comment too */ "third.");
	auto D = LOCTEXT("Wrapped", TEXT("wrapped text"));
	auto E = NSLOCTEXT("Other", "Explicit", "explicit ns");
	auto G = LOCTEXT("Slash", "http://example.com/*not a comment*/ end");
	auto H = LOCTEXT("Raw", "q\?mark \' apostrophe end");
	auto I = LOCTEXT("Emoji", "\U0001F600 and 😀");
}
#undef LOCTEXT_NAMESPACE
''')
        write(cls.root, "Source/Mod/Private/Comments.cpp", r'''#define LOCTEXT_NAMESPACE "Cmt"
// LOCTEXT("InLineComment", "should not count") 中文注释
/* LOCTEXT("InBlock", "should not count")
   中文块注释 */
static const char* Quoted = "LOCTEXT(\"InString\", \"not code\")";
static auto R = R"x(raw "quoted" // not a comment LOCTEXT("InRaw","no"))x";
static auto Real = LOCTEXT("Real", "real one");
#undef LOCTEXT_NAMESPACE
''')
        write(cls.root, "Source/Mod/Public/Header.h", r'''#pragma once
static auto H = NSLOCTEXT("Hdr", "InHeader", "from a header");
''')
        write(cls.root, "Source/Mod/Private/Chinese.cpp", r'''#define LOCTEXT_NAMESPACE "Zh"
void G()
{
	UE_LOG(LogTemp, Warning, TEXT("日志里的中文"));
	Error = FString(TEXT("界面里的中文"));
	auto T = LOCTEXT("Bad", "源文是中文");
	auto Ok = TEXT("an English literal");
	// 注释里的中文没事
	const FString Exempt = TEXT("后端的原话"); // I18N-EXEMPT: 与后端响应里的固定字样比对
	const FString NoReason = TEXT("没写原因"); // I18N-EXEMPT
	FString Listed = TEXT("清单里豁免的中文");
	auto F1 = FText::FromString(TEXT("English only"));
	auto F2 = FText::FromString(TEXT("\n"));
	auto F3 = FText::FromString(TEXT(" / "));
	auto F4 = FText::FromString(TEXT("中文的"));
	auto F5 = FText::FromString(TEXT("Brand"));
	auto F6 = FText::FromString(TEXT("Brand")); // I18N-EXEMPT: product name
}
#undef LOCTEXT_NAMESPACE
''')
        write(cls.root, "Source/Mod/Private/Structure.cpp", r'''#define LOCTEXT_NAMESPACE "Str"
void H(const TCHAR* Var)
{
	auto A = LOCTEXT("NonLiteralKey", Var);
	auto B = LOCTEXT(Var, "x");
	auto C = LOCTEXT("", "empty key");
	auto D = LOCTEXT("EmptyText", "");
	auto E = NSLOCTEXT("Bar|Ns", "K", "pipe in namespace");
	auto F = LOCTEXT("Same", "same text");
	auto G = LOCTEXT("Same", "same text");
	auto I = LOCTEXT("Clash", "one");
	auto J = LOCTEXT("Clash", "two");
	auto K = FText::AsLocalizable_Advanced(TEXT("N"), TEXT("K"), TEXT("dynamic"));
}
''')
        write(cls.root, "Source/Mod/Private/Nul.cpp", r'''#define LOCTEXT_NAMESPACE "Nul"
auto A = LOCTEXT("WithNul", "before\0after");
#undef LOCTEXT_NAMESPACE
''')
        write(cls.root, "Source/Mod/Private/NoNamespace.cpp", r'''void I()
{
	auto A = LOCTEXT("NoNs", "no namespace defined");
}
''')
        write(cls.root, "Source/Mod/Public/BadHeader.h", r'''#pragma once
#define LOCTEXT_NAMESPACE "InHeader"
''')
        write(cls.root, "Source/Mod/Private/Tests/SomeTests.cpp", r'''#define LOCTEXT_NAMESPACE "Tests"
void T()
{
	check(Result == TEXT("测试里断言中文是正常的"));
	auto A = LOCTEXT("TestOnly", "test fixture text");
	auto B = FText::FromString(TEXT("English in a test"));
}
#undef LOCTEXT_NAMESPACE
''')
        write(cls.root, "Tools/Localization/lint_exemptions.json", json.dumps({"exemptions": [
            {"rule": "R1", "file": "Source/Mod/Private/Chinese.cpp", "contains": "清单里豁免", "reason": "protocol literal"},
            {"rule": "R2", "file": "Source/Mod/Private/Chinese.cpp", "contains": "Brand", "reason": "brand"},
            {"rule": "R1", "file": "Source/Mod/Private/Nothing.cpp", "reason": "stale one"},
            {"rule": "R1", "file": "Source/Mod/Private/Chinese.cpp", "contains": "x", "reason": ""},
        ]}, ensure_ascii=False))
        cls.lint = run_lint(cls.root)
        cls.src = {(e["namespace"], e["key"]): e["text"] for e in cls.lint["inventory"]}
        cls.found = [(f["rule"], f["file"].split("/")[-1], f["line"]) for f in cls.lint["violations"]]
        cls.warned = [(f["rule"], f["file"].split("/")[-1]) for f in cls.lint["warnings"]]

    def rules(self, rule, name):
        return sorted(line for r, f, line in self.found if r == rule and f == name)

    def test_escapes_are_decoded_like_the_compiler(self):
        self.assertEqual(self.src[("Esc", "Tab")], 'tab\there "q" \\ back')
        self.assertEqual(self.src[("Esc", "Hex")], "AB A \u00b7 \u4e2d \U0001F600 end")
        self.assertEqual(self.src[("Esc", "Slash")], "http://example.com/*not a comment*/ end")
        self.assertEqual(self.src[("Esc", "Raw")], "q?mark ' apostrophe end")
        self.assertEqual(self.src[("Esc", "Emoji")], "\U0001F600 and \U0001F600")

    def test_adjacent_literals_and_text_wrapper(self):
        self.assertEqual(self.src[("Esc", "Concat")], "first part, second part, third.")
        self.assertEqual(self.src[("Esc", "Wrapped")], "wrapped text")

    def test_namespaces(self):
        self.assertEqual(self.src[("Other", "Explicit")], "explicit ns")
        self.assertEqual(self.src[("Hdr", "InHeader")], "from a header")

    def test_comments_strings_and_raw_strings_do_not_leak_into_the_inventory(self):
        keys = set(self.src)
        self.assertEqual({k for k in keys if k[0] == "Cmt"}, {("Cmt", "Real")})

    def test_chinese_literals(self):
        # 日志、界面、LOCTEXT 源文各一处报 R1；两处豁免（行内带原因、清单里）不报；没写原因的报 R10。
        lines = self.rules("R1", "Chinese.cpp")
        self.assertIn(4, lines)    # UE_LOG
        self.assertIn(5, lines)    # 界面
        self.assertIn(6, lines)    # LOCTEXT 源文
        self.assertNotIn(8, lines)  # 注释
        self.assertNotIn(9, lines)  # 行内豁免
        self.assertNotIn(10, lines)  # 行内标记没写原因：不再报 R1，改报 R10
        self.assertNotIn(11, lines)  # 清单豁免
        self.assertIn(15, lines)   # FromString 里的中文走 R1 不走 R2
        self.assertEqual(self.rules("R10", "Chinese.cpp"), [10])
        by_line = {f["line"]: f["message"] for f in self.lint["violations"] if f["rule"] == "R1" and f["file"].endswith("Chinese.cpp")}
        self.assertIn("日志", by_line[4])
        self.assertIn("LOCTEXT", by_line[6])

    def test_english_fromstring_literal(self):
        self.assertEqual(self.rules("R2", "Chinese.cpp"), [12])      # 只有第 12 行；换行、纯标点、豁免的都不报
        reasons = {e["reason"] for e in self.lint["exempted"]}
        self.assertIn("product name", reasons)
        self.assertIn("brand", reasons)
        self.assertIn("protocol literal", reasons)

    def test_exemption_list_hygiene(self):
        messages = [f["message"] for f in self.lint["violations"] if f["rule"] == "R13"]
        self.assertEqual(len(messages), 2)   # 一条没写 reason，一条过期
        self.assertTrue(any("没写 reason" in m for m in messages))
        self.assertTrue(any("已经没有任何东西匹配" in m for m in messages))

    def test_structure_rules(self):
        self.assertEqual(self.rules("R7", "Structure.cpp"), [4, 5])
        self.assertEqual(self.rules("R12", "Structure.cpp"), [6, 7])
        self.assertEqual(self.rules("R11", "Structure.cpp"), [8])
        self.assertEqual(self.rules("R8", "Structure.cpp"), [12])
        self.assertIn(("R3", "Structure.cpp"), self.warned)
        self.assertIn(("R9", "Structure.cpp"), self.warned)
        self.assertEqual(self.rules("R12", "Nul.cpp"), [2])
        self.assertEqual(self.rules("R5", "NoNamespace.cpp"), [3])
        self.assertEqual(len(self.rules("R4", "BadHeader.h")), 1)
        self.assertIn(("R6", "Structure.cpp"), self.warned)

    def test_collision_keeps_the_first_text(self):
        self.assertEqual(self.src[("Str", "Clash")], "one")

    def test_test_sources_are_exempt_from_literal_rules_but_stay_in_the_inventory(self):
        self.assertEqual([x for x in self.found if x[1] == "SomeTests.cpp"], [])
        self.assertEqual(self.src[("Tests", "TestOnly")], "test fixture text")

    def test_scanner_decode_matches_crc_of_compiled_string(self):
        # 端到端：扫描出来的源文 -> build 出 locres -> 读回的 CRC 与「编译器会给出的字符串」的独立 CRC 一致。
        rows = [(ns, key, src, src) for (ns, key), src in self.src.items()]
        entries, _ = b.read_locres(b.build_locres_bytes(rows))
        want = {(ns, key): utf16_crc(src) for ns, key, src, _ in rows}
        for ns, key, _, _, crc, _ in entries:
            self.assertEqual(crc, want[(ns, key)])

ENGINE_LOCALIZATION = os.path.join(ENGINE_ROOT, "Engine/Content/Localization") if ENGINE_ROOT else None


def read_engine_locres(target, culture="en"):
    if ENGINE_LOCALIZATION is None:
        return None
    path = "%s/%s/%s/%s.locres" % (ENGINE_LOCALIZATION, target, culture, target)
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return b.read_locres(f.read())[0]


class NameToDisplayStringTests(unittest.TestCase):
    """r.name_to_display_string 是 FName::NameToDisplayString（UnrealNames.cpp）的移植。
    属性没写 DisplayName 时，面板上的显示名就是它——移植错一个字母，loctext.json 里的源文就对不上引擎，译文静默失效。"""

    # (属性名, 是不是 bool, 引擎自带 PropertyNames/en 里记的显示名)。每条都是从那份 locres 里挑出来的。
    ENGINE_VECTORS = [
        ("GenerationRadius102400", False, "Generation Radius 102400"),
        ("HardReference2", False, "Hard Reference 2"),
        ("Location_VSOP87", False, "Location VSOP87"),
        ("Float22_2", False, "Float 22 2"),
        ("GMT_TimeZone", False, "GMT Time Zone"),
        ("DefaultTexture_White", False, "Default Texture White"),
        ("bOverride_VolumetricScatteringIntensity", True, "Override Volumetric Scattering Intensity"),
        ("bOverride_BurnInFileNameFormat", True, "Override Burn in File Name Format"),
        ("LODSettings", False, "LODSettings"),
        ("SomeUIClampedFloat", False, "Some UIClamped Float"),
        ("XPBDAnisoStretchWarpScale", False, "XPBDAniso Stretch Warp Scale"),
        ("DestructibleHLODState", False, "Destructible HLODState"),
        ("ParamUIMetadata", False, "Param UIMetadata"),
        ("bShouldBeResolved", True, "Should be Resolved"),
        ("bExportFoliageOnActorsLayer", True, "Export Foliage on Actors Layer"),
        ("bHasBeenPaintedInEditor", True, "Has Been Painted in Editor"),
        ("InitialControlAndModifierUpdates", False, "Initial Control and Modifier Updates"),
        ("bHideInputActionWithKeyboard", True, "Hide Input Action with Keyboard"),
        ("CombinedVelocityAndHeightRTB", False, "Combined Velocity and Height RTB"),
        ("bCanBeDamaged", True, "Can be Damaged"),
        ("bAllowTickBeforeBeginPlay", True, "Allow Tick Before Begin Play"),
        ("bOverrideWireframeColor", True, "Override Wireframe Color"),
        ("_meshObj", False, " Mesh Obj"),     # 开头的下划线变成空格，引擎就是这样（连前导空格都一样）
        ("ItemId", False, "Item Id"),
    ]

    def test_engine_vectors(self):
        for name, is_bool, want in self.ENGINE_VECTORS:
            self.assertEqual(r.name_to_display_string(name, is_bool), want, name)

    def test_the_names_this_plugin_uses(self):
        self.assertEqual(r.name_to_display_string("BackendURL", False), "Backend URL")
        self.assertEqual(r.name_to_display_string("bCreateMaterialInstance", True), "Create Material Instance")
        self.assertEqual(r.name_to_display_string("bAutoStartBackend", True), "Auto Start Backend")
        self.assertEqual(r.name_to_display_string("ImportRoot", False), "Import Root")
        self.assertEqual(r.name_to_display_string("DreamTextureSettings", False), "Dream Texture Settings")

    def test_bool_prefix_is_only_dropped_before_an_uppercase_letter(self):
        self.assertEqual(r.name_to_display_string("bake", True), "Bake")
        self.assertEqual(r.name_to_display_string("bFoo", False), "B Foo")
        self.assertEqual(r.name_to_display_string("b", True), "B")

    def test_articles_stay_lowercase_only_inside_the_name(self):
        self.assertEqual(r.name_to_display_string("InTheEnd", False), "In the End")
        self.assertEqual(r.name_to_display_string("WithInstance", False), "With Instance")   # 后面紧跟小写字母不算冠词

    def test_regression_against_the_whole_engine_property_names_table(self):
        entries = read_engine_locres("PropertyNames")
        if entries is None:
            self.skipTest("本机没有引擎的 PropertyNames.locres")
        ok = bad = 0
        for ns, key, _, _, _, shown in entries:
            if key.count(":") != 1:         # 类:属性；函数参数是 类:函数:参数，不在这里比
                continue
            name = key.split(":", 1)[1]
            # 没写 DisplayName 的属性，显示名就是函数的结果；写了的（约 5%）与函数无关，所以只要求绝大多数对得上。
            if shown in (r.name_to_display_string(name, True), r.name_to_display_string(name, False)):
                ok += 1
            else:
                bad += 1
        self.assertGreater(ok, 60000)
        self.assertLess(bad / float(ok + bad), 0.06, "移植与引擎的显示名对不上的比例异常地高，多半是移植坏了")


class EngineLocalizationConventionTests(unittest.TestCase):
    """钉死引擎对属性面板用的命名空间 / key / 源文约定（见 scan_reflection.py 文件头的出处）。
    引擎自带的 locres 就是这些约定的实物：升级引擎后这里先红，比译文悄悄失效好。"""

    def test_every_engine_entry_stores_the_crc_of_its_native_string(self):
        # 英文（原生文化）那份里「源文 CRC == 显示字符串的 CRC」，也就是引擎拿元数据的**原字符串**去算 CRC。
        for target in ("PropertyNames", "ToolTips", "Category"):
            entries = read_engine_locres(target)
            if entries is None:
                self.skipTest("本机没有引擎的 %s.locres" % target)
            wrong = [(ns, key) for ns, key, _, _, crc, shown in entries if crc != b.str_crc32(shown)]
            self.assertEqual(wrong[:3], [], target)

    def test_namespaces_and_key_shapes(self):
        names = read_engine_locres("PropertyNames")
        tips = read_engine_locres("ToolTips")
        cats = read_engine_locres("Category")
        if names is None or tips is None or cats is None:
            self.skipTest("本机没有引擎的 locres")
        self.assertEqual({e[0] for e in names}, {r.UOBJECT_DISPLAY_NAMES})
        self.assertEqual({e[0] for e in tips}, {r.UOBJECT_TOOLTIPS, "UObjectShortTooltips"})
        self.assertEqual({e[0] for e in cats}, {r.UOBJECT_CATEGORY})
        by_name = {(e[0], e[1]): e[5] for e in names}
        self.assertEqual(by_name[(r.UOBJECT_DISPLAY_NAMES, "Actor:bCanBeDamaged")], "Can be Damaged")   # 类名去掉 A 前缀；冒号；bool 的 b 前缀保留在 key 里
        self.assertIn((r.UOBJECT_DISPLAY_NAMES, "DeveloperSettings"), by_name)                           # 类本身的 key 就是反射名（UDeveloperSettings 去掉 U）
        self.assertIn((r.UOBJECT_TOOLTIPS, "DeveloperSettings"), {(e[0], e[1]) for e in tips})
        by_cat = {e[1]: e[5] for e in cats}
        self.assertEqual(by_cat["ACL Options"], "ACL Options")                                          # 分类：key 与源文都是分类字符串本身


class ReflectionScannerTests(unittest.TestCase):
    """用临时的「插件目录」跑 scan_reflection.scan。"""

    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="dtrefl-")
        write(cls.root, "Source/Mod/Public/Good.h", r"""// Copyright
#pragma once
#include "CoreMinimal.h"
#include "Good.generated.h"

/**
 * 中文说明里提到 UPROPERTY(EditAnywhere, Category = "Fake") int32 Fake; 不能被当成真的。
 */
UCLASS(config = Editor, meta = (DisplayName = "Good Settings", ToolTip = "The good settings."))
class MODULE_API UGoodSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** 中文注释，不会被用作提示 */
	UPROPERTY(EditAnywhere, config, Category = "Alpha", meta = (ToolTip = "Plain one."))
	FString PlainName;

	UPROPERTY(EditAnywhere, Category = "Alpha", meta = (ToolTip = "A flag."))
	bool bEnableThing = false;

	UPROPERTY(VisibleAnywhere, Category = "Beta", meta = (DisplayName = "Custom (ms)", ClampMin = "1", ToolTip = "Uses an explicit name; has ; and , inside."))
	int32 Timeout = 5;

	UPROPERTY(EditAnywhere, Category = "Beta", meta = (ToolTip = "Bit field."))
	uint8 bPacked : 1;

	UPROPERTY(EditAnywhere, Category = Beta, meta = (ToolTip = "Initializer with a string."))
	FString Url = TEXT("http://a;b=c");

	UPROPERTY(EditAnywhere, Category = "Alpha", meta = (ToolTip = "Template type."))
	TMap<FString, int32> Table;

	UPROPERTY(Transient)
	int32 NotInThePanel;

	UPROPERTY(BlueprintReadOnly)
	int32 AlsoNotInThePanel;

	// 注释里的 UPROPERTY(EditAnywhere) int32 FakeInComment;
	const TCHAR* Text = TEXT("UPROPERTY(EditAnywhere, Category = \"X\") int32 InString;");
};

USTRUCT()
struct FGoodStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Alpha", meta = (ToolTip = "Struct member."))
	int32 Count;
};

static const char* Raw = R"x(UPROPERTY(EditAnywhere, Category = "Raw") int32 InRaw; ")x";
""")
        write(cls.root, "Source/Mod/Public/Bad.h", r"""#pragma once
/** 有注释没有 ToolTip */
UCLASS()
class MODULE_API UBadSettings : public UObject
{
	GENERATED_BODY()
public:
	/** 中文注释，没有 ToolTip */
	UPROPERTY(EditAnywhere, Category = "A")
	int32 NoTip;

	UPROPERTY(EditAnywhere, Category = "A")
	int32 NoTipNoComment;

	UPROPERTY(EditAnywhere, Category = "A", meta = (ToolTip = "中文提示"))
	int32 Chinese;

	UPROPERTY(EditAnywhere, Category = "A", meta = (ToolTip = "back\slash"))
	int32 Slash;

	UPROPERTY(EditAnywhere, Category = "A", meta = (ToolTip = "adjacent " "literals"))
	int32 Adjacent;

	UPROPERTY(EditAnywhere, Category = "A", meta = (ToolTip = "LONG"))
	int32 Long;

	UPROPERTY(EditAnywhere, meta = (ToolTip = "No category."))
	int32 NoCategory;

	UPROPERTY(EditAnywhere, Category = "A|B", meta = (ToolTip = "Nested."))
	int32 Nested;

	UFUNCTION()
	void Func();
};

UENUM()
enum class EBad : uint8 { A };

UPROPERTY(EditAnywhere, Category = "A", meta = (ToolTip = "Outside."))
int32 Stray;
""".replace("LONG", "x" * 1100))
        write(cls.root, "Source/Mod/Public/DupA.h", r"""UCLASS(meta = (DisplayName = "One"))
class UDup : public UObject {};
""")
        write(cls.root, "Source/Mod/Public/DupB.h", r"""UCLASS(meta = (DisplayName = "Two"))
class UDup : public UObject {};
""")
        write(cls.root, "Source/Mod/Private/Tests/TestOnly.h", r"""UCLASS()
class UTestOnly : public UObject
{
	UPROPERTY(EditAnywhere, Category = "T")
	int32 NoTipButItIsATest;
};
""")
        write(cls.root, "Source/Mod/Private/Impl.cpp", r"""UPROPERTY(EditAnywhere) int32 NotAHeader;
""")
        cls.result = r.scan(cls.root)
        cls.entries = {(e["ns"], e["key"]): e for e in cls.result.entries}

    def src(self, ns, key):
        return self.entries[(ns, key)]["src"]

    def problems(self, rule, name=None):
        return [p for p in self.result.problems if p.rule == rule and (name is None or p.file.endswith(name))]

    def test_good_header_has_no_problems_and_the_expected_entries(self):
        self.assertEqual([str(p) for p in self.result.problems if p.file.endswith("Good.h")], [])
        D, T, C = r.UOBJECT_DISPLAY_NAMES, r.UOBJECT_TOOLTIPS, r.UOBJECT_CATEGORY
        self.assertEqual(self.src(D, "GoodSettings"), "Good Settings")              # U 前缀去掉；DisplayName 元数据优先
        self.assertEqual(self.src(T, "GoodSettings"), "The good settings.")
        self.assertEqual(self.src(D, "GoodSettings:PlainName"), "Plain Name")
        self.assertEqual(self.src(T, "GoodSettings:PlainName"), "Plain one.")
        self.assertEqual(self.src(D, "GoodSettings:bEnableThing"), "Enable Thing")  # bool 去 b 前缀；key 保留 b
        self.assertEqual(self.src(D, "GoodSettings:Timeout"), "Custom (ms)")
        self.assertEqual(self.src(T, "GoodSettings:Timeout"), "Uses an explicit name; has ; and , inside.")
        self.assertEqual(self.src(D, "GoodSettings:bPacked"), "Packed")             # 位域也是 FBoolProperty
        self.assertEqual(self.src(D, "GoodSettings:Url"), "Url")
        self.assertEqual(self.src(D, "GoodSettings:Table"), "Table")
        self.assertEqual(self.src(D, "GoodStruct"), "Good Struct")                  # F 前缀去掉，没写 DisplayName 走 NameToDisplayString
        self.assertEqual(self.src(D, "GoodStruct:Count"), "Count")
        self.assertEqual(self.src(T, "GoodStruct:Count"), "Struct member.")
        self.assertNotIn((T, "GoodStruct"), self.entries)                           # 没有注释也没有 ToolTip：没有提示，也不算错
        self.assertEqual(self.src(C, "Alpha"), "Alpha")                             # 分类：key 与源文都是分类字符串
        self.assertEqual(self.src(C, "Beta"), "Beta")                               # 裸标识符的 Category 也认
        self.assertEqual(self.entries[(D, "GoodSettings:Timeout")]["file"], "Source/Mod/Public/Good.h")

    def test_things_that_must_not_be_picked_up(self):
        keys = {k for _, k in self.entries}
        for fake in ("Fake", "FakeInComment", "InString", "InRaw", "NotInThePanel", "AlsoNotInThePanel", "NotAHeader", "NoTipButItIsATest"):
            self.assertFalse([k for k in keys if k.endswith(":" + fake)], fake)
        self.assertNotIn("TestOnly", keys)       # Tests/ 下的头文件不查
        self.assertNotIn("Raw", keys)

    def test_m1_needs_an_explicit_tooltip(self):
        found = self.problems("M1", "Bad.h")
        text = "\n".join(p.message for p in found)
        for name in ("UBadSettings", "BadSettings::NoTip", "BadSettings::NoTipNoComment", "BadSettings::Adjacent"):
            self.assertIn(name, text)
        self.assertEqual(len(found), 4)
        self.assertTrue(any("中文" in p.message for p in found))                      # 有注释的：说明中文注释会冒充提示
        self.assertTrue(any("NoTipNoComment" in p.message and "什么都没有" in p.message for p in found))

    def test_m2_meta_strings_must_be_plain_ascii_single_literals_under_the_uht_limit(self):
        self.assertEqual(len(self.problems("M2", "Bad.h")), 4)
        text = "\n".join(p.message for p in self.problems("M2", "Bad.h"))
        for needle in ("非 ASCII", "反斜杠", "相邻字符串字面量", "上限是 %d" % r.MAX_STRING_LENGTH):
            self.assertIn(needle, text)

    def test_m3_category_is_required_and_flat(self):
        self.assertEqual(len(self.problems("M3", "Bad.h")), 2)

    def test_m4_unsupported_reflection_constructs_are_refused_not_ignored(self):
        text = "\n".join(p.message for p in self.problems("M4", "Bad.h"))
        for needle in ("UFUNCTION", "UENUM", "不在任何 UCLASS / USTRUCT 的类体里"):
            self.assertIn(needle, text)
        self.assertEqual(len(self.problems("M4", "Bad.h")), 3)

    def test_m5_same_key_with_different_text(self):
        self.assertEqual(len(self.problems("M5")), 1)

    def test_problems_carry_file_and_line(self):
        for p in self.problems("M1", "Bad.h"):
            self.assertGreater(p.line, 0)
        ufunction = [p for p in self.problems("M4", "Bad.h") if "UFUNCTION" in p.message][0]
        with open(os.path.join(self.root, "Source", "Mod", "Public", "Bad.h"), encoding="utf-8") as f:
            lines = f.read().split(chr(10))
        self.assertEqual(ufunction.line, lines.index(chr(9) + "UFUNCTION()") + 1)

    def test_repository_headers_are_clean(self):
        # 真仓库：没有问题，且项目设置页该有的条目都在。
        result = r.scan(c.PLUGIN)
        self.assertEqual([str(p) for p in result.problems], [])
        keys = {(e["ns"], e["key"]) for e in result.entries}
        self.assertIn((r.UOBJECT_DISPLAY_NAMES, "DreamTextureSettings:bCreateMaterialInstance"), keys)
        self.assertIn((r.UOBJECT_TOOLTIPS, "DreamTextureSettings:BackendExecutable"), keys)
        self.assertIn((r.UOBJECT_CATEGORY, "Connection"), keys)


class ReflectionMergeTests(unittest.TestCase):
    def test_merge_adds_entries_and_reports_conflicts(self):
        existing = [{"ns": "A", "key": "K", "src": "one", "file": "x.cpp"}]
        scan = r.ReflectionScan()
        scan.entries = [{"ns": "UObjectCategory", "key": "Cat", "src": "Cat", "file": "x.h"},
                        {"ns": "A", "key": "K", "src": "one", "file": "x.h"}]
        merged, conflicts = s.merge_reflection(existing, scan)
        self.assertEqual([c.entry_id(e) for e in merged], ["A|K", "UObjectCategory|Cat"])
        self.assertEqual(conflicts, [])
        scan.entries = [{"ns": "A", "key": "K", "src": "two", "file": "x.h"}]
        self.assertEqual(len(s.merge_reflection(existing, scan)[1]), 1)


if __name__ == "__main__":
    c.setup_output()
    unittest.main(verbosity=1)
