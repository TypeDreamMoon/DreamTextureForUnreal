#Requires -Version 7.0
<#
.SYNOPSIS
    停掉 Start-FixtureBackend.ps1 起的那个固定数据后端。

.DESCRIPTION
    **只**停 pid 文件里登记的那个进程，并且先核对它的映像确实是 dreamtexture.exe、启动时间也对得上
    （pid 会被系统复用，pid 文件可能是上次崩溃遗留的，指着一个无关进程）。核对不过就不杀，只清掉
    过期的 pid 文件。没在跑也返回 0，所以可以随便反复调用。

    停完会等进程真的退出、端口真的释放再返回，这样紧接着的 Start 不会撞上还没放手的端口。

.PARAMETER StateDir
    状态目录，必须和 Start 用的一致。默认宿主工程的 Saved\FixtureBackend\。

.PARAMETER Port
    只用来在停完之后核对端口已释放。默认 18777。

.NOTES
    退出码：0 后端已不在运行；1 杀不掉，或停了之后端口还被占着。
#>
[CmdletBinding()]
param(
	[string] $StateDir = '',
	[int] $Port = 18777
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

. (Join-Path $PSScriptRoot 'FixtureBackend.Common.ps1')

if ([string]::IsNullOrWhiteSpace($StateDir)) { $StateDir = Get-DefaultStateDir }
$StateDir = [System.IO.Path]::GetFullPath($StateDir)
$PidFile = Join-Path $StateDir 'backend.pid'

$PidData = Read-FixturePidFile $PidFile
if ($null -eq $PidData) {
	Write-Host '固定数据后端没有在运行（没有 pid 文件）。'
	exit 0
}

$Proc = Get-FixtureProcess $PidData
if ($null -eq $Proc) {
	Write-Host "pid 文件里的进程 $($PidData.pid) 已经不在，或已不是那个 dreamtexture.exe（pid 被复用了）；只清掉过期的 pid 文件，不杀任何进程。"
	Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
	exit 0
}

Write-Host "停止固定数据后端：pid $($Proc.Id)"
Stop-Process -Id $Proc.Id -Force
if (-not $Proc.WaitForExit(10000)) {
	[Console]::Error.WriteLine("Stop-FixtureBackend: 进程 $($Proc.Id) 10 秒内没有退出。")
	exit 1
}
Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue

# 进程没了，监听 socket 也该随之释放；这里确认一下，免得紧接着的 Start 把「端口被占」误报给用户。
$Deadline = (Get-Date).AddSeconds(5)
while ((Get-Date) -lt $Deadline -and $null -ne (Get-PortListener $Port)) { Start-Sleep -Milliseconds 100 }
$Left = Get-PortListener $Port
if ($null -ne $Left) {
	[Console]::Error.WriteLine("Stop-FixtureBackend: 后端退出了，但端口 $Port 仍被 pid $($Left.ProcessId)（$($Left.ProcessName)）占着。")
	exit 1
}
Write-Host '已停止，端口已释放。'
exit 0
