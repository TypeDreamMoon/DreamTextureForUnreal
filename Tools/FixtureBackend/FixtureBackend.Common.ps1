# Start-/Stop-FixtureBackend.ps1 共用的几个函数和默认值。用 . 点号引入，不单独运行。
#
# 这个测试后端是**固定数据**的：output_dir 是 Tests\Fixtures 的拷贝，ComfyUI 指向没人监听的端口。
# 它不拉起 ComfyUI（用户机器上 ComfyUI 和虚幻编辑器会抢显存），也不碰用户真实后端（8777）的数据。

$FixturePort = 18777
$FixtureExeName = 'dreamtexture.exe'
# 没人监听的端口，给 ComfyUI 的 base_url 用：attach 模式只做健康检查，连不上就一直报「未就绪」，
# 绝不会去启动谁。
$FixtureDeadComfyPort = 18779

function Get-PluginRoot {
	# Tools\FixtureBackend\ 的上两级。
	return (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
}

function Get-DefaultStateDir {
	# 宿主工程的 Saved\FixtureBackend\：<宿主>\Plugins\<插件>\ 往上两级若有 .uproject 就是宿主。
	# 不在宿主里（比如在主 checkout 里直接跑）就退到临时目录，不往仓库里写。
	$PluginRoot = Get-PluginRoot
	$PluginsDir = Split-Path -Parent $PluginRoot
	$HostDir = if ($PluginsDir) { Split-Path -Parent $PluginsDir } else { $null }
	if ($HostDir -and (Test-Path -LiteralPath $HostDir -PathType Container) -and
		@(Get-ChildItem -LiteralPath $HostDir -Filter '*.uproject' -File -ErrorAction SilentlyContinue).Count -gt 0) {
		return (Join-Path $HostDir 'Saved\FixtureBackend')
	}
	return (Join-Path ([System.IO.Path]::GetTempPath()) 'DreamTextureFixtureBackend')
}

function Get-ProcessImagePath([int] $ProcessId) {
	# 进程映像的完整路径；进程不在了返回 $null。用 CIM 而不是 Get-Process.Path，
	# 后者对没有权限读的进程会静默给空。
	$Info = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $ProcessId" -ErrorAction SilentlyContinue
	if ($null -eq $Info) { return $null }
	return [string]$Info.ExecutablePath
}

function Read-FixturePidFile([string] $PidFile) {
	# 读不出来（不存在、坏了）一律当作没有。
	if (-not (Test-Path -LiteralPath $PidFile -PathType Leaf)) { return $null }
	try {
		$Data = Get-Content -LiteralPath $PidFile -Raw | ConvertFrom-Json
		if ($null -eq $Data.pid) { return $null }
		return $Data
	}
	catch {
		return $null
	}
}

function Get-FixtureProcess($PidData) {
	# pid 文件里记的那个进程**还活着，并且确实是同一个 dreamtexture.exe**才返回它。
	# 只核对 pid 不够：pid 会被系统复用，上次崩溃留下的 pid 文件可能指着一个无关的进程，
	# 而 Stop 是要杀进程的。所以映像名和启动时间都要对上。
	if ($null -eq $PidData) { return $null }
	$Proc = Get-Process -Id ([int]$PidData.pid) -ErrorAction SilentlyContinue
	if ($null -eq $Proc) { return $null }
	$Image = Get-ProcessImagePath ([int]$PidData.pid)
	if ([string]::IsNullOrEmpty($Image) -or
		-not [string]::Equals([System.IO.Path]::GetFileName($Image), $FixtureExeName, [System.StringComparison]::OrdinalIgnoreCase)) {
		return $null
	}
	if ($PidData.PSObject.Properties['startTicks'] -and $PidData.startTicks) {
		if ($Proc.StartTime.ToUniversalTime().Ticks -ne [long]$PidData.startTicks) { return $null }
	}
	return $Proc
}

function Test-FixtureHealth([int] $Port, [int] $TimeoutSec = 2) {
	# 只有 200 且 {"ok":true} 才算应答；别的服务碰巧占着这个端口不能算。
	try {
		$Response = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/health" -TimeoutSec $TimeoutSec -UseBasicParsing -ErrorAction Stop
		if ($Response.StatusCode -ne 200) { return $false }
		return ($Response.Content | ConvertFrom-Json).ok -eq $true
	}
	catch {
		return $false
	}
}

function Get-PortListener([int] $Port) {
	# 谁在监听这个端口；没人返回 $null。
	$Connection = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($null -eq $Connection) { return $null }
	$Owner = Get-Process -Id $Connection.OwningProcess -ErrorAction SilentlyContinue
	return [pscustomobject]@{
		ProcessId   = [int]$Connection.OwningProcess
		ProcessName = if ($Owner) { $Owner.ProcessName } else { '(未知)' }
		Image       = Get-ProcessImagePath ([int]$Connection.OwningProcess)
	}
}
