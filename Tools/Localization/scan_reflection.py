#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描 UCLASS / USTRUCT / UPROPERTY 的元数据，给「项目设置页」这类属性面板补上本地化条目。

LOCTEXT / NSLOCTEXT 是代码里显式写的；属性面板上的文字不是——属性的显示名、悬停提示、分类，
引擎在运行时用另外三个固定的命名空间去实时表里查（都在 CoreUObject / Engine 源码里核实过）：

    显示名   命名空间 UObjectDisplayNames   key = <类名>:<属性名>   源文 = 元数据 DisplayName，没有就是 FName::NameToDisplayString(属性名, 是不是 bool)
             （Field.cpp 的 FField::GetDisplayNameText；类 / 结构体本身走 Class.cpp 的 UField::GetDisplayNameText，key = <类名>）
    提示     命名空间 UObjectToolTips       key 同上                源文 = 元数据 ToolTip（逐字）
             （Field.cpp 的 FField::GetToolTipText；类走 Class.cpp 的 UField::GetToolTipText）
    分类     命名空间 UObjectCategory       key = 分类字符串        源文 = 分类字符串（逐字）
             （ObjectEditorUtils.cpp 的 FObjectEditorUtils::GetCategoryText）

类名 / 属性名是 UHT 生成的反射名：类去掉 U/A 前缀、结构体去掉 F 前缀，属性名原样（bool 的 b 前缀保留）。
key 里的冒号来自 FField::GetPathName（拥有者路径 + SUBOBJECT_DELIMITER_CHAR）。

引擎拿**元数据里的原字符串**去算源文 CRC，对不上就静默丢掉译文。所以本模块要求：

  * 每个会显示在面板上的 UPROPERTY（带 EditAnywhere / VisibleAnywhere 之类）必须写显式的英文
    meta=(ToolTip="…")。UHT 在没有显式 ToolTip 时会取头文件里的文档注释当提示——而这个项目的注释是中文，
    英文界面就会看到中文提示。文档注释照样保留（仓库约定注释写中文），显式 ToolTip 优先（UhtParsingScope.cs
    的 AddFormattedCommentsAsTooltipMetaData：已经有 ToolTip 就不再用注释）。
  * meta 里的字符串只许 ASCII、不许反斜杠、不许相邻字面量拼接、单个字面量（含引号）不到 1024 个字符。
    UHT 的字符串常量上限是 1024（UhtToken.MaxStringLength）；反斜杠和引号经 UHT 的 GetTokenString 之后到运行时
    是什么样没法在这里离线保证，宁可禁掉。

规则（都是错误，sync_loctext.py 遇到就拒绝生成清单）：

  M1  面板上可见的属性、或带文档注释的类 / 结构体，没有显式的非空 ToolTip
  M2  meta 里的字符串不合规：含非 ASCII（含中文）、反斜杠、相邻字面量，或长度超限
  M3  属性缺 Category、或 Category 含 |（嵌套分类这里没验证过，先拒绝）
  M4  非测试头文件里有扫描器还不认的反射构造（UENUM / UFUNCTION / UINTERFACE / UDELEGATE），
      或看不懂的 UCLASS / UPROPERTY 声明——它们的显示名和提示同样走上面的命名空间，不能静默漏掉
  M5  同一个 命名空间|key 出现了两次且源文不同

这个模块只读源码、不碰引擎；输出的条目和 LOCTEXT 的条目同一格式（ns / key / src / file）。
"""
import bisect
import os
import re

UOBJECT_DISPLAY_NAMES = "UObjectDisplayNames"
UOBJECT_TOOLTIPS = "UObjectToolTips"
UOBJECT_CATEGORY = "UObjectCategory"

# UHT 对单个字符串常量的上限（UhtToken.MaxStringLength，含两端引号）。
MAX_STRING_LENGTH = 1024

# 带这些说明符的属性会进细节面板（CPF_Edit）。其余的（Transient、BlueprintReadOnly 单独出现……）不会，也就没有面板文字。
EDIT_SPECIFIERS = {
    "editanywhere", "editdefaultsonly", "editinstanceonly",
    "visibleanywhere", "visibledefaultsonly", "visibleinstanceonly",
}

# 扫描器认得的反射宏，和认得但没做的。
SUPPORTED_TYPE_MACROS = ("UCLASS", "USTRUCT")
UNSUPPORTED_MACROS = ("UENUM", "UFUNCTION", "UINTERFACE", "UDELEGATE")


# ======================================================================================= FName::NameToDisplayString

# FName::NameToDisplayString 里强制小写的单词（UnrealNames.cpp 的 Articles）。
_ARTICLES = ("In", "As", "To", "Or", "At", "On", "If", "Be", "By", "The", "For", "And", "With", "When", "From")


def _is_lower(c):
    return "a" <= c <= "z" or (c > "\x7f" and c.islower())


def _is_upper(c):
    return "A" <= c <= "Z" or (c > "\x7f" and c.isupper())


def name_to_display_string(name, is_bool):
    """FName::NameToDisplayString 的逐行移植（Core/Private/UObject/UnrealNames.cpp，UE 5.8）。

    属性没写 DisplayName 时，面板上的显示名就是它：bCreateMaterialInstance -> "Create Material Instance"。
    原版里「有例外词」的那一支（AddNameToDisplayStringExemption）引擎自己从不调用，这里省略。
    selftest.py 拿引擎自带的 PropertyNames 英文 locres（约 7 万个 类:属性 -> 显示名）逐条回归过这个函数。
    """
    n = len(name)
    in_run = False
    was_space = was_open_paren = was_number = was_minus = False
    out = []
    i = 0
    while i < n:
        ch = name[i]
        lower = _is_lower(ch)
        upper = _is_upper(ch)
        digit = "0" <= ch <= "9"
        underscore = ch == "_"

        # bool 属性的 b 前缀不要（后面紧跟大写字母才算，免得吃掉 "bake" 的 b）。
        if i == 0 and is_bool and ch == "b" and n > 1 and _is_upper(name[1]):
            i += 1
            continue

        # 大写字母或数字、前一个不是（一串大写 / 数字的中间），要先插个空格。
        if (upper or (digit and not was_minus)) and not in_run and not was_open_paren and not was_number:
            if not was_space and len(out) > 0:
                out.append(" ")
                was_space = True
            in_run = True

        if lower:
            in_run = False

        if underscore:
            ch = " "
            in_run = True

        if len(out) == 0:
            ch = ch.upper()
        elif not digit and (was_space or was_open_paren):
            # 空格后面的第一个字母：冠词 / 介词类的单词保持小写，其余大写。
            is_article = False
            for article in _ARTICLES:
                length = len(article)
                # 原版条件：(Chars.Num() - CharIndex) > ArticleLength，Chars 带结尾 NUL，等价于后面至少还有一个字符。
                if n - i > length and not _is_lower(name[i + length]) and name[i + length] != "\0":
                    if name[i:i + length] == article:
                        is_article = True
                        break
            ch = ch.lower() if is_article else ch.upper()

        was_space = ch == " "
        was_open_paren = ch == "("
        was_minus = ch == "-"
        potential_numeric = was_minus or ch == "."
        was_number = digit or (was_number and potential_numeric)
        out.append(ch)
        i += 1
    return "".join(out)


def engine_type_name(source_name):
    """UHT 的反射名：UDreamTextureSettings -> DreamTextureSettings，FFoo -> Foo。"""
    if len(source_name) > 1 and source_name[0] in "UAFEI" and (source_name[1].isupper() or source_name[1].isdigit()):
        return source_name[1:]
    return None


# ======================================================================================= 词法

def _blank(chars, start, end):
    for k in range(start, end):
        if chars[k] not in "\r\n":
            chars[k] = " "


_IDENT = re.compile(r"[A-Za-z0-9_]")


def lex(text):
    """-> (code, skeleton)，与 text 等长，偏移通用。

    code      注释抹成空白（保留换行），字符串原样。解析说明符、读字面量在这里。
    skeleton  再把字符串 / 字符常量的内容也抹成空白（引号留着）。找括号、分号、标识符在这里，字符串里的 ; ) 不会误伤。
    """
    n = len(text)
    code = list(text)
    skel = list(text)
    i = 0
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            _blank(code, i, j)
            _blank(skel, i, j)
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            _blank(code, i, j)
            _blank(skel, i, j)
            i = j
        elif c == '"':
            raw = _raw_string_end(text, i)
            if raw is not None:
                _blank(skel, i + 1, raw - 1)
                i = raw
                continue
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n - 1)
            _blank(skel, i + 1, j)
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j, n - 1)
            _blank(skel, i + 1, j)
            i = j + 1
        else:
            i += 1
    return "".join(code), "".join(skel)


def _raw_string_end(text, quote):
    """text[quote] 是 " 且前面是 R（可带 u8 / u / U / L 前缀）时，返回原始字符串结束后的下标；否则 None。"""
    if quote == 0 or text[quote - 1] != "R":
        return None
    k = quote - 2
    while k >= 0 and text[k] in "u8UL":
        k -= 1
    if k >= 0 and _IDENT.match(text[k]):
        return None
    m = re.compile(r'"([^()\\\s"]{0,16})\(').match(text, quote)
    if not m:
        return None
    end = text.find(")" + m.group(1) + '"', m.end())
    if end < 0:
        return len(text)
    return end + len(m.group(1)) + 2


def match_close(skel, open_index, open_char, close_char):
    """skel[open_index] 是 open_char，返回配对的 close_char 下标；找不到返回 -1。"""
    depth = 0
    for k in range(open_index, len(skel)):
        c = skel[k]
        if c == open_char:
            depth += 1
        elif c == close_char:
            depth -= 1
            if depth == 0:
                return k
    return -1


def split_top_level(text, sep=","):
    """按 sep 切，括号 / 尖括号以外、字符串以外才算数。"""
    parts = []
    depth = 0
    start = 0
    k = 0
    n = len(text)
    while k < n:
        c = text[k]
        if c == '"':
            k += 1
            while k < n and text[k] != '"':
                k += 2 if text[k] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:k])
            start = k + 1
        k += 1
    parts.append(text[start:])
    return [p.strip() for p in parts if p.strip() != ""]


# ======================================================================================= 说明符

_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"')


class MetaValue(object):
    """meta 里的一个值：要么是一整个字符串字面量（string 不为 None），要么是裸标识符 / 数字。"""

    def __init__(self, raw):
        self.raw = raw.strip()
        self.literals = _LITERAL.findall(self.raw)
        self.is_string = bool(self.literals) and _LITERAL.sub("", self.raw).strip() == ""
        self.value = self.literals[0][1:-1] if self.is_string and len(self.literals) == 1 else None
        # Category = Beta 这种裸标识符 UHT 也认，取值就是它本身；字符串字面量取引号里的内容。
        self.text = self.value if self.is_string else self.raw

    def problems(self):
        """-> [说明]，空表示合规。"""
        if not self.is_string:
            return []
        out = []
        if len(self.literals) > 1:
            out.append("相邻字符串字面量拼接（UHT 的说明符里不支持）")
        for lit in self.literals:
            body = lit[1:-1]
            if "\\" in body:
                out.append("含反斜杠（UHT 对转义的处理到运行时不可预测，改写成不需要转义的句子）")
            if any(ord(ch) > 0x7F for ch in body):
                out.append("含非 ASCII 字符（中文译文放 translations/*.json；英文源文只用 ASCII）")
            if len(lit) >= MAX_STRING_LENGTH:
                out.append("字面量（含引号）%d 个字符，UHT 的上限是 %d" % (len(lit), MAX_STRING_LENGTH))
        return out


def parse_specifiers(args):
    """UCLASS( … ) / UPROPERTY( … ) 括号里的内容 -> (flags: set[小写名], values: {小写名: MetaValue}, meta: {小写名: MetaValue})

    flags   没有 = 的项（EditAnywhere、config、AdvancedDisplay）；
    values  Key=Value 的项（Category="x"）；meta 里的 Key=Value 单独放 meta。
    """
    flags = set()
    values = {}
    meta = {}
    for item in split_top_level(args):
        if "=" not in item:
            flags.add(item.strip().lower())
            continue
        key, _, value = item.partition("=")
        key = key.strip().lower()
        value = value.strip()
        if key == "meta":
            inner = value
            if inner.startswith("(") and inner.endswith(")"):
                inner = inner[1:-1]
            for entry in split_top_level(inner):
                if "=" in entry:
                    mk, _, mv = entry.partition("=")
                    meta[mk.strip().lower()] = MetaValue(mv)
                else:
                    meta[entry.strip().lower()] = MetaValue("")
        else:
            values[key] = MetaValue(value)
    return flags, values, meta


# ======================================================================================= 扫描

class Problem(object):
    def __init__(self, rule, file, line, message):
        self.rule = rule
        self.file = file
        self.line = line
        self.message = message

    def __str__(self):
        return "%s:%d [%s] %s" % (self.file, self.line, self.rule, self.message)


class ReflectionScan(object):
    def __init__(self):
        self.entries = []      # [{"ns","key","src","file"}]，与 loctext.json 同格式
        self.problems = []     # [Problem]
        self.types = []        # 诊断用：[(文件, 反射名)]
        self.properties = []   # 诊断用：[(类反射名, 属性名, 是否在面板上可见)]


def _has_valid_comment_char(comment):
    """UHT 的 HasValidCommentChar：有字母数字或码位 > 0xFF 的字才会生成提示。"""
    return any(ch.isalnum() and ord(ch) < 0x80 or ord(ch) > 0xFF for ch in comment)


def _doc_comment_before(text, code, index):
    """macro 前面紧挨着的注释文本（一个或几个）；没有返回空串。

    从 index 往回走，空白和注释里的字符都算，碰到真的代码就停。这是保守的近似：只用来判断「没有显式 ToolTip 时
    会不会有一段注释被 UHT 拿去当提示」，用不着复刻 UHT 对空行 / 注释块的取舍。
    """
    k = index - 1
    start = index
    while k >= 0:
        in_comment = text[k] != code[k]
        if in_comment or text[k].isspace():
            start = k
            k -= 1
        else:
            break
    return "".join(text[x] for x in range(start, index) if text[x] != code[x])


def _declaration_name(skel_decl):
    """UPROPERTY 后面的声明（到分号之前，字符串已抹空）-> (属性名, 类型文字, 是不是位域)；看不懂返回 None。"""
    decl = skel_decl
    # 初始化器：= 或 {（但不是 ==、<=、>=），[ 是 C 数组。
    cut = len(decl)
    for m in re.finditer(r"=|\{|\[", decl):
        if m.group() == "=" and (decl[m.start() - 1:m.start()] in ("=", "<", ">", "!") or decl[m.end():m.end() + 1] == "="):
            continue
        cut = m.start()
        break
    decl = decl[:cut].strip()
    bitfield = False
    m = re.search(r"(?<!:):(?!:)\s*\d+\s*$", decl)
    if m:
        bitfield = True
        decl = decl[:m.start()].strip()
    m = re.search(r"([A-Za-z_]\w*)\s*$", decl)
    if not m:
        return None
    return m.group(1), decl[:m.start()].strip(), bitfield


def scan_file(text, rel, result):
    code, skel = lex(text)
    line_starts = [0] + [m.end() for m in re.finditer("\n", text)]

    def line_of(index):
        return bisect.bisect_right(line_starts, index)

    def problem(rule, index, message):
        result.problems.append(Problem(rule, rel, line_of(index), message))

    # 反射宏：UCLASS( UPROPERTY( 之类。skeleton 里字符串内容已抹空，字符串里提到宏名不会被当成宏。
    macros = []
    for m in re.finditer(r"(?<![A-Za-z0-9_])(UCLASS|USTRUCT|UENUM|UINTERFACE|UPROPERTY|UFUNCTION|UDELEGATE)\s*\(", skel):
        close = match_close(skel, m.end() - 1, "(", ")")
        if close < 0:
            problem("M4", m.start(), "%s( 的括号没有配上，扫描器读不下去" % m.group(1))
            continue
        macros.append((m.group(1), m.start(), m.end(), close))

    types = []   # (反射名, 类型的源码名, body_start, body_end)
    for name, start, args_start, close in macros:
        if name in UNSUPPORTED_MACROS:
            problem("M4", start, "%s 的显示名与提示同样走 UObjectDisplayNames / UObjectToolTips，但扫描器还不认它：先在 scan_reflection.py 里加支持，再用它" % name)
            continue
        if name not in SUPPORTED_TYPE_MACROS:
            continue
        m = re.compile(r"\s*(?:class|struct)\s+(?:[A-Z][A-Z0-9_]*_API\s+)?([A-Za-z_]\w*)").match(skel, close + 1)
        if not m:
            problem("M4", start, "%s 后面看不懂 class / struct 声明" % name)
            continue
        source_name = m.group(1)
        engine_name = engine_type_name(source_name)
        if engine_name is None:
            problem("M4", start, "%s 不符合 UE 命名（U/A/F/E/I 前缀 + 大写字母），算不出反射名" % source_name)
            continue
        brace = skel.find("{", m.end())
        semi = skel.find(";", m.end())
        if brace < 0 or (0 <= semi < brace):
            problem("M4", start, "%s 只有前置声明，没有类体" % source_name)
            continue
        body_end = match_close(skel, brace, "{", "}")
        if body_end < 0:
            body_end = len(skel)
        types.append((engine_name, source_name, brace, body_end))

        flags, values, meta = parse_specifiers(code[args_start:close])
        result.types.append((rel, engine_name))
        _check_meta_strings(meta, problem, start)

        # 类的显示名：DisplayName 元数据，没有就是 NameToDisplayString(类名)（UField::GetDisplayNameText，原生类取 GetName()）。
        display = meta.get("displayname")
        if display is not None and display.is_string and display.value:
            display_src = display.value
        else:
            display_src = name_to_display_string(engine_name, False)
        _add(result, UOBJECT_DISPLAY_NAMES, engine_name, display_src, rel)

        tooltip = meta.get("tooltip")
        comment = _doc_comment_before(text, code, start)
        if tooltip is not None and tooltip.is_string and tooltip.value:
            _add(result, UOBJECT_TOOLTIPS, engine_name, tooltip.value, rel)
        elif _has_valid_comment_char(comment):
            problem("M1", start, "%s 有文档注释但没有显式 ToolTip：UHT 会拿注释当类的提示（注释是中文，英文界面会看到中文）。"
                    "加 meta=(ToolTip=\"English text\")，注释照留" % source_name)

    for name, start, args_start, close in macros:
        if name != "UPROPERTY":
            continue
        owner = None
        for t in types:
            if t[2] < start < t[3] and (owner is None or t[2] > owner[2]):
                owner = t
        if owner is None:
            problem("M4", start, "UPROPERTY 不在任何 UCLASS / USTRUCT 的类体里，扫描器无法确定它属于谁")
            continue
        engine_name = owner[0]

        semi = skel.find(";", close + 1)
        decl = _declaration_name(skel[close + 1:semi]) if semi >= 0 else None
        if decl is None:
            problem("M4", start, "看不懂 UPROPERTY 后面的声明，算不出属性名")
            continue
        prop, type_text, bitfield = decl
        flags, values, meta = parse_specifiers(code[args_start:close])
        visible = bool(flags & EDIT_SPECIFIERS)
        result.properties.append((engine_name, prop, visible))
        _check_meta_strings(meta, problem, start)
        if not visible:
            continue     # 不进细节面板，没有面板文字

        is_bool = type_text == "bool" or bitfield
        key = "%s:%s" % (engine_name, prop)

        display = meta.get("displayname")
        if display is not None and display.is_string and display.value:
            display_src = display.value
        else:
            display_src = name_to_display_string(prop, is_bool)
        _add(result, UOBJECT_DISPLAY_NAMES, key, display_src, rel)

        tooltip = meta.get("tooltip")
        if tooltip is not None and tooltip.is_string and tooltip.value:
            _add(result, UOBJECT_TOOLTIPS, key, tooltip.value, rel)
        else:
            comment = _doc_comment_before(text, code, start)
            hint = ("它现在会取头文件里的文档注释当提示，注释是中文，英文界面会看到中文。" if _has_valid_comment_char(comment)
                    else "没有注释也没有 ToolTip，面板上悬停什么都没有。")
            problem("M1", start, "%s::%s 在面板上可见却没有显式 ToolTip：%s加 meta=(ToolTip=\"English text\")，文档注释照留"
                    % (engine_name, prop, hint))

        category = values.get("category")
        if category is None or not category.text:
            problem("M3", start, "%s::%s 没有 Category=\"…\"" % (engine_name, prop))
        elif category.problems():
            for why in category.problems():
                problem("M2", start, "Category：%s" % why)
        elif "|" in category.text:
            problem("M3", start, "%s::%s 的 Category 含 |（嵌套分类）：这里没验证过引擎对子分类怎么查本地化，先拒绝" % (engine_name, prop))
        else:
            _add(result, UOBJECT_CATEGORY, category.text, category.text, rel)


def _check_meta_strings(meta, problem, index):
    for key in ("tooltip", "displayname"):
        value = meta.get(key)
        if value is None or not value.is_string:
            continue
        for why in value.problems():
            problem("M2", index, "meta %s：%s" % ("ToolTip" if key == "tooltip" else "DisplayName", why))


def _add(result, ns, key, src, rel):
    """同一个 ns|key 第一次出现的文件为准；再次出现源文必须相同（分类会被多个属性重复提到）。"""
    for e in result.entries:
        if e["ns"] == ns and e["key"] == key:
            if e["src"] != src:
                result.problems.append(Problem("M5", rel, 0, "%s|%s 出现了两种源文：%r 与 %r" % (ns, key, e["src"], src)))
            return
    result.entries.append({"ns": ns, "key": key, "src": src, "file": rel})


def scan(plugin_root):
    """扫描 plugin_root/Source 下所有非测试头文件。"""
    import io
    result = ReflectionScan()
    source_root = os.path.join(plugin_root, "Source")
    paths = []
    for root, _, names in os.walk(source_root):
        for name in names:
            if name.lower().endswith(".h"):
                rel = os.path.relpath(os.path.join(root, name), plugin_root).replace(os.sep, "/")
                if re.search(r"(^|/)Tests/", rel):
                    continue
                paths.append(rel)
    for rel in sorted(paths):       # 字节序，与 localization_lint.ps1 一致：同一个 key 记第一个出现的文件
        with io.open(os.path.join(plugin_root, *rel.split("/")), encoding="utf-8-sig", newline="") as f:
            text = f.read()
        if not re.search(r"\bU(?:CLASS|STRUCT|ENUM|INTERFACE|PROPERTY|FUNCTION|DELEGATE)\b", text):
            continue
        scan_file(text, rel, result)
    return result
