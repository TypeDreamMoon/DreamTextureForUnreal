# Start-/Stop-PipelineBackend.ps1 共用的函数和默认值。用 . 点号引入，不单独运行。
#
# 与 FixtureBackend 的区别：那个是「固定数据 + 没人监听的 ComfyUI」，只测接口契约；
# 这个是「隔离的真 ComfyUI + 真后端」，真的跑分解管线（CHORD / 传统派生），给 DreamTexture.Pipeline.* 用。
# 代价是吃显存，所以起之前先查空闲显存，不够就拒绝。
#
# 两个守则贯穿全部函数：
#   1. 只动自己起的东西。杀进程之前核对映像名和启动时间（pid 会被复用）；拆联接只拆状态目录里的、
#      类型确实是联接的，而且用「非递归删除」——删联接本身，绝不删穿到它指向的目录。
#   2. 绝不碰用户的 ComfyUI（8188）和用户的后端（8777）。端口默认都避开它们，被占了就拒绝，不去杀占用者。

$PipelineBackendExeName = 'dreamtexture.exe'
$PipelineComfyExeName = 'python.exe'
# 后端可执行文件与 ComfyUI 整合包在哪，不在这里写死：Start-PipelineBackend.ps1 按
# 参数 -> 环境变量 -> Tools\LocalSettings.psd1 -> 失败 的顺序解析（Tools\Common.ps1）。
# 隔离的 ComfyUI 里只放这几个节点包（从整合包的 custom_nodes 里联接过来，只读用）。
$PipelineNodePacks = @('ComfyUI-Chord', 'ComfyUI-TextureAlchemy', 'ComfyUI-seamless-tiling')
# 自研包在 custom_nodes 里的目录名，指向后端仓库的 comfy_nodes。
$PipelineOwnNodePackName = 'ComfyUI-DreamTexture'

function Get-PluginRoot {
	# Tools\PipelineBackend\ 的上两级。
	return (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
}

function Get-DefaultStateDir {
	# 宿主工程的 Saved\PipelineBackend\：<宿主>\Plugins\<插件>\ 往上两级若有 .uproject 就是宿主。
	# 不在宿主里（比如在主 checkout 里直接跑）就退到临时目录，不往仓库里写。
	$PluginRoot = Get-PluginRoot
	$PluginsDir = Split-Path -Parent $PluginRoot
	$HostDir = if ($PluginsDir) { Split-Path -Parent $PluginsDir } else { $null }
	if ($HostDir -and (Test-Path -LiteralPath $HostDir -PathType Container) -and
		@(Get-ChildItem -LiteralPath $HostDir -Filter '*.uproject' -File -ErrorAction SilentlyContinue).Count -gt 0) {
		return (Join-Path $HostDir 'Saved\PipelineBackend')
	}
	return (Join-Path ([System.IO.Path]::GetTempPath()) 'DreamTexturePipelineBackend')
}

function Get-ProcessImagePath([int] $ProcessId) {
	# 进程映像的完整路径；进程不在了返回 $null。用 CIM 而不是 Get-Process.Path，
	# 后者对没有权限读的进程会静默给空。
	$Info = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $ProcessId" -ErrorAction SilentlyContinue
	if ($null -eq $Info) { return $null }
	return [string]$Info.ExecutablePath
}

function Read-PipelinePidFile([string] $PidFile) {
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

function Get-RegisteredProcess($PidData, [string] $ExeName) {
	# pid 文件里记的那个进程**还活着，并且确实是同一个进程**才返回它。
	# 只核对 pid 不够：pid 会被系统复用，上次崩溃留下的 pid 文件可能指着一个无关的进程，
	# 而 Stop 是要杀进程的。所以映像名和启动时间都要对上。
	if ($null -eq $PidData) { return $null }
	$Proc = Get-Process -Id ([int]$PidData.pid) -ErrorAction SilentlyContinue
	if ($null -eq $Proc) { return $null }
	$Image = Get-ProcessImagePath ([int]$PidData.pid)
	if ([string]::IsNullOrEmpty($Image) -or
		-not [string]::Equals([System.IO.Path]::GetFileName($Image), $ExeName, [System.StringComparison]::OrdinalIgnoreCase)) {
		return $null
	}
	if ($PidData.PSObject.Properties['startTicks'] -and $PidData.startTicks) {
		if ($Proc.StartTime.ToUniversalTime().Ticks -ne [long]$PidData.startTicks) { return $null }
	}
	return $Proc
}

function Get-DescendantProcessIds([int] $RootPid) {
	# 一个进程的全部后代 pid（孙进程在前）。ComfyUI 偶尔会拉子进程，只杀主进程会留下孤儿占着显存。
	$All = @(Get-CimInstance -ClassName Win32_Process -ErrorAction SilentlyContinue)
	$Result = [System.Collections.Generic.List[int]]::new()
	$Queue = [System.Collections.Generic.Queue[int]]::new()
	$Queue.Enqueue($RootPid)
	while ($Queue.Count -gt 0) {
		$Parent = $Queue.Dequeue()
		foreach ($Child in ($All | Where-Object { [int]$_.ParentProcessId -eq $Parent -and [int]$_.ProcessId -ne $Parent })) {
			$Result.Insert(0, [int]$Child.ProcessId)
			$Queue.Enqueue([int]$Child.ProcessId)
		}
	}
	return $Result
}

function Stop-RegisteredProcess([string] $PidFile, [string] $ExeName, [string] $Label) {
	# 停 pid 文件里登记的那个进程（连同后代），确认真的退出再返回。
	# 没登记、或登记的已不是原来那个进程 -> 只清 pid 文件，不杀任何东西。返回 $false 表示杀不掉。
	$PidData = Read-PipelinePidFile $PidFile
	if ($null -eq $PidData) {
		Write-Host "$Label 没有在运行（没有 pid 文件）。"
		return $true
	}
	$Proc = Get-RegisteredProcess $PidData $ExeName
	if ($null -eq $Proc) {
		Write-Host "$Label：pid 文件里的进程 $($PidData.pid) 已经不在，或已不是那个 $ExeName（pid 被复用了）；只清掉过期的 pid 文件，不杀任何进程。"
	}
	else {
		Write-Host "停止 ${Label}：pid $($Proc.Id)"
		foreach ($Child in (Get-DescendantProcessIds $Proc.Id)) {
			Stop-Process -Id $Child -Force -ErrorAction SilentlyContinue
		}
		Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
		if (-not $Proc.WaitForExit(10000)) {
			[Console]::Error.WriteLine("$Label 的进程 $($Proc.Id) 10 秒内没有退出。")
			return $false
		}
	}
	# ComfyUI 是经 cmd.exe 起的（为了把日志重定向进文件又不让它继承调用者的管道），python 一死 cmd 会自己退出；
	# 万一没退，登记过就核对后收掉。
	if ($PidData.PSObject.Properties['cmdPid'] -and $PidData.cmdPid) {
		$Wrapper = Get-Process -Id ([int]$PidData.cmdPid) -ErrorAction SilentlyContinue
		if ($null -ne $Wrapper -and $Wrapper.ProcessName -ieq 'cmd' -and
			$PidData.PSObject.Properties['cmdStartTicks'] -and $PidData.cmdStartTicks -and
			$Wrapper.StartTime.ToUniversalTime().Ticks -eq [long]$PidData.cmdStartTicks) {
			Stop-Process -Id $Wrapper.Id -Force -ErrorAction SilentlyContinue
		}
	}
	Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
	return $true
}

function Test-HttpOk([string] $Url, [int] $TimeoutSec = 2) {
	try {
		return (Invoke-WebRequest -Uri $Url -TimeoutSec $TimeoutSec -UseBasicParsing -ErrorAction Stop).StatusCode -eq 200
	}
	catch {
		return $false
	}
}

function Test-BackendHealth([int] $Port, [int] $TimeoutSec = 2) {
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

function Test-BackendSeesComfy([int] $Port, [int] $TimeoutSec = 2) {
	# 后端自己看到的 ComfyUI 状态（它有自己的健康巡检，比 ComfyUI 起来晚一拍）。
	try {
		$Response = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/api/comfy/status" -TimeoutSec $TimeoutSec -UseBasicParsing -ErrorAction Stop
		$Status = $Response.Content | ConvertFrom-Json
		return ($Status.alive -eq $true -and $Status.ready -eq $true)
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

function Get-FreeVramMiB {
	# 第一块显卡的空闲显存（MiB）；读不出来返回 $null。
	$Smi = Get-Command 'nvidia-smi' -ErrorAction SilentlyContinue
	$SmiPath = if ($Smi) { $Smi.Source } else { Join-Path $env:SystemRoot 'System32\nvidia-smi.exe' }
	if (-not (Test-Path -LiteralPath $SmiPath -PathType Leaf)) { return $null }
	try {
		$Lines = @(& $SmiPath '--query-gpu=memory.free' '--format=csv,noheader,nounits' 2>$null)
		if ($LASTEXITCODE -ne 0 -or $Lines.Count -eq 0) { return $null }
		$Value = 0
		if ([int]::TryParse(([string]$Lines[0]).Trim(), [ref] $Value)) { return $Value }
		return $null
	}
	catch {
		return $null
	}
}

function Remove-OwnedJunctions([string] $CustomNodesDir) {
	# 拆掉状态目录里的联接。三条保险：
	#   - 这个目录是我们自己建的、只有我们往里放东西，所以里面的联接都算自己的；
	#   - 只拆**类型确实是联接**的条目，真目录一律不动（宁可留下并报出来，也不删用户的东西）；
	#   - 用 Directory.Delete 的非递归形式：对联接，它删的是联接本身。别用 Remove-Item -Recurse，
	#     那是对「目标」的递归删除，一旦哪个版本的行为不一样就会删穿到目标里去。
	if (-not (Test-Path -LiteralPath $CustomNodesDir -PathType Container)) { return 0 }
	$Removed = 0
	foreach ($Entry in @(Get-ChildItem -LiteralPath $CustomNodesDir -Force -ErrorAction SilentlyContinue)) {
		if ($Entry.LinkType -eq 'Junction') {
			[System.IO.Directory]::Delete($Entry.FullName)
			$Removed++
		}
		else {
			Write-Warning "$($Entry.FullName) 不是联接，没有动它。"
		}
	}
	return $Removed
}

function New-OwnedJunction([string] $Link, [string] $Target) {
	# 建联接 Link -> Target。已经是指向同一目标的联接就复用；指向别处的联接先拆（只拆联接本身）再建；
	# 真目录或文件挡路 -> 抛错，不删。
	if (-not (Test-Path -LiteralPath $Target -PathType Container)) {
		throw "联接的目标不存在：$Target"
	}
	$Existing = Get-Item -LiteralPath $Link -Force -ErrorAction SilentlyContinue
	if ($null -ne $Existing) {
		if ($Existing.LinkType -ne 'Junction') {
			throw "$Link 已存在，而且不是联接（是真目录或文件）；不覆盖，请手动处理。"
		}
		$Current = @($Existing.Target)[0]
		if ($Current -and [string]::Equals([System.IO.Path]::GetFullPath($Current).TrimEnd('\'), [System.IO.Path]::GetFullPath($Target).TrimEnd('\'), [System.StringComparison]::OrdinalIgnoreCase)) {
			return
		}
		[System.IO.Directory]::Delete($Existing.FullName)
	}
	New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null
}
