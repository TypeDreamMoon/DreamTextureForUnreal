# Tools/Localization · 本地化工具链

插件的界面文字**源文是英文**，简体中文（`zh-Hans`）是译文。代码注释仍然写中文。
这套工具链从用户自己的 DreamShader 插件移植而来，**整条链不需要打开虚幻编辑器**：扫描是正则，`.locres` 由 Python 直接写。

```
Source/**/*.{h,cpp}
    |  localization_lint.ps1 -Json         扫描 LOCTEXT / NSLOCTEXT（含 C 转义解码）
Source/**/*.h
    |  scan_reflection.py                  扫描 UCLASS / UPROPERTY 的元数据（项目设置页的显示名、提示、分类）
    v
loctext.json                                源文清单          ← sync_loctext.py 写，提交进仓库
translations/*.json                         zh-Hans 译文      ← 人写，工具只读
language_neutral.json                       语言中立条目登记  ← 人写
    |  build_locres.py                      校验 + 写 LocRes 容器
    v
Content/Localization/DreamTextureForUnreal/
    DreamTextureForUnreal.locmeta           原生文化 en
    en/DreamTextureForUnreal.locres         原文 → 原文（原生文化那份，见下面「为什么要有 en/」）
    zh-Hans/DreamTextureForUnreal.locres    译文
```

| 文件 | 干什么 |
|---|---|
| `localization_lint.ps1` | 扫描 + lint（PowerShell 7）。人读模式打报告，有错误退出码 1；`-Json -OutFile x` 给脚本取清单用 |
| `scan_reflection.py` | 扫描头文件里 `UCLASS` / `USTRUCT` / `UPROPERTY` 的元数据，产出项目设置页要的条目（`sync_loctext.py` 调它）；含 `FName::NameToDisplayString` 的移植和规则 M1–M5 |
| `sync_loctext.py` | 刷新 `loctext.json`（LOCTEXT 加属性面板两类条目）；校验译文（缺、孤儿、重复、占位符）；`--check` 只查不写 |
| `build_locres.py` | 写 `.locres` / `.locmeta`；`--check` 比对磁盘；`--verify-hashes 某.locres` 复核任意 locres 里的哈希 |
| `loc_common.py` | 上面几个共用的：路径、译文合并与校验、占位符解析 |
| `lint_exemptions.json` | lint 的豁免清单，**每条必须写原因** |
| `language_neutral.json` | 翻不翻都一样的条目登记，**每条必须写原因** |
| `selftest.py` | 工具链自测，`python Tools/Localization/selftest.py` |
| `loctext.json` | 生成物：源码里所有 `LOCTEXT` / `NSLOCTEXT`、以及项目设置页的属性元数据的清单（`ns`、`key`、`src`、`file`） |

需要：PowerShell 7、Python 3.9+。没有第三方依赖。

---

## 加一条界面文字

**Slate 界面文字**（`.cpp` 里）：用 `LOCTEXT`。文件里已有 `#define LOCTEXT_NAMESPACE "DreamTexture<文件>"`，沿用它，别改。

```cpp
#define LOCTEXT_NAMESPACE "DreamTextureLibraryPanel"
...
.Text(LOCTEXT("ImportAll", "Import all"))
...
#undef LOCTEXT_NAMESPACE      // 文件末尾必须有，否则 unity 合并时会漏进下一个文件
```

**头文件里、或 Client / Assets 模块里给用户看的提示、错误、说明**：用 `NSLOCTEXT`，带参数用 `FText::Format`，最后 `.ToString()` 赋给原来的 `FString` 字段（字段类型不改，公共接口不变）：

```cpp
OutError = FText::Format(
    NSLOCTEXT("DreamTextureImporter", "TextureMissing", "The texture file for {0} was not found: {1}"),
    FText::FromString(Channel), FText::FromString(Path)).ToString();
```

要点：

- **源文写英文**，术语用下面「术语表」里那一套（素材库 = Library、母材质 = master material……），同一个概念全插件只用一个英文词。
- **key 在 namespace 里唯一**，用 UpperCamelCase 写它的用途（`ImportAll`），别拿句子当 key。同一个 key 在多处出现且源文相同是允许的（Slate 里常见），源文不同是错误（R8）。
- **key 和源文创建之后别随便改。** 改源文，译文还留着旧的；改 key，旧译文成了孤儿。两种情况 `sync_loctext.py` 都会报。
- 后端返回的原文（错误信息、ComfyUI 的原因）**原样透传，不翻译**；外面那层说明用本地化文字包起来。
- `UE_LOG` 和断言消息一律英文，不本地化。
- 不要把 `LOCTEXT` 的参数写成变量或宏（R7）：扫描器和引擎的 gather 都读不到它。相邻字面量 `"a" "b"` 是允许的（和编译器一样拼接），用来把长句拆行。
- 不会随文化变的东西（生成的资产里的参数说明、日志、给后端的字段）**不要**用 `LOCTEXT`——否则生成出来的资产会因为谁在哪个语言下导入而不同。

## 术语表

同一个概念全插件只用一个英文词。新写界面文字、翻译文档前先对一遍这张表；表里没有的新概念，先加到这里再用。

| 中文 | English |
|---|---|
| 素材库 | Library |
| 材质套装 / 一套材质 | material set |
| 母材质 | master material |
| 材质实例 | material instance |
| 贴图 | texture |
| 通道（basecolor、normal…） | channel |
| 底色 / 法线 / 粗糙度 / 金属度 / 环境光遮蔽 / 高度 | Base Color / Normal / Roughness / Metallic / Ambient Occlusion / Height |
| 打包图（ORM） | packed texture (ORM) |
| 导入 | Import |
| 赋给选中物体 | Assign to Selection |
| 材质槽 | material slot |
| 生成 | Generate |
| 出图模型 / 分解模型 | image model / decompose model |
| 提示词 / 扩写 | prompt / Refine prompt |
| 变体 | variant |
| 种子 | seed |
| 回流 | Reflow |
| 补齐缺失通道 / 改写外观 | Fill missing channels / Restyle |
| 新建 / 原地覆盖 | Create new / Overwrite in place |
| 平铺 / UV 展开 | Tileable / UV-unwrapped |
| 重绘幅度 | denoise strength |
| 传统派生 | Classic derivation |
| 后端 | backend |
| 长连接 | event stream |
| 推送 / 发送到 UE | push / Send to UE |
| 环境自检 | Environment check |
| 仅限研究用途 / 不可商用 | Research use only / Not for commercial use |
| 来源记录 | provenance record |
| 指认 | manual assignment |
| 写实 / 手绘（后端 style 值 realistic / stylized） | Realistic / Stylized |
| Web 端 | web UI |
| 已失联（长连接超时） | Timed out |
| 计数类文字 | 英文源文用 `{0}\|plural(one=…,other=…)`，参数传裸数字，不要 `FText::AsNumber`；中文译文直接 `{0}` |

## 属性面板（项目设置页）

项目设置 → 插件 → DreamTexture 这一页（`UDreamTextureSettings`，`Source/DreamTextureClient/Public/DreamTextureSettings.h`）上的文字**不是 `LOCTEXT`**：
属性的显示名、悬停提示、分类，是引擎在运行时拿 `UPROPERTY` 的元数据去实时表里查的。命名空间、key、源文是引擎规定的
（出处是 UE 5.8 引擎源码，`<引擎根目录>\Engine\Source\Runtime\`）：

| 文字 | 命名空间 | key | 源文（引擎拿它算 CRC，必须逐字相同） | 引擎代码 |
|---|---|---|---|---|
| 属性显示名 | `UObjectDisplayNames` | `类名:属性名`，如 `DreamTextureSettings:BackendURL` | 元数据 `DisplayName`；没写就是 `FName::NameToDisplayString(属性名, 是不是bool)`，如 `bCreateMaterialInstance` → `Create Material Instance` | `CoreUObject/Private/UObject/Field.cpp` 的 `FField::GetDisplayNameText`（913 行起） |
| 属性悬停提示 | `UObjectToolTips` | 同上 | 元数据 `ToolTip`（逐字，未经 `@see` 替换或整理） | 同文件 `FField::GetToolTipText`（938 行起） |
| 类的标题 / 说明（项目设置里这一栏的名字与说明） | `UObjectDisplayNames` / `UObjectToolTips` | `类名`，即 `DreamTextureSettings` | 同上 | `CoreUObject/Private/UObject/Class.cpp` 的 `UField::GetDisplayNameText`（303 行起）/ `GetToolTipText`（324 行起），`DeveloperSettings/Private/Engine/DeveloperSettings.cpp` 的 `GetSectionText` / `GetSectionDescription` 调的就是它们 |
| 分类标题 | `UObjectCategory` | 分类字符串，如 `Connection` | 分类字符串本身 | `Engine/Private/ObjectEditorUtils.cpp` 的 `FObjectEditorUtils::GetCategoryText`；细节面板走 `DetailCategoryBuilderImpl.cpp` 的 `SetDisplayName` |

类名是 UHT 的**反射名**：`UDreamTextureSettings` 去掉 `U`（`A`、`F` 同理）；属性名原样，bool 的 `b` 前缀保留在 key 里（`Actor:bHidden`），只在推导显示名时去掉。
key 里的冒号来自 `FField::GetPathName`。这些约定用引擎自带的 `PropertyNames` / `ToolTips` / `Category` 三份英文 `.locres` 核对过（约 27.5 万条，`selftest.py` 里有对照）：
每一条的源文 CRC 都等于显示字符串的 CRC。

所以 `.locres` 里要有这几个命名空间的条目，源文还必须与引擎看到的**元数据原字符串**完全一致。`scan_reflection.py` 读头文件，把它们算出来放进 `loctext.json`
（和 `LOCTEXT` 的条目同一格式），译文照常写进 `translations/*.json`（现在在 `Settings.json`）。

**给设置类加一个属性**：

```cpp
/** 中文注释照写，讲清楚为什么——它只给读代码的人看，不会出现在界面上。 */
UPROPERTY(EditAnywhere, config, Category = "Import", meta = (ToolTip = "English tooltip, one string literal."))
bool bCreateMaterialInstance;
```

1. **必须写显式的英文 `ToolTip`。** UHT 在没有显式 `ToolTip` 时会拿头文件里的文档注释当提示（`UhtParsingScope.cs` 的 `AddFormattedCommentsAsTooltipMetaData`），注释是中文，英文界面就会看到中文。
   显式 `ToolTip` 优先，注释照留（仓库约定注释写中文）。类 / 结构体有文档注释的同理。
2. `ToolTip` / `DisplayName` 只写 ASCII，一个字面量，不用反斜杠和双引号（UHT 对转义的处理到运行时不可预测）；单个字面量（含引号）不到 1024 个字符。例子里的 `-config "路径"` 这类写法改成 `-config followed by the path of a file`。
3. `Category` 写成不含 `|` 的一级分类。
4. 显示名没写 `DisplayName` 的由属性名推出（`bAutoStartBackend` → `Auto Start Backend`），中文照样要翻。英文显示名要改，就写 `DisplayName`。
5. 跑 `python Tools/Localization/sync_loctext.py`：它会列出新增的 `UObjectDisplayNames|…`、`UObjectToolTips|…`、`UObjectCategory|…` 条目；
   到 `translations/Settings.json` 补译文（提示的中文可以直接用原来的中文注释），再 `build_locres.py`。显示名是产品名那种不用翻的，登记进 `language_neutral.json`。
6. 英文 `ToolTip` 改了，旧译文还在——`sync_loctext.py` 会提示「源文改了」，回头对一遍。

`scan_reflection.py` 的规则（全是错误，`sync_loctext.py` 遇到就拒绝生成清单）：

| 规则 | 内容 |
|---|---|
| M1 | 面板上可见的属性（带 `EditAnywhere` / `VisibleAnywhere` 一类说明符），或带文档注释的 `UCLASS` / `USTRUCT`，没有显式非空 `ToolTip` |
| M2 | `ToolTip` / `DisplayName` / `Category` 的字符串不合规：非 ASCII（含中文）、反斜杠、相邻字面量拼接、过长 |
| M3 | 属性缺 `Category`，或 `Category` 含 `|`（嵌套分类没验证过，先拒绝） |
| M4 | 头文件里有扫描器还不认的反射构造（`UENUM` / `UFUNCTION` / `UINTERFACE` / `UDELEGATE`），或看不懂的 `UCLASS` / `UPROPERTY` 声明——它们同样走上面的命名空间，不能静默漏掉。要用就先在 `scan_reflection.py` 里加支持 |
| M5 | 同一个 `命名空间\|key` 出现两次且源文不同（也包括与 `LOCTEXT` 的条目撞车） |

测试头文件（路径里有 `Tests/`）不查。引擎里的真验证是 `DreamTexture.Localization.ZhHans.SettingsPage`：直接问引擎要每个属性的显示名 / 提示 / 分类，
`zh-Hans` 下必须是译文、`en` 下必须是英文源文（而不是中文注释）。

## 写译文

译文放 `translations/*.json`，**这个目录下所有 `.json` 会被合并**，所以几个人可以各写各的文件、不撞车。约定的文件名：
`Client.json`、`Assets.json`、`Editor.Library.json`、`Editor.Workflow.json`；项目设置页（属性元数据）在 `Settings.json`；测试用的固定条目在 `Tests.Localization.json`。

格式是一个对象，键是 `"<命名空间>|<key>"`，值是中文：

```json
{
 "DreamTextureLibraryPanel|ImportAll": "全部导入",
 "DreamTextureImportQueue|Queued": "已加入导入队列：{0}"
}
```

规矩（`sync_loctext.py` 都会检查，不过就不生成 `.locres`）：

- **每条源文都要有译文。** 缺的会列出来。
- **占位符一致**：源文里有哪些 `{0}` / `{Name}`，译文里就要有哪些（比的是参数名，不比出现次数）。
  `{0} {0}|plural(one=file,other=files)` 这种复数写法，整个 `|plural(…)` 当作 `{0}` 的修饰符，括号里的字不算占位符；
  中文没有单复数，译文直接写 `{0} 个文件` 即可。引擎转义是反引号：`` `{ `` `` `} `` `` `| `` `` `` ` ``。
- **同一个键只能出现在一个文件里**（包括同一文件里写两遍），哪怕两处的译文一样。冲突时说明里会写「文字相同」或「文字冲突」。
- **不能指向不存在的 key**（孤儿）。key 改名或删了，旧译文要一并改。
- **JSON 里的换行写 `\n`，制表符写 `\t`**；换行个数与源文不同只给提醒。
- 中文直接沿用界面上原来的中文原句。

### 翻不翻都一样的：`language_neutral.json`

`ComfyUI`、`DreamTexture` 这样的产品名，`{0}×{1}` 这样只有占位符和符号的，不需要译文。**不要为它们写一条一模一样的译文**，登记到 `language_neutral.json`，值写明原因：

```json
{
 "DreamTextureStatusPanel|RowComfy": "产品名 ComfyUI，中文界面里也写原名。"
}
```

登记了的条目在 `zh-Hans` 的 `.locres` 里是「源文 → 源文」；登记了又写了译文、或登记的 key 已经不存在，都是错误。
没有原因的登记也是错误。

## 重新生成

```powershell
python Tools/Localization/sync_loctext.py     # 1. 扫描源码，刷新 loctext.json，报缺译文 / 孤儿 / 占位符
python Tools/Localization/build_locres.py     # 2. 校验通过后写 Content/Localization/DreamTextureForUnreal/
python Tools/Localization/sync_loctext.py --check   # 3. 只查不写：清单、译文、.locres 都是最新才退出 0
```

提交时把 `loctext.json`、`translations/`、`Content/Localization/` 一起提交。`.locres` 是二进制，`.gitattributes` 已标 `binary`。

一次跑完全部检查（和 CI 同一组命令）：

```powershell
pwsh -NoProfile -File Tools/Test.ps1 -LintOnly    # 只查本地化，几秒钟，不需要引擎
pwsh -NoProfile -File Tools/Test.ps1              # 默认先查本地化，通过了再构建和跑测试
pwsh -NoProfile -File Tools/Test.ps1 -NoLint      # 跳过本地化检查（没有 Python 的机器）
```

退出码 4 表示本地化检查没过。（老命令里的 `-Lint` 还认，但已经是默认行为。）

## lint 规则

非测试源码（路径里不含 `Tests/`）里：

| 规则 | 级别 | 内容 |
|---|---|---|
| R1 | 错误 | **字符串字面量里不许有中日韩字符**（注释不查）。界面文字改 `LOCTEXT`/`NSLOCTEXT`；`UE_LOG` 和断言消息必须英文；`LOCTEXT` 的源文含中文也是这条——中文译文写进 `translations/` |
| R2 | 错误 | `FText::FromString` / `FromName` 里直接放含英文字母的字面量：gather 看不见它、翻不了。换行、纯标点不算 |
| R3 | 警告 | 同一个 key 在多处出现且源文相同（默认不列出，`-All` 才列） |
| R4 | 错误 | 头文件里 `#define LOCTEXT_NAMESPACE` |
| R5 | 错误 | `LOCTEXT` 前面没有有效的 `#define LOCTEXT_NAMESPACE`（头文件请用 `NSLOCTEXT`） |
| R6 | 警告 | 定义了命名空间但文件末尾没有 `#undef` |
| R7 | 错误 | `LOCTEXT` / `NSLOCTEXT` 的参数不是纯字符串字面量 |
| R8 | 错误 | 同一个 `命名空间\|key` 对应不同的源文 |
| R9 | 警告 | `AsLocalizable_Advanced`：动态引用，扫描器看不见 |
| R10 | 错误 | `I18N-EXEMPT` 注释没写原因 |
| R11 | 错误 | 命名空间或 key 里有 `\|` |
| R12 | 错误 | key 或源文为空，或含 NUL |
| R13 | 错误 | 豁免清单里有条目没写原因，或已经没有任何东西匹配它（过期的豁免必须删） |

测试源码不查 R1 / R2 / R9：测试里断言中文、造中文输入是正常的。测试里的 `LOCTEXT` 仍然进清单（和引擎的 GatherText 一样不分文件）。

### 豁免

确实该留着的（典型：与**后端**的固定字样逐字比较、处理后端原文），有两种写法，**都必须写原因**：

1. 行内，只对那一行的字面量生效：
   ```cpp
   if (Job.Stage == TEXT("等待 ComfyUI")) // I18N-EXEMPT: 后端把阶段名写成这句中文，逐字比较，不是显示文字
   ```
2. `lint_exemptions.json`，适合一个字面量写在不方便加注释的地方：
   ```json
   { "rule": "R1", "file": "Source/…/X.cpp", "contains": "提交中", "reason": "…" }
   ```
   `file` 支持 `*` 通配（相对插件根目录，用 `/`）；`contains` 可省，写了就必须是被豁免字面量的一部分。

当前的豁免见 `lint_exemptions.json`：全部是「处理后端原文」——后端的任务阶段名是中文（`等待 ComfyUI`、`提交中`）要逐字比较，
后端失败原因末尾可能带中文句号 / 分号要先去掉再接我们自己的句子，以及旧版插件 / 后端留下的 `上传失败：` `提交失败：` 前缀。

## 测试

引擎里的真验证，`Tools/Test.ps1 -Filter DreamTexture.Localization`：

| 测试 | 验证什么 |
|---|---|
| `DreamTexture.Localization.ZhHans.Strings` | `loctext.json` 里**每一条**：切到 `zh-Hans` 后引擎实时表里取出的是译文，切回 `en` 后是英文源文。收尾一定切回原文化 |
| `DreamTexture.Localization.ZhHans.CompiledCallSites` | 本测试文件里用真的 `LOCTEXT` / `NSLOCTEXT` 宏写了带 C 转义、相邻字面量、占位符、复数写法的几条：编译器给出的字符串必须与扫描器解码出的 `src` 逐字相同，且 `zh-Hans` 下这些 `FText` 本身变成中文 |
| `DreamTexture.Localization.ZhHans.Coverage` | `loctext.json` 每条都有译文（或登记了语言中立），译文没有指向不存在的 key |
| `DreamTexture.Localization.ZhHans.SettingsPage` | 项目设置页：直接问引擎要 `UDreamTextureSettings` 每个属性的显示名（`GetDisplayNameText`）、悬停提示（`GetToolTipText`）、属性与细节面板的分类标题、分区的标题与说明（`GetSectionText` / `GetSectionDescription`）。`zh-Hans` 下是译文、`en` 下是英文源文（不是头文件里的中文注释），切回 `en` 后又是英文；类上每个面板可见的属性必须在清单里有显示名、提示、分类。期望值取自清单和译文，另夹几条写死的字面量 |

`Tools/Test.ps1` 把测试编辑器钉在 `-culture=en`，其它测试比对的是英文源文；要测中文译文的测试自己切 `zh-Hans`、再切回来。
离线的 `selftest.py` 钉死扫描器、哈希、LocRes 的读写和译文校验，CI 也跑它。

## 为什么能不开编辑器

`.locres` 是个很小的二进制容器（`TextLocalizationResource.cpp` 的 `LoadFromArchive` 是权威）。我们写 `Optimized_CityHash64_UTF16`（版本 3）：

```
魔数(16) 版本(1) 字符串表偏移(int64) 条目数(u32) 命名空间数(u32)
  { 命名空间哈希(u32) 命名空间(FString) key 数(u32)
      { key 哈希(u32) key(FString) 源文 CRC(u32) 字符串表下标(i32) } … } …
字符串表：数量(u32) { 译文(FString) 引用计数(i32) } …
```

**静默失败的两个点**——哈希对不上不会报错，引擎只是悄悄丢掉那条译文，界面回落成英文：

- **命名空间 / key 的哈希**：`CityHash64(UTF-16LE 字节)` 折成 32 位 = `lo32 + hi32 × 23`
  （`TextKey.cpp` 的 `HashString`、`TypeHash.h` 的 `GetTypeHash64`）。加载时 `FTextKey::SerializeWithHash` 直接信文件里的值。
  CityHash64 用引擎自带的 `Editor` / `Engine` / `ToolTips` / `PropertyNames` 四份 `zh-Hans.locres`（约 37.5 万个键，长度覆盖 CityHash 的全部分支）逐个重算，**零不符**：
  `python build_locres.py --verify-hashes <某.locres>`。
- **源文 CRC**：不是源文本身，是 `FCrc::StrCrc32<TCHAR>(源文)`（`TextLocalizationResource.h` 的 `HashString`）。`FindDisplayString_Internal`
  拿代码里 `LOCTEXT` 的**实际字符串**算同一个 CRC，对不上就丢译文。所以 `loctext.json` 的 `src` 必须是编译器看到的字符串，
  不是源码里写的样子：lint 解码了 `\n \t \" \\ \xNN \uNNNN \UNNNNNNNN \ooo`，拼接了相邻字面量，对 `\0` 报错。
  `TCHAR` 是 UTF-16 码元，非 BMP 字符（emoji）是两个码元，CRC 逐码元算——DreamShader 里逐码点（`ord()`）的版本对 emoji 不对，这里修了，并用 `zlib` 独立复算校验。

`.locmeta` 的布局取自 `FTextLocalizationMetaDataResource::SaveToArchive`（版本 2）：魔数、版本、原生文化、原生 locres 路径、编译了哪些文化、`bIsUGC`（`bool` 在 `FArchive` 里占 4 字节）。
与引擎自带的 `Editor.locmeta` 逐字节同构（`selftest.py` 里有对照）。

### 为什么要有 `en/`

引擎切到某个文化时，**只**把「该文化的 `.locres`」里有的条目覆盖进实时表。原生文化（`en`）在 `LoadLocalizedResourcesFromPaths` 里
被特判：当前文化就是原生文化时不去加载「原生资源」，只加载 `en/<目标>.locres`。如果没有这份，从 `zh-Hans` 切回 `en` 时实时表里没有任何东西去覆盖中文——
编辑器里切语言之后，这个插件的界面还是中文。引擎自带的目标几乎都有 `en/`（`Editor`、`Engine`、`ToolTips`、`PropertyNames`、`OnlineSubsystem` ……），本地化面板生成目标时也总会生成它。

本仓库实测过：把 `en/` 挪走再跑 `.Strings` / `.CompiledCallSites`，切回 `en` 之后全部条目仍是中文；放回去就通过。DreamShader 没带这份，它存在这个问题。

### 为什么不用编辑器的 Localization Dashboard

- 它的 gather 要开编辑器、跑 commandlet，慢，也没法进 CI（GitHub 上没有虚幻引擎）。
- 它得先配 `Config/Localization/<目标>.ini`、维护 manifest / archive / PO 文件一大堆中间文件；我们只有一份 JSON 译文。
- 它的 gather 只认 `LOCTEXT` 的字面量写法，而我们本来就要求这一点，扫描器的规则和它的一样（并且更严：R7）。
- 我们需要的是**可重复的校验**：每条有译文、占位符一致、没有孤儿；哈希对不对用引擎里的自动化测试兜底。这些用脚本几秒钟就能在 CI 里做完。

代价：不能用 Dashboard 的界面翻译、也不能导出 PO 给翻译平台。要导出的话，`loctext.json` + `translations/*.json` 是现成的数据源，写个转换很容易。

## 新增一种语言

1. `loc_common.py` 的 `TRANSLATED_CULTURES` 加上文化名（如 `"ja"`）。
2. 译文要**按语言分开**：现在 `translations/*.json` 只是 `zh-Hans` 的。多语言时把目录改成 `translations/<文化>/*.json`，并让 `load_translations` 按文化读——这一步现在没做（YAGNI），需要时改 `loc_common.py` 和 `build_locres.py` 的 `outputs()`。
3. 目录名必须是引擎认的文化名（`zh-Hans`、`ja`、`ko`、`pt-BR`），引擎按这个目录名去找。

## 已知限制

- 分类标题（`UObjectCategory`）是**全局**的：引擎按分类字符串查译文，不分哪个类。`Connection` / `Import` / `Generate` 译成 `连接` / `导入` / `生成`，
  别的插件或引擎界面里恰好也有叫这个名字的分类，在中文下会跟着显示这个译文（引擎自带的 `Category.locres` 里没有这三个名字；在英文下不受影响）。
- 属性面板只认 `UCLASS` / `USTRUCT` / `UPROPERTY`（见上面的规则 M4）。以后加 `UENUM` / `UFUNCTION` 要先扩 `scan_reflection.py`，加上去之前扫描器会直接报错而不是漏掉。
- 插件描述（`.uplugin` 的 `FriendlyName` / `Description`）和 Plugins 窗口里的文字不走本地化目标，这里没处理。
- 本地化目标的加载策略是 `Editor`：只在编辑器里加载，插件的模块也全是 `Editor` 类型，没有运行时界面。
- `loctext.json` 里的 `file` 是同一个 key 出现在多个文件时按路径字节序第一个出现的文件。
