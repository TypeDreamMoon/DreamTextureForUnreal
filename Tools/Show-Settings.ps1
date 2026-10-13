#Requires -Version 7.0
<#
.SYNOPSIS
    打印 Tools\ 下脚本会用到的本机路径，以及每一个是从哪一层解析出来的。只读，不启动任何东西。

.DESCRIPTION
    解析顺序见 Tools\Common.ps1：脚本参数 -> 环境变量 -> Tools\LocalSettings.psd1 -> 自动 -> 失败。
    这里没有脚本参数这一层（要看某次运行的实际值，直接看那个脚本开头打印的行）。
    找不到的值标 (未设置) 并给出该设什么。退出码恒为 0：有些值本来就是可选的（比如没有 -Pipeline 就用不着 ComfyUI）。

.EXAMPLE
    pwsh -NoProfile -File Tools\Show-Settings.ps1
#>
[CmdletBinding()]
param()

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

function Show-Resolved([string] $Name, $Result) {
	if ($null -eq $Result.Value) {
		Write-Host ("{0,-22} (未设置)  {1}" -f $Name, $Result.Hint) -ForegroundColor Yellow
	}
	else {
		$Exists = Test-Path -LiteralPath $Result.Value
		Write-Host ("{0,-22} {1}" -f $Name, $Result.Value) -ForegroundColor $(if ($Exists) { 'Green' } else { 'Red' })
		Write-Host ("{0,-22} 来源：{1}{2}" -f '', $Result.Source, $(if ($Exists) { '' } else { '   <-- 路径不存在' })) -ForegroundColor DarkGray
	}
}

Write-Host "设置文件：$(Join-Path $PSScriptRoot 'LocalSettings.psd1')  $(if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'LocalSettings.psd1')) { '(存在)' } else { '(没有，全靠环境变量和自动查找)' })"
Write-Host ''

$Project = Resolve-DreamTextureProject
Show-Resolved '宿主工程' $Project
Show-Resolved '引擎目录' (Resolve-DreamTextureEngineRoot -ProjectPath ([string]$Project.Value))
Show-Resolved '固定数据后端 exe' (Resolve-DreamTextureBackendExe -Kind Fixture)
Show-Resolved '流水线后端 exe' (Resolve-DreamTextureBackendExe -Kind Pipeline)
Show-Resolved 'ComfyUI 根目录' (Resolve-DreamTextureComfyRoot)
Show-Resolved '宿主目录（创建用）' (Resolve-DreamTextureHostRoot)
Show-Resolved '插件仓库（创建用）' (Resolve-DreamTexturePluginRepo)
