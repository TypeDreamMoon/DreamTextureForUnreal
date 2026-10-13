# Contributing

> [Reference](index.md) » **Contributing**

How to build, test, localize and release the plugin.

| | |
| :-- | :-- |
| Repository | <https://github.com/TypeDreamMoon/DreamTextureForUnreal> |
| Engine | Unreal Engine `5.8` (Launcher build 5.8.2 is what development uses), Win64 |
| License | MIT |

The repository root **is** the plugin root: `DreamTextureForUnreal.uplugin` is in the top directory and there is no
`.uproject`. The plugin cannot build by itself; it needs a host project.

## Modules

Dependencies only go one way: **Client ← Assets ← Editor**.

| Module | Does | Rule |
| :-- | :-- | :-- |
| `DreamTextureClient` | Talks to the backend: REST, WebSocket, manifest parsing, downloads, process hosting, push session. | No dependency on UnrealEd or any Slate module, so it can run in automation, commandlets or Python. |
| `DreamTextureAssets` | Asset side: texture import, master material and instances, assignment, material analysis, export, Reflow planning. | May use UnrealEd. No Slate. |
| `DreamTextureEditor` | The panel, pages, menus, notifications. | |

Tests are in each module's `Private/Tests/` as `*Tests.cpp`, with test paths starting `DreamTexture.`
(`DreamTexture.Client.Manifest`, `DreamTexture.Assets.Import`, ...). Fixed material data is in
`Tests/Fixtures/<material id>/`: a 64x64 set whose manifest sizes and hashes were recomputed by the backend's
algorithm.

## Conventions

### Source encoding

**`.h`, `.cpp` and `.cs` files are UTF-8 with BOM.** This is not a style choice; a missing BOM breaks the build in a
way that is hard to trace. UE 5.8's UBT does not pass `/utf-8` to MSVC, so a file without BOM is read in the system
code page (936/GBK on Simplified Chinese Windows). A Chinese character in a comment can end in the byte `0x5C`
(`\`), which swallows the line break and pulls the next line of code into the comment; the compiler error then points
somewhere else entirely.

After adding any of those files, run:

```bash
bash Tools/ensure-bom.sh
```

It is idempotent and only touches files without a BOM. If UBT happens to have a file open, its `mv` can fail with
`Device or resource busy`; that file stays without a BOM, so wait a moment and run it again. `.ps1` files in `Tools/`
are also BOM files (Windows PowerShell 5.1 reads BOM-less UTF-8 in the system code page). Markdown, JSON and Python
files do not need one.

### Code style

- Tabs for indentation, as in the engine. UE naming: `F` plain classes, `U` UObjects, `S` Slate widgets, `E` enums,
  `I` interfaces; PascalCase for functions and variables.
- Every source file begins with `// Copyright TypeDreamMoon. All Rights Reserved.`
- Comments in source are written in Chinese and explain **why**, including the pitfalls hit. That is part of
  what the project documents. Pull requests may write them in English.
- Helpers in an anonymous namespace carry a file-specific prefix, and test files never write `using namespace`.
  Both are invisible in incremental builds (see *Before a release*) and collide in a unity build.
- Forward declarations of engine types (`class FJsonObject;`) go in the global scope. Written inside the plugin's own
  namespace they declare a different type. `TWeakObjectPtr` needs `UObject/WeakObjectPtr.h`.
- JSON from the backend is parsed tolerating `null`: Go serializes a nil slice as `null`, not `[]`. Do not assume an
  array field exists.
- The event stream is receive-only. Operations go through REST, however convenient it looks to send a command on it.

### Backend contract

The backend lives in its own repository, <https://github.com/TypeDreamMoon/DreamTexture>. These values and formats
are fixed on both sides; do not invent a second set. If one has to change, change it in both repositories together.

| Item | Value |
| :-- | :-- |
| Event stream heartbeat | The backend sends a `{"type":"hb"}` text frame every 15 seconds (`HeartbeatInterval` in `internal/api/websocket.go`). |
| Stale detection | 40 000 ms without any message means *Timed out* (`STALE_AFTER` in `web/src/store.ts`). The watchdog watches **messages**, not the close event: a half-dead connection never fires one. |
| Reconnect backoff | Starts at 1000 ms, doubles, capped at 15 000 ms, reset once connected (`connectEvents()` in `web/src/store.ts`). |
| Material format | `docs/manifest-v1.md` in the backend repository. Frozen. |
| `sha256` in a manifest | The SHA-256 of the **whole file**, as lowercase hex, **first 16 characters** (`internal/material/manifest.go`). |
| Normals | Manifests state `y: directx`; both pipelines are already normalized to it. The green channel is flipped only when `y` is `opengl`, which the backend has not produced so far. |
| Responses and events | `comfy.Health` (`internal/comfy/supervisor.go`), `/api/checks` (`internal/api/checks.go`), event types (`internal/job/events.go`), material list, detail, file and favorite handlers (`internal/api/server.go`). |

The plugin reports three separate levels of state and they must not be merged into one lamp: whether the backend
process answers (`IsBackendReachable()`), whether the event stream is connected (`GetLink()`), and whether ComfyUI is
ready (`GetHealth()`). The backend starts and answers even when ComfyUI cannot start; showing "ComfyUI not ready" as
"cannot connect" sends people to restart the wrong thing.

**Push protocol** (user-facing description in [Push from Web](features/push.md)). The editor's event stream stays
receive-only; sessions are registered over REST.

| Request | Meaning |
| :-- | :-- |
| `POST /api/ue/sessions` `{"name","project","engine","pid"}` returns `{"session_id"}` | Register after connecting. `name` is what the web UI lists. |
| `POST /api/ue/sessions/{id}/heartbeat`, optional body `{"ack":["<delivery_id>",...]}` | Every 15 seconds. The backend drops a session after 45 seconds without one. Replies **200** `{"pending":[{"delivery_id","material_id","sent_at"}]}`: deliveries not yet acknowledged, kept for 10 minutes. HTTP 404 means the session is gone and the plugin registers again. |
| `DELETE /api/ue/sessions/{id}` | Unregister when the editor closes, best effort. |
| `GET /api/ue/sessions` | `{"sessions":[{"session_id","name","project","engine","last_seen"}]}`, `[]` if none. |
| `POST /api/materials/{id}/send-to-ue` `{"session_id"?}` | Without `session_id` and with exactly one session online, it goes there; none online is 409; several is 409 with the list. Success is 202 with a `delivery_id`. |

The push itself is an event on the stream that every subscriber receives, and the plugin filters by its own session id:
`{"type":"ue.import","data":{"session_id","material_id","delivery_id"}}`. The stream alone can lose a push (the editor
is in reconnect backoff while the session is still alive), so the plugin also applies deliveries listed in heartbeat
replies. It deduplicates by `delivery_id`, handles each once, and acknowledges it in the next heartbeat.

Two details of the HTTP side that cost time before: multipart uploads use `IHttpRequest::SetContent(TArray<uint8>&&)`
with a `Content-Type` header that carries the `boundary`, and the backend reads the file by field name
(`r.FormFile("image")`), so another name is a 400. The backend keeps `derived_from` in the job's parameters
(`__derived_from`), and `GET /api/jobs/{id}` returns it, which is how tests check that the backend recorded it.

## Test host and worktree

Development uses a small project that contains only this plugin, with the plugin checked out into it as a **git
worktree**. That keeps your own project, its editor, DLLs and engine lock out of the way, and reveals dependencies
the plugin forgot to declare, since no other plugin is enabled.

```powershell
pwsh -NoProfile -File Tools\TestHost\New-DreamTextureTestHost.ps1 -WhatIf            # show what it would do
pwsh -NoProfile -File Tools\TestHost\New-DreamTextureTestHost.ps1 -KeepCurrentHead   # host already on a dev branch
pwsh -NoProfile -File Tools\TestHost\New-DreamTextureTestHost.ps1 -Detach            # new host following master
```

The script is idempotent and never deletes, resets or switches branches in an existing worktree. It
creates the host from `Tools/TestHost/Template/`, which sits outside `Source/` so no project that installs the
plugin picks up its `.Build.cs` or `.Target.cs`. **Do not commit the host's `.uproject` to this repository.**
Details and the full parameter list: [`Tools/TestHost/README.md`](../Tools/TestHost/README.md).

Build only the Editor target. The Game target is monolithic and would compile the whole engine into the host:

```powershell
& '<Engine>\Engine\Build\BatchFiles\Build.bat' DreamTextureTestHostEditor Win64 Development `
  '-Project=<Host>\DreamTextureTestHost.uproject' -WaitMutex -NoHotReloadFromIDE
```

Do not build while that host's editor is open: it holds the plugin DLLs and takes VRAM.

### Local settings

The scripts in `Tools/` do not assume where anything is installed. Each value is looked up in this order, and the
first hit wins:

1. the script's own parameter (`-Project`, `-EngineRoot`, `-BackendExe`, ...);
2. an environment variable (`DREAMTEXTURE_ENGINE_ROOT`, `DREAMTEXTURE_BACKEND_EXE`, `DREAMTEXTURE_COMFY_ROOT`);
3. `Tools/LocalSettings.psd1`, an untracked file; copy `Tools/LocalSettings.example.psd1` and fill in what you need;
4. an automatic default where one exists: the host project is the `.uproject` two directories above the plugin
   (`<host>/Plugins/DreamTextureForUnreal`), and the engine is found from that project's `EngineAssociation` (the Launcher
   registry key `HKLM:\SOFTWARE\EpicGames\Unreal Engine\<version>`, or `HKCU:\Software\Epic Games\Unreal Engine\Builds` for a
   source build);
5. otherwise the script stops and names the parameter, variable and settings key to set.

With the plugin checked out into a host project and a Launcher engine installed, nothing needs to be configured.

## Running the tests

`Tools/Test.ps1` checks the localization data first (a few seconds, no engine needed), then builds the host, runs the
automation tests headless with `UnrealEditor-Cmd`, and prints a result table. A run exits `0` only if something really
ran and nothing failed.

```powershell
pwsh -NoProfile -File Tools\Test.ps1 -LintOnly                               # only the localization checks, same as CI
pwsh -NoProfile -File Tools\Test.ps1 -BuildOnly                              # does it compile
pwsh -NoProfile -File Tools\Test.ps1                                         # localization checks, build, and every DreamTexture.* test
pwsh -NoProfile -File Tools\Test.ps1 -SkipBuild -Filter DreamTexture.Client  # a subset, no build
pwsh -NoProfile -File Tools\Test.ps1 -Live                                   # also the tests that talk to a backend
pwsh -NoProfile -File Tools\Test.ps1 -Pipeline                               # isolated ComfyUI + backend, real decompose
pwsh -NoProfile -File Tools\Test.ps1 -Render                                 # real RHI: compile and render the master material
```

| Switch | Does |
| :-- | :-- |
| `-Filter` | Prefix given to `Automation RunTests`. Default `DreamTexture`. |
| `-SkipBuild` / `-BuildOnly` | Skip the build / only build. |
| `-LintOnly` / `-NoLint` | Run only the localization checks (no host project or engine needed) / skip them. They run by default, before the build; a failure exits `4` and nothing is built. Python 3 and PowerShell 7 are needed; a machine without Python uses `-NoLint`. (`-Lint` is still accepted and does nothing extra.) |
| `-Live` | Starts the **fixture backend** (`Tools/FixtureBackend/`, port 18777, ComfyUI deliberately absent) and sets `DREAMTEXTURE_LIVE_URL`. Runs `DreamTexture.Live.*`. |
| `-Pipeline` | Starts an **isolated ComfyUI** (18188) and a new backend (18787) (`Tools/PipelineBackend/`) and runs `DreamTexture.Pipeline.*`: real generation and reflow, down to which texture a material instance parameter points at. Needs about 10 GiB of free VRAM; it refuses to start with less, and never touches your own ComfyUI or backend. |
| `-Render` | Starts the editor **without** `-nullrhi`. Compiles every switch combination of the master material and renders a 3x3 tiled preview. Catches sampler type mismatches and Substrate conversion errors. |
| `-TimeoutMinutes`, `-ExtraEditorArgs`, `-Project`, `-EngineRoot` | The test process limit (20), extra editor arguments, host project, engine (see *Local settings*). |

How the result is judged:

- **No test found is a failure**, not a pass. A mistyped filter is the easiest way to get a false green.
- All tests skipped is a failure. A test that passes with a message starting with the skip marker (for example
  `DreamTexture.Live.*` with no `DREAMTEXTURE_LIVE_URL`) is shown as skipped.
- An editor crash, a timeout or a missing report is a failure.
- A test against a backend that does not support an endpoint (404 or 501) skips and says so, so the whole suite
  also runs against an older backend.

Exit codes: `0` pass, `1` test failure (including zero tests, all skipped, crash), `2` build failed, `3` environment
(project, engine, backend could not start, or no Python without `-NoLint`), `4` the localization checks failed.

The test editor never connects to a real backend on port 8777: `Test.ps1` overrides `BackendURL` on the command
line from the first moment (a dead address, the fixture backend, or the pipeline backend). Do not bypass it by
starting the editor yourself for Live tests.

Test backends can use another executable: the `BackendExe` setting, or `DREAMTEXTURE_BACKEND_EXE`, set to a
`dreamtexture.exe` (needed when the plugin depends on backend changes your installed one lacks). `Tools/LocalSettings.psd1`
can also give the pipeline backend its own executable (`PipelineBackendExe`) and the ComfyUI install it isolates a copy
of (`ComfyRoot`). Logs and reports of each run are under the host's
`Saved\DreamTextureTest\<timestamp>\`.

Rules for tests:

- A test's full name must not be a prefix of another test's name, or `RunTests <prefix>` silently misses it. Give
  leaf tests one more segment (`...LibraryView.Panel`).
- Change texture properties between `PreEditChange(nullptr)` and `PostEditChange()`. With a real RHI textures
  compile asynchronously and an unbracketed change logs an error. Only a non-`-nullrhi` run shows it.
- Do not keep pointers into temporary arrays; an access violation takes the whole editor down and the report
  shows nothing for every later test.
- Tests that need a backend (`DreamTexture.Live.*`) run only when `DREAMTEXTURE_LIVE_URL` is set and otherwise skip with a
  message that starts with the skip marker. They run against the fixture backend, never against a backend someone is using.
- Material data for tests is under `Tests/Fixtures/`. Do not edit a fixture by hand: its `bytes` and `sha256` must match
  what the backend's algorithm computes.
- A test that provokes a failed save has to call `AddExpectedError` first: the engine logs `LogSavePackage` as an error,
  and the automation framework counts any logged error as a failure.

## UE 5.8 pitfalls

All of these were hit and verified (one is marked as read from the engine source only). The code involved usually
carries a comment as well. Each entry is the trap, then what happens, then what to do.

### Platform, HTTP and threading

- **`FPlatformMisc::GetSHA256Signature` has no Windows implementation.** It falls to a generic stub whose first line is `checkf(false)`. Use the plugin's `Private/DreamTextureSha256`.
- **`IHttpRequest::ProcessRequest()` that cannot send calls the completion callback synchronously, then returns false.** Handling the return value as well runs the completion twice. New code goes through `SendRequest` in Client, which calls back exactly once, on the next frame.
- **`FTSTicker::RemoveTicker` called from the ticker's own callback deadlocks.** Reset the handle (`Handle.Reset()`) or set a flag and return false from the callback.
- **`FString::operator==` ignores case.** Use `Equals(..., ESearchCase::CaseSensitive)` where case matters.
- **`FGenericPlatformHttp::UrlEncode` breaks surrogate pairs (emoji)**, judged from reading the code, not tested. Use `PercentEncode` in Client. Surrogate checks use `StringConv::IsHighSurrogate` (there is none on `FChar`).
- **`FPlatformProcess::CreateProc` without pipes does not inherit handles.** Keep that in mind when a child must see a handle.
- **Connecting to a local port nobody listens on takes about 2 seconds to be refused on Windows**, not zero. A health probe timeout shorter than that reports a live backend as down.
- **`FPaths::ProjectDir()` is a relative path in the editor.** Make it absolute before building paths for `RelativeToGameDir`.
- **Module shutdown must not touch UObjects, the settings CDO included.** In a GUI editor the plugin module shuts down after the UObject system, and `GetDefault<>()` returns a dangling pointer; the push session crashed that way on exit. Cache what shutdown needs while running. The automation editor shuts down in another order and cannot show this: check with *File ▸ Exit* in a GUI editor.
- **A module you first use after your own startup is unloaded before you.** Modules unload in reverse load order, and a
  module's place in that order is recorded when its `StartupModule` returns, so modules loaded *inside* your
  `StartupModule` unload after you, and modules first loaded later (on the first request, the next frame) unload before
  you. A `Build.cs` dependency does not change this. If the shutdown path then calls into such a module, the engine tries
  to load it again and asserts (`!ModuleInfo->bWasUnloadedAtShutdown`). `FLwsWebSocket::Close()` does exactly that via
  `FWebSocketsModule::Get()`, which crashed the editor on exit. Load every module the shutdown path needs in
  `StartupModule` (Client loads `WebSockets` and `HTTP` first). The stock Launcher editor hides the problem because the
  default-enabled `OnlineSubsystem` plugin loads `HTTP`, `XMPP` and through it `WebSockets` early; see *Before a release*
  for how to reproduce a project without it.
- **`TMap<FString, TUniquePtr<T>>` inside an exported class (`*_API`) does not compile**, because the compiler instantiates the implicit copy. Delete the copy constructor explicitly. A nested struct in an exported class needs its own `*_API` when a default argument refers to its static functions, otherwise LNK2019, since default arguments are evaluated in the caller's module.
- **`FSlateDynamicImageBrush` is a `struct`.** Forward declaring it as `class` gives C4099, which is an error here.
- **The `Settings` module can only be referenced with `PrivateIncludePathModuleNames`**; UnrealEd does not re-export it. Persisting default config uses `UObject::TryUpdateDefaultConfigFile()`.
- **Package metadata is `FMetaData`** (`UPackage::GetMetaData()` returns a reference) and sits entirely inside `#if WITH_METADATA`; saving for cook does not write it.

### Slate

- **Starship `Icons.Warning` and `Icons.Error` are the same image with a different tint.** Use `Icons.X` for "broken". There is no `Icons.Stop`; use `Icons.Toolbar.Stop`.
- **`FStyleColors::Warning`, `Success` and `Error` already are `FSlateColor`.** Do not wrap them again.
- **`SComboBox::SetSelectedItem` takes one argument and fires `OnSelectionChanged` with `ESelectInfo::Direct`.** That is how to tell a programmatic set from a user click.
- **`SlatePrepass()` works under `-nullrhi`**, so panel layout and property callbacks can be verified in automation.

### Materials and textures

- **Some material expression APIs are not exported, and the failure is LNK2019 at link time.** `UMaterialExpressionMakeMaterialAttributes::GetExpressionInput` and `UMaterialExpressionBreakMaterialAttributes::PropertyToIOIndexMap` are the known ones. Read the public member directly, or copy the index table (see `DreamTextureMaterialGraph.cpp`).
- **`UMaterial::GetExpressionInputForProperty` returns an input even for properties that are not in effect**, and `IsPropertyActive` depends on Substrate (with Substrate on and hidden conversion off, every legacy pin counts as inactive). Material analysis therefore does not prune with either.
- **`UMaterialExpressionTextureObjectParameter` derives from `UMaterialExpressionTextureSampleParameter`, i.e. from `TextureSample`.** `Cast<UMaterialExpressionTextureSample>` also catches it; test for it first.
- **On a material function call node, `FunctionInputs[i].ExpressionInput` and `FunctionOutputs[i].ExpressionOutput` are transient pointers** that can be null right after loading. Look them up inside the function by `ExpressionInputId` / `ExpressionOutputId`.
- **A channel is really selected where an `FExpressionInput`'s own mask and the connected output's mask intersect**, as the engine computes it at compile time.
- **`UMaterialInterface::GetMaterialResource(ERHIFeatureLevel)` is deprecated and always returns `nullptr`.** Using it to look for compile errors yields a false "no errors". Use the `EShaderPlatform` overload.
- **Material shaders are submitted for compilation on demand.** After `PostEditChange` / `FinishAllCompilation` the shader map can be an empty shell (0 shaders measured). Do what `UMaterialEditingLibrary::GetStatistics` does: if the map is incomplete, `SubmitCompileJobs_GameThread`, then `FinishCompilation`. Under `-nullrhi` nothing is compiled at all.
- **`UMaterialEditingLibrary::DeleteAllMaterialExpressions` deletes while iterating and skips every other expression** (half remain). Copy the array first, then delete one by one.
- **`GetSamplerTypeForTexture` / `VerifySamplerType` are deprecated;** use the same-named functions in `MaterialExpressionUtils::`. The sampler type for a height map (`TC_Grayscale`, sRGB off) is `SAMPLERTYPE_LinearGrayscale`, not `Grayscale`.
- **A material instance holds its own `UsageFlags` in 5.8.** When the master lacks a usage flag, the engine adds it and marks dirty the **instance's** package. Preset the flags on the master so instances inherit them.
- **`SetMaterialInstanceStaticSwitchParameterValue` creates a `UMaterialEditorInstanceConstant` as a side effect.**
- **Whether a static switch exists: use `GetAllParametersOfType(EMaterialParameterType::StaticSwitch, Map)`.** `GetMaterialInstanceStaticSwitchParameterValue` returns false for a switch that does not exist, so "off" and "missing" look the same.
- **`GetTextureParameterValue(..., bOveriddenOnly=false)` returns the effective value**, including the engine's default placeholder texture on a master parameter. To know whether the user attached a texture, exclude engine content or pass `bOveriddenOnly=true` (the parameter is spelled that way in the engine).
- **An instance created by the plugin that lacks a texture has the matching static switch (`UseNormal` / `UseORM`) off.** Replacing only the texture parameter in place changes nothing visible: "overwrite succeeded, material unchanged". Turn the switch on in the same transaction, on the plugin's own instances only. A user's master with a switch of the same name is not touched.
- **`UMeshComponent::SetMaterial` does not call `Modify()`, and components are not `RF_Transactional` by default.** For undo, set the flag and call `Modify()` yourself. `UStaticMesh::SetMaterial` opens its own `FScopedTransaction`, which merges into an outer one.
- **Changing texture properties (`SRGB`, `CompressionSettings`, `bFlipGreenChannel`, `VirtualTextureStreaming`, ...) must sit between `PreEditChange(nullptr)` and `PostEditChange()`, in tests too.** With a real RHI textures compile asynchronously, the completion callback sees the changed property and re-registers with the compile manager, and `LogTexture` reports an error. `-nullrhi` does not compile asynchronously, so only a run without it shows this. `ValidateSettingsAfterImportOrEdit` also forces sRGB off for `TC_Masks` / `TC_Normalmap`; do not build states the editor cannot produce.
- **`FImageUtils::CompressImage(..., "png")` does not convert sRGB/linear for 8-bit surfaces;** it writes the bytes as they are. **`FImageCore::ResizeImage` shrinks with a default filter that has negative lobes**, which rings next to hard edges. Export scaling uses `Triangle`.
- **A texture from `UTexture2D::CreateTransient` has platform data (mip 0 in memory) but no `Source`.** "Pixels are readable" does not mean "source data can be exported".
- **`FPreviewScene`'s sky light needs `UpdateCaptureContents()`**, or the back side is pitch black. TSR and output dithering make consecutive frames differ by 1 to 2 gray levels, so do not use a hash to decide that a render is stable.

### Assets, packages and the registry

- **`GetTransientPackage()` is `/Engine/Transient`.** `StartsWith("/Engine/")` as an "engine content" test classifies every transient object as engine content. Check that the package file's disk path is under the engine directory (`TryConvertLongPackageNameToFilename` plus `FPaths::IsUnderDirectory`) and exclude transient packages (Assign and Reflow each have a copy of this check).
- **The asset registry's `GetDependencies` does not record class packages under `/Script/`.** To prove an asset does not reference the plugin's classes, also read the `.uasset` name table.
- **`FindObject` still finds an object that has only been `MarkAsGarbage`d.** To free a path, `Rename` it into a temporary package first.
- **A failed save logs `LogSavePackage` as an error,** which the automation framework counts as a test failure. A test that causes one on purpose calls `AddExpectedError` first. Making a file read-only makes `SavePackages` really fail (`FPlatformFileManager::Get().GetPlatformFile().SetReadOnly`; `IFileManager` has no such function).
- **`AsObject()` on an `FJsonValue` that is not an object writes an error to `LogJson`** (a failure in automation). Check `Type == EJson::Object` first.

### Tests and the transaction buffer

- **`UTransactor::GetQueueLength()` counts the whole undo buffer, undone entries waiting for redo included.** Starting a transaction drops the redo entries, so "length grew by one" assertions fail falsely when earlier tests undid something. Use `GetQueueLength() - GetUndoCount()`. The test needs `#include "Editor/Transactor.h"`; `Editor.h` does not bring it in.
- **Never keep a pointer into a temporary array.** `FindView(Actions.GetChannelViews(), ...)` returns a pointer into a temporary that is gone when the statement ends. The access violation takes the whole editor down, the later tests never run, and the report only says "no result". Store the array in a local first.

### Build hygiene

- **In a git worktree, adaptive unity excludes every file you have been editing from unity merging.** Incremental builds therefore never merge anything, hiding same-named helpers in anonymous namespaces and leaked `using namespace`, which a clean checkout hits at once. See *Before a release*.
- **`Tools/ensure-bom.sh` can fail with `Device or resource busy`** when UBT has the file open; the file then keeps no BOM. Wait and run it again.

## Before a release

**Run everything together once, in one command:**

```powershell
pwsh -NoProfile -File Tools\Test.ps1 -Live -Pipeline -Render -Filter DreamTexture
```

Real RHI, the fixture backend, and a real ComfyUI together. Running the groups separately has missed problems that
only appear under a real RHI. It must end with **zero failures and zero skips**. Close the Unreal Editor first.

Also needed, since incremental builds hide it:

- **Clean build without adaptive unity.** In a worktree, adaptive unity excludes every file you have been editing from
  unity merging, so name collisions and leaked `using namespace` stay hidden until a clean checkout compiles. Delete the
  plugin's `Intermediate` and `Binaries`, and do a full build with `-DisableAdaptiveUnity`.
- **Strict includes:** `RunUAT BuildPlugin -StrictIncludes`, which is equivalent to `-NoPCH -NoSharedPCH -DisableUnity`
  (plain `-DisableUnity` is hidden by the shared PCH):

  ```powershell
  & '<Engine>\Engine\Build\BatchFiles\RunUAT.bat' BuildPlugin `
    -Plugin='<PluginDir>\DreamTextureForUnreal.uplugin' -Package='<OutDir>\DreamTextureForUnreal' `
    -TargetPlatforms=Win64 -Rocket -StrictIncludes
  ```

- Check exit from a GUI editor with *File ▸ Exit*. Code on the module shutdown path must not touch UObjects (including
  the settings CDO): the automation editor shuts down in another order and cannot show it.
- Check it once more as a project without `OnlineSubsystem`'s early module loads, connected to a backend, so that
  `WebSockets` is first loaded by this plugin:

  ```powershell
  UnrealEditor.exe <Host>.uproject "-ini:Engine:[OnlineSubsystem]:AdditionalModulesToLoad=HTTP" "-LogCmds=LogModuleManager Verbose"
  ```

  On exit the log must show `Shutting down and abandoning module DreamTextureClient` before `... WebSockets`, and no
  assertion.
- Check the interface in Chinese as well as English (see Localization).

## Localization

Interface text is **English in source**, with a Simplified Chinese (`zh-Hans`) translation, and follows the editor language.

- Slate text uses `LOCTEXT` (a file's `LOCTEXT_NAMESPACE` and keys are never renamed once published).
- User-visible messages from Client and Assets use `NSLOCTEXT` and `FText::Format`, converted with `.ToString()` into
  the existing `FString` field, so public interfaces stay the same.
- Text that comes from the backend (error messages, ComfyUI reasons) is passed through as it is, with the wrapper text around it localized.
- **The Project Settings page** (`UDreamTextureSettings`) is not `LOCTEXT`: the engine looks up property names, tooltips and
  categories under `UObjectDisplayNames`, `UObjectToolTips` and `UObjectCategory`. Every `UPROPERTY` shown there needs an explicit
  English `meta = (ToolTip = "...")`; otherwise UHT uses the Chinese doc comment as the tooltip and English users see Chinese.
  The Chinese comment stays; the Chinese tooltip goes in `Tools/Localization/translations/Settings.json`. The scanner
  (`scan_reflection.py`, run by `sync_loctext.py`) rejects a missing or malformed `ToolTip`.
- `UE_LOG` stays English and is not localized.
- Chinese translations are in `Tools/Localization/translations/*.json` (all files in that directory merge), written as
  `"<namespace>|<key>": "中文"`, with the same placeholders as the source.
- Use the same English term for the same concept everywhere: Library, material set, master material, material instance,
  channel, Assign to Selection, Generate, Reflow, Fill missing channels, Restyle, Tileable, UV-unwrapped, Create new,
  Overwrite in place, Classic derivation, Research use only. The full glossary (Chinese term, English term) is in
  [`Tools/Localization/README.md`](../Tools/Localization/README.md#术语表).
- The build and checking tools, the commands and how to add a language are in
  [`Tools/Localization/README.md`](../Tools/Localization/README.md). The generated resources are in
  `Content/Localization/DreamTextureForUnreal/zh-Hans/`.
- The editor in tests is started with `-culture=en`, so assertions compare English text. A test of the Chinese
  translation switches to `zh-Hans` and back (`DreamTexture.Localization.ZhHans.*`, including the settings page).
- `Tools/Test.ps1` runs the localization checks before every build; `-NoLint` skips them, `-LintOnly` runs only them.

## Continuous integration

Two workflows are in `.github/workflows/`:

| Workflow | When | Does |
| :-- | :-- | :-- |
| `ci.yml` | push and pull request | Checks that the `.uplugin` is valid JSON with a consistent version, and runs the localization checks (see `Tools/Localization/README.md`). Takes seconds. |
| `release.yml` | tag `v*.*.*` or `v*.*.*b`, or a manual run | Packages the plugin source and publishes a GitHub Release. |

**GitHub's hosted runners have no Unreal Engine, so CI does not compile anything and runs no automation tests.**
A green CI means the metadata and the localization data are consistent, not that the plugin builds. Building and testing
is your job, with `Tools/Test.ps1`, before opening a pull request.

## Releasing

1. Finish the checks in *Before a release*.
2. In `CHANGELOG.md`, add a `## x.y.z - YYYY-MM-DD` section (`### Added`, `### Changed`, `### Fixed`; each entry a bold first
   sentence that describes the behavior). The release notes are cut from this section.
3. In `DreamTextureForUnreal.uplugin`, set `VersionName` (`x.y.z`), raise `Version`, and keep `IsBetaVersion` as intended.
   While `IsBetaVersion` is true, the release is a pre-release and its tag ends in `b`.
4. Commit, then tag and push:

   ```powershell
   git tag v0.9.0b
   git push origin v0.9.0b
   ```

   The workflow **derives** the tag from the descriptor (`v` + `x.y.z`, plus `b` for beta) and fails if the pushed tag differs,
   so edit the descriptor first.
5. The workflow publishes `DreamTextureForUnreal-<x.y.z>[b].zip`. The archive holds source (`Source/`),
   `Resources/`, `Config/`, `Content/` (the localization resources), the docs, `Images/`, `LICENSE` and the `.uplugin`. It has no
   binaries and does not contain `Tools/` or `Tests/`.

The beta is installed like any other release: extract the `DreamTextureForUnreal` folder into `<Project>/Plugins/`, and
the editor builds it.

## Pull requests

- Keep commits small and self-contained, one working change each.
- Run `Tools/Test.ps1` and say what you ran. Changes to anything that talks to the backend should also run `-Live`.
- Behavior the plugin guarantees (a cancelled batch, one undo for assignment, no overwriting assets it did not create)
  should have a test.
