#Requires -Version 7.0
<#
.SYNOPSIS
    起一个固定数据的 DreamTexture 测试后端（127.0.0.1:18777），在它真正应答之后才返回。

.DESCRIPTION
    给 DreamTexture.Live.* 自动化测试用。后端是真正的 DreamTexture 后端（Go 编出来的 dreamtexture.exe），
    但喂给它的东西全是固定的、隔离的：

      output_dir    Tests\Fixtures 的**拷贝**（里面有 mat_20260827_777001_ref\）。后端的
                    /api/materials/{id} 和 /files/ 接口直接读磁盘，所以能直接服务它。
      data_dir      独立的目录（SQLite 在里面），不碰用户真实后端（8777）的数据。
      workflows_dir 后端仓库自己的 workflows\（只读）。
      comfy         attach 模式，base_url 指向一个没人监听的端口，auto_restart 关闭。
                    **不会拉起 ComfyUI**：用户机器上 ComfyUI 和虚幻编辑器会抢显存。

    状态都放在 -StateDir（默认宿主工程的 Saved\FixtureBackend\）：fixture.json（配置）、backend.pid、
    logs\dreamtexture.log（后端自己的日志）、output\、data\。

    可以反复、无参数地调用：
      - 已经在跑且应答 -> 什么都不做，返回 0；
      - 没在跑 -> 重新铺 output\ 与 data\（每次冷启动都是同一份固定数据），起进程，写 pid 文件，
        轮询 /api/health 直到应答再返回 0；
      - 端口被别的进程占了 -> 说清楚是谁，返回 1，不动那个进程。

.PARAMETER Port
    监听端口。默认 18777。

.PARAMETER BackendExe
    后端可执行文件。没给就依次看环境变量 DREAMTEXTURE_BACKEND_EXE、Tools\LocalSettings.psd1 的 BackendExe，
    都没有则失败（说明该设什么）。

.PARAMETER WorkflowsDir
    工作流目录。默认在 BackendExe 同目录的 workflows\。

.PARAMETER StateDir
    状态目录。默认宿主工程的 Saved\FixtureBackend\，不在宿主里则用临时目录。

.PARAMETER TimeoutSeconds
    等待后端应答的上限。默认 30。

.PARAMETER KeepData
    冷启动时不清 data\（保留上次的 SQLite）。默认每次冷启动都清，保证起点一致。

.NOTES
    退出码：0 后端在跑且应答；1 没能做到，原因已打印。
#>
[CmdletBinding()]
param(
	[int] $Port = 18777,
	[string] $BackendExe = '',
	[string] $WorkflowsDir = '',
	[string] $StateDir = '',
	[int] $TimeoutSeconds = 30,
	[switch] $KeepData
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
# PS 7.3+ 里原生命令写 stderr 就会抛异常（见 Tools/Test.ps1）。这里不直接调原生命令，保险起见同样关掉。
$PSNativeCommandUseErrorActionPreference = $false

. (Join-Path $PSScriptRoot 'FixtureBackend.Common.ps1')
. (Join-Path $PSScriptRoot '..\Common.ps1')

function Stop-WithFailure([string[]] $Messages) {
	foreach ($Message in $Messages) { [Console]::Error.WriteLine("Start-FixtureBackend: $Message") }
	exit 1
}

# 后端可执行文件：-BackendExe -> 环境变量 DREAMTEXTURE_BACKEND_EXE -> LocalSettings.psd1 的 BackendExe。
# 后端有改动（推送、回流）要拿新编的可执行文件来测，又不能覆盖正在用的那个 dreamtexture.exe，就用环境变量指过去。
# 工作流目录随可执行文件走（见下一行）。
$BackendResolved = Resolve-DreamTextureBackendExe -Explicit $BackendExe -Kind Fixture
if ($null -eq $BackendResolved.Value) { Stop-WithFailure @($BackendResolved.Hint) }
$BackendExe = $BackendResolved.Value
if ([string]::IsNullOrWhiteSpace($StateDir)) { $StateDir = Get-DefaultStateDir }
if ([string]::IsNullOrWhiteSpace($WorkflowsDir)) { $WorkflowsDir = Join-Path (Split-Path -Parent $BackendExe) 'workflows' }
$BackendExe = [System.IO.Path]::GetFullPath($BackendExe)
$StateDir = [System.IO.Path]::GetFullPath($StateDir)
$WorkflowsDir = [System.IO.Path]::GetFullPath($WorkflowsDir)

$PluginRoot = Get-PluginRoot
$FixturesSource = Join-Path $PluginRoot 'Tests\Fixtures'
$PidFile = Join-Path $StateDir 'backend.pid'
$ConfigPath = Join-Path $StateDir 'fixture.json'
$OutputDir = Join-Path $StateDir 'output'
$DataDir = Join-Path $StateDir 'data'
$LogDir = Join-Path $StateDir 'logs'

# ---- 已经在跑？ 先看自己登记的那个进程。
$PidData = Read-FixturePidFile $PidFile
$Running = Get-FixtureProcess $PidData
if ($null -ne $Running) {
	if (Test-FixtureHealth $Port) {
		Write-Host "固定数据后端已在运行（pid $($Running.Id)，http://127.0.0.1:$Port）。"
		exit 0
	}
	# 进程活着但不应答：可能刚起、可能卡死。给它一个宽限期；还不应答就当它死了，杀掉重来。
	Write-Host "登记的后端进程 $($Running.Id) 还活着但 /api/health 不应答，等待最多 10 秒……"
	$Deadline = (Get-Date).AddSeconds(10)
	while ((Get-Date) -lt $Deadline -and -not $Running.HasExited -and -not (Test-FixtureHealth $Port)) { Start-Sleep -Milliseconds 250 }
	if (Test-FixtureHealth $Port) {
		Write-Host '后端已应答。'
		exit 0
	}
	Write-Host '仍不应答，结束这个进程后重新启动。'
	Stop-Process -Id $Running.Id -Force -ErrorAction SilentlyContinue
	$Running.WaitForExit(5000) | Out-Null
}
if (Test-Path -LiteralPath $PidFile) { Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue }

# ---- 前置检查
if (-not (Test-Path -LiteralPath $BackendExe -PathType Leaf)) {
	Stop-WithFailure @("找不到后端可执行文件：$BackendExe（来源：$($BackendResolved.Source)）", '用 -BackendExe 指定，或在后端仓库里 go build 一个。')
}
if (-not (Test-Path -LiteralPath (Join-Path $FixturesSource 'mat_20260827_777001_ref\manifest.json') -PathType Leaf)) {
	Stop-WithFailure @("找不到固定素材：$FixturesSource\mat_20260827_777001_ref\manifest.json")
}
if (-not (Test-Path -LiteralPath $WorkflowsDir -PathType Container)) {
	Stop-WithFailure @("找不到工作流目录：$WorkflowsDir（后端启动要载入工作流，缺了会起不来）。")
}

# ---- 端口被谁占着？ 走到这里说明不是我们登记的进程。
$Listener = Get-PortListener $Port
if ($null -ne $Listener) {
	$Lines = @("端口 $Port 已被占用：pid $($Listener.ProcessId)（$($Listener.ProcessName)，$($Listener.Image)），不是本脚本登记的固定数据后端。")
	if ($Listener.Image -and [System.IO.Path]::GetFileName($Listener.Image) -ieq $FixtureExeName) {
		$Lines += '它是个 dreamtexture.exe，多半是 pid 文件丢了之后遗留的孤儿。确认不是你自己在用的之后，手动结束它再重试。'
	}
	else {
		$Lines += '本脚本不会去动别人的进程。换端口（-Port）或先结束它；DREAMTEXTURE_LIVE_URL 要跟着改。'
	}
	Stop-WithFailure $Lines
}

# ---- 铺固定数据：output\ 是 Fixtures 的全新拷贝，data\ 清空。
# 每次冷启动都重铺，这样上一次测试里改过的东西（收藏标记之类）不会漏到下一次。
New-Item -ItemType Directory -Path $StateDir -Force | Out-Null
if (Test-Path -LiteralPath $OutputDir) { Remove-Item -LiteralPath $OutputDir -Recurse -Force }
Copy-Item -LiteralPath $FixturesSource -Destination $OutputDir -Recurse -Force
if (-not $KeepData -and (Test-Path -LiteralPath $DataDir)) { Remove-Item -LiteralPath $DataDir -Recurse -Force }
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null

# 路径全用绝对路径：后端把相对路径解析到「配置文件所在目录的上一级」（见后端 docs/desktop-shell.md），
# 而 StateDir 里没有那一级的约定，写绝对路径省得算。
$Config = [ordered]@{
	addr          = "127.0.0.1:$Port"
	output_dir    = $OutputDir
	data_dir      = $DataDir
	workflows_dir = $WorkflowsDir
	comfy         = [ordered]@{
		mode            = 'attach'
		base_url        = "http://127.0.0.1:$FixtureDeadComfyPort"
		start_timeout   = '2s'
		health_interval = '30s'
		auto_restart    = $false
		max_queue_depth = 8
	}
}
[System.IO.File]::WriteAllText($ConfigPath, ($Config | ConvertTo-Json -Depth 4), [System.Text.UTF8Encoding]::new($false))

# ---- 起进程
#
# **不要给 Start-Process 加 -RedirectStandardOutput / -RedirectStandardError。** 一旦重定向，.NET 用
# bInheritHandles=true 建进程，调用者交给本脚本的 stdout/stderr 管道（可能是虚幻自动化测试、CI、
# `| cat` 接的管道）会被后端一并继承；脚本退出后后端还活着，管道就一直不关，调用者读输出读到
# EOF 永远等不到——表现为「Start 明明返回了，调用它的东西却挂住」。实测踩过。
# 不重定向时 Start-Process 走 ShellExecute，不继承任何句柄。后端自己会把日志写进 -log-dir，
# 够用；它在应答之前就挂掉的话，原因也是先写进那个日志文件的。
$LogFile = Join-Path $LogDir 'dreamtexture.log'
if (Test-Path -LiteralPath $LogFile) { Remove-Item -LiteralPath $LogFile -Force -ErrorAction SilentlyContinue }
Write-Host "启动固定数据后端：$BackendExe"
Write-Host "  配置 $ConfigPath"
$Proc = Start-Process -FilePath $BackendExe `
	-ArgumentList @('-config', $ConfigPath, '-log-dir', $LogDir) `
	-WorkingDirectory $StateDir -WindowStyle Hidden -PassThru

$PidRecord = [ordered]@{
	pid        = $Proc.Id
	startTicks = $Proc.StartTime.ToUniversalTime().Ticks
	exe        = $BackendExe
	port       = $Port
	stateDir   = $StateDir
}
[System.IO.File]::WriteAllText($PidFile, ($PidRecord | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))

# ---- 等它应答
$Deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$Answered = $false
while ((Get-Date) -lt $Deadline) {
	if ($Proc.HasExited) { break }
	if (Test-FixtureHealth $Port 1) { $Answered = $true; break }
	Start-Sleep -Milliseconds 200
}

if ($Answered) {
	Write-Host "固定数据后端已应答：pid $($Proc.Id)，http://127.0.0.1:$Port（output=$OutputDir）"
	exit 0
}

# ---- 失败：把现场摆出来，并且不留一个半死的进程
$Reason = if ($Proc.HasExited) { "进程在应答之前就退出了（退出码 $($Proc.ExitCode)）" } else { "$TimeoutSeconds 秒内 /api/health 没有应答" }
if (-not $Proc.HasExited) {
	Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
	$Proc.WaitForExit(5000) | Out-Null
}
Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
$Tail = @()
if (Test-Path -LiteralPath $LogFile) { $Tail += @(Get-Content -LiteralPath $LogFile -Tail 15 -ErrorAction SilentlyContinue) }
Stop-WithFailure (@("后端没能就绪：$Reason。", "完整日志：$LogFile") + $Tail)
