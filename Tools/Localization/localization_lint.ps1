#Requires -Version 7.0
<#
.SYNOPSIS
    DreamTextureForUnreal 的本地化扫描与 lint（从 DreamShader 的 localization_lint.ps1 移植，规则按本插件的约定重写）。

.DESCRIPTION
    做两件事：

      1. 扫描  Source/**/*.{h,cpp} 里的 LOCTEXT / NSLOCTEXT，解码 C 转义，得到「运行时看到的源文」清单。
               清单交给 sync_loctext.py 写成 loctext.json，再由 build_locres.py 做成 .locres。
               清单里每条的 src 必须与编译器给 LOCTEXT 的字符串逐字相同——.locres 里存的是它的 CRC，
               对不上，引擎加载时静默丢掉译文（见 build_locres.py 的说明）。
      2. lint   约定见 Docs/contributing.md 的 Localization 一节：界面文字的源文是英文，中文是 zh-Hans 译文。

    规则（错误让退出码为 1；警告只提醒）：

      R1   错误  非测试源码里的字符串字面量含中日韩字符。界面文字改 LOCTEXT/NSLOCTEXT（源文英文），
                 UE_LOG 与断言消息也必须是英文。注释不查。LOCTEXT 的源文含中文也是这条——中文译文放 translations/*.json。
      R2   错误  FText::FromString / FText::FromName 里直接放含英文字母的字面量：gather 看不见它，翻不了。
                 改 LOCTEXT；确实不该翻的（专名、协议字面量）登记豁免。纯标点、换行、空串不查。
      R3   警告  同一个 命名空间|key 在多处出现且源文相同（引擎允许，但改一处忘一处就变成 R8）。
      R4   错误  头文件里 #define LOCTEXT_NAMESPACE（会漏进所有包含它的文件），头文件里用 NSLOCTEXT。
      R5   错误  LOCTEXT 前面没有有效的 #define LOCTEXT_NAMESPACE。
      R6   警告  #define LOCTEXT_NAMESPACE 之后没有对应的 #undef（unity 合并时会漏进下一个文件）。
      R7   错误  LOCTEXT / NSLOCTEXT 的参数不是纯字符串字面量（变量、宏、拼接表达式）：扫描器和 gather 都读不到它。
                 相邻字面量 "a" "b" 与 TEXT("…") 包装是允许的，和编译器一样会拼接。
      R8   错误  同一个 命名空间|key 对应了不同的源文。
      R9   警告  FText::AsLocalizable_Advanced：动态引用，扫描器看不见。
      R10  错误  I18N-EXEMPT 注释没写原因。
      R11  错误  命名空间或 key 里含 | （译文文件的键是 "命名空间|key"，含 | 就拆不开）。
      R12  错误  key 或源文为空，或者里面有 NUL（\0）——运行时的字符串在那里就截断了。
      R13  错误  豁免清单有问题：没写原因，或者已经没有任何东西匹配它（过期的豁免必须删）。

    测试源码（路径里有 Tests/ 的）不查 R1 / R2 / R9：测试里断言中文、造中文输入是正常的。
    测试里的 LOCTEXT 仍然进清单（和 GatherText 一样不分文件），所以要有译文。

    豁免有两种，都必须写原因：
      - 行内：同一行写  // I18N-EXEMPT: 原因   （只对那一行的字面量生效）
      - 清单：Tools/Localization/lint_exemptions.json，每条 {rule, file, contains, reason}

.PARAMETER Json
    不打人读的报告，输出 JSON（summary / violations / warnings / exempted / inventory），并且**总是退出 0**——
    这是给 sync_loctext.py 取清单用的数据模式，lint 结果由人读模式的退出码负责。

.PARAMETER OutFile
    配合 -Json：把 JSON 写到这个文件（UTF-8 无 BOM），不写到标准输出。控制台代码页是 936 时重定向会把中文写坏，所以工具走文件。

.PARAMETER PluginRoot
    插件根目录。默认是本脚本往上两级；自测时指向临时目录。

.PARAMETER All
    人读报告默认只列前 150 条违规，加这个全列。

.NOTES
    只需要 PowerShell 7+，不需要编辑器，不需要任何模块。
#>

[CmdletBinding()]
param(
	[switch] $Json,
	[string] $OutFile = '',
	[string] $PluginRoot = '',
	[switch] $All
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if ([string]::IsNullOrWhiteSpace($PluginRoot)) {
	$PluginRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$PluginRoot = (Resolve-Path -LiteralPath $PluginRoot).Path

# --------------------------------------------------------------------------------------- 词法

# 一个正则把注释、字符串、字符常量一次切出来，从左到右的最左匹配保证了「字符串里的 //」不会被当成注释、
# 「注释里的引号」不会被当成字符串。顺序就是优先级：原始字符串要排在普通字符串前面。
$StrBody = '(?:\\(?:\r\n|.)|[^"\\\r\n])*'
$TokenPattern =
	'(?<lc>//[^\r\n]*)' +
	'|(?<bc>/\*.*?\*/)' +
	'|(?<raw>(?:u8|u|U|L)?R"(?<d>[^()\\\s"]{0,16})\(.*?\)\k<d>")' +
	'|(?<str>(?:u8|u|U|L)?"' + $StrBody + '")' +
	'|(?<chr>(?:u8|u|U|L)?''(?:\\(?:\r\n|.)|[^''\\\r\n])+'')'
$TokenRegex = [regex]::new($TokenPattern, [System.Text.RegularExpressions.RegexOptions]::Singleline -bor [System.Text.RegularExpressions.RegexOptions]::Compiled)
$BlankRegex = [regex]::new('[^\r\n]', [System.Text.RegularExpressions.RegexOptions]::Compiled)

# 中日韩：统一表意文字（含扩展 A）、CJK 符号与标点、假名、兼容表意文字、全角半角形式，以及扩展 B 起的代理对。
# 省略号 … 与破折号 — 不算：英文界面文字里也会用。
$CjkRegex = [regex]::new('[　-〿぀-ヿ㐀-䶿一-鿿豈-﫿＀-￯]|[\uD840-\uD87F][\uDC00-\uDFFF]', [System.Text.RegularExpressions.RegexOptions]::Compiled)

# 一段「相邻字符串字面量」：可以带 u8/u/U/L 前缀，之间隔着空白或（已被抹成空白的）注释，C++ 会把它们拼起来。
$StrSeq = '(?:(?:u8|u|U|L)?"' + $StrBody + '"\s*)+'
$StrPiece = [regex]::new('(?:u8|u|U|L)?"(?<b>' + $StrBody + ')"', [System.Text.RegularExpressions.RegexOptions]::Singleline -bor [System.Text.RegularExpressions.RegexOptions]::Compiled)
$Arg = '(?:TEXT\s*\(\s*)?(?<{0}>' + $StrSeq + ')(?:\)\s*)?'
$LocTextCall = [regex]::new('\GLOCTEXT\s*\(\s*' + ($Arg -f 'key') + ',\s*' + ($Arg -f 'text') + '\)', [System.Text.RegularExpressions.RegexOptions]::Singleline)
$NsLocTextCall = [regex]::new('\GNSLOCTEXT\s*\(\s*' + ($Arg -f 'ns') + ',\s*' + ($Arg -f 'key') + ',\s*' + ($Arg -f 'text') + '\)', [System.Text.RegularExpressions.RegexOptions]::Singleline)
$FromTextCall = [regex]::new('\G(?<fn>FText::From(?:String|Name))\s*\(\s*(?:TEXT\s*\(\s*)?(?<lit>' + $StrSeq + ')', [System.Text.RegularExpressions.RegexOptions]::Singleline)

$CallStart = [regex]::new('(?<![A-Za-z0-9_])(?<k>NSLOCTEXT|LOCTEXT)\s*\(')
$FromStart = [regex]::new('(?<![A-Za-z0-9_:])FText::From(?:String|Name)\s*\(')
$AdvancedStart = [regex]::new('(?<![A-Za-z0-9_])AsLocalizable_Advanced(?:_LocText)?\s*\(')
$DefineRegex = [regex]::new('(?m)^[ \t]*#[ \t]*define[ \t]+LOCTEXT_NAMESPACE[ \t]+"(?<ns>(?:\\.|[^"\\\r\n])*)"')
$UndefRegex = [regex]::new('(?m)^[ \t]*#[ \t]*undef[ \t]+LOCTEXT_NAMESPACE\b')
$LogRegex = [regex]::new('\b(?:UE_C?LOG|UE_LOGFMT|UE_CLOGFMT|ensure(?:Msgf|Always|AlwaysMsgf)?|check(?:f|Slow|fSlow)?|verify(?:f)?|checkNoEntry)\s*\(')
$ExemptMarker = [regex]::new('I18N-EXEMPT(?<r>\s*:\s*\S.*)?')

function ConvertFrom-CBody {
	# C 字符串字面量的内容（不含两端引号）-> 运行时的字符串。必须与编译器一致，理由见文件头。
	param([string] $Body)

	if ($Body.IndexOf([char]'\') -lt 0) { return $Body }
	$sb = [System.Text.StringBuilder]::new()
	$i = 0
	$n = $Body.Length
	$hex = '0123456789abcdefABCDEF'
	while ($i -lt $n) {
		$ch = $Body[$i]
		if ($ch -ne '\') {
			[void]$sb.Append($ch)
			$i++
			continue
		}
		$i++
		if ($i -ge $n) {
			[void]$sb.Append('\')
			break
		}
		$e = [string]$Body[$i]
		switch -CaseSensitive ($e) {
			'n' { [void]$sb.Append("`n"); $i++ }
			'r' { [void]$sb.Append("`r"); $i++ }
			't' { [void]$sb.Append("`t"); $i++ }
			'a' { [void]$sb.Append([char]7); $i++ }
			'b' { [void]$sb.Append([char]8); $i++ }
			'f' { [void]$sb.Append([char]12); $i++ }
			'v' { [void]$sb.Append([char]11); $i++ }
			'\' { [void]$sb.Append('\'); $i++ }
			'"' { [void]$sb.Append('"'); $i++ }
			"'" { [void]$sb.Append("'"); $i++ }
			'?' { [void]$sb.Append('?'); $i++ }
			"`r" {
				# 行尾反斜杠 = 续行：连换行一起吃掉。
				$i++
				if ($i -lt $n -and $Body[$i] -eq "`n") { $i++ }
			}
			"`n" { $i++ }
			'x' {
				# \x 后面有多少个十六进制数字就吃多少个，和编译器一样。
				$i++
				$start = $i
				while ($i -lt $n -and $hex.IndexOf($Body[$i]) -ge 0) { $i++ }
				if ($i -eq $start) { [void]$sb.Append('\x') }
				else { [void]$sb.Append([char][Convert]::ToInt32($Body.Substring($start, $i - $start), 16)) }
			}
			'u' {
				$digits = $Body.Substring($i + 1, [Math]::Min(4, $n - $i - 1))
				if ($digits.Length -eq 4 -and $digits -match '^[0-9a-fA-F]{4}$') {
					[void]$sb.Append([char][Convert]::ToInt32($digits, 16))
					$i += 5
				}
				else { [void]$sb.Append('\u'); $i++ }
			}
			'U' {
				$digits = $Body.Substring($i + 1, [Math]::Min(8, $n - $i - 1))
				if ($digits.Length -eq 8 -and $digits -match '^[0-9a-fA-F]{8}$') {
					$cp = [Convert]::ToInt32($digits, 16)
					if ($cp -ge 0xD800 -and $cp -le 0xDFFF) { [void]$sb.Append([char]$cp) }
					elseif ($cp -le 0x10FFFF) { [void]$sb.Append([char]::ConvertFromUtf32($cp)) }
					$i += 9
				}
				else { [void]$sb.Append('\U'); $i++ }
			}
			default {
				if ($e -match '^[0-7]$') {
					# 八进制，最多三位。
					$start = $i
					while ($i -lt $n -and ($i - $start) -lt 3 -and $Body[$i] -match '^[0-7]$') { $i++ }
					[void]$sb.Append([char][Convert]::ToInt32($Body.Substring($start, $i - $start), 8))
				}
				else {
					[void]$sb.Append('\')
					[void]$sb.Append($e)
					$i++
				}
			}
		}
	}
	return $sb.ToString()
}

function Join-StrSeq {
	# 相邻字面量序列 -> 拼接并解码后的字符串。
	param([string] $Seq)
	$sb = [System.Text.StringBuilder]::new()
	foreach ($m in $StrPiece.Matches($Seq)) {
		[void]$sb.Append((ConvertFrom-CBody $m.Groups['b'].Value))
	}
	return $sb.ToString()
}

function New-Finding {
	param([string] $Rule, [string] $Severity, [string] $File, [int] $Line, [string] $Message, [string] $Literal = '')
	[pscustomobject]@{
		rule = $Rule
		severity = $Severity
		file = $File
		line = $Line
		message = $Message
		literal = $Literal
	}
}

function Get-Preview {
	param([string] $Text, [int] $Max = 48)
	$one = $Text.Replace("`r", '\r').Replace("`n", '\n').Replace("`t", '\t')
	if ($one.Length -gt $Max) { return $one.Substring(0, $Max) + '...' }
	return $one
}

# --------------------------------------------------------------------------------------- 豁免清单

$ExemptionPath = Join-Path $PluginRoot 'Tools/Localization/lint_exemptions.json'
$Exemptions = New-Object System.Collections.Generic.List[object]
$Findings = New-Object System.Collections.Generic.List[object]
if (Test-Path -LiteralPath $ExemptionPath -PathType Leaf) {
	$Parsed = Get-Content -LiteralPath $ExemptionPath -Raw -Encoding UTF8 | ConvertFrom-Json
	$Index = 0
	foreach ($Item in @($Parsed.exemptions)) {
		$Index++
		$Reason = if ($Item.PSObject.Properties['reason']) { [string]$Item.reason } else { '' }
		$Contains = if ($Item.PSObject.Properties['contains']) { [string]$Item.contains } else { '' }
		$Exemptions.Add([pscustomobject]@{
				index = $Index
				rule = [string]$Item.rule
				file = [string]$Item.file
				contains = $Contains
				reason = $Reason
				used = 0
			})
		if ([string]::IsNullOrWhiteSpace($Reason)) {
			$Findings.Add((New-Finding 'R13' 'error' 'Tools/Localization/lint_exemptions.json' 0 "第 $Index 条豁免（$($Item.file)）没写 reason：每条豁免都要说明为什么。"))
		}
	}
}

# --------------------------------------------------------------------------------------- 逐文件分析

$SourceRoot = Join-Path $PluginRoot 'Source'
# 按相对路径的**字节序**排：同一个 key 在几个文件里出现时，清单记第一个出现的文件，
# 顺序若随系统的区域排序规则变（Windows 与 Linux 的 CI 不同），loctext.json 的 file 字段就会在两边打架。
$ByPath = [System.Collections.Generic.SortedDictionary[string, System.IO.FileInfo]]::new([System.StringComparer]::Ordinal)
foreach ($Found in Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -Include *.h, *.cpp) {
	$ByPath[[System.IO.Path]::GetRelativePath($PluginRoot, $Found.FullName).Replace('\', '/')] = $Found
}
$Files = @($ByPath.Values)
$Inventory = New-Object System.Collections.Generic.List[object]
$Warnings = New-Object System.Collections.Generic.List[object]
$Exempted = New-Object System.Collections.Generic.List[object]
$SeenIds = @{}

function Test-Exempt {
	# 行内标记或清单里有没有豁免这一处。返回豁免的原因，没有返回 $null。
	param([string] $Rule, [string] $Rel, [string] $Literal, [string] $LineText, [string] $LocationFile, [int] $Line)

	$marker = $ExemptMarker.Match($LineText)
	if ($marker.Success) {
		if (-not $marker.Groups['r'].Success) {
			$Findings.Add((New-Finding 'R10' 'error' $LocationFile $Line 'I18N-EXEMPT 后面要写原因：  // I18N-EXEMPT: 为什么这里不用本地化'))
			return '<行内标记没写原因>'
		}
		return $marker.Groups['r'].Value.TrimStart(' ', "`t", ':')
	}
	foreach ($ex in $Exemptions) {
		if ($ex.rule -ne $Rule) { continue }
		if (-not ($Rel -like $ex.file)) { continue }
		if ($ex.contains -ne '' -and -not $Literal.Contains($ex.contains)) { continue }
		$ex.used++
		return $ex.reason
	}
	return $null
}

foreach ($File in $Files) {
	$Rel = [System.IO.Path]::GetRelativePath($PluginRoot, $File.FullName).Replace('\', '/')
	$IsTest = $Rel -match '(^|/)Tests/'
	$IsHeader = $File.Extension -ieq '.h'
	$Text = [System.IO.File]::ReadAllText($File.FullName)

	# 行首下标，用来把偏移换成行号。
	$LineStarts = New-Object System.Collections.Generic.List[int]
	$LineStarts.Add(0)
	for ($p = $Text.IndexOf("`n"); $p -ge 0; $p = $Text.IndexOf("`n", $p + 1)) { $LineStarts.Add($p + 1) }
	$LineStartArray = $LineStarts.ToArray()
	function Get-Line([int] $Index) {
		$r = [Array]::BinarySearch($LineStartArray, $Index)
		if ($r -ge 0) { return $r + 1 }
		return -bnot $r
	}
	function Get-LineText([int] $Index) {
		$ln = Get-Line $Index
		$s = $LineStartArray[$ln - 1]
		$e = if ($ln -lt $LineStartArray.Length) { $LineStartArray[$ln] } else { $Text.Length }
		return $Text.Substring($s, $e - $s)
	}

	# 两份掩码，长度与原文相同，所以偏移通用：
	#   $NoComments  注释抹成空白（保留换行）。参数解析、#define 在这里找。
	#   $CodeOnly    再把字符串 / 字符常量整个换成 \u0001。找「代码里出现的标识符」在这里，字符串里的 LOCTEXT( 不会误中。
	$sbC = [System.Text.StringBuilder]::new($Text.Length)
	$sbK = [System.Text.StringBuilder]::new($Text.Length)
	$Strings = New-Object System.Collections.Generic.List[object]
	$pos = 0
	foreach ($m in $TokenRegex.Matches($Text)) {
		$gap = $Text.Substring($pos, $m.Index - $pos)
		[void]$sbC.Append($gap)
		[void]$sbK.Append($gap)
		$pos = $m.Index + $m.Length
		if ($m.Groups['lc'].Success -or $m.Groups['bc'].Success) {
			$blank = $BlankRegex.Replace($m.Value, ' ')
			[void]$sbC.Append($blank)
			[void]$sbK.Append($blank)
		}
		else {
			[void]$sbC.Append($m.Value)
			[void]$sbK.Append($BlankRegex.Replace($m.Value, [string][char]1))
			if ($m.Groups['str'].Success -or $m.Groups['raw'].Success) {
				$Strings.Add([pscustomobject]@{ index = $m.Index; length = $m.Length; text = $m.Value })
			}
		}
	}
	[void]$sbC.Append($Text.Substring($pos))
	[void]$sbK.Append($Text.Substring($pos))
	$NoComments = $sbC.ToString()
	$CodeOnly = $sbK.ToString()

	# --- LOCTEXT_NAMESPACE 的 define / undef，按出现顺序排成事件流。
	$Events = New-Object System.Collections.Generic.List[object]
	foreach ($m in $DefineRegex.Matches($NoComments)) {
		$Events.Add([pscustomobject]@{ index = $m.Index; kind = 'define'; ns = (ConvertFrom-CBody $m.Groups['ns'].Value) })
		if ($IsHeader) {
			$Findings.Add((New-Finding 'R4' 'error' $Rel (Get-Line $m.Index) '头文件里不能 #define LOCTEXT_NAMESPACE（会漏进所有包含它的文件）；头文件用 NSLOCTEXT。'))
		}
	}
	foreach ($m in $UndefRegex.Matches($NoComments)) {
		$Events.Add([pscustomobject]@{ index = $m.Index; kind = 'undef'; ns = $null })
	}
	$Events = @($Events | Sort-Object index)
	$Defines = @($Events | Where-Object { $_.kind -eq 'define' })
	foreach ($d in $Defines) {
		$later = @($Events | Where-Object { $_.kind -eq 'undef' -and $_.index -gt $d.index })
		if ($later.Count -eq 0) {
			$Warnings.Add((New-Finding 'R6' 'warning' $Rel (Get-Line $d.index) 'LOCTEXT_NAMESPACE 定义了但文件末尾没有 #undef。'))
		}
	}

	# --- LOCTEXT / NSLOCTEXT
	$CallSpans = New-Object System.Collections.Generic.List[object]
	foreach ($c in $CallStart.Matches($CodeOnly)) {
		$kind = $c.Groups['k'].Value
		$line = Get-Line $c.Index
		$call = if ($kind -eq 'LOCTEXT') { $LocTextCall.Match($NoComments, $c.Index) } else { $NsLocTextCall.Match($NoComments, $c.Index) }
		if (-not $call.Success) {
			# 宏定义本身（#define LOCTEXT(...)）不算。
			$lineText = (Get-LineText $c.Index).TrimStart()
			if ($lineText -match '^#\s*define\b') { continue }
			$ctx = Get-Preview ($NoComments.Substring($c.Index, [Math]::Min(80, $NoComments.Length - $c.Index))) 80
			$Findings.Add((New-Finding 'R7' 'error' $Rel $line "$kind 的参数不是纯字符串字面量，扫描器与 gather 都读不到：$ctx"))
			continue
		}
		$CallSpans.Add([pscustomobject]@{ start = $call.Index; end = $call.Index + $call.Length })

		$key = Join-StrSeq $call.Groups['key'].Value
		$src = Join-StrSeq $call.Groups['text'].Value
		if ($kind -eq 'NSLOCTEXT') {
			$ns = Join-StrSeq $call.Groups['ns'].Value
		}
		else {
			$active = $null
			foreach ($ev in $Events) {
				if ($ev.index -gt $c.Index) { break }
				$active = if ($ev.kind -eq 'define') { $ev.ns } else { $null }
			}
			$ns = $active
			if ($null -eq $ns) {
				$Findings.Add((New-Finding 'R5' 'error' $Rel $line 'LOCTEXT 前面没有有效的 #define LOCTEXT_NAMESPACE；头文件或没有定义命名空间的地方请用 NSLOCTEXT。'))
				continue
			}
		}

		if ($ns.Contains('|') -or $key.Contains('|')) {
			$Findings.Add((New-Finding 'R11' 'error' $Rel $line "命名空间 / key 里不能含 |：$ns|$key"))
			continue
		}
		if ($key -eq '' -or $src -eq '') {
			$Findings.Add((New-Finding 'R12' 'error' $Rel $line "key 或源文为空：$ns|$key"))
			continue
		}
		# 源文里的 NUL（\0）：运行时的 TCHAR* 在那里就截断了，CRC 也只算到那里，清单里留着后半段就永远对不上。
		if ($src.Contains([string][char]0) -or $key.Contains([string][char]0)) {
			$Findings.Add((New-Finding 'R12' 'error' $Rel $line "key 或源文里有 NUL 字符（\0）：运行时会在那里截断：$ns|$key"))
			continue
		}

		$id = "$ns|$key"
		if ($SeenIds.ContainsKey($id)) {
			$prev = $SeenIds[$id]
			if ($prev.src -cne $src) {
				$Findings.Add((New-Finding 'R8' 'error' $Rel $line "同一个 $id 在 $($prev.file):$($prev.line) 的源文是「$(Get-Preview $prev.src)」，这里是「$(Get-Preview $src)」。"))
			}
			else {
				$Warnings.Add((New-Finding 'R3' 'warning' $Rel $line "$id 在 $($prev.file):$($prev.line) 已经出现过，源文相同。"))
			}
			continue
		}
		$entry = [pscustomobject]@{ namespace = $ns; key = $key; text = $src; file = $Rel; line = $line }
		$SeenIds[$id] = [pscustomobject]@{ src = $src; file = $Rel; line = $line }
		$Inventory.Add($entry)
	}

	if ($IsTest) { continue }

	# --- R1：字符串字面量里的中日韩字符
	foreach ($s in $Strings) {
		if (-not $CjkRegex.IsMatch($s.text)) { continue }
		$line = Get-Line $s.index
		$decoded = Join-StrSeq $s.text
		if ([string]::IsNullOrEmpty($decoded)) { $decoded = $s.text }

		$inLoc = $false
		foreach ($sp in $CallSpans) {
			if ($s.index -ge $sp.start -and $s.index -lt $sp.end) { $inLoc = $true; break }
		}
		$stmtStart = [Math]::Max($CodeOnly.LastIndexOf(';', $s.index), [Math]::Max($CodeOnly.LastIndexOf('{', $s.index), $CodeOnly.LastIndexOf('}', $s.index)))
		$stmt = $CodeOnly.Substring($stmtStart + 1, $s.index - $stmtStart - 1)
		$inLog = $LogRegex.IsMatch($stmt)

		# 每条只说是哪一类，完整的改法在报告末尾说一次，免得几百行重复同一段话。
		$why = if ($inLoc) { 'LOCTEXT 源文要英文' }
		elseif ($inLog) { '日志/断言要英文' }
		else { '界面或错误文字要走 LOCTEXT' }

		$reason = Test-Exempt 'R1' $Rel $decoded (Get-LineText $s.index) $Rel $line
		if ($null -ne $reason) {
			$Exempted.Add([pscustomobject]@{ rule = 'R1'; file = $Rel; line = $line; literal = (Get-Preview $decoded); reason = $reason })
			continue
		}
		$Findings.Add((New-Finding 'R1' 'error' $Rel $line "含中文的字符串字面量（$why）：「$(Get-Preview $decoded)」" $decoded))
	}

	# --- R2：FText::FromString / FromName 里放英文字面量
	foreach ($c in $FromStart.Matches($CodeOnly)) {
		$call = $FromTextCall.Match($NoComments, $c.Index)
		if (-not $call.Success) { continue }
		$lit = Join-StrSeq $call.Groups['lit'].Value
		if ($CjkRegex.IsMatch($lit)) { continue }   # 中文的已经由 R1 报了
		if ($lit -notmatch '[A-Za-z]') { continue }
		$line = Get-Line $c.Index
		$reason = Test-Exempt 'R2' $Rel $lit (Get-LineText $c.Index) $Rel $line
		if ($null -ne $reason) {
			$Exempted.Add([pscustomobject]@{ rule = 'R2'; file = $Rel; line = $line; literal = (Get-Preview $lit); reason = $reason })
			continue
		}
		$Findings.Add((New-Finding 'R2' 'error' $Rel $line "$($call.Groups['fn'].Value) 里直接放了英文字面量，翻不了：「$(Get-Preview $lit)」" $lit))
	}

	# --- R9
	foreach ($c in $AdvancedStart.Matches($CodeOnly)) {
		$Warnings.Add((New-Finding 'R9' 'warning' $Rel (Get-Line $c.Index) 'AsLocalizable_Advanced 是动态引用，扫描器看不见它，.locres 里不会有这一条。'))
	}
}

# 豁免清单里没被任何东西用到的：过期了，必须删。
foreach ($ex in $Exemptions) {
	if ($ex.used -eq 0 -and -not [string]::IsNullOrWhiteSpace($ex.reason)) {
		$Findings.Add((New-Finding 'R13' 'error' 'Tools/Localization/lint_exemptions.json' 0 "第 $($ex.index) 条豁免（$($ex.rule) $($ex.file)$(if ($ex.contains -ne '') { " 含「$($ex.contains)」" })）已经没有任何东西匹配，删掉它。"))
	}
}

# --------------------------------------------------------------------------------------- 输出

$Sorted = @($Findings | Sort-Object file, line, rule)
$SortedWarnings = @($Warnings | Sort-Object file, line, rule)
$SortedInventory = @($Inventory | Sort-Object namespace, key)
$Summary = [pscustomobject]@{
	pluginRoot = $PluginRoot
	fileCount = $Files.Count
	entryCount = $SortedInventory.Count
	errorCount = $Sorted.Count
	warningCount = $SortedWarnings.Count
	exemptedCount = $Exempted.Count
}

if ($Json) {
	$Doc = [pscustomobject]@{
		summary = $Summary
		violations = $Sorted
		warnings = $SortedWarnings
		exempted = $Exempted.ToArray()
		inventory = $SortedInventory
	} | ConvertTo-Json -Depth 8
	if ($OutFile -ne '') {
		[System.IO.File]::WriteAllText($OutFile, $Doc, [System.Text.UTF8Encoding]::new($false))
	}
	else {
		$Doc
	}
	exit 0
}

# 输出被管道接走时（CI、别的脚本捕获），PowerShell 按控制台代码页（本机 936）编码，中文会变乱码。
try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false) } catch { }

Write-Host "本地化 lint：$($Summary.fileCount) 个源文件，$($Summary.entryCount) 条 LOCTEXT/NSLOCTEXT。"
$Limit = if ($All) { [int]::MaxValue } else { 150 }
$Shown = 0
foreach ($f in $Sorted) {
	if ($Shown -ge $Limit) { break }
	Write-Host ("{0}:{1} [{2}] {3}" -f $f.file, $f.line, $f.rule, $f.message) -ForegroundColor Red
	$Shown++
}
if ($Sorted.Count -gt $Shown) {
	Write-Host "…… 还有 $($Sorted.Count - $Shown) 条没列出（加 -All 全列）。" -ForegroundColor Red
}
# R3（同一个 key 在多处、源文相同）在 Slate 代码里是惯用写法，几十条淹没真正的警告；只在 -All 时列出，数目照常计在总数里。
$QuietWarnings = 0
foreach ($w in $SortedWarnings) {
	if ($w.rule -eq 'R3' -and -not $All) { $QuietWarnings++; continue }
	Write-Host ("{0}:{1} [{2}] {3}" -f $w.file, $w.line, $w.rule, $w.message) -ForegroundColor Yellow
}
if ($QuietWarnings -gt 0) {
	Write-Host "（另有 $QuietWarnings 条 R3：同一个 key 在多处出现且源文相同，不影响结果；加 -All 列出。）" -ForegroundColor DarkGray
}
if ($Exempted.Count -gt 0) {
	Write-Host "已豁免 $($Exempted.Count) 处：" -ForegroundColor DarkGray
	foreach ($x in ($Exempted | Sort-Object file, line)) {
		Write-Host ("  {0}:{1} [{2}] 「{3}」—— {4}" -f $x.file, $x.line, $x.rule, $x.literal, $x.reason) -ForegroundColor DarkGray
	}
}
Write-Host "结果：$($Summary.errorCount) 个错误，$($Summary.warningCount) 个警告，$($Summary.exemptedCount) 处豁免。"
if ($Summary.errorCount -gt 0) { exit 1 }
exit 0
