# Backend

> [Reference](index.md) » **Backend**

The plugin talks to the [DreamTexture backend](https://github.com/TypeDreamMoon/DreamTexture): a single Go
program that queues jobs, drives ComfyUI, stores the material library and serves a web UI. This page covers
what the plugin needs from it. The backend's own documentation (`README.md` and `docs/` in its repository)
is the reference for everything else.

## Install the backend

Requirements: Go 1.26 or newer. The web UI is already built and committed in the repository, so Node is not
needed to build the backend.

```powershell
git clone https://github.com/TypeDreamMoon/DreamTexture.git
cd DreamTexture
go build -o dreamtexture.exe ./cmd/dreamtexture
./dreamtexture.exe -config configs/dreamtexture.json
```

Open <http://127.0.0.1:8777>. The backend starts even when ComfyUI is not installed or not running, so the
web UI is there to help you set it up.

The web UI's source and a Windows desktop app (a Tauri shell that wraps the same web UI in a native window and ships the backend with it) are in a separate repository, [DreamTextureDesktop](https://github.com/TypeDreamMoon/DreamTextureDesktop). The backend repository carries a prebuilt copy of the web UI, so the backend exe alone still serves it.
The plugin does not care which one you use. If a backend is already answering on the configured address, the
plugin attaches to it.

## Install ComfyUI

The backend drives a ComfyUI. Two ways to get one:

- **One-click deploy.** In the web UI, *Settings ▸ ComfyUI environment ▸ one-click deploy* installs a
  separate ComfyUI with its own Python and a CUDA PyTorch (using [uv](https://docs.astral.sh/uv/), which
  must be installed first) next to the backend. It also fetches the four node packs the pipelines need.
  It never touches a ComfyUI you already have; you switch over by choosing *Apply to config*.
- **Your own ComfyUI.** Point the backend at it (see below). The pipelines need these node packs:
  ComfyUI-Chord, ComfyUI-seamless-tiling, ComfyUI-TextureAlchemy, and the backend's own
  `ComfyUI-DreamTexture` (the `comfy_nodes/` folder of its repository).

The *Models* page of the web UI lists which models each pipeline needs, which are missing, and downloads
the ones that have a direct link. The *Environment check* button on the plugin's **Connection Status** page runs the
same self-check from the editor: ComfyUI connection, VRAM, nodes, models, output directory.

## Two meanings of attach and managed

The words are used twice, and the two are independent.

**1. The backend and ComfyUI** (`comfy.mode` in the backend's `configs/dreamtexture.json`):

| Mode | The backend... |
| :-- | :-- |
| `managed` | starts and watches the ComfyUI child process (needs `python` and `main_py` paths). Normal setup. |
| `attach` | connects to a ComfyUI you started yourself at `comfy.base_url`, and only health-checks it. |

With `attach`, ComfyUI will not start by itself. The **Connection Status** page says so when ComfyUI is down.

**2. The plugin and the backend:**

| | |
| :-- | :-- |
| **Attached** | A backend already answers at *Backend URL*. The plugin connects and **never stops it**, however it was started (desktop shell, a terminal, another editor). |
| **Launched** | Nothing answered and *Backend Executable* is set, so the plugin started `dreamtexture.exe` itself. It stops that process, and only that one, on *Stop Backend* or when the editor exits. |

Details of launching:

- The plugin checks `GET /api/health` first. Only if nothing answers does it start a process, then waits up
  to 90 seconds for the backend to answer.
- The process is placed in a Windows Job Object that kills it when the editor's handle closes. If the editor
  crashes, the backend and its ComfyUI do not linger.
- No console window is shown and no pipes are connected. The backend writes its own log under `logs/`.
- *Auto Start Backend* is off by default. Turn it on only if you want the backend, and with it ComfyUI,
  to start with every editor session.

*Backend Arguments* are appended to the command line unchanged. If you pass `-config <file>`, the `addr`
in that file must match *Backend URL*, or the process starts and nothing answers where the plugin looks.

## VRAM

The Unreal Editor and ComfyUI share one GPU. This is the most common cause of "generation never finishes".

- Local SDXL image generation needs about 8 GB free, CHORD 3 to 5 GB. A cloud base image plus the classic
  derivation needs very little. The plugin was developed on a 16 GB card.
- When VRAM runs out, the NVIDIA driver can silently fall back to system memory instead of failing. Jobs then
  run tens of times slower, with no error, and may never finish.
- The backend's `comfy.reserve_vram_gb` (default 1) makes ComfyUI swap models out early instead of waiting for
  the driver to degrade. With the editor open, 2 to 3 is a better value.
- The backend cancels a job that shows no activity for 8 minutes and puts the current VRAM figure in the error.
- The **Connection Status** page's *Environment check* reports free VRAM and names programs using the GPU, so you can
  see `UnrealEditor` in the list.

More in [Troubleshooting](troubleshooting.md#generation-never-finishes-or-is-very-slow).

## Cold starts

ComfyUI can take from tens of seconds to several minutes to start, and first runs of some custom nodes
install their dependencies. During this the **Connection Status** page shows *Starting (N s)*, not "not connected".
Just wait: restarting only starts the wait again.

If startup always takes minutes, the cause is usually ComfyUI-Manager fetching its registry. The backend's
`docs/comfyui-notes.md` explains the fix (`network_mode = private` in the Manager config).

## Which backend version

The plugin needs these backend features. They are on the backend's `master` branch; check that your
backend includes them rather than relying on a version number.

| Plugin feature | Needs from the backend |
| :-- | :-- |
| Library, Import, Generate | The base REST API and event stream (any backend with manifest v1). |
| **Push from Web**, and the *Push* row on the **Connection Status** page | The editor session and push endpoints: `/api/ue/sessions` (register, heartbeat, unregister, list) and `/api/materials/{id}/send-to-ue`, with `delivery_id` and heartbeat redelivery. |
| **Reflow: Fill missing channels** | The `src-upload-v1` image segment (decompose an uploaded base image) and `POST /api/uploads`. |
| **Reflow** records and **tileable** switch | `derived_from` on `POST /api/generate` (stored with the job) and the `tileable` parameter, backed by the CHORD wrap-padding switch in the backend's ComfyUI nodes. |
| Generate: whole batch accepted or rejected | A queue full returns HTTP 429 for the whole batch, with nothing left queued. |

On an older backend, the plugin disables the affected feature and says why, in place:

- Push: the *Push* row on **Connection Status** shows *Not supported by backend*. Upgrade and click *Reconnect*;
  the editor registers by itself.
- Reflow: the page says the backend does not offer the *Existing base image* segment or a local image
  segment with a reference image.

Everything else keeps working.
