# DreamTexture for Unreal reference

DreamTexture for Unreal is an editor-only plugin that connects the Unreal Editor to a running
[DreamTexture backend](https://github.com/TypeDreamMoon/DreamTexture). It imports the backend's PBR material
sets into the project, generates new ones, and sends materials already in the project back to the backend
to fill in missing channels or restyle them.

| | |
| :-- | :-- |
| Version | `0.9.0` (beta) |
| Engine | Unreal Engine `5.8`, Win64 |
| Modules | `DreamTextureClient`, `DreamTextureAssets`, `DreamTextureEditor` (all Editor) |
| Project settings | *Project Settings ▸ Plugins ▸ DreamTexture* |
| Panel | *Tools ▸ DreamTexture* |
| License | MIT |

New here? Start with **[Getting started](getting-started.md)**.

## Pages

| | |
| :-- | :-- |
| [Getting started](getting-started.md) | Install the plugin, connect to a backend, import and assign a first set |
| [Backend](backend.md) | Installing and configuring the DreamTexture backend, attach and managed modes, VRAM, which backend version |
| [Library](features/library.md) | Browse, import, assign, license badges |
| [Generate](features/generate.md) | Generate material sets from the editor |
| [Reflow](features/reflow.md) | Fill missing channels, restyle, tileable and UV-unwrapped |
| [Push from Web](features/push.md) | *Send to UE* from the web UI |
| [Troubleshooting](troubleshooting.md) | Reading the three status levels, common failures |
| [Contributing](contributing.md) | Test host, running the tests, localization, releasing |

## How the pieces fit

```text
 Unreal Editor                      DreamTexture backend              ComfyUI
 ┌────────────────────┐   REST      ┌──────────────────┐   HTTP/WS   ┌─────────┐
 │ DreamTexture panel │ ──────────▶ │ queue, library,  │ ──────────▶ │ models, │
 │ (this plugin)      │ ◀────────── │ manifests, web UI│ ◀────────── │ nodes   │
 └────────────────────┘  event      └──────────────────┘             └─────────┘
                         stream (WS, receive only)
```

- The plugin does all work over REST. The WebSocket event stream is receive-only: it carries job progress,
  library changes and pushes.
- Three things can be up or down independently: the backend process, the event stream, and ComfyUI. The
  plugin shows them separately. See [Troubleshooting](troubleshooting.md#reading-the-three-status-levels).
- A material set is a folder in the backend's output directory with a `manifest.json` and the PNG channels.
  The plugin only ever reads what the manifest says.
