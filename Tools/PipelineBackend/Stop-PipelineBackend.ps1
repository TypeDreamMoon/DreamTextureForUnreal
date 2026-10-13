#Requires -Version 7.0
<#
.SYNOPSIS
    停掉 Start-PipelineBackend.ps1 起的后端和隔离的 ComfyUI，拆掉联接，确认端口释放、显存回来。

.DESCRIPTION
    **只**停 pid 文件里登记的两个进程（后端先、ComfyUI 后），并且先核对映像名和启动时间对得上
    （pid 会被系统复用，pid 文件可能是上次崩溃遗留的，指着一个无关进程）。核对不过就不杀，只清掉过期的
    pid 文件。用户自己的 ComfyUI（8188）和后端（8777）不在登记里，永远不会被碰。

    联接只拆状态目录 comfy\custom_nodes\ 下**类型确实是联接**的条目，而且用非递归删除——删的是联接本身，
    不会删穿到它指向的目录（整合包里的节点包、后端仓库的 comfy_nodes）。真目录一律不动。

    没在跑也返回 0，所以可以随便反复调用。

    停完会等：
      - 两个端口真的释放（别人占着不算我们的事，但占着的是谁会说清楚）；
      - 显存回到启动前的水平（启动时记在 vram.json 里）。显存是整机共享的，别的程序的占用会上下浮动，
        所以没回到只警告，不算失败。

.PARAMETER StateDir
    状态目录，必须和 Start 用的一致。默认宿主工程的 Saved\PipelineBackend\。

.PARAMETER ComfyPort
    只用来在停完之后核对端口已释放。默认 18188。

.PARAMETER BackendPort
    同上。默认 18787。

.PARAMETER VramToleranceMiB
    显存恢复到「启动前 - 容差」就算回来了。默认 768。

.PARAMETER VramWaitSeconds
    等显存回来的上限。默认 20。

.NOTES
    退出码：0 已不在运行（含本来就没在跑）；1 进程杀不掉，或停了之后端口还被占着。
#>
[CmdletBinding()]
param(
	[string] $StateDir = '',
	[int] $ComfyPort = 18188,
	[int] $BackendPort = 18787,
	[int] $VramToleranceMiB = 768,
	[int] $VramWaitSeconds = 20
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

. (Join-Path $PSScriptRoot 'PipelineBackend.Common.ps1')

if ([string]::IsNullOrWhiteSpace($StateDir)) { $StateDir = Get-DefaultStateDir }
$StateDir = [System.IO.Path]::GetFullPath($StateDir)
$ComfyPidFile = Join-Path $StateDir 'comfy.pid'
$BackendPidFile = Join-Path $StateDir 'backend.pid'
$VramFile = Join-Path $StateDir 'vram.json'
$CustomNodesDir = Join-Path $StateDir 'comfy\custom_nodes'

$HadAnything = (Test-Path -LiteralPath $ComfyPidFile) -or (Test-Path -LiteralPath $BackendPidFile)
$Stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

$Ok = Stop-RegisteredProcess $BackendPidFile $PipelineBackendExeName '后端'
$Ok = (Stop-RegisteredProcess $ComfyPidFile $PipelineComfyExeName 'ComfyUI') -and $Ok
if (-not $Ok) {
	[Console]::Error.WriteLine('Stop-PipelineBackend: 有进程没能停掉，见上面；联接先不拆（进程还可能在读）。')
	exit 1
}

$Removed = Remove-OwnedJunctions $CustomNodesDir
if ($Removed -gt 0) { Write-Host "已拆掉 $Removed 个联接（只拆联接本身，目标目录没动）。" }

if (-not $HadAnything) {
	# 没登记任何进程：本来就没在跑。不去查端口——占着它的多半是别人，不是我们的事。
	Write-Host '流水线后端没有在运行。'
	exit 0
}

# ---- 端口释放了吗
$Deadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $Deadline -and
	($null -ne (Get-PortListener $ComfyPort) -or $null -ne (Get-PortListener $BackendPort))) { Start-Sleep -Milliseconds 100 }
$Stuck = $false
foreach ($Item in @(@{ Port = $ComfyPort; What = 'ComfyUI' }, @{ Port = $BackendPort; What = '后端' })) {
	$Left = Get-PortListener $Item.Port
	if ($null -ne $Left) {
		[Console]::Error.WriteLine("Stop-PipelineBackend: $($Item.What)已停，但端口 $($Item.Port) 仍被 pid $($Left.ProcessId)（$($Left.ProcessName)）占着。")
		$Stuck = $true
	}
}
if ($Stuck) { exit 1 }
Write-Host "端口 $ComfyPort / $BackendPort 已释放。"

# ---- 显存回来了吗
$Before = $null
if (Test-Path -LiteralPath $VramFile -PathType Leaf) {
	try { $Before = [int](Get-Content -LiteralPath $VramFile -Raw | ConvertFrom-Json).freeBeforeMiB } catch { }
}
$Now = Get-FreeVramMiB
if ($null -ne $Before -and $null -ne $Now) {
	$Target = $Before - $VramToleranceMiB
	$VramDeadline = (Get-Date).AddSeconds($VramWaitSeconds)
	while ($Now -lt $Target -and (Get-Date) -lt $VramDeadline) {
		Start-Sleep -Milliseconds 500
		$Now = Get-FreeVramMiB
	}
	if ($null -ne $Now -and $Now -ge $Target) {
		Write-Host "显存已回来：空闲 $Now MiB（启动前 $Before MiB）。"
	}
	else {
		Write-Warning "显存没回到启动前的水平：空闲 $Now MiB（启动前 $Before MiB，容差 $VramToleranceMiB MiB）。显存是整机共享的，多半是别的程序涨了；若怀疑 ComfyUI 没退干净，查 nvidia-smi。"
	}
}
else {
	Write-Host "显存空闲 $Now MiB（没有启动前的记录可比）。"
}

Write-Host ("流水线后端已停止（{0:N1} 秒）。" -f $Stopwatch.Elapsed.TotalSeconds)
exit 0
