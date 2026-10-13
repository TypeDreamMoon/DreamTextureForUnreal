# DreamTexture for Unreal

**Generate PBR material sets with DreamTexture and use them in the Unreal Editor, without leaving it.**

<p>
  <img alt="Unreal Engine 5.8" src="https://img.shields.io/badge/Unreal%20Engine-5.8-313131" />
  <img alt="Version 0.9.0 beta" src="https://img.shields.io/badge/version-0.9.0%20beta-orange" />
  <img alt="License MIT" src="https://img.shields.io/badge/license-MIT-green" />
</p>

[中文文档](README.zh-CN.md) · [Documentation](Docs/index.md) · [Getting started](Docs/getting-started.md) · [Changelog](CHANGELOG.md) · [Issues](https://github.com/TypeDreamMoon/DreamTextureForUnreal/issues)

DreamTexture for Unreal is an editor-only plugin. It connects the editor to a running
[DreamTexture backend](https://github.com/TypeDreamMoon/DreamTexture), which turns a prompt into a full
PBR material set (Base Color, Normal, Roughness, Metallic, AO, Height, packed ORM) on a local ComfyUI.

From one panel you can browse the backend's library, import a set as correctly configured textures plus a
material instance, assign it to the selected objects, generate new sets, and send materials you already
have in the project back to the backend to fill in missing channels or restyle them. A material sent
from the web UI shows up in the editor with one click.

> [!NOTE]
> This is a **beta**. The plugin needs the DreamTexture backend and ComfyUI; it does not generate anything
> by itself. Some features need a recent backend; see [Backend](Docs/backend.md#which-backend-version).

<p align="center">
  <img alt="The Library page" src="./Images/library.png" />
</p>

## Features

| | |
| :-- | :-- |
| **Library** | Browse the backend's material library inside the editor: search, filter by style, favorites. One click imports a set. Details show the prompt, seed, resolution and **license**. [Library](Docs/features/library.md) |
| **Import** | A set lands as textures in `/Game/DreamTexture/<name>_<id>/` with the right compression and sRGB per channel. Importing the same set again downloads nothing; changed channels update in place, so existing materials do not break. |
| **Material instance and Assign** | Every import also builds `MI_<name>` from a master material the plugin generates in your project. *Assign to Selection* puts it on the selected actors or static meshes as one undoable transaction. |
| **Generate** | Write a prompt, pick an image model and a decompose model, get 1 to 8 variants. Progress comes from the same event stream the web UI uses. Optional cloud base image and prompt refinement. [Generate](Docs/features/generate.md) |
| **Reflow: Fill missing channels** | Pick any material in the project. The plugin finds which texture is which channel, sends only the Base Color to the backend, and gets back the Normal, Roughness, AO and Height it was missing. Your original Base Color stays. [Reflow](Docs/features/reflow.md) |
| **Reflow: Restyle** | Use the existing Base Color as a reference image, repaint it from a prompt, and decompose the result into a full set. |
| **Tileable and UV-unwrapped** | Reflow handles seamless tiling materials and UV-unwrapped textures (characters, props, trim sheets) separately; unwrapped textures get no seamless processing and a hard cap on repaint strength. |
| **Push from Web** | *Send to UE* on a material's page in the web UI imports it into a running editor. A push is not lost if the editor was reconnecting. [Push](Docs/features/push.md) |
| **Backend process hosting** | The plugin can start and stop the backend for you. It attaches to a backend that is already running and never stops one it did not start. [Backend](Docs/backend.md) |

<p align="center">
  <img alt="The Generate page" src="./Images/generate.png" />
</p>

<p align="center">
  <img alt="The Reflow page" src="./Images/reflow.png" />
</p>

<p align="center">
  <img alt="The Connection Status page" src="./Images/status.png" />
</p>

## Requirements

| | |
| :-- | :-- |
| Unreal Engine | **5.8**. Editor only; nothing is added to a packaged game. |
| OS | Windows (Win64). The plugin has not been built or tested elsewhere. |
| Backend | The [DreamTexture backend](https://github.com/TypeDreamMoon/DreamTexture) and a ComfyUI it can drive. The backend can install its own ComfyUI. Push, Reflow and the CHORD seamless switch need a recent backend; see [Backend](Docs/backend.md#which-backend-version). |
| GPU | An NVIDIA GPU. Developed and tested on 16 GB. Local SDXL needs about 8 GB free, CHORD 3 to 5 GB. A cloud base image needs far less. |
| Toolchain | A C++ toolchain that can build for UE 5.8 (Visual Studio with the C++ workload). The plugin is source only. |

The editor and ComfyUI compete for VRAM. With a large level open and a local model loaded, generation can
slow down to a crawl without any error. See [Troubleshooting](Docs/troubleshooting.md#generation-never-finishes-or-is-very-slow).

## Installation

**From git:**

```powershell
cd <YourProject>/Plugins
git clone https://github.com/TypeDreamMoon/DreamTextureForUnreal.git
```

**From a release:** download `DreamTextureForUnreal-<version>.zip` from the
[Releases](https://github.com/TypeDreamMoon/DreamTextureForUnreal/releases) page and extract the
`DreamTextureForUnreal` folder into `<YourProject>/Plugins/`.

Then open the project, enable **DreamTexture for Unreal** in *Edit ▸ Plugins* if it is not already, and let
the editor build the plugin when asked. The panel is under *Tools ▸ DreamTexture*.

## Quick start

1. **Start the backend.** Run `dreamtexture.exe`, then open <http://127.0.0.1:8777> to check that it is up.
   [Backend](Docs/backend.md) covers installing it. Or set *Backend Executable* in
   *Project Settings ▸ Plugins ▸ DreamTexture* and let the plugin start it.
2. **Open the panel** from *Tools ▸ DreamTexture*. The strip at the top shows three separate states:
   backend process, event stream, ComfyUI. Only the first one has to be green to browse the library.
3. **Import a set.** On the **Library** page, select a material and click *Import*. A notification tells
   you what was created and links to the Content Browser.
4. **Assign it.** Select an actor with a mesh in the level, or a static mesh in the Content Browser, and
   click *Assign to Selection*. The dialog says how many material slots will be replaced before anything
   changes. Ctrl+Z undoes all of it.

**Generate a new set.** On the **Generate** page, pick an image model and a decompose model, write a prompt
(*Refine prompt* expands it into one that works for tiling textures), choose the resolution and the number
of variants, and click *Generate*. When a variant finishes, click *Import*, or turn on *Import automatically when done*.

**Reflow an existing material.** Select a material instance (or a material slot on a selected actor), open
the **Reflow** page, and click *Get Selection*. Check the channels the plugin recognized, pick
*Fill missing channels* or *Restyle*, pick *Tileable* or *UV-unwrapped*, and click *Start*. By default the result
becomes a new texture set and a new instance; *Overwrite in place* edits the selected instance instead.

## Licensing: CHORD is research-only

The realistic pipeline can decompose with [CHORD](https://github.com/ubisoft/ComfyUI-Chord), which is released
by Ubisoft under a **research-only** license. **Materials decomposed with CHORD must not be used
commercially.**

The plugin marks them everywhere it can: a *Research use only* badge in the Library and on generated
variants, a warning on the *Generate* button before you submit such a combination, and the license line in
the import notification. Decompose with *Classic derivation* instead if the result has to ship.
The `license_flags` field in each material's manifest carries the same information.

Marking a material is a convenience, not a legal check. Read the licenses of the models you use.

## Documentation

| | |
| :-- | :-- |
| [Getting started](Docs/getting-started.md) | Install, connect, first import |
| [Backend](Docs/backend.md) | Installing the DreamTexture backend, configuration, attach and managed modes, VRAM |
| [Library](Docs/features/library.md) | Browse, import, assign |
| [Generate](Docs/features/generate.md) | Generate in the editor |
| [Reflow](Docs/features/reflow.md) | Fill missing channels, restyle, tileable and UV-unwrapped |
| [Push from Web](Docs/features/push.md) | Send to UE |
| [Troubleshooting](Docs/troubleshooting.md) | Reading the three status levels, common failures |
| [Contributing](Docs/contributing.md) | Test host, running the tests, localization, releasing |
| [Changelog](CHANGELOG.md) | |

## Development and testing

The repository root is the plugin root, so the plugin cannot build by itself. Development happens in a
small test host project that contains only this plugin, as a git worktree. The automated tests run
headless against a fixture backend:

```powershell
pwsh -NoProfile -File Tools\Test.ps1            # build and run every DreamTexture.* test
pwsh -NoProfile -File Tools\Test.ps1 -Live      # also run the tests that talk to a backend
```

[Contributing](Docs/contributing.md) has the details, including what must be run once before a release.

## Project info

| | |
| :-- | :-- |
| Version | `0.9.0` (beta) |
| Unreal Engine | `5.8` |
| Modules | `DreamTextureClient`, `DreamTextureAssets`, `DreamTextureEditor` (all Editor) |
| Author | TypeDreamMoon |
| Backend | <https://github.com/TypeDreamMoon/DreamTexture> |
| License | [MIT](LICENSE) |

## License

DreamTexture for Unreal is released under the [MIT license](LICENSE). That covers this plugin only. The
backend and the models it runs have their own licenses; see the note on CHORD above. For bug reports and
feature requests, open an [issue](https://github.com/TypeDreamMoon/DreamTextureForUnreal/issues/new).
