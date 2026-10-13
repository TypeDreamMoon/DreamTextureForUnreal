# Getting started

> [Reference](index.md) » **Getting started**

This page takes a project from "plugin copied in" to "a generated material on a mesh".

## Install

1. Put the plugin in the project:

   ```powershell
   cd <YourProject>/Plugins
   git clone https://github.com/TypeDreamMoon/DreamTextureForUnreal.git
   ```

   or extract the `DreamTextureForUnreal` folder of a release zip into `<YourProject>/Plugins/`.

2. Open the project. Enable **DreamTexture for Unreal** in *Edit ▸ Plugins* if it is not already, and
   let the editor build it when asked. The plugin ships source only, so the machine needs a C++ toolchain
   that can build for UE 5.8 (Visual Studio with the C++ workload).
3. Confirm the settings page exists: *Project Settings ▸ Plugins ▸ DreamTexture*.

The plugin is editor-only. It adds no module to a packaged game and its content (the master material) is
generated into your `/Game` folder, so disabling the plugin later does not break materials already imported.

If your project has Substrate enabled, nothing needs to change: the master material is a traditional
material, and the engine converts it.

## Get a backend

The plugin does not generate anything itself. You need the DreamTexture backend and a ComfyUI it can drive.
[Backend](backend.md) covers installing it. The short version:

```powershell
go build -o dreamtexture.exe ./cmd/dreamtexture
./dreamtexture.exe -config configs/dreamtexture.json
```

Open <http://127.0.0.1:8777>. If the web UI loads, the backend is up. ComfyUI does not have to be ready
for browsing the library.

Check *Backend URL* in *Project Settings ▸ Plugins ▸ DreamTexture*. The default is `http://127.0.0.1:8777`,
which matches the backend's default `addr`.

## Open the panel

*Tools ▸ DreamTexture* opens the panel as a dockable tab. It has four pages: **Library**, **Generate**,
**Reflow** and **Connection Status**.

The strip across the top shows the three status levels:

| Level | Meaning |
| :-- | :-- |
| Backend process | The backend answers REST requests. |
| Event stream | The WebSocket to the backend is live (heartbeat every 15 seconds). |
| ComfyUI | ComfyUI is running and ready. |

Click the strip to go to the **Connection Status** page, which explains each level and has *Reconnect*, *Start Backend*,
*Stop Backend*, *Open Web UI*, *Settings* and *Environment check*.

## Import a set

1. Open the **Library** page. If it is empty, generate a set in the web UI or on the **Generate** page; it
   appears here by itself when the job finishes.
2. Select a material and click *Import*.
3. A notification reports how many textures were created, updated or unchanged, whether a material instance
   was built, and the license. Its button shows the result in the Content Browser.

Result, with the default *Import Root* of `/Game/DreamTexture`:

```text
/Game/DreamTexture/
├─ M_DreamTexture_Master            the master material, generated once per project
└─ <name>_<id tail>/
   ├─ T_<name>_<id tail>_BC         Base Color (sRGB)
   ├─ T_<name>_<id tail>_N          Normal
   ├─ T_<name>_<id tail>_ORM        packed: R = AO, G = Roughness, B = Metallic
   ├─ T_<name>_<id tail>_H          Height
   └─ MI_<name>_<id tail>           material instance of the master
```

Separate Roughness, Metallic and AO textures (`_R`, `_M`, `_AO`) are only imported when
*Import Separate Channels* is on; the master material samples the packed ORM texture.

## Assign it

Select an actor with a mesh component in the level, or a static mesh asset in the Content Browser, and click
*Assign to Selection* on the Library page. The dialog says how many material slots will be replaced and lets
you pick which slot. Assigning is one transaction: Ctrl+Z undoes all of it. Meshes that ship with the engine
are never modified.

## Generate

On the **Generate** page, pick an image model and a decompose model, write a prompt, and click *Generate*.
See [Generate](features/generate.md). If the backend is not running and *Backend Executable* is set, the
button becomes *Start backend and generate*.

## Settings

*Project Settings ▸ Plugins ▸ DreamTexture*, saved in the project's `Config/DefaultEditor.ini`:

| Setting | Default | What it does |
| :-- | :-- | :-- |
| Backend URL | `http://127.0.0.1:8777` | Where the backend listens. Must be `http://` or `https://`. Anything else falls back to the default. |
| Backend Executable | empty | Path to `dreamtexture.exe`, relative to the project directory or absolute. Lets the plugin start the backend. |
| Backend Arguments | empty | Appended to the command line as written, for example `-config "D:\DreamTexture\configs\my.json"`. The backend's `addr` must match *Backend URL*. |
| Auto Start Backend | off | Start the backend when the editor starts, if an executable is set and nothing answers. Off by default because ComfyUI takes VRAM. |
| Import Root | `/Game/DreamTexture` | Where imports and the master material go. |
| Import Separate Channels | off | Also import Roughness, Metallic and AO as separate textures. |
| Create Material Instance | on | Build or update `MI_<name>` after each import. |
| Auto Import Generated | off | Import variants from the Generate page as soon as they finish. |
| Reconnect Base / Max, Stale After | 1000 / 15000 / 40000 ms | Event stream reconnect backoff and the silence after which it counts as dead. Advanced; they match the web UI. |

## Next

- [Library](features/library.md)
- [Generate](features/generate.md)
- [Reflow](features/reflow.md)
- [Troubleshooting](troubleshooting.md) if something is red
