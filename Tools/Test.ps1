#Requires -Version 7.0
<#
.SYNOPSIS
    构建宿主工程的 Editor target，并用 UnrealEditor-Cmd 无头跑 DreamTexture 的自动化测试。

.DESCRIPTION
    这是本插件的闸门：改完源码后跑一遍，退出码 0 才算过。

      0. 本地化检查  默认先跑（不需要引擎，几秒钟）：lint、清单与译文是否最新、.locres 是否最新。不过就以退出码 4 结束，
               不再构建。-NoLint 跳过；-LintOnly 只跑这一步。
      1. 构建  宿主 Editor target（DreamTextureTestHostEditor Win64 Development），用 Launcher 版 5.8 引擎。
               -SkipBuild 跳过。
      2. -Live 先起固定数据测试后端（Tools\FixtureBackend\），给子进程设 DREAMTEXTURE_LIVE_URL，
               跑完**无论成败**都停掉它。没有 -Live 时这个环境变量会被清掉，DreamTexture.Live.* 一律跳过。
      2b. -Pipeline 先起流水线后端（Tools\PipelineBackend\：隔离的 ComfyUI + 新后端，真的跑分解管线），
               给子进程设 DREAMTEXTURE_PIPELINE_URL，跑完**无论成败**都停掉它。没有 -Pipeline 时这个环境变量
               同样会被清掉，DreamTexture.Pipeline.* 一律跳过。显存不够（默认空闲不足 10 GiB）它会拒绝启动。
      3. 测试  UnrealEditor-Cmd.exe 无头跑 "Automation RunTests <Filter>"。-abslog 和 -ReportExportPath
               每次都是唯一路径，旧的日志和报告不可能冒充这一次的结果。
      4. 判定  解析 -ReportExportPath 下的 index.json，逐个测试打一张结果表。

    **找不到测试不算通过。** 过滤串写错（或模块没编进去、测试没注册）时一个测试都不会跑，
    此时「没有失败」恰好是空话——所以零个测试、或全部被跳过，都按失败处理。

    编辑器进程自己崩了（退出码非 0 却没有测试失败的记录）同样按失败处理，并打印日志末尾。

    每次运行的产物都在 <宿主>\Saved\DreamTextureTest\<时间戳>\ 下：editor.log、report\、build.log。
    只保留最近 10 次。

.PARAMETER Filter
    传给 "Automation RunTests" 的过滤串。默认 DreamTexture，即所有 DreamTexture.* 测试。
    例：DreamTexture.Client、DreamTexture.Client.Manifest、"DreamTexture.Client+DreamTexture.Assets"。

.PARAMETER SkipBuild
    不构建，直接用上次编好的二进制跑测试。

.PARAMETER BuildOnly
    只构建，不跑测试。写代码时反复「编过没有」用它，比整套测试快得多。
    （取代了 M0 时的 Toolsuild.ps1。）

.PARAMETER Live
    起固定数据后端并跑 DreamTexture.Live.*。

.PARAMETER Pipeline
    起流水线后端（隔离的 ComfyUI 端口 18188 + 新后端端口 18787）并跑 DreamTexture.Pipeline.*。
    没给 -Filter 时过滤串默认改成 DreamTexture.Pipeline（别的测试不需要为它多等 ComfyUI 起来）；要连别的一起跑就显式给 -Filter。
    和 -Render 同用且没给 -Filter 时，两组一起跑（DreamTexture.Render+DreamTexture.Pipeline）。
    可以和 -Live 同用；此时插件全局连接的 BackendURL 仍指向固定数据后端（Live 测试要它），Pipeline 测试靠环境变量自己找流水线后端。
    **吃显存**：起之前查空闲显存，不足会拒绝（退出码 3）。细节见 Tools\PipelineBackend\README.md。

.PARAMETER Render
    不带 -nullrhi 启动编辑器（真实 RHI，要有显卡），用来跑 DreamTexture.Render.*：真的编译母材质与材质实例的着色器、渲染预览图。
    没给 -Filter 时过滤串默认改成 DreamTexture.Render（只跑这一组，因为别的测试用不着渲染，也不该为它们多等着色器）；
    要连别的一起跑就显式给 -Filter DreamTexture。可以和 -Live 同用。
    不带 -Render 时这组测试仍会被收集，但在 -nullrhi 下会检测到不能渲染，跳过并写明原因。
    渲染产物（预览图）在这一次运行的产物目录的 render\ 下。

.PARAMETER Lint
    已经是默认行为，保留这个开关只为了老命令还能用，加不加都一样。

.PARAMETER NoLint
    跳过构建与测试之前的本地化检查。本地化检查默认会跑（Tools\Localization\）：localization_lint.ps1
    （源码里不许有中文字符串字面量等）加 sync_loctext.py --check（loctext.json 是否最新、项目设置页等属性面板的元数据合规、
    每条源文都有 zh-Hans 译文、占位符一致、没有孤儿或重复的译文、.locres 是否最新）。任何一项不过就以退出码 4 结束，不再构建。
    不需要引擎，需要 pwsh 7 与 Python 3；没有 Python 的机器用 -NoLint。

.PARAMETER LintOnly
    只跑本地化检查，不构建、不跑测试，也不要求宿主工程与引擎存在。和 CI 跑的是同一组命令。不能和 -NoLint 同用。

.PARAMETER Project
    宿主 .uproject。没给就看 Tools\LocalSettings.psd1 的 HostProject，再没有就用插件往上两级目录里的那个 .uproject
    （插件作为 git worktree 放在 <宿主>\Plugins\DreamTextureForUnreal\ 时就是宿主自己）。都没有则失败。

.PARAMETER EngineRoot
    引擎目录。没给就依次看环境变量 DREAMTEXTURE_ENGINE_ROOT、Tools\LocalSettings.psd1 的 EngineRoot，再没有就按 .uproject 的
    EngineAssociation 查（Launcher 版是 "5.8"，查 HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.8 的 InstalledDirectory，
    再退到 LauncherInstalled.dat；GUID 则查 HKCU 的 Builds）。都查不到则失败。

.PARAMETER TimeoutMinutes
    测试进程的时限，超时会被杀掉并按失败处理。默认 20。

.PARAMETER ExtraEditorArgs
    原样追加给 UnrealEditor-Cmd 的参数。

.EXAMPLE
    pwsh -NoProfile -File Tools\Test.ps1
    本地化检查 + 构建 + 跑全部 DreamTexture.* 测试。

.EXAMPLE
    pwsh -NoProfile -File Tools\Test.ps1 -LintOnly
    只跑本地化检查（几秒，不要引擎）。

.EXAMPLE
    pwsh -NoProfile -File Tools\Test.ps1 -NoLint -SkipBuild -Filter DreamTexture.Localization
    跳过本地化检查，不构建，只跑本地化的引擎内测试。

.EXAMPLE
    pwsh -NoProfile -File Tools\Test.ps1 -SkipBuild -Filter DreamTexture.Client
    不构建，只跑 Client 模块的测试。

.EXAMPLE
    pwsh -NoProfile -File Tools\Test.ps1 -Live
    连固定数据后端一起跑，含 DreamTexture.Live.*。

.EXAMPLE
    pwsh -NoProfile -File Tools\Test.ps1 -Render
    不带 -nullrhi 启动编辑器，只跑 DreamTexture.Render.*（编译母材质/实例的着色器、渲染预览图）。

.EXAMPLE
    pwsh -NoProfile -File Tools\Test.ps1 -Pipeline
    起隔离的 ComfyUI + 新后端，只跑 DreamTexture.Pipeline.*（把图送去真的分解管线）。

.NOTES
    退出码：
      0  -BuildOnly 时：构建通过。否则：构建通过（若构建了），且至少有一个测试真正跑过、没有失败、编辑器正常退出；
      1  有测试失败 / 一个测试都没找到 / 全部被跳过 / 编辑器崩溃或超时 / 读不到报告；
      2  构建失败；
      3  环境不对（工程、引擎、后端起不来、找不到 Python），没能开始；
      4  本地化检查（默认会跑；-NoLint 跳过）没过。
#>
[CmdletBinding()]
param(
	[string] $Filter = 'DreamTexture',
	[switch] $SkipBuild,
	[switch] $BuildOnly,
	[switch] $Lint,
	[switch] $NoLint,
	[switch] $LintOnly,
	[switch] $Live,
	[switch] $Pipeline,
	[switch] $Render,
	[string] $Project = '',
	[string] $EngineRoot = '',
	[int] $TimeoutMinutes = 20,
	[string[]] $ExtraEditorArgs = @()
)

Set-StrictMode -Version 3.0
# 宿主工程、引擎目录、后端、ComfyUI 在哪：参数 -> 环境变量 -> Tools\LocalSettings.psd1 -> 自动 -> 失败。
. (Join-Path $PSScriptRoot 'Common.ps1')
# -Render / -Pipeline 且没明说过滤串：只跑对应的组（两个都给就两组一起）。（$PSBoundParameters 区分「没给」与「给了默认值」。）
if (-not $PSBoundParameters.ContainsKey('Filter')) {
	$DefaultGroups = @()
	if ($Render) { $DefaultGroups += 'DreamTexture.Render' }
	if ($Pipeline) { $DefaultGroups += 'DreamTexture.Pipeline' }
	if ($DefaultGroups.Count -gt 0) { $Filter = $DefaultGroups -join '+' }
}
$ErrorActionPreference = 'Stop'
# PowerShell 7.3+ 里，原生命令只要往 stderr 写东西就按 $ErrorActionPreference 抛异常，而 UBT 正常也往 stderr
# 写进度。不关掉的话，会在真正的错误出现之前被一个假异常打断，日志反而看不全。
$PSNativeCommandUseErrorActionPreference = $false

$LiveUrl = 'http://127.0.0.1:18777'
# 流水线后端（Tools\PipelineBackend\）的地址；端口与 Start-PipelineBackend.ps1 的默认一致。
$PipelineUrl = 'http://127.0.0.1:18787'
# 没人监听的地址，不带 -Live 时测试编辑器的全局连接指向它（与固定后端里 ComfyUI 的死端口同一个）。
$DeadBackendUrl = 'http://127.0.0.1:18779'
$KeepRuns = 10

function Write-Step([string] $Message) { Write-Host ''; Write-Host "=== $Message ===" -ForegroundColor Cyan }
function Stop-WithFailure([int] $Code, [string[]] $Messages) {
	foreach ($Message in $Messages) { Write-Host $Message -ForegroundColor Red }
	exit $Code
}

# ------------------------------------------------------------------------------------------ 本地化检查

function Resolve-Python {
	# Windows 自带的 python.exe 常是应用商店的占位程序（运行就退出码 9009），所以要真的跑一下 --version。
	foreach ($Candidate in @('python', 'python3', 'py')) {
		$Command = Get-Command $Candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
		if ($null -eq $Command) { continue }
		try {
			$Version = (& $Command.Source '--version' 2>&1 | Out-String)
			if ($LASTEXITCODE -eq 0 -and $Version -match 'Python 3') { return $Command.Source }
		}
		catch { }
	}
	return $null
}

# 本地化检查默认就跑：改了源文、属性元数据或译文却忘了同步 loctext.json / .locres，以前要等到有人记得加 -Lint 才发现，
# 而这类错误在引擎里是静默的（译文悄悄不生效）。-NoLint 才跳过它。
if ($NoLint -and $LintOnly) {
	Stop-WithFailure 3 @('-NoLint 与 -LintOnly 互相矛盾：一个要跳过本地化检查，一个只跑本地化检查。')
}
if ($LintOnly -or -not $NoLint) {
	Write-Step '本地化检查：lint + sync_loctext.py --check（-NoLint 跳过）'
	$LocalizationDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'Tools\Localization'
	$Python = Resolve-Python
	if ($null -eq $Python) {
		Stop-WithFailure 3 @('找不到可用的 Python 3（python / python3 / py 都试过）。本地化检查需要它；确实没有就加 -NoLint 跳过。')
	}
	$LintFailures = [System.Collections.Generic.List[string]]::new()

	Write-Host 'localization_lint.ps1' -ForegroundColor DarkGray
	& pwsh -NoProfile -File (Join-Path $LocalizationDir 'localization_lint.ps1')
	if ($LASTEXITCODE -ne 0) { $LintFailures.Add("localization_lint.ps1 退出码 $LASTEXITCODE") }

	Write-Host 'sync_loctext.py --check（清单是否最新、属性面板元数据、译文覆盖与占位符、.locres 是否最新）' -ForegroundColor DarkGray
	& $Python (Join-Path $LocalizationDir 'sync_loctext.py') '--check'
	if ($LASTEXITCODE -ne 0) { $LintFailures.Add("sync_loctext.py --check 退出码 $LASTEXITCODE") }

	if ($LintFailures.Count -gt 0) {
		Stop-WithFailure 4 (@('', '本地化检查没过：') + @($LintFailures | ForEach-Object { "  $_" }) + @('细节在上面；怎么补见 Tools\Localization\README.md。'))
	}
	Write-Host '本地化检查通过。' -ForegroundColor Green
	if ($LintOnly) { exit 0 }
}

# ------------------------------------------------------------------------------------------ 工程与引擎

$ProjectResolved = Resolve-DreamTextureProject -Explicit $Project
if ($null -eq $ProjectResolved.Value) {
	Stop-WithFailure 3 @($ProjectResolved.Hint, '还没有宿主就先跑 Tools\TestHost\New-DreamTextureTestHost.ps1 建一个。')
}
$Project = [System.IO.Path]::GetFullPath($ProjectResolved.Value)
if (-not (Test-Path -LiteralPath $Project -PathType Leaf)) {
	Stop-WithFailure 3 @("找不到宿主工程：$Project（来源：$($ProjectResolved.Source)）", '先跑 Tools\TestHost\New-DreamTextureTestHost.ps1 建宿主。')
}
$ProjectDir = Split-Path -Parent $Project
$ProjectName = [System.IO.Path]::GetFileNameWithoutExtension($Project)
$Target = "${ProjectName}Editor"
$PluginRoot = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path -LiteralPath (Join-Path $ProjectDir 'Plugins\DreamTextureForUnreal\DreamTextureForUnreal.uplugin') -PathType Leaf)) {
	Stop-WithFailure 3 @("宿主里看不到插件：$ProjectDir\Plugins\DreamTextureForUnreal\DreamTextureForUnreal.uplugin", '先跑 Tools\TestHost\New-DreamTextureTestHost.ps1。')
}

# 引擎目录：-EngineRoot -> DREAMTEXTURE_ENGINE_ROOT -> LocalSettings.psd1 的 EngineRoot -> 按 .uproject 的引擎关联查注册表
# （Launcher 版 HKLM 的 InstalledDirectory，源码构建 HKCU 的 Builds，再退到 LauncherInstalled.dat）。解析细节在 Tools\Common.ps1。
$EngineResolved = Resolve-DreamTextureEngineRoot -Explicit $EngineRoot -ProjectPath $Project
if ($null -eq $EngineResolved.Value) {
	Stop-WithFailure 3 @($EngineResolved.Hint)
}
$EngineRoot = [System.IO.Path]::GetFullPath($EngineResolved.Value)
$BuildBat = [System.IO.Path]::Combine($EngineRoot, 'Engine\Build\BatchFiles\Build.bat')
$EditorCmd = [System.IO.Path]::Combine($EngineRoot, 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe')
if (-not (Test-Path -LiteralPath $BuildBat -PathType Leaf)) {
	Stop-WithFailure 3 @("找不到 Build.bat：$BuildBat", '-EngineRoot 要指向引擎根目录（里面有 Engine\），不是 Engine\ 本身。')
}
if (-not (Test-Path -LiteralPath $EditorCmd -PathType Leaf)) {
	Stop-WithFailure 3 @("找不到 UnrealEditor-Cmd.exe：$EditorCmd")
}

# 这一次运行的产物目录。时间戳加一段随机，同一秒内连跑两次也不会撞。
$RunId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6)
$RunRoot = Join-Path $ProjectDir "Saved\DreamTextureTest\$RunId"
$ReportDir = Join-Path $RunRoot 'report'
$EditorLog = Join-Path $RunRoot 'editor.log'
$BuildLog = Join-Path $RunRoot 'build.log'
$ConsoleLog = Join-Path $RunRoot 'editor.console.txt'
New-Item -ItemType Directory -Path $RunRoot -Force | Out-Null

Write-Host "宿主工程：$Project（$($ProjectResolved.Source)）"
Write-Host "引擎：    $EngineRoot（$($EngineResolved.Source)）"
Write-Host "过滤串：  $Filter$(if ($Live) { '   （-Live：连固定数据后端）' })$(if ($Pipeline) { '   （-Pipeline：隔离的 ComfyUI + 新后端）' })$(if ($Render) { '   （-Render：真实 RHI）' })"
Write-Host "产物目录：$RunRoot"

# 开着的编辑器会占住插件的 DLL，链接阶段直接失败；它还占显存。只警告：脚本分不清是不是用户自己要开着的。
$OpenEditors = @(Get-CimInstance Win32_Process -Filter "Name = 'UnrealEditor.exe'" -ErrorAction SilentlyContinue |
		Where-Object { $_.CommandLine -and $_.CommandLine.IndexOf($ProjectName, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 })
if ($OpenEditors.Count -gt 0) {
	Write-Warning "宿主工程的编辑器正开着（pid $($OpenEditors.ProcessId -join ', ')）；它占着插件的 DLL，构建的链接阶段多半会失败。关掉它再跑。"
}

# ------------------------------------------------------------------------------------------ 构建

if ($SkipBuild) {
	Write-Step '构建：已跳过（-SkipBuild）'
}
else {
	Write-Step "构建 $Target Win64 Development"
	# 只编 Editor target，不要编 Game target：Game target 是整体式的，会把整个引擎编进宿主。
	$BuildArgs = @($Target, 'Win64', 'Development', "-Project=$Project", '-WaitMutex', '-NoHotReloadFromIDE')
	Write-Host "`"$BuildBat`" $($BuildArgs -join ' ')" -ForegroundColor DarkGray

	$Lines = [System.Collections.Generic.List[string]]::new()
	$Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
	& $BuildBat @BuildArgs 2>&1 | ForEach-Object {
		$Line = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.ToString() } else { [string]$_ }
		Write-Host $Line
		$Lines.Add($Line)
	}
	$BuildExit = $LASTEXITCODE
	$Stopwatch.Stop()
	$Lines | Set-Content -LiteralPath $BuildLog -Encoding UTF8

	if ($BuildExit -ne 0) {
		Write-Host ''
		Write-Host "构建失败，退出码 $BuildExit，耗时 $([int]$Stopwatch.Elapsed.TotalSeconds) 秒。" -ForegroundColor Red
		$ErrorLines = @($Lines | Where-Object { $_ -match '(?i)\berror\b|\bfatal\b|^\s*LINK\s*:|unresolved external' } | Select-Object -Unique)
		if ($ErrorLines.Count -gt 0) {
			Write-Host "UBT / 编译器报的错（共 $($ErrorLines.Count) 条）：" -ForegroundColor Red
			foreach ($Line in $ErrorLines) { Write-Host "    $Line" -ForegroundColor Red }
		}
		Write-Host "完整构建日志：$BuildLog"
		exit 2
	}
	Write-Host "构建通过，耗时 $([int]$Stopwatch.Elapsed.TotalSeconds) 秒。" -ForegroundColor Green
}

if ($BuildOnly) {
	Write-Host ''
	Write-Host '只构建（-BuildOnly），不跑测试。' -ForegroundColor Green
	exit 0
}

# ------------------------------------------------------------------------------------------ 固定数据后端

$PreviousLiveUrl = [Environment]::GetEnvironmentVariable('DREAMTEXTURE_LIVE_URL', 'Process')
$PreviousPipelineUrl = [Environment]::GetEnvironmentVariable('DREAMTEXTURE_PIPELINE_URL', 'Process')
$PreviousOutputDir = [Environment]::GetEnvironmentVariable('DREAMTEXTURE_TEST_OUTPUT_DIR', 'Process')
$PreviousBackendExe = [Environment]::GetEnvironmentVariable('DREAMTEXTURE_BACKEND_EXE', 'Process')
$BackendStarted = $false
$PipelineStarted = $false
$StopBackend = Join-Path $PluginRoot 'Tools\FixtureBackend\Stop-FixtureBackend.ps1'
$StartPipeline = Join-Path $PluginRoot 'Tools\PipelineBackend\Start-PipelineBackend.ps1'
$StopPipeline = Join-Path $PluginRoot 'Tools\PipelineBackend\Stop-PipelineBackend.ps1'

try {
	if ($Live) {
		Write-Step '启动固定数据后端'
		& pwsh -NoProfile -File (Join-Path $PluginRoot 'Tools\FixtureBackend\Start-FixtureBackend.ps1')
		if ($LASTEXITCODE -ne 0) {
			Stop-WithFailure 3 @("固定数据后端没能启动（退出码 $LASTEXITCODE），上面有原因。")
		}
		$BackendStarted = $true
		[Environment]::SetEnvironmentVariable('DREAMTEXTURE_LIVE_URL', $LiveUrl, 'Process')
	}
	else {
		if ($PreviousLiveUrl) {
			Write-Host "环境里有 DREAMTEXTURE_LIVE_URL=$PreviousLiveUrl，但没给 -Live；为保证结果可重现，这次运行里把它清掉。" -ForegroundColor Yellow
		}
		[Environment]::SetEnvironmentVariable('DREAMTEXTURE_LIVE_URL', $null, 'Process')
	}

	# 流水线后端：隔离的 ComfyUI + 新后端，真的跑分解管线。吃显存，Start 自己会先查，不够就拒绝。
	if ($Pipeline) {
		Write-Step '启动流水线后端（隔离的 ComfyUI + 新后端）'
		# 先置标记再起：Start 中途失败时它自己会收拾干净，但万一没收拾净，finally 里的 Stop 兜底（Stop 幂等，只停登记的）。
		$PipelineStarted = $true
		& pwsh -NoProfile -File $StartPipeline
		if ($LASTEXITCODE -ne 0) {
			Stop-WithFailure 3 @("流水线后端没能启动（退出码 $LASTEXITCODE），上面有原因（显存不够、端口被占、ComfyUI 起不来……）。")
		}
		[Environment]::SetEnvironmentVariable('DREAMTEXTURE_PIPELINE_URL', $PipelineUrl, 'Process')
	}
	else {
		if ($PreviousPipelineUrl) {
			Write-Host "环境里有 DREAMTEXTURE_PIPELINE_URL=$PreviousPipelineUrl，但没给 -Pipeline；为保证结果可重现，这次运行里把它清掉。" -ForegroundColor Yellow
		}
		[Environment]::SetEnvironmentVariable('DREAMTEXTURE_PIPELINE_URL', $null, 'Process')
	}

	# -------------------------------------------------------------------------------------- 跑测试

	Write-Step "自动化测试：Automation RunTests $Filter"
	# 把模块自己那条全局连接的 BackendURL 从一开始就压到测试专用的地址：
	# 带 -Live 是固定数据后端，只带 -Pipeline 是流水线后端，都不带就是一个没人监听的死端口。
	# （-Live 与 -Pipeline 同用时给固定数据后端：Live 测试靠全局连接，Pipeline 测试靠环境变量自己找。）
	# 默认值 127.0.0.1:8777 是用户真实后端的位置，
	# 测试编辑器一启动就会去探它；用户的后端开着时，测试就真的连上去了。
	# 语法 -ini:<ini 名>:[<段>]:<键>=<值>（ConfigCacheIni.cpp 的 OverrideFromCommandline）：按第一个 = 切分，
	# 所以值里带 http:// 的冒号没事；整个参数不能有空格，也不要加引号。DreamTextureSettings 是 config=Editor。
	$BackendUrl = if ($Live) { $LiveUrl } elseif ($Pipeline) { $PipelineUrl } else { $DeadBackendUrl }
	# 测试要写产物（渲染的预览图）时往哪放：这一次运行的产物目录下。不设的话测试自己退回 Saved\DreamTextureTest\Manual。
	[Environment]::SetEnvironmentVariable('DREAMTEXTURE_TEST_OUTPUT_DIR', $RunRoot, 'Process')
	# 测试里要自己拉起后端的（进程托管、生成页的自动启动）靠环境变量 DREAMTEXTURE_BACKEND_EXE 找 dreamtexture.exe，找不到就跳过。
	# 环境里没有的话用 Tools\LocalSettings.psd1 的 BackendExe，免得本机明明配过、测试却跳过。只对编辑器进程生效，跑完还原；
	# 放在流水线后端启动之后设，所以不会改变流水线后端自己选的那份 exe。
	if ([string]::IsNullOrWhiteSpace($PreviousBackendExe)) {
		$ExeResolved = Resolve-DreamTextureBackendExe -Kind Fixture
		if ($null -ne $ExeResolved.Value) {
			[Environment]::SetEnvironmentVariable('DREAMTEXTURE_BACKEND_EXE', $ExeResolved.Value, 'Process')
			Write-Host "测试用的后端可执行文件：$($ExeResolved.Value)（$($ExeResolved.Source)）" -ForegroundColor DarkGray
		}
	}
	$EditorArgs = @(
		"`"$Project`"",
		"-ini:Editor:[/Script/DreamTextureClient.DreamTextureSettings]:BackendURL=$BackendUrl",
		"-ExecCmds=`"Automation RunTests $Filter; Quit`"",
		# 把文化钉在 en：界面文字的源文是英文，中文是 zh-Hans 译文（Content/Localization）。不钉的话编辑器会跟着
		# 操作系统的语言走，这台机器是中文系统，比对文案的测试就会随机器而变。要测中文译文的测试自己切文化再切回来。
		'-culture=en',
		'-unattended', '-nopause', '-nosplash', '-NoSound', '-NoLiveCoding',
		"-abslog=`"$EditorLog`"",
		"-ReportExportPath=`"$ReportDir`""
	) + $(if ($Render) { @() } else { @('-nullrhi') }) + $ExtraEditorArgs
	Write-Host "`"$EditorCmd`" $($EditorArgs -join ' ')" -ForegroundColor DarkGray

	# 输出落文件，不让几千行引擎日志淹没结果表；要看细节去 editor.log。
	$Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
	$Process = Start-Process -FilePath $EditorCmd -ArgumentList $EditorArgs -NoNewWindow -PassThru `
		-RedirectStandardOutput $ConsoleLog -RedirectStandardError (Join-Path $RunRoot 'editor.stderr.txt')
	$TimedOut = $false
	if (-not $Process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
		$TimedOut = $true
		Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
		$Process.WaitForExit(10000) | Out-Null
	}
	$EditorExit = if ($TimedOut) { -1 } else { $Process.ExitCode }
	$Stopwatch.Stop()
	Write-Host "编辑器进程结束：退出码 $EditorExit，耗时 $([int]$Stopwatch.Elapsed.TotalSeconds) 秒。"
}
finally {
	# 无论成败都停后端。Stop 只杀自己登记的进程。
	if ($BackendStarted) {
		Write-Step '停止固定数据后端'
		& pwsh -NoProfile -File $StopBackend
	}
	if ($PipelineStarted) {
		Write-Step '停止流水线后端'
		& pwsh -NoProfile -File $StopPipeline
	}
	[Environment]::SetEnvironmentVariable('DREAMTEXTURE_LIVE_URL', $PreviousLiveUrl, 'Process')
	[Environment]::SetEnvironmentVariable('DREAMTEXTURE_PIPELINE_URL', $PreviousPipelineUrl, 'Process')
	[Environment]::SetEnvironmentVariable('DREAMTEXTURE_TEST_OUTPUT_DIR', $PreviousOutputDir, 'Process')
	[Environment]::SetEnvironmentVariable('DREAMTEXTURE_BACKEND_EXE', $PreviousBackendExe, 'Process')
}

# ------------------------------------------------------------------------------------------ 判定

Write-Step '结果'

# 只留最近 $KeepRuns 次的产物。两处要小心，都是实测踩过的：
#   1. 只认运行目录（名字是 yyyyMMdd-HHmmss-xxxxxx）。同一个父目录里还住着别的东西（Export、Overrides、Upload、
#      ProcessBackend……），按名字倒序排的话它们（字母）排在数字前面，把「保留最近 N 次」的名额占光，
#      真正的运行目录反而被删到只剩几个。
#   2. 只删超过 2 小时没动过的。几个 agent 会同时跑 Test.ps1，别人那一次还在跑时它的目录是新的；
#      把它删了的话，编辑器连日志都写不出来，表现为退出码 255、没有报告，看上去像编辑器崩了。
$StaleBefore = (Get-Date).AddHours(-2)
Get-ChildItem -LiteralPath (Split-Path -Parent $RunRoot) -Directory -ErrorAction SilentlyContinue |
	Where-Object { $_.Name -match '^\d{8}-\d{6}-[0-9a-f]{6}$' } |
	Sort-Object Name -Descending | Select-Object -Skip $KeepRuns |
	Where-Object { $_.LastWriteTime -lt $StaleBefore } |
	Remove-Item -Recurse -Force -ErrorAction SilentlyContinue

function Write-LogTail([int] $Count = 40) {
	if (Test-Path -LiteralPath $EditorLog) {
		Write-Host "---- $EditorLog 末尾 $Count 行 ----" -ForegroundColor DarkGray
		Get-Content -LiteralPath $EditorLog -Tail $Count -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ -ForegroundColor DarkGray }
	}
	else {
		Write-Host "编辑器连日志都没写出来：$EditorLog。控制台输出在 $ConsoleLog。" -ForegroundColor Red
	}
}

if ($TimedOut) {
	Write-LogTail
	Stop-WithFailure 1 @('', "测试失败：编辑器进程超过 $TimeoutMinutes 分钟没结束，已被杀掉。")
}

$IndexPath = Join-Path $ReportDir 'index.json'
if (-not (Test-Path -LiteralPath $IndexPath -PathType Leaf)) {
	# 过滤串一个测试都没匹配上时，引擎在日志里明说，且根本不会写报告——这是「找不到测试」，不是「崩了」。
	$NoMatch = (Test-Path -LiteralPath $EditorLog -PathType Leaf) -and
		($null -ne (Select-String -LiteralPath $EditorLog -Pattern 'No automation tests matched' -SimpleMatch -Quiet -ErrorAction SilentlyContinue))
	if ($NoMatch) {
		Write-LogTail 12
		Stop-WithFailure 1 @('', "测试失败：过滤串 '$Filter' 一个测试都没找到（引擎日志：No automation tests matched）。",
			'这不算通过。检查：过滤串拼写；DreamTextureClient / DreamTextureEditor 是否编进了宿主；测试文件是否在 Private/Tests/ 下、路径是否以 DreamTexture. 打头。')
	}
	Write-LogTail
	Stop-WithFailure 1 @('', "测试失败：没有生成报告 $IndexPath（编辑器退出码 $EditorExit）。",
		'常见原因：编辑器没起来 / 起来就崩了 / 模块没编进去。看上面的日志末尾。')
}

$Report = Get-Content -LiteralPath $IndexPath -Raw -Encoding UTF8 | ConvertFrom-Json
$Tests = @($Report.tests)

function Get-TestState($Test) {
	# 引擎自己的 Skipped 状态之外，还要认「以通过收场、但信息里写了已跳过」的：DreamTexture.Live.* 在没有
	# DREAMTEXTURE_LIVE_URL 时就是这么跳的（AddInfo("已跳过：…") 然后 return true）。不认的话，
	# 没连后端的一次运行会把它们全算成「通过」，表上看着全绿，实际一个 Live 断言都没跑。
	$State = [string]$Test.state
	if ($State -eq 'Success' -and $Test.PSObject.Properties['entries'] -and $null -ne $Test.entries) {
		foreach ($Entry in @($Test.entries)) {
			if ($Entry.PSObject.Properties['event'] -and $Entry.event -and
				[string]$Entry.event.type -eq 'Info' -and ([string]$Entry.event.message) -match '^\s*(已跳过|Skipped|SKIPPED)') {
				return 'Skipped'
			}
		}
	}
	return $State
}
function Get-EntryMessages($Test, [string[]] $Types) {
	# 某类条目（Error / Warning / Info）的消息，带上文件和行号。字段缺了一律容忍。
	$Messages = [System.Collections.Generic.List[string]]::new()
	if ($Test.PSObject.Properties['entries'] -and $null -ne $Test.entries) {
		foreach ($Entry in @($Test.entries)) {
			$Event = if ($Entry.PSObject.Properties['event']) { $Entry.event } else { $null }
			if ($null -eq $Event) { continue }
			if ([string]$Event.type -notin $Types) { continue }
			$Where = ''
			if ($Entry.PSObject.Properties['filename'] -and $Entry.filename) {
				$Where = "  ($($Entry.filename):$($Entry.lineNumber))"
			}
			$Messages.Add("$($Event.message)$Where")
		}
	}
	return $Messages
}

$SkipStates = @('Skipped', 'Skip')
$Passed = @($Tests | Where-Object { (Get-TestState $_) -eq 'Success' })
$Failed = @($Tests | Where-Object { (Get-TestState $_) -eq 'Fail' })
$Skipped = @($Tests | Where-Object { (Get-TestState $_) -in $SkipStates })
# 排了队却没有结果的（NotRun / InProcess）：编辑器在它们跑到一半时没了，不能当没发生过。
$Unfinished = @($Tests | Where-Object { (Get-TestState $_) -notin (@('Success', 'Fail') + $SkipStates) })

$Longest = 0
foreach ($Test in $Tests) { $Longest = [Math]::Max($Longest, ([string]$Test.fullTestPath).Length) }
foreach ($Test in ($Tests | Sort-Object { [string]$_.fullTestPath })) {
	$State = Get-TestState $Test
	if ($State -eq 'Success') { $Label = '通过'; $Color = 'Green' }
	elseif ($State -eq 'Fail') { $Label = '失败'; $Color = 'Red' }
	elseif ($State -in $SkipStates) { $Label = '跳过'; $Color = 'Yellow' }
	else { $Label = "未完成($State)"; $Color = 'Red' }
	$Duration = if ($Test.PSObject.Properties['duration'] -and $null -ne $Test.duration) { ('{0,7:N3}s' -f [double]$Test.duration) } else { '' }
	Write-Host ("[{0}] {1}  {2}" -f $Label, ([string]$Test.fullTestPath).PadRight($Longest), $Duration) -ForegroundColor $Color
	# 失败的列出错误；跳过的把原因（Warning / Info 里的消息）也列出来，免得「跳过」成了黑洞。
	if ($State -notin (@('Success') + $SkipStates)) {
		foreach ($Message in (Get-EntryMessages $Test @('Error'))) { Write-Host "         $Message" -ForegroundColor Red }
	}
	if ($State -in $SkipStates) {
		foreach ($Message in (Get-EntryMessages $Test @('Warning', 'Info'))) { Write-Host "         $Message" -ForegroundColor DarkYellow }
	}
}

Write-Host ''
Write-Host ("共 {0} 个：通过 {1}，失败 {2}，跳过 {3}，未完成 {4}。报告：{5}" -f $Tests.Count, $Passed.Count, $Failed.Count, $Skipped.Count, $Unfinished.Count, $IndexPath)

if ($Tests.Count -eq 0) {
	Write-LogTail 25
	Stop-WithFailure 1 @('', "测试失败：过滤串 '$Filter' 一个测试都没找到。",
		'这不算通过。检查：过滤串拼写；DreamTextureClient / DreamTextureEditor 是否编进了宿主；测试文件是否在 Private/Tests/ 下、路径是否以 DreamTexture. 打头。')
}
if ($Failed.Count -gt 0 -or $Unfinished.Count -gt 0) {
	# 测试失败时引擎本来就以非 0 退出，失败信息上面已经列了；只有「没跑完」才像崩溃，才需要看日志末尾。
	if ($Unfinished.Count -gt 0) { Write-LogTail }
	Stop-WithFailure 1 @('', "测试失败：$($Failed.Count) 个失败，$($Unfinished.Count) 个没跑完（编辑器退出码 $EditorExit）。")
}
if ($Passed.Count -eq 0) {
	Stop-WithFailure 1 @('', "测试失败：$($Tests.Count) 个测试全被跳过，没有一个真正跑过。$(if (-not $Live) { '（DreamTexture.Live.* 需要 -Live。）' })$(if (-not $Pipeline) { '（DreamTexture.Pipeline.* 需要 -Pipeline。）' })$(if (-not $Render) { '（DreamTexture.Render.* 需要 -Render。）' })")
}
if ($EditorExit -ne 0) {
	Write-LogTail
	Stop-WithFailure 1 @('', "测试失败：测试都通过了，但编辑器进程退出码是 $EditorExit，多半是收尾时崩了；看上面的日志末尾。")
}

Write-Host ''
Write-Host "全部通过（$($Passed.Count) 通过，$($Skipped.Count) 跳过）。" -ForegroundColor Green
exit 0
