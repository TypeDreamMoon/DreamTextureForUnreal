# 固定数据的测试后端

给 `DreamTexture.Live.*` 自动化测试用的后端：还是真正的 DreamTexture 后端（Go 写的
`dreamtexture.exe`），但喂给它的东西全是固定的、隔离的。测试连的是真实的
HTTP / WebSocket 实现，而不是我们自己编的假服务，所以契约漂了会立刻红。

`dreamtexture.exe` 在哪儿，按 参数 `-BackendExe` → 环境变量 `DREAMTEXTURE_BACKEND_EXE` →
`Tools\LocalSettings.psd1` 的 `BackendExe` 的顺序找，都没有就失败并说明（见 [`../README.md`](../README.md#本机设置东西装在哪)）。

```powershell
pwsh -NoProfile -File Tools\FixtureBackend\Start-FixtureBackend.ps1   # 起，应答之后才返回
pwsh -NoProfile -File Tools\FixtureBackend\Stop-FixtureBackend.ps1    # 停
$env:DREAMTEXTURE_LIVE_URL = 'http://127.0.0.1:18777'                 # 测试靠这个变量决定跑不跑 Live.*
```

`Tools\Test.ps1 -Live` 把这三步包好了：先 Start，给子进程设环境变量，跑完无论成败都 Stop。

## 它隔离了什么

| 后端配置 | 值 | 为什么 |
| --- | --- | --- |
| `addr` | `127.0.0.1:18777` | 不是默认的 8777，所以能和你自己在用的后端同时开。 |
| `output_dir` | `Tests\Fixtures` 的**拷贝**（在状态目录的 `output\`） | `/api/materials/{id}` 和文件接口直接读磁盘，固定素材 `mat_20260827_777001_ref` 立刻可服务；用拷贝是因为后端可能往里写东西（收藏标记等），不能污染仓库里的固定素材。 |
| `data_dir` | 状态目录的 `data\` | SQLite 独立，不碰你自己后端的数据。每次冷启动都清空，起点一致（`-KeepData` 保留）。 |
| `workflows_dir` | 后端 `dreamtexture.exe` 同目录的 `workflows\`（只读） | 后端启动要载入工作流，缺了起不来。 |
| `comfy.mode` | `attach` | 只做健康检查，不管 ComfyUI 的生命周期。 |
| `comfy.base_url` | `http://127.0.0.1:18779`（没人监听） | **这样不会拉起 ComfyUI**：ComfyUI 和虚幻编辑器会抢显存，测试期间不能让它们碰头。 |
| `comfy.auto_restart` | `false` | 同上，连「掉了自动拉起」也不要。 |

路径全部用绝对路径：后端把相对路径解析到「配置文件所在目录的上一级」（见后端 `docs/desktop-shell.md`），
状态目录里没有那一级的约定，写绝对路径省得算。

状态目录默认是宿主工程的 `Saved\FixtureBackend\`（脚本从自己所在位置往上找 `.uproject`；
不在宿主里就退到 `%TEMP%\DreamTextureFixtureBackend\`，绝不往仓库里写）。里面有：

```
fixture.json            生成的后端配置
backend.pid             登记起的进程（pid、启动时间、映像路径）
output\                 Tests\Fixtures 的拷贝
data\                   SQLite
logs\dreamtexture.log   后端自己的日志；起不来时先看这个
```

## 行为约定

两个脚本**无参数、可反复调用**——连接测试会在测试中途自己调 Stop 再调 Start，来验证断线重连。

**Start**

- 已经在跑且 `/api/health` 应答 → 什么都不做，返回 0。
- 没在跑 → 重铺 `output\` 与 `data\`，起进程，写 pid 文件，轮询 `/api/health` 直到 `{"ok":true}`
  才返回 0。应答之前进程退出、或 30 秒（`-TimeoutSeconds`）没应答 → 杀掉半死的进程、清 pid、
  打印日志末尾、返回 1。
- 登记的进程活着却不应答 → 给 10 秒宽限，仍不应答就杀掉重启。
- 端口被别的进程占着 → 说清楚是谁（pid、进程名、映像路径），返回 1，**不动那个进程**。
  是个 `dreamtexture.exe` 的话会额外提示它多半是 pid 文件丢了之后遗留的孤儿。

**Stop**

- **只**停 pid 文件里登记的那个进程，并且先核对它的映像确实是 `dreamtexture.exe`、启动时间对得上。
  pid 会被系统复用，pid 文件可能是上次崩溃遗留的，指着一个无关进程——核对不过就不杀，只清掉过期的 pid 文件。
- 没在跑也返回 0。
- 停完会等进程退出、端口释放再返回，所以紧接着的 Start 不会撞上还没放手的端口。

退出码：Start `0` 后端在跑且应答 / `1` 没做到；Stop `0` 后端已不在运行 / `1` 杀不掉或端口没释放。

## 踩过的坑

**不要给 `Start-Process` 加 `-RedirectStandardOutput` / `-RedirectStandardError`。**
一重定向，.NET 就用 `bInheritHandles=true` 建进程，调用者交给本脚本的 stdout/stderr 管道会被后端一并继承；
脚本退出后后端还活着，管道就一直不关。调用者（`| cat`、CI、`Test.ps1` 的子进程捕获）读输出读到 EOF
永远等不到——表现为「Start 明明返回了，调用它的东西却挂住」，一直挂到后端被停。不重定向时 `Start-Process`
走 ShellExecute，不继承任何句柄。后端自己会把日志写进 `logs\`，够用。

## 端点实测

起来后（后端 `internal/api/server.go` 的路由）：

| | 结果 |
| --- | --- |
| `GET /api/health` | `200`，`{"ok":true}` |
| `GET /api/materials` | `200`，`{"fts":true,"materials":[]}`——**空数组 `[]`，不是 `null`**。列表走 SQLite 索引，固定素材只在磁盘上，没入索引，所以是空的。 |
| `GET /api/materials/mat_20260827_777001_ref` | `200`，`{"index":null,"manifest":{…}}`——manifest 来自磁盘；`index` 是 `null`（同样因为没入索引），解析时要容忍。 |
| `GET /api/materials/mat_20260827_777001_ref/files/basecolor.png` | `200`，10601 字节，等于 manifest 里的 `bytes`。 |
| `GET /api/ws` | 能握手（`ws://127.0.0.1:18777/api/ws`）；心跳是每 15 秒一个 `{"type":"hb"}` 文本帧。 |

ComfyUI 没连上，所以 `/api/checks`、`/api/comfy/status` 之类会如实报「未就绪」，这是预期的——
正好用来测「后端在、ComfyUI 不在」这一级状态。
