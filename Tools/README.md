# Tools · 开发与测试工具链

这个仓库的根**就是插件根**（`DreamTextureForUnreal.uplugin` 在根目录），里面没有 `.uproject`，自己编不了。
开发和测试都在一个**只装这一个插件的宿主工程**里做，不在你自己的工作工程里做。

| | |
|---|---|
| 宿主工程 | 一个极小的虚幻工程，Editor target `DreamTextureTestHostEditor`；模板在 [`TestHost/Template/`](TestHost/Template)，用 `New-DreamTextureTestHost.ps1` 建 |
| 插件所在 | 宿主的 `Plugins\DreamTextureForUnreal` 是本仓库的 **git worktree**，有自己的 `Binaries/` `Intermediate/` |
| 引擎 | Launcher 版 5.8（Installed Build），工程关联 `"5.8"` |

宿主用 Launcher 版引擎、自己的 worktree，就不会和你的工作工程排同一把 Build.bat 互斥锁，也不会去写对方正加载着的 DLL。
完整的理由见 [`TestHost/README.md`](TestHost/README.md)。

## 本机设置（东西装在哪）

脚本不假设任何东西装在哪。宿主工程、引擎目录、后端、ComfyUI 每一个都按下面的顺序找，先找到的算数：

1. 脚本自己的参数（`-Project`、`-EngineRoot`、`-BackendExe`、`-ComfyRoot` ……）；
2. 环境变量：`DREAMTEXTURE_ENGINE_ROOT`、`DREAMTEXTURE_BACKEND_EXE`、`DREAMTEXTURE_COMFY_ROOT`
   （另有 `DREAMTEXTURE_LIVE_URL` / `DREAMTEXTURE_PIPELINE_URL`，那两个是脚本给测试编辑器设的，不是路径）；
3. `Tools\LocalSettings.psd1`：本机私有、**不进仓库**（`.gitignore` 忽略）的设置文件。把
   [`LocalSettings.example.psd1`](LocalSettings.example.psd1) 复制过去，按需填，每个键在模板里都有说明；
4. 能自动推出来的就自动推：宿主 `.uproject` 取插件往上两级目录里的那个（`<宿主>\Plugins\DreamTextureForUnreal\` 的布局）；
   引擎按它的 `EngineAssociation` 查——Launcher 版查 `HKLM:\SOFTWARE\EpicGames\Unreal Engine\<版本>` 的 `InstalledDirectory`，
   源码构建（GUID）查 `HKCU:\Software\Epic Games\Unreal Engine\Builds`，最后退到 `LauncherInstalled.dat`；
5. 都没有就失败，并告诉你该设哪个参数、环境变量或设置键。

插件放进宿主工程、装的是 Launcher 版引擎时**什么都不用配**。需要自己的后端、ComfyUI（`-Pipeline`）、
或者宿主不在插件往上两级的位置时，才写 `LocalSettings.psd1`。后端有两个键：`BackendExe`（固定数据后端，`-Live`）
和 `PipelineBackendExe`（流水线后端，`-Pipeline`；没填就退到 `BackendExe`），因为后端有改动时测新接口用的往往是另编的那份。

看现在每个值最终是从哪一层解析出来的（只读，不启动任何东西）：

```powershell
pwsh -NoProfile -File Tools\Show-Settings.ps1
```

## 东西都在哪

| | |
|---|---|
| [`Test.ps1`](Test.ps1) | **闸门**：构建宿主 + 无头跑自动化测试 + 打结果表。日常只用这一个。 |
| [`TestHost/`](TestHost/README.md) | 宿主工程的模板和创建脚本 `New-DreamTextureTestHost.ps1`。 |
| `Common.ps1` | 各脚本共用的「东西在哪」解析（上一节的顺序），用 `.` 点号引入。 |
| `LocalSettings.example.psd1` / `Show-Settings.ps1` | 本机设置文件的模板；打印每个值从哪一层来。`LocalSettings.psd1` 自己不进仓库。 |
| [`FixtureBackend/`](FixtureBackend/README.md) | 固定数据的测试后端（`Start-` / `Stop-FixtureBackend.ps1`），`DreamTexture.Live.*` 测试靠它。 |
| [`PipelineBackend/`](PipelineBackend/README.md) | 隔离的 ComfyUI + 新后端（`Start-` / `Stop-PipelineBackend.ps1`），真的跑分解管线，`DreamTexture.Pipeline.*` 测试靠它。吃显存，起之前先查。 |
| [`Localization/`](Localization/README.md) | 本地化工具链（扫描 → 校验 → 写 `.locres`，不需要编辑器）、lint 与豁免清单、译文（`translations/*.json`）。涵盖 `LOCTEXT` / `NSLOCTEXT` 和项目设置页（`UPROPERTY` 元数据）。`Test.ps1` 默认先跑它的检查，`-NoLint` 跳过，`-LintOnly` 只跑它。 |
| `ensure-bom.sh` | 给源文件补 UTF-8 BOM。新建 `.h` / `.cpp` / `.cs` 之后必须跑，理由见 [`../Docs/contributing.md`](../Docs/contributing.md) 的 Source encoding 一节。 |

## 日常用法

```powershell
pwsh -NoProfile -File Tools\Test.ps1 -BuildOnly                        # 只看编过没有（本地化检查先跑，通过了才构建）
pwsh -NoProfile -File Tools\Test.ps1                                   # 本地化检查 + 构建 + 跑全部 DreamTexture.*
pwsh -NoProfile -File Tools\Test.ps1 -SkipBuild -Filter DreamTexture.Client   # 不构建，只跑一部分
pwsh -NoProfile -File Tools\Test.ps1 -Live                             # 连固定数据后端，含 DreamTexture.Live.*
pwsh -NoProfile -File Tools\Test.ps1 -Pipeline                         # 起隔离的 ComfyUI + 新后端，跑 DreamTexture.Pipeline.*
pwsh -NoProfile -File Tools\Test.ps1 -LintOnly                         # 只跑本地化检查（几秒，不要引擎），与 CI 同一组命令
pwsh -NoProfile -File Tools\Test.ps1 -NoLint                           # 跳过本地化检查（没有 Python 的机器；或者明知它没过、只想先看别的）
bash Tools/ensure-bom.sh                                               # 写完新文件后
```

**本地化检查默认就跑**：`lint` 与 `sync_loctext.py --check`（清单是否最新、项目设置页的元数据合规、每条源文都有译文、`.locres` 是否最新）。
这类错误在引擎里是静默的——译文悄悄不生效、界面回落成英文——不靠闸门拦就没人发现。老命令里的 `-Lint` 还认，但已经是默认行为，加不加都一样。

`Test.ps1` 的判定：

- **一个测试都没找到 → 失败**，不是通过。过滤串写错时最容易被当成全绿。
- 全部被跳过 → 失败（一个都没真正跑过）。「跳过」除了引擎自己的 `Skipped` 状态，还认**以通过收场、但信息里以「已跳过」打头**的测试——
  `DreamTexture.Live.*` 在没有 `DREAMTEXTURE_LIVE_URL` 时就是这么跳的。表上标 `[跳过]` 并列出原因，不会把它们混进「通过」里装全绿。
- 编辑器崩了、超时、没写出报告 → 失败。
- 每次运行的日志与报告都在宿主的 `Saved\DreamTextureTest\<时间戳>\`，路径每次唯一，旧结果不可能冒充这一次的。

退出码：`0` 通过；`1` 测试失败（含零测试、全跳过、崩溃）；`2` 构建失败；`3` 环境不对没能开始（含没有 Python 又没加 `-NoLint`）；`4` 本地化检查没过。

## 构建脚本的几条经验

早期的独立构建脚本已经并进 `Test.ps1 -BuildOnly`。它留下的、仍然成立的经验：

- **PowerShell 7.3+ 里原生命令只要写 stderr 就会抛异常**，而 UBT 正常也往 stderr 写进度。
  `Test.ps1` 开头把 `$PSNativeCommandUseErrorActionPreference` 关掉，UBT 的输出原样直出、不吞。
- **`.ps1` 存 UTF-8 带 BOM。** 中文系统的代码页是 936，Windows PowerShell 5.1 读不带 BOM 的 UTF-8 会按 GBK 解，中文全乱。
- 生成 IDE 工程文件走 `Build.bat -ProjectFiles`，不走 `GenerateProjectFiles.bat`——后者出错时会 `pause`，
  非交互环境里会把整条流水线挂死等回车。（现在不需要，记着以备要生成 `.sln`。）

## 手动构建

只编 Editor target，**不要编 Game target**（Game target 是整体式的，会把整个引擎编进宿主）：

```powershell
& '<引擎根目录>\Engine\Build\BatchFiles\Build.bat' DreamTextureTestHostEditor Win64 Development `
  '-Project=<宿主目录>\DreamTextureTestHost.uproject' -WaitMutex -NoHotReloadFromIDE
```

## 已知会挡路的东西

**宿主里编辑器开着的时候别编。** `UnrealEditor.exe` 会占住 `UnrealEditor-DreamTexture*.dll`，链接阶段直接失败；
它还占着显存。`Test.ps1` 发现宿主的编辑器在开着会警告。别人的测试编辑器开着导致链接报 DLL 被占用，等一会儿再编。

**构建失败的错误在别人的源码里。** 同一个 worktree 里可能有别人写到一半的代码。UHT 是按 target 整体跑的，
任何一个模块的头文件过不了，整个构建就停在那里。`Test.ps1` 失败时把带 `error` 的行单独复述一遍，
看文件路径就知道是不是自己的。

**想在不编译的前提下确认「插件挂没挂上」**：UBT 的 `JsonExport` 模式只导出 target 的模块图，
不跑 UHT、不编译，几秒钟就完：

```powershell
& '<引擎根目录>\Engine\Build\BatchFiles\Build.bat' DreamTextureTestHostEditor Win64 Development `
  '-project=<宿主目录>\DreamTextureTestHost.uproject' `
  '-Mode=JsonExport' "-OutputFile=$env:TEMP\DreamTextureTestHostEditor.json"
```

看 `Modules` 里有没有 `DreamTextureClient` / `DreamTextureAssets` / `DreamTextureEditor`，以及依赖有没有全部解析出来。

**测试里的编辑器不会碰你自己的后端（默认端口 8777）。** 插件模块的全局连接默认指向 `127.0.0.1:8777`，
无头编辑器一启动就会去探它，你的后端开着时测试就真的连上去了。`Test.ps1` 启动 `UnrealEditor-Cmd` 时用命令行
覆盖配置，把 `DreamTextureSettings.BackendURL` 从一开始就压住：

```
-ini:Editor:[/Script/DreamTextureClient.DreamTextureSettings]:BackendURL=<地址>
```

不带 `-Live` 是没人监听的 `http://127.0.0.1:18779`，带 `-Live` 是固定数据后端 `http://127.0.0.1:18777`，只带 `-Pipeline` 是流水线后端 `http://127.0.0.1:18787`。
语法见引擎 `ConfigCacheIni.cpp` 的 `OverrideFromCommandline`：参数按第一个 `=` 切分，所以值里 `http://` 的冒号没事；
整个参数不能含空格，不要加引号。`Editor` 是 ini 名，因为 `UDreamTextureSettings` 声明为 `config = Editor`。
这样无论 Live 测试有没有自己临时改设置，模块那条全局连接起点就是固定后端或死地址。

验证方法：日志里 grep `(?<!1)8777`（排除 `18777`）应为 0；`-ExtraEditorArgs '-LogCmds="LogHttp Verbose"'`
能在日志里看到全局连接对 `18777/api/health` 的请求。
