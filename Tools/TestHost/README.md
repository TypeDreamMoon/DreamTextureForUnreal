# DreamTexture 测试宿主

一个极小的虚幻工程，只用来编译 DreamTextureForUnreal 并跑它的自动化测试，离你真正干活的工程远远的。

## 为什么要有它

插件仓库的根就是插件根，自己没有 `.uproject`，编译和跑测试都得借一个工程。借自己平时开发的工程（下面叫「工作工程」）有几个代价：

- **你同时在工作工程里干别的事，并且开着编辑器。** 在那里编译，链接阶段要写
  `UnrealEditor-DreamTexture*.dll`，而开着的编辑器正占着它们；跑自动化测试还要再起一个编辑器进程。
- **排同一把 Build.bat 互斥锁。** 两个工程共用同一份引擎时，UBT 靠一个全局互斥锁串行化，
  在这边编译会挡住那边的构建（反过来也一样）。
- **半成品会漏到对方那里。** 工作工程里装的是插件的哪个检出，就编出哪个检出；开发中的分支
  编出来，会出现在你平时用的编辑器里。

宿主把这几件事都分开：

| | 工作工程（你的） | 宿主（这里的） |
| --- | --- | --- |
| 引擎 | 你平时用的那份（可能是源码构建） | Launcher 版（Installed Build），工程关联 `"5.8"`；也可以用别的，见下 |
| 插件 | 你装的那份检出 | 本仓库的 **git worktree**（开发分支） |
| Binaries / Intermediate / Saved | 各自一份 | 各自一份 |
| 启用的插件 | 你装的全部 | 只有 DreamTextureForUnreal（及它声明的依赖） |

引擎若是另一份，就不会排工作工程的 Build.bat 互斥锁，也不会去写对方正加载着的 DLL；
worktree 是另一份，所以这边的半成品不会出现在工作工程里。**不要在你的工作工程里编译开发中的插件。**

"只启用本插件"还顺带解决了第四件事：能编过的工程不等于能编过的插件。一个启用了三十个插件的工程，
可能掩盖本插件漏声明的依赖。

## 目录结构

```
DreamTextureTestHost/                     (位置由 -Root 或 LocalSettings.psd1 的 HostRoot 决定)
  DreamTextureTestHost.uproject           来自 Template/
  Config/DefaultEngine.ini                来自 Template/
  Config/DefaultGame.ini                  来自 Template/
  Source/DreamTextureTestHost*.Target.cs  来自 Template/
  Source/DreamTextureTestHost/            来自 Template/，一个空的主游戏模块
  Plugins/DreamTextureForUnreal/          本仓库的 git worktree
  .dreamtexture-testhost.json             记录宿主是从什么生成的（模板版本、仓库、分支、引擎）
  Binaries/ Intermediate/ Saved/          首次构建和运行时产生
```

模板不放在插件的 `Source/` 或 `Tests/` 下面——UBT 只在插件的这两个位置找模块和 target 规则，
所以任何把本插件放进 `Plugins/` 的工程都不会误捡到模板里的 `.Build.cs` / `.Target.cs`。

**宿主的 `.uproject` 不进本仓库**：仓库根就是插件根。

## 创建与核对

```powershell
pwsh -NoProfile -File Tools\TestHost\New-DreamTextureTestHost.ps1 -WhatIf            # 只看会做什么
pwsh -NoProfile -File Tools\TestHost\New-DreamTextureTestHost.ps1 -KeepCurrentHead   # 宿主已在开发分支上
pwsh -NoProfile -File Tools\TestHost\New-DreamTextureTestHost.ps1 -Detach            # 全新建，游离跟随 master
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `-Root` | `LocalSettings.psd1` 的 `HostRoot`（再退到 `HostProject` 所在目录、插件往上两级的宿主）；都没有则必须给 | 宿主目录。在 C 盘会被拒绝，除非 `-AllowSystemDrive`。 |
| `-RepoPath` | `LocalSettings.psd1` 的 `PluginRepo`；没有就用脚本自己所在的 git 仓库 | 插件仓库的任意一个工作区。 |
| `-Branch` | `master` | worktree 必须检出的分支（新建时也用它）。 |
| `-EngineRoot` | 环境变量 `DREAMTEXTURE_ENGINE_ROOT` → `LocalSettings.psd1` 的 `EngineRoot` → 按 `.uproject` 的引擎关联查 | 见下。 |
| `-Force` | 关 | 替换与模板不一致的文件，每个旧文件先备份成 `<名字>.bak-<时间戳>`。 |
| `-KeepCurrentHead` | 关 | 接受已有 worktree 当前检出的任何东西（开发分支、游离提交）。 |
| `-Detach` | 关 | 用游离 HEAD 跟随 `-Branch`。 |
| `-AllowSystemDrive` | 关 | 接受 C 盘上的宿主根（或设 `DREAMTEXTURE_ALLOW_DRIVE_C=1`）。 |
| `-WhatIf` | 关 | 只汇报，不写。 |

脚本幂等，随时可以重跑。**对 worktree 它只读**：已存在的 worktree 不会被删除、重置、切换分支；
有未提交改动时只报告个数，绝不碰。别的会话正在里面写代码是常态。唯一会移动 HEAD 的情形是
`-Detach` 下一个**游离且干净**的 worktree，它被移到分支当前末端。

两种典型场景：

- **宿主一直跟着某个开发分支干活**：用 `-KeepCurrentHead`。不给的话，分支不是 `master`
  脚本会报错退出（退出码 1），这是故意的——它从不替你切分支。
- **全新建宿主**：`master` 已经在主 checkout 检出，而一个分支同一时间只能被一个 worktree 检出，
  所以用 `-Detach`，宿主游离在 `master` 末端，只测已经提交的东西。

它做的检查：`git` 在 PATH 里；宿主根不在 C 盘（含目录联接解析后的真实盘）；宿主根不是仓库也不在仓库里；
宿主根里没有别的 `.uproject`（`-Root` 手滑落在真实工程上时，不会把模板文件建进去）；引擎存在；
worktree 属于同一个仓库、有自己的工作区而不是主工作区的链接、在仓库的 worktree 列表里。

**引擎怎么查**（没给 `-EngineRoot`、环境变量和设置文件里也没有时；逻辑在 [`../Common.ps1`](../Common.ps1)）：
按模板 `.uproject` 的 `EngineAssociation` 来。Launcher 版的关联是版本号（`"5.8"`），查
`HKLM\SOFTWARE\EpicGames\Unreal Engine\<版本>` 的 `InstalledDirectory`；源码构建的关联是 GUID，查
`HKCU\Software\Epic Games\Unreal Engine\Builds`；两处都没有时再看
`C:\ProgramData\Epic\UnrealEngineLauncher\LauncherInstalled.dat`，取 `AppName` 为 `UE_5.8` 的那条，
去掉 `UE_` 前缀当标识符（引擎自己也是这么认的，见引擎 `DesktopPlatformBase.cpp` 的
`EnumerateLauncherEngineInstallations()`）。都查不到就失败，并告诉你设 `-EngineRoot`、`DREAMTEXTURE_ENGINE_ROOT`
还是 `LocalSettings.psd1` 的 `EngineRoot`。

退出码：

| | |
| --- | --- |
| `0` | 宿主与模板一致，worktree 通过全部检查。 |
| `1` | 有检查没过，原因已打印，失败那一步之后什么都没做。 |
| `2` | 宿主能用，但与模板不一致：有文件不同且被保留（没给 `-Force`），或 `-WhatIf` 发现有东西要写。 |

## 构建与测试

日常用 `Tools\Test.ps1`，它把构建和无头跑测试合在一起，细节见 [`../README.md`](../README.md)。

```powershell
pwsh -NoProfile -File Tools\Test.ps1                 # 构建 + 跑 DreamTexture.*
pwsh -NoProfile -File Tools\Test.ps1 -Live           # 另外起固定数据后端，跑 DreamTexture.Live.*
```

手动构建（只编 Editor target，**不要编 Game target**）：

```powershell
& '<引擎根目录>\Engine\Build\BatchFiles\Build.bat' DreamTextureTestHostEditor Win64 Development `
  '-Project=<宿主目录>\DreamTextureTestHost.uproject' -WaitMutex -NoHotReloadFromIDE
```

Launcher 版引擎是 Installed Build，引擎模块都是预编译的，构建只编插件和宿主模块，不会重编引擎。

## 试一个 PR 或分支

宿主 worktree 就是插件的一份独立工作区，想试别人的提交：

```powershell
$Wt = '<宿主目录>\Plugins\DreamTextureForUnreal'
git -C $Wt status                       # 先确认里面没有别人没提交的东西
git -C $Wt switch --detach <提交或分支>
pwsh -NoProfile -File $Wt\Tools\Test.ps1
git -C $Wt switch <原来的分支>           # 切回去
```

你的工作工程全程看不到这些。

## 派生数据缓存

宿主故意没有任何派生数据缓存配置，共用本机 Zen 库。缓存命中要求键相同，而着色器的键包含工程的渲染设置
和目标着色器格式，所以 `Config/DefaultEngine.ini` 里相关段落模仿的是一个常见的工作工程（Substrate、DX12 SM6……）。
想和**你自己的**工作工程共用缓存，就把那几段从你工程的 `DefaultEngine.ini` 抄过来；改了其中一段，
第一次启动会把所有着色器重编一遍。脚本会打印 Zen 库的真实落点，因为默认目录在 C 盘用户目录下，
只有目录联接能把它挪开。

## 宿主和你的工作工程的差别

- **只测 worktree 里的东西。** 主 checkout 或工作工程里没提交、没检出到这个 worktree 的改动，宿主看不到。
- **没有别的插件。** 本插件对其他插件的隐含依赖，在这里会第一时间暴露。
- **没有内容。** 宿主本身不含任何资产，启动地图是引擎自带的 `/Engine/Maps/Entry`。
