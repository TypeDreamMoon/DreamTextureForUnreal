# 流水线后端（隔离的 ComfyUI + 新后端）

给 `DreamTexture.Pipeline.*` 端到端测试用：把一张图送进后端，**真的**跑分解管线（CHORD / 传统派生），拿回
Normal / Roughness / AO / ORM。和 [固定数据后端](../FixtureBackend/README.md) 的分工：

| | 固定数据后端 | 流水线后端 |
| --- | --- | --- |
| ComfyUI | 没有（指向没人监听的端口） | **真的**，隔离的一份 |
| 后端 | `LocalSettings.psd1` 的 `BackendExe` | `PipelineBackendExe`（没填就退到 `BackendExe`），通常是**另编的、带着插件所需新接口的**那份 |
| 端口 | `18777` | ComfyUI `18188`，后端 `18787` |
| 测什么 | 接口契约（素材读取、事件流、会话） | 回流的完整链路（上传底图 → 分解 → 产物） |
| 代价 | 几乎没有 | **吃显存**，所以起之前先查 |

```powershell
pwsh -NoProfile -File Tools\PipelineBackend\Start-PipelineBackend.ps1   # 起，两个都应答之后才返回；最后一行打印地址
pwsh -NoProfile -File Tools\PipelineBackend\Stop-PipelineBackend.ps1    # 停，拆联接，确认端口与显存
$env:DREAMTEXTURE_PIPELINE_URL = 'http://127.0.0.1:18787'               # 测试靠这个变量决定跑不跑 Pipeline.*
```

`Tools\Test.ps1 -Pipeline` 把这三步包好了：先 Start，给子进程设环境变量，跑完无论成败都 Stop。

## 为什么要隔离

**不碰你自己的 ComfyUI 和你自己的后端。** 你很可能同时开着 ComfyUI、虚幻编辑器和自己的后端（默认端口 8777）。
测试往里面塞任务、改模型、占显存，会直接干扰你正在做的事。所以：

- ComfyUI 是**另起的一份**：`--base-directory` 指向状态目录下的 `comfy\`，它自己的 `input` / `output` / `temp` / `user`
  全在那里，不碰你自己 ComfyUI 的任何目录。
- 端口全避开：ComfyUI 默认 8188、后端默认 8777、固定数据后端是 18777，这里是 18188 / 18787。端口被别人占了就**拒绝**，
  不去杀占用者。
- 停的时候只停 pid 文件里登记、并且核对过映像名和启动时间的进程（pid 会被复用）。

**只加载需要的节点包。** 整合包的 `custom_nodes` 里有十几个包（Manager、Impact-Pack、视频类……），全加载进来启动要几分钟，
而且 Manager 启动期会拖慢任务一个数量级（后端仓库 `docs/comfyui-notes.md` 的坑 0）。隔离的 ComfyUI 的
`comfy\custom_nodes\` 里只有四个**目录联接**：

| 联接 | 指向 |
| --- | --- |
| `ComfyUI-Chord` | `<整合包>\ComfyUI\custom_nodes\ComfyUI-Chord`（只读用） |
| `ComfyUI-TextureAlchemy` | 同上 |
| `ComfyUI-seamless-tiling` | 同上 |
| `ComfyUI-DreamTexture` | 后端 `dreamtexture.exe` 同目录的 `comfy_nodes\`（**新版**自研包，随 `-BackendExe` 走） |

最后一个要特别说一下：你自己的 ComfyUI 里的 `ComfyUI-DreamTexture` 若指向旧的后端 checkout，就**没有** CHORD 的包装节点
（`DT_ChordMaterialEstimation` / `DT_ChordNormalToHeight`，`tileable=false` 靠它们关环绕填充）。所以不能直接复用你的
ComfyUI，这是要另起一份的第二个理由。冷启动 ComfyUI 约 15~20 秒。

**模型不用另挂。** 整合包的 `ComfyUI\extra_model_paths.yaml` 无论 `--base-directory` 指向哪里都会被读，所以你在那里挂好的
模型目录（`checkpoints` 里要有 `chord_v1.safetensors`）这里直接可用。要多挂别的用 `-ExtraModelPaths`。
起来之后脚本会查一下 checkpoints 列表，缺 `chord_v1.safetensors` 会警告（只跑传统派生用不到它）。

## 显存阈值

起之前用 `nvidia-smi` 查**空闲显存**，低于 `-MinFreeVramGiB`（默认 **10 GiB**）就拒绝，什么都不动，退出码 1，并说清楚原因。

为什么这么严：ComfyUI 和开着的虚幻编辑器抢显存，显存吃满时驱动**不报错**，静默回退到系统内存，
任务会慢几十倍甚至假死，不报错也不结束（见 `docs/comfyui-notes.md`、后端的「显存被占导致的假死」）。
与其让测试挂住，不如进门就拒绝。

- 阈值是按「最重的情况」定的：本地 SDXL 出图（改写外观）要 7GB 多的底模。**只做补齐通道**（`src-upload-v1`，CHORD 约 3~5 GB，
  传统派生几乎不占）用不了这么多，可以用 `-MinFreeVramGiB 6` 之类调低，风险自负。`0` 关闭检查。
- 读不到显存（没有 `nvidia-smi`）也拒绝，因为那样没法确认；确认过没有别的东西占着显卡再用 `-MinFreeVramGiB 0`。
- 已经在跑时不检查（它自己占着的显存不算）。
- 显存是整机共享的，别的程序（浏览器、Unity、聊天软件……）的占用会上下浮动。16 GiB 的卡常见空闲 10~11 GiB，
  离阈值不远——测试前顺手关掉开着的虚幻编辑器。

## 状态目录

默认宿主工程的 `Saved\PipelineBackend\`（脚本从自己所在位置往上找 `.uproject`；不在宿主里就退到
`%TEMP%\DreamTexturePipelineBackend\`，绝不往仓库里写）：

```
comfy\                  ComfyUI 的 --base-directory
  custom_nodes\           四个联接（见上）
  input\ output\ temp\    ComfyUI 自己的；每次冷启动清空
  user\
output\                 后端的素材目录：Tests\Fixtures 的拷贝（里面有 mat_20260827_777001_ref\），每次冷启动重铺
data\                   后端的 SQLite；每次冷启动清空（-KeepData 保留）
logs\comfy.log          ComfyUI 的输出（UTF-8）
logs\dreamtexture.log   后端自己的日志
pipeline.json           生成的后端配置
comfy.pid backend.pid   登记起的进程（pid、启动时间、映像路径）
vram.json               启动前的空闲显存，Stop 拿来对比
```

后端的 `output\` 里预铺固定素材，所以 `GET /api/materials/mat_20260827_777001_ref` 立刻可用；新生成的材质也落在这里。

## 行为约定

两个脚本**无参数、可反复调用**。

**Start**

- 两个都在跑且都应答 -> 什么都不做，打印地址，返回 0。
- 只活了一个、或活着却不应答 -> 不猜它是刚起还是卡死，把自己登记的都停掉按冷启动重来。
- 冷启动：查显存 -> 查端口 -> 建联接、清上次的残留 -> 起 ComfyUI，等 `/system_stats` 应答 -> 确认自研包的三个节点
  （`DT_EnsureImageShape`、`DT_ChordMaterialEstimation`、`DT_ChordNormalToHeight`）真的注册了 -> 起后端，等
  `/api/health` 应答、**并且后端自己看到 ComfyUI 就绪** -> 返回 0，最后一行打印 `DREAMTEXTURE_PIPELINE_URL=http://127.0.0.1:18787`。
- 任何一步失败都会把已经起的收拾干净（停进程、拆联接）再返回 1，并打印日志末尾，不留半死的进程。
- 端口被别的进程占着 -> 说清楚是谁（pid、进程名、映像路径），返回 1，**不动那个进程**。

**Stop**

- 先停后端、再停 ComfyUI；两者都**只**停 pid 文件里登记的、核对过映像与启动时间的进程，连同它的后代进程。
- 拆联接：只拆 `comfy\custom_nodes\` 下**类型确实是联接**的条目，且用非递归删除（删联接本身，不删穿到目标）。
  真目录一律不动，只警告。
- 确认两个端口真的释放；显存回到启动前的水平（容差 768 MiB，最多等 20 秒）。显存没回来只**警告**不算失败，
  因为它是整机共享的；端口没放才是失败。
- 没在跑也返回 0。

退出码：Start `0` 两个都在跑且应答 / `1` 没做到；Stop `0` 已不在运行 / `1` 进程杀不掉或端口没释放。

常用参数：`-ComfyPort` `-BackendPort` `-BackendExe` `-ComfyRoot` `-ComfyPython` `-ComfyMain` `-CustomNodesSource`
`-DreamTextureNodes` `-ExtraModelPaths` `-StateDir` `-MinFreeVramGiB` `-KeepData`。

`-BackendExe` 没给时看环境变量 `DREAMTEXTURE_BACKEND_EXE`（与固定数据后端同一个），再看 `Tools\LocalSettings.psd1` 的
`PipelineBackendExe`、`BackendExe`；`-ComfyRoot` 没给时看环境变量 `DREAMTEXTURE_COMFY_ROOT`，再看 `ComfyRoot`。都没有就失败并说明
（见 [`../README.md`](../README.md#本机设置东西装在哪)）。工作流目录和自研节点包都随后端 exe 所在的目录走。

## 从测试里用它

```powershell
$Base = $env:DREAMTEXTURE_PIPELINE_URL     # http://127.0.0.1:18787
# 1. 上传底图（multipart，字段名 image），拿回文件名
curl.exe -s -F "image=@basecolor.png" "$Base/api/uploads"          # {"name":"dt_ref_xxxx.png",...}
# 2. 提交：已有底图 x 传统派生，不要平铺（UV 展开贴图）
curl.exe -s -H "Content-Type: application/json" "$Base/api/generate" -d '{
  "workflow_id": "src-upload-v1.dec-derive-v1",
  "name": "MI_Rock",
  "params": { "source_image": "dt_ref_xxxx.png", "tileable": false },
  "derived_from": { "kind": "ue_material", "asset_path": "/Game/M/MI_Rock", "engine": "5.8.2",
                    "mode": "fill_channels", "maps_in": { "basecolor": "0123456789abcdef" } } }'
# 3. 等 WebSocket 上的 job.done（或轮询 GET /api/jobs/{id}），再 GET /api/materials/{material_id} 取 manifest 和各通道图
```

要点：

- `workflow_id` 是 `出图段.分解段`：`src-upload-v1.dec-derive-v1`（传统派生，约 2~3 秒）、`src-upload-v1.dec-chord-v1`（CHORD，
  热的时候约 2~3 秒，冷启动要加载 2.7 GB 模型约 16 秒；**产物不可商用**，manifest 的 `license_flags` 会写明）。
- `source_image` 必须是 `/api/uploads` 返回的 `name`。
- 超过一个任务时后端只有一个 worker，按顺序排队；换工作流时它会先让 ComfyUI 卸载上一条管线的模型。
- 产物在后端的 `output\<material_id>\`，也可以直接走 `/api/materials/{id}/files/{name}`。

## 踩过的坑

**ComfyUI 的输出重定向进文件时，必须强制 Python 用 UTF-8（`PYTHONUTF8=1`、`PYTHONIOENCODING=utf-8`）。**
输出不进控制台时 Python 按系统代码页编码，中文 Windows 上是 936/GBK。TextureAlchemy 的节点会 `print` 带「✓」的进度行，GBK 编不了
-> 抛 `UnicodeEncodeError`；更糟的是 ComfyUI 处理这个异常时又往日志里写，同样编不了，第二次异常把执行线程带进死路：
**任务永远停在「生成中」，不报错也不结束**，和显存被占满的假死一模一样，极难想到是编码。后端的卡死看门狗要 8 分钟才会打断。
实测踩过：第一版没设这两个变量，传统派生任务卡住不动，设了之后 2 秒完成。

**不要给 `Start-Process` 加 `-RedirectStandardOutput` / `-RedirectStandardError`。** 理由和固定数据后端一样：一重定向，.NET 用
`bInheritHandles=true` 建进程，调用者交给脚本的 stdout/stderr 管道（`| cat`、CI、`Test.ps1` 的子进程捕获）会被 ComfyUI 一并继承，
脚本退出后 ComfyUI 还活着，管道就一直不关，调用者读输出读不到 EOF——「Start 明明返回了，调用它的东西却挂住」。
所以 ComfyUI 是**经 `cmd.exe /c` 起的，由 cmd 把输出重定向进 `logs\comfy.log`**，句柄只在 cmd 与 python 之间，不泄漏。
代价是 pid 文件里登记的得是 python 而不是 cmd：脚本等 cmd 的子进程里出现 `python.exe` 再登记（cmd 的 pid 也记着，停的时候收尾）。
后端本身不重定向，自己写 `logs\dreamtexture.log`。

**拆联接绝不能递归删。** `Remove-Item -Recurse` 作用在联接上是对**目标**的递归删除，一旦行为不一样就会删穿进整合包的节点包或后端仓库。
所以用 `[System.IO.Directory]::Delete(path)`（非递归，对联接删的是联接本身），并且先确认条目类型确实是 `Junction`。

**后端看到 ComfyUI 就绪比 ComfyUI 自己应答晚一拍**（后端有自己的健康巡检），所以 Start 要等**后端**的 `/api/comfy/status`
也报 `ready`，而不是只看 ComfyUI 的 `/system_stats`——不然测试一提交就撞上「ComfyUI 不可用，任务排队等待」。

**自研包的联接指到旧代码时测试会莫名失败。** `DT_ChordMaterialEstimation` 没注册的话，走 CHORD 的任务在 ComfyUI 校验期就报
节点缺失，错误离根因很远。所以 Start 在 ComfyUI 起来后主动查这三个节点注册了没有，没有就当场失败并指出联接指向哪里。

**「不平铺」不是「跳过环绕节点」。** 本管线的 `tileable=false` 靠显式把 `SeamlessTile` / `MakeCircularVAE` 的 `tiling` 设成
`disable`、CHORD 包装节点设成 `disable` 来关环绕填充。`SeamlessTile` 改的是**模型对象本身**，而模型对象会被 ComfyUI 缓存、
跨任务复用；只摘掉节点的话，上一个平铺任务留在模型上的环绕状态会原样带进这一个。这对写测试的人的含义是：同一个 ComfyUI 里
先跑 `tileable=true` 再跑 `tileable=false`，后者必须和「冷启动后第一个不平铺任务」逐像素一致（后端实测差 0.00000）。
测试若要比较平铺 / 不平铺的产物，不用担心顺序污染；若发现污染，是后端或包装节点的 bug，不要在测试里绕。
细节见后端仓库 `docs/comfyui-notes.md` 的「不要平铺时」。

**`source_image` 传了不是文件的名字，ComfyUI 不会报错，而是卡住。** 实测把 `input\` 下一个子目录名（`3d`）当 `source_image` 传给
`LoadImage`，任务停在节点 1，后端看门狗 8 分钟后才判定卡死并打断。始终用 `/api/uploads` 返回的名字。

**显存阈值和「已经在跑」。** 阈值只在冷启动前查；脚本发现它已经在跑就直接返回，不会因为它自己占着显存而把自己拒绝。
反过来，测试编辑器（`-nullrhi` 无头）几乎不占显存，所以不必为它留余量。

## 实测（RTX 4070 Ti SUPER 16GB）

| | |
| --- | --- |
| Start（冷启动） | 17.5~21 秒（ComfyUI 15~16 秒，后端 0.5 秒） |
| Stop | 12~13 秒（含等端口、等显存） |
| 起来之后空闲显存 | 比启动前少约 150~200 MiB（ComfyUI 空闲，没有载模型） |
| `src-upload-v1.dec-derive-v1`，`tileable=false` | 2~3 秒 |
| `src-upload-v1.dec-chord-v1`，`tileable=false` | 热 2~3 秒；冷启动约 16 秒（加载 CHORD） |
