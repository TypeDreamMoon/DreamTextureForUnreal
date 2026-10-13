# Tools\ 下各脚本共用的「东西在哪」解析。用 . 点号引入，不单独运行。
#
# 脚本不假设任何东西装在哪。每一个值按下面的顺序找，先找到的算数：
#
#   1. 脚本自己的参数（-Project、-EngineRoot、-BackendExe、-ComfyRoot ……）；
#   2. 环境变量（DREAMTEXTURE_ENGINE_ROOT、DREAMTEXTURE_BACKEND_EXE、DREAMTEXTURE_COMFY_ROOT）；
#   3. Tools\LocalSettings.psd1：本机私有、不进仓库的设置文件（模板是 Tools\LocalSettings.example.psd1，每个键都有说明）；
#   4. 能自动推出来的就自动推：
#        宿主工程  插件自己的位置往上两级的 *.uproject（<宿主>\Plugins\DreamTextureForUnreal\）；
#        引擎目录  宿主 .uproject 的 EngineAssociation —— Launcher 版查 HKLM:\SOFTWARE\EpicGames\Unreal Engine\<版本>
#                  的 InstalledDirectory；源码构建（GUID）查 HKCU:\Software\Epic Games\Unreal Engine\Builds；
#                  再退到 LauncherInstalled.dat；
#   5. 都没有就失败，并说清楚该设哪个参数、环境变量、设置键。
#
# 每个 Resolve-* 函数返回 [pscustomobject]@{ Value; Source; Hint }：
#   Value   找到的值，没找到是 $null；
#   Source  它是从哪一层来的（打印出来，排查「为什么用了这个路径」时看）；
#   Hint    没找到时给用户看的整段话（找到时也有，是同一句，可以不用）。

$script:DreamTextureToolsDir = $PSScriptRoot
$script:DreamTexturePluginRoot = Split-Path -Parent $PSScriptRoot
$script:DreamTextureSettingsFile = Join-Path $PSScriptRoot 'LocalSettings.psd1'
$script:DreamTextureSettingsCache = $null

# LocalSettings.psd1 里认识的键。多出来的键多半是拼错了，会警告。
$script:DreamTextureKnownSettingKeys = @(
	'EngineRoot', 'HostProject', 'HostRoot', 'PluginRepo',
	'BackendExe', 'PipelineBackendExe', 'ComfyRoot'
)

function Get-DreamTexturePluginRoot {
	return $script:DreamTexturePluginRoot
}

function Get-DreamTextureLocalSettings {
	# 读 Tools\LocalSettings.psd1。没有这个文件是正常的，返回空表；文件坏了就直接失败，别悄悄忽略。
	if ($null -ne $script:DreamTextureSettingsCache) { return $script:DreamTextureSettingsCache }
	$Settings = @{}
	if (Test-Path -LiteralPath $script:DreamTextureSettingsFile -PathType Leaf) {
		try {
			$Loaded = Import-PowerShellDataFile -LiteralPath $script:DreamTextureSettingsFile -ErrorAction Stop
		}
		catch {
			throw "读不了 $($script:DreamTextureSettingsFile)：$($_.Exception.Message)（语法见 Tools\LocalSettings.example.psd1）"
		}
		foreach ($Key in $Loaded.Keys) {
			if ($script:DreamTextureKnownSettingKeys -cnotcontains [string]$Key) {
				Write-Warning "LocalSettings.psd1 里有不认识的键 '$Key'（认识的：$($script:DreamTextureKnownSettingKeys -join ', ')），多半是拼错了。"
				continue
			}
			$Value = $Loaded[$Key]
			if ($Value -is [string] -and -not [string]::IsNullOrWhiteSpace($Value)) { $Settings[[string]$Key] = $Value.Trim() }
		}
	}
	$script:DreamTextureSettingsCache = $Settings
	return $Settings
}

function Resolve-ToolSetting {
	# 通用的「参数 -> 环境变量 -> 设置文件 -> 自动」。
	#   -What         给人看的名字，如「后端可执行文件」；
	#   -Explicit     脚本参数的值（空串算没给）；
	#   -ParamName    参数的名字，如 -BackendExe（只用于提示）；
	#   -EnvVar       环境变量名，可不给；
	#   -SettingKeys  LocalSettings.psd1 里的键，按顺序找，可多个；
	#   -Auto         可选的脚本块，返回 @{ Value = ...; Source = ... } 或 $null。
	param(
		[Parameter(Mandatory)] [string] $What,
		[string] $Explicit = '',
		[string] $ParamName = '',
		[string] $EnvVar = '',
		[string[]] $SettingKeys = @(),
		[scriptblock] $Auto = $null
	)

	$Ways = [System.Collections.Generic.List[string]]::new()
	if ($ParamName) { $Ways.Add("参数 $ParamName") }
	if ($EnvVar) { $Ways.Add("环境变量 $EnvVar") }
	if ($SettingKeys.Count -gt 0) { $Ways.Add("Tools\LocalSettings.psd1 里的 $($SettingKeys -join ' / ')（模板见 Tools\LocalSettings.example.psd1）") }
	$Hint = "没有指定 $What。任选其一：" + ($Ways -join '；') + '。'

	if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
		return [pscustomobject]@{ Value = $Explicit.Trim(); Source = "参数 $ParamName"; Hint = $Hint }
	}
	if ($EnvVar) {
		$FromEnv = [Environment]::GetEnvironmentVariable($EnvVar)
		if (-not [string]::IsNullOrWhiteSpace($FromEnv)) {
			return [pscustomobject]@{ Value = $FromEnv.Trim(); Source = "环境变量 $EnvVar"; Hint = $Hint }
		}
	}
	$Settings = Get-DreamTextureLocalSettings
	foreach ($Key in $SettingKeys) {
		if ($Settings.ContainsKey($Key)) {
			return [pscustomobject]@{ Value = [string]$Settings[$Key]; Source = "LocalSettings.psd1 的 $Key"; Hint = $Hint }
		}
	}
	if ($null -ne $Auto) {
		$Found = & $Auto
		if ($null -ne $Found -and -not [string]::IsNullOrWhiteSpace([string]$Found.Value)) {
			return [pscustomobject]@{ Value = [string]$Found.Value; Source = [string]$Found.Source; Hint = $Hint }
		}
	}
	return [pscustomobject]@{ Value = $null; Source = $null; Hint = $Hint }
}

# ------------------------------------------------------------------------------------------ 宿主工程与引擎

function Find-DreamTextureHostProject {
	# 插件在 <宿主>\Plugins\DreamTextureForUnreal\ 里时，宿主的 .uproject 在往上两级。恰好一个才算数。
	$HostDir = Split-Path -Parent (Split-Path -Parent $script:DreamTexturePluginRoot)
	if ([string]::IsNullOrEmpty($HostDir) -or -not (Test-Path -LiteralPath $HostDir -PathType Container)) { return $null }
	$Found = @(Get-ChildItem -LiteralPath $HostDir -Filter '*.uproject' -File -ErrorAction SilentlyContinue)
	if ($Found.Count -ne 1) { return $null }
	return $Found[0].FullName
}

function Resolve-EngineRootFromAssociation([string] $Association) {
	# 返回 @{ Value; Source } 或 $null。
	if ([string]::IsNullOrWhiteSpace($Association)) { return $null }
	$Association = $Association.Trim()

	# Launcher 版：关联是版本号（"5.8"），安装目录登记在 HKLM 的 InstalledDirectory 下。
	if ($Association -notmatch '^\{') {
		try {
			$Key = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association"
			$Dir = (Get-ItemProperty -LiteralPath $Key -Name 'InstalledDirectory' -ErrorAction Stop).InstalledDirectory
			if (-not [string]::IsNullOrWhiteSpace($Dir)) { return @{ Value = [string]$Dir; Source = "自动：注册表 $Key" } }
		}
		catch { }
	}

	# 源码构建：关联是 GUID，登记在 HKCU 的 Builds 下。
	try {
		$Key = 'HKCU:\Software\Epic Games\Unreal Engine\Builds'
		$Dir = (Get-ItemProperty -LiteralPath $Key -Name $Association -ErrorAction Stop).$Association
		if (-not [string]::IsNullOrWhiteSpace($Dir)) { return @{ Value = [string]$Dir; Source = "自动：注册表 $Key" } }
	}
	catch { }

	# 新版 Launcher 不一定写 HKLM，引擎自己是读 LauncherInstalled.dat（AppName 为 UE_5.8，去掉 UE_ 就是关联值）。
	$Dat = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
	if (Test-Path -LiteralPath $Dat -PathType Leaf) {
		try {
			foreach ($Entry in (Get-Content -LiteralPath $Dat -Raw | ConvertFrom-Json).InstallationList) {
				if ($Entry.AppName -match '^UE_(.+)$' -and $Matches[1] -ceq $Association -and $Entry.InstallLocation) {
					return @{ Value = [string]$Entry.InstallLocation; Source = "自动：$Dat" }
				}
			}
		}
		catch { }
	}
	return $null
}

function Resolve-DreamTextureProject {
	# 宿主 .uproject：-Project -> LocalSettings 的 HostProject -> 插件上两级的 *.uproject。
	param([string] $Explicit = '')
	Resolve-ToolSetting -What '宿主工程（.uproject）' -Explicit $Explicit -ParamName '-Project' `
		-SettingKeys @('HostProject') -Auto {
		$Found = Find-DreamTextureHostProject
		if ($Found) { @{ Value = $Found; Source = '自动：插件往上两级的 .uproject' } }
	}
}

function Resolve-DreamTextureEngineRoot {
	# 引擎目录：-EngineRoot -> DREAMTEXTURE_ENGINE_ROOT -> LocalSettings 的 EngineRoot -> 按宿主 .uproject 的关联查。
	param([string] $Explicit = '', [string] $ProjectPath = '')
	$Result = Resolve-ToolSetting -What '引擎目录（里面有 Engine\ 的那一层）' -Explicit $Explicit -ParamName '-EngineRoot' `
		-EnvVar 'DREAMTEXTURE_ENGINE_ROOT' -SettingKeys @('EngineRoot') -Auto {
		if ($ProjectPath -and (Test-Path -LiteralPath $ProjectPath -PathType Leaf)) {
			$Association = [string](Get-Content -LiteralPath $ProjectPath -Raw | ConvertFrom-Json).EngineAssociation
			Resolve-EngineRootFromAssociation $Association
		}
	}
	if ($null -eq $Result.Value) {
		$Assoc = ''
		if ($ProjectPath -and (Test-Path -LiteralPath $ProjectPath -PathType Leaf)) {
			$Assoc = [string](Get-Content -LiteralPath $ProjectPath -Raw | ConvertFrom-Json).EngineAssociation
		}
		$Extra = if ($Assoc) { "宿主工程的引擎关联是 '$Assoc'，注册表和 LauncherInstalled.dat 里都查不到它。" } else { '' }
		$Result.Hint = "$Extra$($Result.Hint)".Trim()
	}
	return $Result
}

# ------------------------------------------------------------------------------------------ 后端与 ComfyUI

function Resolve-DreamTextureBackendExe {
	# 后端可执行文件。-Kind Fixture（固定数据后端）：-BackendExe -> DREAMTEXTURE_BACKEND_EXE -> BackendExe。
	# -Kind Pipeline（流水线后端）：同上，但设置文件里先看 PipelineBackendExe（它通常是后端 worktree 里新编的那份），
	# 没有再退到 BackendExe。
	param([string] $Explicit = '', [ValidateSet('Fixture', 'Pipeline')] [string] $Kind = 'Fixture')
	$Keys = if ($Kind -eq 'Pipeline') { @('PipelineBackendExe', 'BackendExe') } else { @('BackendExe') }
	Resolve-ToolSetting -What 'dreamtexture.exe（后端可执行文件）' -Explicit $Explicit -ParamName '-BackendExe' `
		-EnvVar 'DREAMTEXTURE_BACKEND_EXE' -SettingKeys $Keys
}

function Resolve-DreamTextureComfyRoot {
	# ComfyUI 整合包根目录（里面有 python\ 和 ComfyUI\）。
	param([string] $Explicit = '')
	Resolve-ToolSetting -What 'ComfyUI 根目录（里面有 python\ 和 ComfyUI\）' -Explicit $Explicit -ParamName '-ComfyRoot' `
		-EnvVar 'DREAMTEXTURE_COMFY_ROOT' -SettingKeys @('ComfyRoot')
}

# ------------------------------------------------------------------------------------------ 宿主的创建脚本用

function Resolve-DreamTextureHostRoot {
	# 宿主工程目录：-Root -> LocalSettings 的 HostRoot -> HostProject 所在目录 -> 插件上两级的宿主。
	param([string] $Explicit = '')
	Resolve-ToolSetting -What '宿主工程目录' -Explicit $Explicit -ParamName '-Root' -SettingKeys @('HostRoot') -Auto {
		$Settings = Get-DreamTextureLocalSettings
		if ($Settings.ContainsKey('HostProject')) {
			@{ Value = (Split-Path -Parent $Settings['HostProject']); Source = 'LocalSettings.psd1 的 HostProject 所在目录' }
		}
		else {
			$Found = Find-DreamTextureHostProject
			if ($Found) { @{ Value = (Split-Path -Parent $Found); Source = '自动：插件往上两级的宿主' } }
		}
	}
}

function Resolve-DreamTexturePluginRepo {
	# 插件仓库的任意一个工作区：-RepoPath -> LocalSettings 的 PluginRepo -> 本脚本所在的仓库（git 顶层）。
	param([string] $Explicit = '')
	Resolve-ToolSetting -What '插件仓库的工作区' -Explicit $Explicit -ParamName '-RepoPath' -SettingKeys @('PluginRepo') -Auto {
		if ($null -ne (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) {
			$Top = (& git -C $script:DreamTexturePluginRoot rev-parse --show-toplevel 2>$null)
			if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($Top)) {
				@{ Value = ([string]$Top).Trim().Replace('/', '\'); Source = '自动：本脚本所在的 git 仓库' }
			}
		}
	}
}
