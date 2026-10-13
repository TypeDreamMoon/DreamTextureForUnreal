#Requires -Version 7.0
<#
.SYNOPSIS
    起一个隔离的 ComfyUI 和一个新后端，让 DreamTexture.Pipeline.* 能真的跑分解管线；两者都应答之后才返回。

.DESCRIPTION
    给「回流」类端到端测试用（M8 补齐通道 / 改写外观、M9 UV 展开）。与 FixtureBackend 不同，这里的 ComfyUI 是真的，
    所以先查显存，再起两个进程：

      1. nvidia-smi 查空闲显存，低于 -MinFreeVramGiB（默认 10 GiB）就拒绝并说清楚。用户这台机器上
         ComfyUI 和开着的虚幻编辑器会抢显存，显存被占满时驱动静默回退到内存，任务会慢几十倍甚至假死，
         不报错也不结束——与其让测试挂住，不如进门就拒绝。
      2. 隔离的 ComfyUI（-ComfyPort，默认 18188）。--base-directory 指向 <状态目录>\comfy\，所以它自己的
         input / output / temp / user 全在那里，不碰用户 ComfyUI 的任何目录；其下 custom_nodes\ 里只放几个
         目录联接：Chord、TextureAlchemy、seamless-tiling（指向整合包里的那份，只读用），以及自研包
         ComfyUI-DreamTexture（指向后端仓库的 comfy_nodes\，那里有新的 CHORD 包装节点）。不加载别的节点包，
         所以不会有 ComfyUI-Manager 抓注册表那种启动期暴风雨，几十秒就绪。
         模型不用挂：整合包的 ComfyUI\extra_model_paths.yaml 无论 --base-directory 指哪里都会被读，模型目录若在那里
         挂好了就直接可用。要多挂别的用 -ExtraModelPaths。
      3. 新后端（-BackendExe，端口 -BackendPort 默认 18787），
         attach 到上面那个 ComfyUI。data\、output\、logs\ 都在状态目录下，output\ 预铺 Tests\Fixtures 的拷贝。

    都应答了（ComfyUI 的 /system_stats、后端的 /api/health、后端自己看到的 ComfyUI 就绪）才返回 0，
    并在最后一行打印 DREAMTEXTURE_PIPELINE_URL=http://127.0.0.1:<端口>。

    状态都放在 -StateDir（默认宿主工程的 Saved\PipelineBackend\）：
      comfy\            ComfyUI 的 --base-directory
      output\ data\     后端的素材目录与 SQLite
      logs\             comfy.log、dreamtexture.log
      pipeline.json     生成的后端配置
      comfy.pid backend.pid vram.json

    可以反复、无参数地调用：已经在跑且都应答 -> 什么都不做，返回 0；半死不活 -> 先停掉自己登记的再冷启动；
    端口被别人占着 -> 说清楚是谁，返回 1，不动那个进程。

.PARAMETER ComfyPort
    隔离 ComfyUI 的端口。默认 18188（用户自己的 ComfyUI 是 8188，别撞）。

.PARAMETER BackendPort
    新后端的端口。默认 18787（用户的真实后端是 8777，固定数据后端是 18777）。

.PARAMETER BackendExe
    后端可执行文件。没给就依次看环境变量 DREAMTEXTURE_BACKEND_EXE、Tools\LocalSettings.psd1 的 PipelineBackendExe 与 BackendExe，
    都没有则失败（说明该设什么）。
    工作流目录（workflows\）和自研节点包（comfy_nodes\）都随它所在的目录走。

.PARAMETER ComfyRoot
    ComfyUI 整合包根目录（里面有 python\ 和 ComfyUI\）。没给就依次看环境变量 DREAMTEXTURE_COMFY_ROOT、
    Tools\LocalSettings.psd1 的 ComfyRoot，都没有则失败；-ComfyPython、-ComfyMain、-CustomNodesSource 三个都给了就不需要它。

.PARAMETER ComfyPython
    Python 解释器。默认 <ComfyRoot>\python\python.exe。

.PARAMETER ComfyMain
    ComfyUI 的 main.py。默认 <ComfyRoot>\ComfyUI\main.py。

.PARAMETER CustomNodesSource
    Chord / TextureAlchemy / seamless-tiling 三个节点包所在的目录。默认 <ComfyRoot>\ComfyUI\custom_nodes。

.PARAMETER DreamTextureNodes
    自研节点包目录。默认 <BackendExe 所在目录>\comfy_nodes。

.PARAMETER ExtraModelPaths
    额外的 extra_model_paths.yaml（可多个），原样传给 --extra-model-paths-config。
    整合包自带的那个不用管，ComfyUI 会自己读。

.PARAMETER StateDir
    状态目录。默认宿主工程的 Saved\PipelineBackend\，不在宿主里则用临时目录。

.PARAMETER MinFreeVramGiB
    冷启动前要求的空闲显存（GiB）。默认 10。设 0 关闭检查。已经在跑时不检查（它自己占着的显存不算）。

.PARAMETER ComfyTimeoutSeconds
    等 ComfyUI 应答的上限。默认 180。

.PARAMETER BackendTimeoutSeconds
    等后端应答、并看到 ComfyUI 就绪的上限。默认 60。

.PARAMETER KeepData
    冷启动时不清 data\（保留上次的 SQLite）。默认每次冷启动都清，保证起点一致。

.NOTES
    退出码：0 两个都在跑且应答；1 没能做到，原因已打印（含显存不够、端口被占、起不来）。
#>
[CmdletBinding()]
param(
	[int] $ComfyPort = 18188,
	[int] $BackendPort = 18787,
	[string] $BackendExe = '',
	[string] $ComfyRoot = '',
	[string] $ComfyPython = '',
	[string] $ComfyMain = '',
	[string] $CustomNodesSource = '',
	[string] $DreamTextureNodes = '',
	[string[]] $ExtraModelPaths = @(),
	[string] $StateDir = '',
	[double] $MinFreeVramGiB = 10,
	[int] $ComfyTimeoutSeconds = 180,
	[int] $BackendTimeoutSeconds = 60,
	[switch] $KeepData
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
# PS 7.3+ 里原生命令写 stderr 就会抛异常（见 Tools/Test.ps1）。nvidia-smi 之类不该因此中断。
$PSNativeCommandUseErrorActionPreference = $false

. (Join-Path $PSScriptRoot 'PipelineBackend.Common.ps1')
. (Join-Path $PSScriptRoot '..\Common.ps1')

function Stop-WithFailure([string[]] $Messages) {
	foreach ($Message in $Messages) { [Console]::Error.WriteLine("Start-PipelineBackend: $Message") }
	exit 1
}

# ---- 路径与默认值
# 后端：-BackendExe -> DREAMTEXTURE_BACKEND_EXE -> LocalSettings.psd1 的 PipelineBackendExe -> BackendExe。
$BackendResolved = Resolve-DreamTextureBackendExe -Explicit $BackendExe -Kind Pipeline
if ($null -eq $BackendResolved.Value) { Stop-WithFailure @($BackendResolved.Hint) }
$BackendExe = $BackendResolved.Value
# ComfyUI 整合包：-ComfyRoot -> DREAMTEXTURE_COMFY_ROOT -> LocalSettings.psd1 的 ComfyRoot。
# 三个子路径都显式给了就用不着根目录，此时拿 main.py 的上一级顶上（它只用作 ComfyUI 进程的工作目录）。
$AllComfyPathsGiven = -not ([string]::IsNullOrWhiteSpace($ComfyPython) -or [string]::IsNullOrWhiteSpace($ComfyMain) -or [string]::IsNullOrWhiteSpace($CustomNodesSource))
$ComfyResolved = Resolve-DreamTextureComfyRoot -Explicit $ComfyRoot
if ($null -ne $ComfyResolved.Value) { $ComfyRoot = $ComfyResolved.Value }
elseif ($AllComfyPathsGiven) { $ComfyRoot = Split-Path -Parent $ComfyMain }
else { Stop-WithFailure @($ComfyResolved.Hint) }
if ([string]::IsNullOrWhiteSpace($ComfyPython)) { $ComfyPython = Join-Path $ComfyRoot 'python\python.exe' }
if ([string]::IsNullOrWhiteSpace($ComfyMain)) { $ComfyMain = Join-Path $ComfyRoot 'ComfyUI\main.py' }
if ([string]::IsNullOrWhiteSpace($CustomNodesSource)) { $CustomNodesSource = Join-Path $ComfyRoot 'ComfyUI\custom_nodes' }
if ([string]::IsNullOrWhiteSpace($StateDir)) { $StateDir = Get-DefaultStateDir }
$BackendExe = [System.IO.Path]::GetFullPath($BackendExe)
$ComfyRoot = [System.IO.Path]::GetFullPath($ComfyRoot)
$ComfyPython = [System.IO.Path]::GetFullPath($ComfyPython)
$ComfyMain = [System.IO.Path]::GetFullPath($ComfyMain)
$CustomNodesSource = [System.IO.Path]::GetFullPath($CustomNodesSource)
$StateDir = [System.IO.Path]::GetFullPath($StateDir)
$BackendDir = Split-Path -Parent $BackendExe
if ([string]::IsNullOrWhiteSpace($DreamTextureNodes)) { $DreamTextureNodes = Join-Path $BackendDir 'comfy_nodes' }
$DreamTextureNodes = [System.IO.Path]::GetFullPath($DreamTextureNodes)
$WorkflowsDir = Join-Path $BackendDir 'workflows'
$ExtraModelPaths = @($ExtraModelPaths | ForEach-Object { [System.IO.Path]::GetFullPath($_) })

$PluginRoot = Get-PluginRoot
$FixturesSource = Join-Path $PluginRoot 'Tests\Fixtures'
$ComfyBase = Join-Path $StateDir 'comfy'
$CustomNodesDir = Join-Path $ComfyBase 'custom_nodes'
$OutputDir = Join-Path $StateDir 'output'
$DataDir = Join-Path $StateDir 'data'
$LogDir = Join-Path $StateDir 'logs'
$ConfigPath = Join-Path $StateDir 'pipeline.json'
$ComfyPidFile = Join-Path $StateDir 'comfy.pid'
$BackendPidFile = Join-Path $StateDir 'backend.pid'
$VramFile = Join-Path $StateDir 'vram.json'
$ComfyLog = Join-Path $LogDir 'comfy.log'
$BackendLog = Join-Path $LogDir 'dreamtexture.log'
$ComfyUrl = "http://127.0.0.1:$ComfyPort"
$BackendUrl = "http://127.0.0.1:$BackendPort"
$UrlLine = "DREAMTEXTURE_PIPELINE_URL=$BackendUrl"

function Stop-OwnedProcesses {
	# 按「后端先、ComfyUI 后」的顺序停掉自己登记的两个进程，再拆联接。失败路径和「半死不活」都用它。
	$Ok = Stop-RegisteredProcess $BackendPidFile $PipelineBackendExeName '后端'
	$Ok = (Stop-RegisteredProcess $ComfyPidFile $PipelineComfyExeName 'ComfyUI') -and $Ok
	$null = Remove-OwnedJunctions $CustomNodesDir
	return $Ok
}

function Get-LogTail([string] $Path, [int] $Count = 15) {
	if (Test-Path -LiteralPath $Path -PathType Leaf) { return @(Get-Content -LiteralPath $Path -Tail $Count -ErrorAction SilentlyContinue) }
	return @()
}

# ---- 已经在跑？ 先看自己登记的两个进程。
$ComfyProc = Get-RegisteredProcess (Read-PipelinePidFile $ComfyPidFile) $PipelineComfyExeName
$BackendProc = Get-RegisteredProcess (Read-PipelinePidFile $BackendPidFile) $PipelineBackendExeName
if ($null -ne $ComfyProc -and $null -ne $BackendProc -and
	(Test-HttpOk "$ComfyUrl/system_stats") -and (Test-BackendHealth $BackendPort) -and (Test-BackendSeesComfy $BackendPort)) {
	Write-Host "流水线后端已在运行（ComfyUI pid $($ComfyProc.Id) :$ComfyPort，后端 pid $($BackendProc.Id) :$BackendPort）。"
	Write-Output $UrlLine
	exit 0
}
if ((Test-Path -LiteralPath $ComfyPidFile) -or (Test-Path -LiteralPath $BackendPidFile)) {
	# 只活了一个、或活着却不应答：不猜它是刚起还是卡死，全部停掉按冷启动重来。
	# 只停自己登记的，且核对过映像与启动时间（见 Stop-RegisteredProcess）。
	Write-Host '登记的进程不完整或不应答，先停掉再冷启动。'
	if (-not (Stop-OwnedProcesses)) { Stop-WithFailure @('没能停掉上次遗留的进程，见上面。') }
}

# ---- 前置检查：东西都在不在
$Missing = @()
if (-not (Test-Path -LiteralPath $BackendExe -PathType Leaf)) { $Missing += "后端可执行文件：$BackendExe（来源：$($BackendResolved.Source)；用 -BackendExe 指定，或在后端仓库里 go build 一个）" }
if (-not (Test-Path -LiteralPath $WorkflowsDir -PathType Container)) { $Missing += "工作流目录：$WorkflowsDir" }
if (-not (Test-Path -LiteralPath $ComfyPython -PathType Leaf)) { $Missing += "ComfyUI 的 Python：$ComfyPython（-ComfyPython / -ComfyRoot）" }
if (-not (Test-Path -LiteralPath $ComfyMain -PathType Leaf)) { $Missing += "ComfyUI 的 main.py：$ComfyMain（-ComfyMain / -ComfyRoot）" }
foreach ($Pack in $PipelineNodePacks) {
	if (-not (Test-Path -LiteralPath (Join-Path $CustomNodesSource "$Pack\__init__.py") -PathType Leaf)) {
		$Missing += "节点包 $Pack：$(Join-Path $CustomNodesSource $Pack)（-CustomNodesSource）"
	}
}
if (-not (Test-Path -LiteralPath (Join-Path $DreamTextureNodes '__init__.py') -PathType Leaf)) { $Missing += "自研节点包：$DreamTextureNodes（-DreamTextureNodes）" }
if (-not (Test-Path -LiteralPath (Join-Path $FixturesSource 'mat_20260827_777001_ref\manifest.json') -PathType Leaf)) { $Missing += "固定素材：$FixturesSource\mat_20260827_777001_ref\manifest.json" }
foreach ($Yaml in $ExtraModelPaths) {
	if (-not (Test-Path -LiteralPath $Yaml -PathType Leaf)) { $Missing += "额外的模型路径配置：$Yaml" }
}
if ($Missing.Count -gt 0) { Stop-WithFailure (@('缺少下面这些东西，没法启动：') + ($Missing | ForEach-Object { "  - $_" })) }

# ---- 端口被谁占着？ 走到这里说明不是我们登记的进程。
foreach ($Item in @(@{ Port = $ComfyPort; What = 'ComfyUI' }, @{ Port = $BackendPort; What = '后端' })) {
	$Listener = Get-PortListener $Item.Port
	if ($null -ne $Listener) {
		Stop-WithFailure @(
			"$($Item.What)的端口 $($Item.Port) 已被占用：pid $($Listener.ProcessId)（$($Listener.ProcessName)，$($Listener.Image)），不是本脚本登记的进程。",
			'本脚本不会去动别人的进程。换端口（-ComfyPort / -BackendPort，DREAMTEXTURE_PIPELINE_URL 要跟着改）或先结束它。')
	}
}

# ---- 显存够不够？ 冷启动之前查，不够就拒绝，什么都还没动。
$FreeBefore = Get-FreeVramMiB
$NeedMiB = [int][Math]::Round($MinFreeVramGiB * 1024)
if ($NeedMiB -gt 0) {
	if ($null -eq $FreeBefore) {
		Stop-WithFailure @('读不到显存（找不到 nvidia-smi，或它报错了），没法确认显存够用，不敢起 ComfyUI。',
			'确认没有别的东西占着显卡后，可以用 -MinFreeVramGiB 0 跳过这项检查。')
	}
	if ($FreeBefore -lt $NeedMiB) {
		Stop-WithFailure @(
			("空闲显存只有 {0} MiB（{1:N1} GiB），低于要求的 {2} MiB（{3:N1} GiB），拒绝启动。" -f $FreeBefore, ($FreeBefore / 1024), $NeedMiB, ($NeedMiB / 1024)),
			'这台机器上 ComfyUI 和开着的虚幻编辑器会抢显存：显存吃满时驱动不报错，静默回退到系统内存，',
			'任务会慢几十倍甚至假死，不报错也不结束。与其让测试挂住，不如现在就拒绝。',
			'先关掉占显存的东西（开着的虚幻编辑器、别的 ComfyUI），再重试；或者用 -MinFreeVramGiB 调低阈值（风险自负）。')
	}
}

# ---- 准备状态目录
$Total = [System.Diagnostics.Stopwatch]::StartNew()
New-Item -ItemType Directory -Path $StateDir -Force | Out-Null
New-Item -ItemType Directory -Path $CustomNodesDir -Force | Out-Null

# 联接：先把自己上次留下的拆干净（只拆联接本身），再按这次的参数建。
$null = Remove-OwnedJunctions $CustomNodesDir
foreach ($Pack in $PipelineNodePacks) {
	New-OwnedJunction (Join-Path $CustomNodesDir $Pack) (Join-Path $CustomNodesSource $Pack)
}
New-OwnedJunction (Join-Path $CustomNodesDir $PipelineOwnNodePackName) $DreamTextureNodes

# ComfyUI 自己的 input / output / temp 每次冷启动都清：上次上传的图和产物不该影响这一次。
# （只删这三个普通目录；custom_nodes\ 里是联接，绝不递归删它。）
foreach ($Sub in @('input', 'output', 'temp')) {
	$Path = Join-Path $ComfyBase $Sub
	if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

# 后端的 output\ 是 Fixtures 的全新拷贝，data\ 清空（-KeepData 除外）。
if (Test-Path -LiteralPath $OutputDir) { Remove-Item -LiteralPath $OutputDir -Recurse -Force }
Copy-Item -LiteralPath $FixturesSource -Destination $OutputDir -Recurse -Force
if (-not $KeepData -and (Test-Path -LiteralPath $DataDir)) { Remove-Item -LiteralPath $DataDir -Recurse -Force }
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
foreach ($Log in @($ComfyLog, $BackendLog)) {
	if (Test-Path -LiteralPath $Log) { Remove-Item -LiteralPath $Log -Force -ErrorAction SilentlyContinue }
}

# 路径全用绝对路径：后端把相对路径解析到「配置文件所在目录的上一级」（见后端 docs/desktop-shell.md），
# 状态目录里没有那一级的约定，写绝对路径省得算。
$Config = [ordered]@{
	addr          = "127.0.0.1:$BackendPort"
	output_dir    = $OutputDir
	data_dir      = $DataDir
	workflows_dir = $WorkflowsDir
	comfy         = [ordered]@{
		mode            = 'attach'
		base_url        = $ComfyUrl
		start_timeout   = '30s'
		health_interval = '10s'
		auto_restart    = $false
		max_queue_depth = 8
	}
}
[System.IO.File]::WriteAllText($ConfigPath, ($Config | ConvertTo-Json -Depth 4), [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText($VramFile, ([ordered]@{ freeBeforeMiB = $FreeBefore; atUtc = (Get-Date).ToUniversalTime().ToString('o') } | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))

# ---- 起 ComfyUI
#
# **经 cmd.exe 起，由 cmd 把输出重定向进文件；不要给 Start-Process 加 -RedirectStandardOutput / -RedirectStandardError。**
# 一旦重定向，.NET 用 bInheritHandles=true 建进程，调用者交给本脚本的 stdout/stderr 管道（自动化测试、CI、
# `| cat` 接的管道）会被 ComfyUI 一并继承；脚本退出后 ComfyUI 还活着，管道就一直不关，调用者读输出读到 EOF
# 永远等不到——表现为「Start 明明返回了，调用它的东西却挂住」（FixtureBackend 实测踩过）。
# 不重定向的 Start-Process 走 ShellExecute，不继承任何句柄；重定向交给 cmd，句柄只在 cmd 与 python 之间。
# 代价是 pid 文件里登记的得是 python 而不是 cmd：下面等到 cmd 的子进程里出现 python.exe 再登记。
Write-Host "启动隔离的 ComfyUI：$ComfyPython"
Write-Host "  --base-directory $ComfyBase"
$ComfyArgs = @('-s', "`"$ComfyMain`"", '--listen', '127.0.0.1', '--port', "$ComfyPort",
	'--base-directory', "`"$ComfyBase`"", '--preview-method', 'none')
foreach ($Yaml in $ExtraModelPaths) { $ComfyArgs += @('--extra-model-paths-config', "`"$Yaml`"") }
$Inner = "`"$ComfyPython`" $($ComfyArgs -join ' ') > `"$ComfyLog`" 2>&1"
# 给子进程的环境变量，起完立刻还原，不污染调用者：
#   PYTHONDONTWRITEBYTECODE  不往整合包 / 后端仓库的目录里写 .pyc。
#   PYTHONUNBUFFERED         输出不缓冲，起不来时日志才是完整的。
#   PYTHONUTF8 / PYTHONIOENCODING  **必须**。输出重定向进文件后，Python 按系统代码页（本机是 936/GBK）编码，
#       而 TextureAlchemy 的节点会 print 带「✓」的进度行——编码不了就抛 UnicodeEncodeError；更糟的是
#       ComfyUI 在处理这个异常时又往日志里写（同样编不了），第二次异常把执行线程带进死路：任务永远停在「生成中」，
#       不报错也不结束。实测踩过，表现和显存被占满的假死一模一样，极难想到是编码。
$ChildEnv = [ordered]@{
	PYTHONDONTWRITEBYTECODE = '1'
	PYTHONUNBUFFERED        = '1'
	PYTHONUTF8              = '1'
	PYTHONIOENCODING        = 'utf-8'
}
$PreviousEnv = @{}
foreach ($Key in $ChildEnv.Keys) {
	$PreviousEnv[$Key] = [Environment]::GetEnvironmentVariable($Key, 'Process')
	[Environment]::SetEnvironmentVariable($Key, $ChildEnv[$Key], 'Process')
}
try {
	$Wrapper = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\cmd.exe') `
		-ArgumentList "/d /s /c `"$Inner`"" -WorkingDirectory $ComfyRoot -WindowStyle Hidden -PassThru
}
finally {
	foreach ($Key in $ChildEnv.Keys) { [Environment]::SetEnvironmentVariable($Key, $PreviousEnv[$Key], 'Process') }
}

$Python = $null
$FindDeadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $FindDeadline -and $null -eq $Python) {
	$Kid = Get-CimInstance -ClassName Win32_Process -Filter "ParentProcessId = $($Wrapper.Id)" -ErrorAction SilentlyContinue |
		Where-Object { $_.Name -ieq $PipelineComfyExeName } | Select-Object -First 1
	if ($null -ne $Kid) { $Python = Get-Process -Id ([int]$Kid.ProcessId) -ErrorAction SilentlyContinue }
	if ($null -eq $Python) {
		if ($Wrapper.HasExited) { break }
		Start-Sleep -Milliseconds 100
	}
}
if ($null -eq $Python) {
	if (-not $Wrapper.HasExited) { Stop-Process -Id $Wrapper.Id -Force -ErrorAction SilentlyContinue }
	$null = Remove-OwnedJunctions $CustomNodesDir
	Stop-WithFailure (@('ComfyUI 的 Python 没有起来。', "日志：$ComfyLog") + (Get-LogTail $ComfyLog))
}
[System.IO.File]::WriteAllText($ComfyPidFile, ([ordered]@{
			pid           = $Python.Id
			startTicks    = $Python.StartTime.ToUniversalTime().Ticks
			exe           = $ComfyPython
			cmdPid        = $Wrapper.Id
			cmdStartTicks = $Wrapper.StartTime.ToUniversalTime().Ticks
			port          = $ComfyPort
			stateDir      = $StateDir
		} | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))

$Step = [System.Diagnostics.Stopwatch]::StartNew()
$ComfyReady = $false
$Deadline = (Get-Date).AddSeconds($ComfyTimeoutSeconds)
while ((Get-Date) -lt $Deadline) {
	if ($Python.HasExited) { break }
	if (Test-HttpOk "$ComfyUrl/system_stats" 2) { $ComfyReady = $true; break }
	Start-Sleep -Milliseconds 500
}
if (-not $ComfyReady) {
	$Reason = if ($Python.HasExited) { "进程在应答之前就退出了（退出码 $($Python.ExitCode)）" } else { "$ComfyTimeoutSeconds 秒内 /system_stats 没有应答" }
	$Tail = Get-LogTail $ComfyLog 25
	$null = Stop-OwnedProcesses
	Stop-WithFailure (@("ComfyUI 没能就绪：$Reason。", "完整日志：$ComfyLog") + $Tail)
}
$ComfySeconds = $Step.Elapsed.TotalSeconds
Write-Host ("ComfyUI 已应答：pid {0}，{1}（{2:N1} 秒）" -f $Python.Id, $ComfyUrl, $ComfySeconds)

# 自研包真的加载了、而且是带包装节点的新版？ 联接指到旧代码的话这里就能发现，比测试跑到一半报
# 「节点未注册」清楚得多。
foreach ($NodeName in @('DT_EnsureImageShape', 'DT_ChordMaterialEstimation', 'DT_ChordNormalToHeight')) {
	$Registered = $false
	try {
		$Info = (Invoke-WebRequest -Uri "$ComfyUrl/object_info/$NodeName" -TimeoutSec 10 -UseBasicParsing -ErrorAction Stop).Content | ConvertFrom-Json
		$Registered = $null -ne $Info.PSObject.Properties[$NodeName]
	}
	catch { }
	if (-not $Registered) {
		$Tail = Get-LogTail $ComfyLog 25
		$null = Stop-OwnedProcesses
		Stop-WithFailure (@("ComfyUI 起来了，但节点 $NodeName 没有注册。",
				"自研包联接指向 $DreamTextureNodes，确认那里是带 CHORD 包装节点（chord_tiling.py）的新版。", "日志：$ComfyLog") + $Tail)
	}
}

# 模型在不在：不在不算起不来（只跑传统派生用不到 CHORD），但要说出来，不然测试跑到分解那一步才一脸茫然。
try {
	$Checkpoints = @((Invoke-WebRequest -Uri "$ComfyUrl/models/checkpoints" -TimeoutSec 10 -UseBasicParsing -ErrorAction Stop).Content | ConvertFrom-Json)
	if ($Checkpoints -notcontains 'chord_v1.safetensors') {
		Write-Warning 'ComfyUI 的 checkpoints 里没有 chord_v1.safetensors：走 CHORD 分解的管线会失败（传统派生不受影响）。模型路径见整合包的 extra_model_paths.yaml，或用 -ExtraModelPaths 补。'
	}
}
catch {
	Write-Warning "读不到 ComfyUI 的 checkpoints 列表：$($_.Exception.Message)"
}

# ---- 起后端
#
# 同样不给 Start-Process 加重定向（理由见上）。后端自己把日志写进 -log-dir，够用。
Write-Host "启动新后端：$BackendExe"
$Step.Restart()
$Backend = Start-Process -FilePath $BackendExe `
	-ArgumentList @('-config', $ConfigPath, '-log-dir', $LogDir) `
	-WorkingDirectory $StateDir -WindowStyle Hidden -PassThru
[System.IO.File]::WriteAllText($BackendPidFile, ([ordered]@{
			pid        = $Backend.Id
			startTicks = $Backend.StartTime.ToUniversalTime().Ticks
			exe        = $BackendExe
			port       = $BackendPort
			stateDir   = $StateDir
		} | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))

$BackendReady = $false
$Deadline = (Get-Date).AddSeconds($BackendTimeoutSeconds)
while ((Get-Date) -lt $Deadline) {
	if ($Backend.HasExited) { break }
	# 后端自己的健康巡检比 ComfyUI 晚一拍，等它也看到 ComfyUI 就绪才算数：
	# 不然测试一提交就撞上「ComfyUI 不可用，任务排队等待」。
	if ((Test-BackendHealth $BackendPort 1) -and (Test-BackendSeesComfy $BackendPort 1)) { $BackendReady = $true; break }
	Start-Sleep -Milliseconds 200
}
if (-not $BackendReady) {
	$Reason = if ($Backend.HasExited) { "进程在应答之前就退出了（退出码 $($Backend.ExitCode)）" } else { "$BackendTimeoutSeconds 秒内 /api/health 没有应答，或后端没看到 ComfyUI 就绪" }
	$Tail = Get-LogTail $BackendLog 15
	$null = Stop-OwnedProcesses
	Stop-WithFailure (@("后端没能就绪：$Reason。", "完整日志：$BackendLog") + $Tail)
}
$BackendSeconds = $Step.Elapsed.TotalSeconds
$FreeAfter = Get-FreeVramMiB
Write-Host ("后端已应答：pid {0}，{1}（{2:N1} 秒）" -f $Backend.Id, $BackendUrl, $BackendSeconds)
Write-Host ("流水线后端就绪，共 {0:N1} 秒。显存空闲：启动前 {1} MiB -> 现在 {2} MiB。" -f $Total.Elapsed.TotalSeconds, $FreeBefore, $FreeAfter)
Write-Host "  状态目录 $StateDir（日志在 logs\）"
Write-Output $UrlLine
exit 0
