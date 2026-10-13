#Requires -Version 7.0
<#
.SYNOPSIS
    创建或核对 DreamTexture 测试宿主：一个极小的虚幻工程，它的 Plugins\DreamTextureForUnreal
    是本插件仓库的一个 git worktree。

.DESCRIPTION
    插件仓库的根就是插件根，自己没有 .uproject，编译和跑自动化测试都得借一个工程。
    借自己平时干活的工程来编译和测试有几个麻烦：它多半用着另一份引擎，而且你同时在里面开着编辑器；
    在那里编译会排同一把 Build.bat 互斥锁，还会去写对方正加载着的 DLL。
    宿主是一个独立工程：自己的 Binaries / Intermediate / Saved，用 Launcher 版引擎，
    只启用本插件，所以编译和测试都不碰你的工作工程。

    脚本幂等，反复运行只汇报状态：

      1. 先检查，什么都没改之前：git 在 PATH 里、宿主根不在 C 盘、仓库是 git 仓库、模板齐全、引擎存在。
      2. Plugins\DreamTextureForUnreal：已存在就必须是 -RepoPath 的 worktree（同一个 git 公共目录、
         自己的工作区，不是主工作区的链接），且检出的是 -Branch。不存在就用 "git worktree add" 建。
         **已存在的 worktree 绝不会被删除、重置、切换分支，有未提交改动的更是只读不碰**——
         别的会话可能正在里面写代码。
      3. 把 Template\ 拷进宿主根。缺的写入；内容一致的不动；不一致的只列出来保留，
         除非给了 -Force，那样会先备份成 <名字>.bak-<时间戳> 再替换。
      4. 写 <Root>\.dreamtexture-testhost.json（模板版本、仓库、分支、创建时间），内容有变才写。
      5. 打印构建与跑测试的命令。

    除 -Root 以外不写任何地方，唯一的例外是 "git worktree add" 自己记在仓库 .git\worktrees 里的登记。

.PARAMETER Root
    宿主工程目录。不许在 C 盘。没给就看 Tools\LocalSettings.psd1 的 HostRoot（再退到 HostProject 所在目录），
    再没有就取本插件往上两级的宿主；都没有则失败。

.PARAMETER RepoPath
    插件仓库的任意一个工作区；新 worktree 加到这个仓库里。没给就看 Tools\LocalSettings.psd1 的 PluginRepo，
    再没有就用本脚本所在的 git 仓库。

.PARAMETER Branch
    worktree 必须检出（以及建出来时用）的分支。

.PARAMETER EngineRoot
    引擎目录。没给就依次看环境变量 DREAMTEXTURE_ENGINE_ROOT、Tools\LocalSettings.psd1 的 EngineRoot，再没有就按模板 .uproject 里的
    EngineAssociation 查：Launcher 版（"5.8" 这样的版本号）查 HKLM\SOFTWARE\EpicGames\Unreal Engine\<版本> 的 InstalledDirectory，
    源码构建（GUID）查 HKCU\Software\Epic Games\Unreal Engine\Builds，最后退到 LauncherInstalled.dat
    （AppName 是 UE_5.8，去掉 UE_ 前缀就是关联值）。都查不到则失败。

.PARAMETER Force
    替换与模板不一致的文件，每个旧文件都会先备份。

.PARAMETER KeepCurrentHead
    接受已有 worktree 当前检出的任何东西（别的分支、游离的提交），而不是要求它在 -Branch 上。
    正在某个开发分支上干活的 worktree 就用这个。

.PARAMETER Detach
    用游离 HEAD 跟随 -Branch，而不是把分支检出。一个分支同一时间只能被一个 worktree 检出，
    -Branch 通常是主仓库正在用的 master，宿主就拿不到它；用 -Detach 则建 worktree 时
    "git worktree add --detach" 到分支末端，之后每次运行把**干净的**游离 worktree 移到分支当前末端
    （脏的只警告，不动）。这是脚本唯一会移动 worktree HEAD 的情形，且只对游离且干净的。

.PARAMETER AllowSystemDrive
    接受 C 盘上的宿主根。拒绝 C 盘是因为这台机器 C 盘几乎满了；只有 C 盘且有空间的机器可以用它
    （或设环境变量 DREAMTEXTURE_ALLOW_DRIVE_C=1）。

.EXAMPLE
    pwsh -NoProfile -File .\New-DreamTextureTestHost.ps1 -WhatIf
    只汇报，不写任何东西。

.EXAMPLE
    pwsh -NoProfile -File .\New-DreamTextureTestHost.ps1 -KeepCurrentHead
    宿主已经在某个开发分支上干活，接受它现在的样子，核对其余部分。

.EXAMPLE
    pwsh -NoProfile -File .\New-DreamTextureTestHost.ps1 -Detach
    全新建宿主：master 已在主仓库检出，宿主游离跟随它。

.NOTES
    退出码：
      0  宿主与模板一致，worktree 通过全部检查；
      1  有检查没过；失败原因已打印，失败那一步之后什么都没做；
      2  宿主能用，但与模板不一致：有文件不同且被保留（没给 -Force），或 -WhatIf 发现有东西要写。
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string] $Root = '',
    [string] $RepoPath = '',
    [string] $Branch = 'master',
    [string] $EngineRoot = '',
    [switch] $Force,
    [switch] $KeepCurrentHead,
    [switch] $Detach,
    [switch] $AllowSystemDrive
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
# 宿主目录、插件仓库、引擎目录在哪：参数 -> 环境变量 -> Tools\LocalSettings.psd1 -> 自动 -> 失败（解析逻辑在 Tools\Common.ps1）。
. (Join-Path $PSScriptRoot '..\Common.ps1')
# git 的退出码在每次调用后（Invoke-Git）手工检查，不能让它变成异常。
$PSNativeCommandUseErrorActionPreference = $false

# Template\ 下任何东西变了就加一，宿主因此能记下自己是按哪一版模板建的。
$TemplateVersion = 2
$ProjectFileName = 'DreamTextureTestHost.uproject'
$MetadataFileName = '.dreamtexture-testhost.json'
$TemplateEngineIniRelative = 'Config\DefaultEngine.ini'
# 低于这个值，首次构建（插件和工程的中间文件、日志）有写满的风险。
$MinimumFreeGigabytes = 15

# ----------------------------------------------------------------------------------------------- 工具函数

function Write-Step([string] $Message) {
    Write-Host ''
    Write-Host "== $Message"
}

function Write-Info([string] $Message) {
    Write-Host "   $Message"
}

function Stop-WithFailure([string[]] $Messages) {
    foreach ($Message in $Messages) {
        [Console]::Error.WriteLine("错误: $Message")
    }
    exit 1
}

function Resolve-UserPath([string] $Path) {
    # 相对路径按调用者所在的 PowerShell 位置展开，而不是进程当前目录（GetFullPath 用的是后者）。
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function ConvertTo-NormalPath([string] $Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }
    $Separator = [System.IO.Path]::DirectorySeparatorChar
    $Full = [System.IO.Path]::GetFullPath($Path.Trim().Replace('/', [string]$Separator))
    $Trimmed = $Full.TrimEnd($Separator)
    # 单独的 "C:" 是「C 盘的当前目录」，要保留根的分隔符。
    if ($Trimmed.Length -eq 2 -and $Trimmed[1] -eq ':') {
        return $Trimmed + $Separator
    }
    return $Trimmed
}

function Test-SamePath([string] $A, [string] $B) {
    return [string]::Equals((ConvertTo-NormalPath $A), (ConvertTo-NormalPath $B), [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-PathUnder([string] $Path, [string] $Directory) {
    $NormalPath = ConvertTo-NormalPath $Path
    $NormalDirectory = (ConvertTo-NormalPath $Directory).TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    return $NormalPath.StartsWith($NormalDirectory, [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-PhysicalPath([string] $Path) {
    # 顺着路径上每一个符号链接和目录联接走到底，这样拿到的盘符才是字节真正写到的那块盘。
    # 还不存在的部分原样接在后面：它们会建在已存在的父目录的真实位置下。
    $Separator = [System.IO.Path]::DirectorySeparatorChar
    $Full = ConvertTo-NormalPath $Path
    $PathRoot = [System.IO.Path]::GetPathRoot($Full)
    $Current = $PathRoot
    $Parts = @($Full.Substring($PathRoot.Length).Split($Separator, [System.StringSplitOptions]::RemoveEmptyEntries))
    for ($Index = 0; $Index -lt $Parts.Count; $Index++) {
        $Candidate = Join-Path $Current $Parts[$Index]
        $Item = Get-Item -LiteralPath $Candidate -Force -ErrorAction SilentlyContinue
        if ($null -eq $Item) {
            return ConvertTo-NormalPath (Join-Path $Current (($Parts[$Index..($Parts.Count - 1)]) -join $Separator))
        }
        $Hops = 0
        while ($null -ne $Item -and $Item.LinkType -and $Hops -lt 16) {
            $Target = @($Item.Target)[0]
            if ([string]::IsNullOrWhiteSpace($Target)) {
                break
            }
            if (-not [System.IO.Path]::IsPathRooted($Target)) {
                $Target = Join-Path (Split-Path -Parent $Candidate) $Target
            }
            $Candidate = ConvertTo-NormalPath $Target
            $Item = Get-Item -LiteralPath $Candidate -Force -ErrorAction SilentlyContinue
            $Hops++
        }
        $Current = $Candidate
    }
    return ConvertTo-NormalPath $Current
}

function Get-DriveName([string] $Path) {
    return ([System.IO.Path]::GetPathRoot((ConvertTo-NormalPath $Path))).TrimEnd([System.IO.Path]::DirectorySeparatorChar).ToUpperInvariant()
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)] [string] $WorkingDirectory,
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [switch] $AllowFailure
    )
    # stderr 并进结果，不让它变成终止性错误；输出一律按 UTF-8 解码，不管控制台是什么代码页。
    $PreviousPreference = $ErrorActionPreference
    $PreviousEncoding = $null
    try {
        $ErrorActionPreference = 'Continue'
        try {
            $PreviousEncoding = [Console]::OutputEncoding
            [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
        }
        catch {
            $PreviousEncoding = $null
        }
        $Output = & git -C $WorkingDirectory @Arguments 2>&1
        $ExitCode = $LASTEXITCODE
    }
    finally {
        if ($null -ne $PreviousEncoding) {
            try { [Console]::OutputEncoding = $PreviousEncoding } catch { }
        }
        $ErrorActionPreference = $PreviousPreference
    }
    # 什么都不打印的命令会让 $Output 是 null，而 @($null) 是一个 null 元素而不是空数组。
    $Text = (@($Output) | Where-Object { $null -ne $_ } | ForEach-Object { $_.ToString() }) -join "`n"
    if ($ExitCode -ne 0 -and -not $AllowFailure) {
        throw "git $($Arguments -join ' ')（在 $WorkingDirectory）失败，退出码 $ExitCode。`n$Text"
    }
    return [pscustomobject]@{ ExitCode = $ExitCode; Output = $Text.Trim() }
}

function Get-WorktreeEntries([string] $Repository) {
    # "git worktree list --porcelain"：每个 worktree 一块，块之间空一行。
    $Entries = [System.Collections.Generic.List[object]]::new()
    $Listing = (Invoke-Git -WorkingDirectory $Repository -Arguments @('worktree', 'list', '--porcelain')).Output
    foreach ($Block in ($Listing -split "`n\s*`n")) {
        $Entry = [ordered]@{ Path = ''; Branch = ''; Head = ''; Prunable = $false }
        foreach ($Line in ($Block -split "`n")) {
            $Line = $Line.Trim()
            if ($Line.StartsWith('worktree ')) { $Entry.Path = ConvertTo-NormalPath $Line.Substring(9) }
            elseif ($Line.StartsWith('branch ')) { $Entry.Branch = $Line.Substring(7) }
            elseif ($Line.StartsWith('HEAD ')) { $Entry.Head = $Line.Substring(5) }
            elseif ($Line.StartsWith('prunable')) { $Entry.Prunable = $true }
        }
        if ($Entry.Path) {
            $Entries.Add([pscustomobject]$Entry)
        }
    }
    return $Entries.ToArray()
}

function Test-IsBinaryTemplateFile([string] $Path) {
    # 引擎保存出来的内容按字节比：当文本解码的话，两个不同的包可能因为非法序列都变成同一个替换字符而读成一样。
    return @('.uasset', '.umap', '.uexp', '.ubulk') -contains [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
}

function ConvertTo-ComparableText([byte[]] $Bytes) {
    # 换行风格和 BOM 不算差异：git 检出模板可能是任一种换行，编辑器也可能加或去 BOM。
    $Text = [System.Text.Encoding]::UTF8.GetString($Bytes).TrimStart([char]0xFEFF)
    return $Text.Replace("`r`n", "`n")
}

function Get-FirstDifference([string] $Existing, [string] $Wanted) {
    $ExistingLines = $Existing -split "`n"
    $WantedLines = $Wanted -split "`n"
    $Count = [Math]::Max($ExistingLines.Count, $WantedLines.Count)
    for ($Index = 0; $Index -lt $Count; $Index++) {
        $Left = if ($Index -lt $ExistingLines.Count) { $ExistingLines[$Index] } else { '<文件结束>' }
        $Right = if ($Index -lt $WantedLines.Count) { $WantedLines[$Index] } else { '<文件结束>' }
        if ($Left -cne $Right) {
            $Shorten = { param([string] $S) if ($S.Length -gt 100) { $S.Substring(0, 100) + '...' } else { $S } }
            return "第 $($Index + 1) 行：磁盘上是 '$(& $Shorten $Left)'，模板是 '$(& $Shorten $Right)'"
        }
    }
    return '没有哪一行不同'
}

function Get-ZenDataDirectory {
    # 按 ZenServerInterface.cpp（DetermineDataPath）的顺序解析，去掉脚本看不到的命令行和编辑器自己的设置。
    # 只用来警告，从不用来配置。
    $Candidates = @(
        @{ Name = '环境变量 UE-ZenSubprocessDataPath'; Value = [Environment]::GetEnvironmentVariable('UE-ZenSubprocessDataPath') }
    )
    $RegistryValue = $null
    try {
        $RegistryValue = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Epic Games\Zen' -Name 'DataPath' -ErrorAction Stop).DataPath
    }
    catch {
        $RegistryValue = $null
    }
    $Candidates += @{ Name = '注册表 HKCU\Software\Epic Games\Zen DataPath'; Value = $RegistryValue }
    $Candidates += @{ Name = '环境变量 UE-ZenDataPath'; Value = [Environment]::GetEnvironmentVariable('UE-ZenDataPath') }
    $LocalCache = [Environment]::GetEnvironmentVariable('UE-LocalDataCachePath')
    if (-not [string]::IsNullOrWhiteSpace($LocalCache) -and $LocalCache -ne 'None') {
        $Candidates += @{ Name = '环境变量 UE-LocalDataCachePath（加 \Zen）'; Value = (Join-Path $LocalCache 'Zen') }
    }
    foreach ($Candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($Candidate.Value)) {
            return [pscustomobject]@{ Path = $Candidate.Value; Source = $Candidate.Name }
        }
    }
    $Default = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'UnrealEngine\Common\Zen\Data'
    return [pscustomobject]@{ Path = $Default; Source = '引擎默认值（[Zen.AutoLaunch] DataPath）' }
}

# ------------------------------------------------------------------------------------------ 预检

Write-Step '改动任何东西之前先检查'

$Failures = [System.Collections.Generic.List[string]]::new()
$PendingChanges = 0

$TemplateDirectory = Join-Path $PSScriptRoot 'Template'
$TemplateProject = Join-Path $TemplateDirectory $ProjectFileName
$TemplateEngineIni = Join-Path $TemplateDirectory $TemplateEngineIniRelative

$RootResolved = Resolve-DreamTextureHostRoot -Explicit $Root
if ($null -eq $RootResolved.Value) { Stop-WithFailure @($RootResolved.Hint) }
$RepoResolved = Resolve-DreamTexturePluginRepo -Explicit $RepoPath
if ($null -eq $RepoResolved.Value) { Stop-WithFailure @($RepoResolved.Hint, '（本脚本不在 git 仓库里，或 git 不在 PATH 里。）') }
$Root = ConvertTo-NormalPath (Resolve-UserPath $RootResolved.Value)
$RepoPath = ConvertTo-NormalPath (Resolve-UserPath $RepoResolved.Value)
$WorktreePath = Join-Path $Root 'Plugins\DreamTextureForUnreal'
Write-Info "宿主根：  $Root（$($RootResolved.Source)）"
Write-Info "插件仓库：$RepoPath（$($RepoResolved.Source)）"
Write-Info "分支：    $Branch$(if ($KeepCurrentHead) { '（已有 worktree 在别的分支上也接受）' })"

$GitAvailable = $null -ne (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)
if (-not $GitAvailable) {
    $Failures.Add('git 不在 PATH 里。')
}

if (-not (Test-Path -LiteralPath $TemplateProject -PathType Leaf) -or -not (Test-Path -LiteralPath $TemplateEngineIni -PathType Leaf)) {
    $Failures.Add("模板不全或不存在：需要 $TemplateProject 和 $TemplateEngineIni。")
}

# C 盘只剩几个 G，单是首次构建写的东西就比这多。
$RootDrive = Get-DriveName $Root
$RootPhysical = Resolve-PhysicalPath $Root
$RootPhysicalDrive = Get-DriveName $RootPhysical
if (-not $AllowSystemDrive -and $env:DREAMTEXTURE_ALLOW_DRIVE_C -eq '1') { $AllowSystemDrive = [switch]$true }
if (($RootDrive -eq 'C:' -or $RootPhysicalDrive -eq 'C:') -and -not $AllowSystemDrive) {
    $Failures.Add("宿主根不许在 C 盘（$Root$(if ($RootPhysical -ne $Root) { " -> $RootPhysical" })）。用 -Root 换一块盘，或 C 盘确实有空间时给 -AllowSystemDrive。")
}
if ((Test-SamePath $Root $RepoPath) -or (Test-PathUnder $Root $RepoPath)) {
    $Failures.Add("宿主根（$Root）不能是插件仓库或在它里面（$RepoPath）。")
}
# -Root 手滑写成了一个真实工程的话，模板文件会被建进那个工程里。
if (Test-Path -LiteralPath $Root -PathType Container) {
    $OtherProjects = @(Get-ChildItem -LiteralPath $Root -Filter '*.uproject' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne $ProjectFileName })
    if ($OtherProjects.Count -gt 0) {
        $Failures.Add("$Root 里已经有别的工程（$($OtherProjects[0].Name)）；宿主需要自己的目录。")
    }
}

$RepoCommonDirectory = ''
$RepoTopLevel = ''
if (-not (Test-Path -LiteralPath $RepoPath -PathType Container)) {
    $Failures.Add("插件仓库路径不存在：$RepoPath")
}
elseif ($GitAvailable) {
    $Common = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('rev-parse', '--path-format=absolute', '--git-common-dir') -AllowFailure
    $Top = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('rev-parse', '--show-toplevel') -AllowFailure
    if ($Common.ExitCode -ne 0 -or $Top.ExitCode -ne 0) {
        $Failures.Add("$RepoPath 不是 git 工作区：$($Common.Output)")
    }
    else {
        $RepoCommonDirectory = ConvertTo-NormalPath $Common.Output
        $RepoTopLevel = ConvertTo-NormalPath $Top.Output
        Write-Info "仓库 git 目录：$RepoCommonDirectory"
    }
}

$EngineAssociation = ''
if (Test-Path -LiteralPath $TemplateProject -PathType Leaf) {
    try {
        $EngineAssociation = [string](Get-Content -LiteralPath $TemplateProject -Raw | ConvertFrom-Json).EngineAssociation
    }
    catch {
        $Failures.Add("模板的 $ProjectFileName 不是合法 JSON：$($_.Exception.Message)")
    }
}
$EngineResolved = Resolve-DreamTextureEngineRoot -Explicit $EngineRoot -ProjectPath $TemplateProject
if ($null -eq $EngineResolved.Value) {
    $Failures.Add($EngineResolved.Hint)
    $EngineRoot = ''
}
else {
    $EngineRoot = ConvertTo-NormalPath (Resolve-UserPath $EngineResolved.Value)
    Write-Info "引擎（引擎关联 '$EngineAssociation'）：$EngineRoot（$($EngineResolved.Source)）"
}
$BuildBatch = if ($EngineRoot) { Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat' } else { '' }
$EditorCommand = if ($EngineRoot) { Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe' } else { '' }
if ($EngineRoot -eq '') {
    # 上面已经记了失败：没有解析出引擎目录。
}
elseif (-not (Test-Path -LiteralPath $BuildBatch -PathType Leaf)) {
    $Failures.Add("$EngineRoot 下没有引擎（期望有 $BuildBatch）。用 -EngineRoot 指定。")
}
elseif (-not (Test-Path -LiteralPath $EditorCommand -PathType Leaf)) {
    Write-Warning "$EngineRoot 的引擎还没有 $EditorCommand；宿主能建出来，但引擎编好之前什么都跑不了。"
}

if ($Failures.Count -gt 0) {
    Stop-WithFailure $Failures.ToArray()
}

# 下面两条只是警告：都不归脚本管。
try {
    $Drive = [System.IO.DriveInfo]::new((Get-DriveName $RootPhysical))
    $FreeGigabytes = [Math]::Round($Drive.AvailableFreeSpace / 1GB, 1)
    Write-Info "$($Drive.Name) 剩余空间：$FreeGigabytes GB"
    if ($FreeGigabytes -lt $MinimumFreeGigabytes) {
        Write-Warning "$($Drive.Name) 只剩 $FreeGigabytes GB；宿主首次构建需要好几个 G。"
    }
}
catch {
    Write-Info "读不到 $RootPhysicalDrive 的剩余空间。"
}

# 派生数据缓存：引擎的本地 Zen 库是全机共享的，宿主故意不配置它（见 Template\Config\DefaultEngine.ini）。
# 值得知道的是它的字节真正落在哪：默认目录在 C 盘用户目录下，只有目录联接能把它挪开。
$Zen = Get-ZenDataDirectory
$ZenPhysical = Resolve-PhysicalPath $Zen.Path
Write-Info "派生数据缓存（本地 Zen 库）：$($Zen.Path)$(if (-not (Test-SamePath $ZenPhysical $Zen.Path)) { " -> $ZenPhysical" })，来自 $($Zen.Source)"
if ((Get-DriveName $ZenPhysical) -eq 'C:') {
    Write-Warning "共享的派生数据缓存实际在 C 盘（$ZenPhysical）。宿主不改它：这是这份引擎上所有工程共用的缓存。C 盘快满了就用目录联接或环境变量 UE-ZenDataPath 把它挪走。"
}

# ------------------------------------------------------------------------------------------ worktree

Write-Step 'Plugins\DreamTextureForUnreal 作为仓库的 worktree'

$WorktreeEntries = @(Get-WorktreeEntries $RepoPath)
$ResolvedBranch = $Branch

if (Test-Path -LiteralPath $WorktreePath) {
    $Common = Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--path-format=absolute', '--git-common-dir') -AllowFailure
    if ($Common.ExitCode -ne 0) {
        Stop-WithFailure @("$WorktreePath 存在但不是 git 工作区。脚本不动它；要让脚本重建 worktree，请先把它挪走。")
    }
    $Top = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--show-toplevel')).Output
    if (-not (Test-SamePath $Top $WorktreePath)) {
        Stop-WithFailure @("$WorktreePath 只是工作区 $Top 里的一个文件夹，不是自己的 worktree。")
    }
    if (-not (Test-SamePath $Common.Output $RepoCommonDirectory)) {
        Stop-WithFailure @("$WorktreePath 属于另一个仓库（git 目录 $($Common.Output)，期望 $RepoCommonDirectory）。")
    }
    # 指向仓库自己工作区的链接能过上面两条检查，却会让宿主的每次构建都落进宿主本来要躲开的那个工作区。
    if (Test-SamePath (Resolve-PhysicalPath $WorktreePath) (Resolve-PhysicalPath $RepoTopLevel)) {
        Stop-WithFailure @("$WorktreePath 是仓库自己的工作区（$RepoTopLevel）经链接过来的，不是独立的 worktree。")
    }
    $Listed = @($WorktreeEntries | Where-Object { Test-SamePath $_.Path $WorktreePath })
    if ($Listed.Count -eq 0) {
        Stop-WithFailure @("$WorktreePath 不在仓库的 worktree 列表里（git -C `"$RepoPath`" worktree list）。")
    }

    $SymbolicHead = Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('symbolic-ref', '--quiet', '--short', 'HEAD') -AllowFailure
    $CurrentBranch = if ($SymbolicHead.ExitCode -eq 0) { $SymbolicHead.Output } else { '' }
    $ShortHead = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--short', 'HEAD')).Output
    $Described = if ($CurrentBranch) { "分支 $CurrentBranch（$ShortHead）" } else { "游离 HEAD（$ShortHead）" }
    # -Detach：worktree 用游离 HEAD 跟随分支。干净的会被移到分支当前末端；
    # 脏的不动，因为移动可能丢掉别人的工作。
    if ($Detach -and -not $CurrentBranch) {
        $Tip = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('rev-parse', '--verify', '--quiet', "refs/heads/$Branch") -AllowFailure
        $HeadCommit = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', 'HEAD')).Output
        $DirtyNow = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('status', '--porcelain')).Output
        if ($Tip.ExitCode -ne 0) {
            Stop-WithFailure @("-Detach：仓库里没有分支 $Branch。")
        }
        elseif ($Tip.Output -ne $HeadCommit) {
            if ($DirtyNow) {
                Write-Warning "$WorktreePath 游离在 $ShortHead 且有未提交改动；没有移到 $Branch 的末端。"
            }
            elseif ($PSCmdlet.ShouldProcess($WorktreePath, "git checkout --detach $Branch")) {
                $Moved = Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('checkout', '--detach', $Branch) -AllowFailure
                if ($Moved.ExitCode -ne 0) {
                    Stop-WithFailure @("git checkout --detach $Branch 在 ${WorktreePath} 里失败：", $Moved.Output)
                }
                $ShortHead = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--short', 'HEAD')).Output
                $Described = "游离 HEAD（$ShortHead）"
                Write-Info "已把游离的 worktree 移到 $Branch 的末端（$ShortHead）。"
            }
        }
        else {
            Write-Info "游离的 worktree 就在 $Branch 的末端（$ShortHead）。"
        }
    }
    elseif ($CurrentBranch -cne $Branch) {
        # 分支名对 git 区分大小写，哪怕文件系统不区分。
        if (-not $KeepCurrentHead) {
            Stop-WithFailure @(
                "$WorktreePath 现在是 $Described，不是分支 $Branch。",
                '本脚本从不切换、重置或删除 worktree。请自己在那里检出分支（git -C "<worktree>" switch <分支>），或用 -Branch 指定它现有的分支，或用 -KeepCurrentHead 原样接受。'
            )
        }
        Write-Warning "$WorktreePath 现在是 $Described，不是分支 $Branch；按 -KeepCurrentHead 原样保留。"
    }
    $ResolvedBranch = if ($CurrentBranch) { $CurrentBranch } elseif ($Detach) { "$Branch (detached)" } else { "(detached at $ShortHead)" }
    Write-Info "已有 worktree，$Described。"

    # 只读：宿主按 worktree 现在的样子构建，包括别人还没提交的改动。
    $Dirty = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('status', '--porcelain')).Output
    if ($Dirty) {
        $DirtyCount = @($Dirty -split "`n").Count
        Write-Info "里面有 $DirtyCount 处未提交改动；宿主构建的就是 worktree 此刻的样子，未提交的也算。脚本不碰它们。"
    }
}
else {
    $Missing = @($WorktreeEntries | Where-Object { Test-SamePath $_.Path $WorktreePath })
    if ($Missing.Count -gt 0) {
        Stop-WithFailure @(
            "$WorktreePath 不存在，但仍登记为仓库的 worktree。",
            "请自己清掉这条过期登记（git -C `"$RepoPath`" worktree prune）再运行。"
        )
    }
    $Holder = @($WorktreeEntries | Where-Object { $_.Branch -ceq "refs/heads/$Branch" })
    if ($Holder.Count -gt 0 -and -not $Detach) {
        Stop-WithFailure @(
            "分支 $Branch 已经在 $($Holder[0].Path) 检出$(if ($Holder[0].Prunable) { '（那个 worktree 的文件夹已经没了）' })。",
            '一个分支同一时间只能被一个 worktree 检出。给别的 -Branch，或加 -Detach 让宿主游离跟随它，或先释放那一个。'
        )
    }
    $LocalBranch = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('show-ref', '--verify', '--quiet', "refs/heads/$Branch") -AllowFailure
    if ($LocalBranch.ExitCode -ne 0) {
        $RemoteRefs = (Invoke-Git -WorkingDirectory $RepoPath -Arguments @('for-each-ref', '--format=%(refname)', 'refs/remotes')).Output -split "`n"
        $Tracking = @($RemoteRefs | Where-Object { $_ -match "^refs/remotes/[^/]+/$([regex]::Escape($Branch))$" })
        if ($Tracking.Count -ne 1) {
            Stop-WithFailure @("分支 $Branch 既不在本地，也不恰好在一个远端上（远端有 $($Tracking.Count) 份）。")
        }
        Write-Info "分支 $Branch 只有 $($Tracking[0])；git 会建一个跟踪它的本地分支。"
    }

    $PendingChanges++
    $AddArguments = if ($Detach) { @('worktree', 'add', '--detach', $WorktreePath, $Branch) } else { @('worktree', 'add', $WorktreePath, $Branch) }
    if ($PSCmdlet.ShouldProcess($WorktreePath, "git $($AddArguments -join ' ')")) {
        $PluginsDirectory = Split-Path -Parent $WorktreePath
        if (-not (Test-Path -LiteralPath $PluginsDirectory)) {
            New-Item -ItemType Directory -Path $PluginsDirectory -Force | Out-Null
        }
        $Added = Invoke-Git -WorkingDirectory $RepoPath -Arguments $AddArguments -AllowFailure
        if ($Added.ExitCode -ne 0) {
            Stop-WithFailure @('git worktree add 失败：', $Added.Output)
        }
        $PendingChanges--
        $ShortHead = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--short', 'HEAD')).Output
        if ($Detach) {
            $ResolvedBranch = "$Branch (detached)"
            Write-Info "已建 worktree，游离在 $Branch 的末端（$ShortHead）。"
        }
        else {
            Write-Info "已建 worktree，分支 $Branch（$ShortHead）。"
        }
    }
}

# ------------------------------------------------------------------------------------------ 模板

Write-Step '来自模板的工程文件'

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Created = [System.Collections.Generic.List[string]]::new()
$Unchanged = [System.Collections.Generic.List[string]]::new()
$Replaced = [System.Collections.Generic.List[string]]::new()
$Kept = [System.Collections.Generic.List[string]]::new()

$TemplateFiles = @(Get-ChildItem -LiteralPath $TemplateDirectory -Recurse -File | Sort-Object FullName)
foreach ($TemplateFile in $TemplateFiles) {
    $Relative = [System.IO.Path]::GetRelativePath($TemplateDirectory, $TemplateFile.FullName)
    $Destination = Join-Path $Root $Relative
    $Wanted = [System.IO.File]::ReadAllBytes($TemplateFile.FullName)

    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        $PendingChanges++
        if ($PSCmdlet.ShouldProcess($Destination, '按模板创建')) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
            [System.IO.File]::WriteAllBytes($Destination, $Wanted)
            $PendingChanges--
        }
        $Created.Add($Relative)
        continue
    }

    $ExistingBytes = [System.IO.File]::ReadAllBytes($Destination)
    if (Test-IsBinaryTemplateFile $Relative) {
        if ([System.Linq.Enumerable]::SequenceEqual($ExistingBytes, $Wanted)) {
            $Unchanged.Add($Relative)
            continue
        }
        $Difference = "字节不同（磁盘上 $($ExistingBytes.Length)，模板里 $($Wanted.Length)）"
    }
    else {
        $ExistingText = ConvertTo-ComparableText $ExistingBytes
        $WantedText = ConvertTo-ComparableText $Wanted
        if ($ExistingText -ceq $WantedText) {
            $Unchanged.Add($Relative)
            continue
        }
        $Difference = Get-FirstDifference $ExistingText $WantedText
    }
    if ($Force) {
        $PendingChanges++
        if ($PSCmdlet.ShouldProcess($Destination, "替换（首个差异在 $Difference）；旧文件保留为 $([System.IO.Path]::GetFileName($Destination)).bak-$Stamp")) {
            Copy-Item -LiteralPath $Destination -Destination "$Destination.bak-$Stamp" -Force
            [System.IO.File]::WriteAllBytes($Destination, $Wanted)
            $PendingChanges--
        }
        $Replaced.Add("$Relative ($Difference)")
    }
    else {
        $PendingChanges++
        $Kept.Add("$Relative ($Difference)")
    }
}

$Verb = if ($WhatIfPreference) { '将' } else { '已' }
foreach ($Name in $Created) { Write-Info "${Verb}创建：$Name" }
foreach ($Name in $Replaced) { Write-Info "${Verb}替换：$Name" }
foreach ($Name in $Unchanged) { Write-Info "一致：  $Name" }
foreach ($Name in $Kept) { Write-Info "不一致，原样保留：$Name" }
if ($Kept.Count -gt 0) {
    Write-Warning "$($Kept.Count) 个文件与模板不一致，没动。加 -Force 重新运行可替换（每个旧文件会先备份在原地）。"
}

# ------------------------------------------------------------------------------------------ 元数据

Write-Step "宿主元数据（$MetadataFileName）"

$MetadataPath = Join-Path $Root $MetadataFileName
$Now = (Get-Date).ToString('o')
$Metadata = [ordered]@{
    schema            = 1
    templateVersion   = $TemplateVersion
    repoPath          = $RepoPath
    gitCommonDir      = $RepoCommonDirectory
    worktreePath      = $WorktreePath
    branch            = $ResolvedBranch
    engineAssociation = $EngineAssociation
    engineRoot        = $EngineRoot
    createdBy         = 'Plugins/DreamTextureForUnreal/Tools/TestHost/New-DreamTextureTestHost.ps1'
    createdAt         = $Now
    updatedAt         = $Now
}
$StableKeys = @('schema', 'templateVersion', 'repoPath', 'gitCommonDir', 'worktreePath', 'branch', 'engineAssociation', 'engineRoot', 'createdBy')
$NeedsWrite = $true
if (Test-Path -LiteralPath $MetadataPath -PathType Leaf) {
    try {
        $Previous = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
        $Same = $true
        foreach ($Key in $StableKeys) {
            $Property = $Previous.PSObject.Properties[$Key]
            if ($null -eq $Property -or [string]$Property.Value -cne [string]$Metadata[$Key]) {
                $Same = $false
            }
        }
        $PreviousCreated = $Previous.PSObject.Properties['createdAt']
        if ($null -ne $PreviousCreated -and $PreviousCreated.Value) {
            # ConvertFrom-Json 会把 ISO 时间戳变成 DateTime；写回去要还原成同样的形式。
            $Value = $PreviousCreated.Value
            $Metadata['createdAt'] = if ($Value -is [datetime]) { $Value.ToString('o') } else { [string]$Value }
        }
        $NeedsWrite = -not $Same
    }
    catch {
        Write-Warning "$MetadataPath 不是合法 JSON；会被重写。"
    }
}
if ($NeedsWrite) {
    $PendingChanges++
    if ($PSCmdlet.ShouldProcess($MetadataPath, '写宿主元数据')) {
        New-Item -ItemType Directory -Path $Root -Force | Out-Null
        $Json = $Metadata | ConvertTo-Json -Depth 3
        [System.IO.File]::WriteAllText($MetadataPath, $Json + "`n", [System.Text.UTF8Encoding]::new($false))
        $PendingChanges--
        Write-Info "已写入：模板版本 $TemplateVersion，分支 $ResolvedBranch。"
    }
}
else {
    Write-Info '没变。'
}

# ------------------------------------------------------------------------------------------ 下一步

Write-Step '下一步'

$Gate = Join-Path $WorktreePath 'Tools\Test.ps1'
Write-Info '构建宿主并跑自动化测试：'
Write-Info "  pwsh -NoProfile -File `"$Gate`""
Write-Info '连固定数据后端一起跑 DreamTexture.Live.* ：'
Write-Info "  pwsh -NoProfile -File `"$Gate`" -Live"
Write-Info '首次构建只编译插件和宿主模块，引擎模块原样复用。'

if ($PendingChanges -gt 0) {
    exit 2
}
exit 0
