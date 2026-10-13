# Troubleshooting

> [Reference](index.md) » **Troubleshooting**

## Reading the three status levels

The strip at the top of the panel, and the **Connection Status** page, show three states that are independent of each other.
Do not read them as one light: the fix for each is different.

| Level | What it tells you | Typical values |
| :-- | :-- | :-- |
| **1. Backend process** | Does `dreamtexture.exe` answer REST requests at *Backend URL*? | Responding / Starting / Connecting / Not responding |
| **2. Event stream** | Is the WebSocket to the backend live? It only carries status, job progress, library changes and pushes. | Connected / Connecting / Timed out / Disconnected |
| **3. ComfyUI** | Is ComfyUI running and ready? Reported by the backend. | Ready / Starting (N s) / Running, not ready / Not running / Stopped |

Rules of thumb:

- **ComfyUI down does not mean the backend is broken.** The backend starts and answers without ComfyUI. Do not
  restart the backend for a level 3 problem.
- **The Library only needs level 1.** Browsing and importing work with ComfyUI off.
- **Generate and Reflow need all three** to run. Without level 2 you can still submit, and progress catches up
  after reconnecting.
- **Timed out** on level 2 means no message arrived for 40 seconds (the backend sends a heartbeat every
  15). The numbers on screen are stale; it reconnects automatically with a backoff from 1 to 15 seconds.
- ComfyUI's last known state is kept while the stream has timed out, with a note saying so.

## ComfyUI is not running

Open the **Connection Status** page.

- **Starting (N s).** Cold starts take anywhere from tens of seconds to a few minutes, and first runs of some custom
  nodes install their dependencies. Wait. Restarting only starts the wait over.
- **Not running, backend `managed`.** Look at the backend's console page or `logs/` for why ComfyUI exited. The
  backend restarts it automatically when `comfy.auto_restart` is on.
- **Not running, backend `attach`.** The backend never starts ComfyUI in this mode. Start it yourself at the
  address in `comfy.base_url`.
- **Stopped.** Someone stopped it by hand; it did not crash.
- **Running, not ready.** The process is up but not answering requests yet.
- Run *Environment check* for missing nodes, missing models, VRAM and output directory problems. See
  [Backend](backend.md#install-comfyui).

## The backend does not answer

- Is `dreamtexture.exe` running? Open the *Backend URL* in a browser.
- Does the address match? *Backend URL* in the project settings must equal the backend's `addr`. A URL that
  is not `http://` or `https://` is silently replaced by the default `http://127.0.0.1:8777`.
- **Port in use.** If another program holds the port, the backend will not start. Change `addr` in the
  backend config and *Backend URL* together. After changing the URL, click *Reconnect* instead of waiting
  for the backoff.
- On Windows, connecting to a local port that nobody listens on takes about 2 seconds to be refused. A slow
  failure is normal.
- **Start Backend does nothing useful.** It needs *Backend Executable*. With extra *Backend Arguments*, the
  `addr` inside that config must match *Backend URL*, or the process starts and nothing answers where the
  plugin looks (it waits up to 90 seconds, then reports an error).
- *Start Backend* says the backend was already running: it attached to it. The plugin does not stop a
  backend it did not start.

## Generation never finishes or is very slow

First check whether the Unreal Editor is open next to a local ComfyUI. This is the usual cause.

- ComfyUI looks at free VRAM only when it loads a model. When the editor or another program grows
  afterwards, the NVIDIA driver can quietly fall back to system memory. There is no error and no end, only jobs
  tens of times slower (a job that takes 12 seconds can take half an hour).
- Check free VRAM with `nvidia-smi`, or *Environment check* on the **Connection Status** page, which lists the
  programs using the GPU.
- Close the programs using the GPU, or generate with a cloud base image and Classic derivation, which need
  little VRAM.
- In the backend config, raise `comfy.reserve_vram_gb` to 2 or 3 when the editor stays open.
- For a loud failure instead of a silent one, in NVIDIA Control Panel set *CUDA - Sysmem Fallback Policy* to
  *Prefer No Sysmem Fallback*. Out-of-memory then fails immediately.
- The backend cancels a job after 8 minutes without activity and reports the VRAM figure in the error.
- ComfyUI that just started can also be slow for a few minutes while its Manager fetches data. This is covered in
  the backend's `docs/comfyui-notes.md`.

## A job fails

The Generate page shows the backend's own reason on the failed row. Common causes:

- Missing nodes or models: *Environment check*, and the backend's *Models* page.
- **Cloud base image** fails: the API key is not set, or the gateway does not offer the model. Set the key in the
  web UI, then reload the model list.
- Reflow says *Failed at "Upload"* or similar: the step name tells you where. *Upload* and *Submit* need a
  backend with the `src-upload-v1` segment; see [Backend](backend.md#which-backend-version).

## Push from the web UI does not arrive

1. Open **Connection Status**. The *Push* row should say *Registered* and show a session name.
2. *Not registered* or *Registering*: level 2 is down, or the last registration failed; the reason is shown and it
   retries every 15 seconds.
3. *Not supported by backend*: the backend is too old for editor sessions. Upgrade it, then click *Reconnect*.
4. The web UI says no editor is online (409): see step 1. Registration happens after the editor connects,
   and a session expires 45 seconds after the last heartbeat.
5. Several editors online: the web UI asks which one. Check you picked this project.
6. Different *Backend URL*: the web UI's backend and the editor's must be the same instance.
7. The editor was reconnecting when you sent: the push is delivered after reconnecting, once. Wait for the
   next heartbeat (15 seconds).

## Old backend

A backend without the newer endpoints does not break the plugin. These features turn themselves off and say why:

| Feature | What you see |
| :-- | :-- |
| Push | *Push* row: *Not supported by backend*. |
| Reflow: Fill missing channels | The page says the backend has no *Existing base image* segment. |
| Reflow: Restyle | The page says the backend has no local image segment with a reference image and denoise. |

Update the backend (see [Backend](backend.md#which-backend-version)), then click *Reconnect*.

## Import problems

- **Notification says some textures were not imported.** The output log has the reason per texture
  (`LogDreamTexture`).
- **A download is rejected.** Size or SHA-256 did not match the manifest; the file is discarded, never half-used.
  Import again.
- **No material instance.** The notification says why: *Create Material Instance* is off, or the master material
  could not be built, or a user's own asset sits at the master's or the instance's name. The plugin never
  overwrites an asset it did not create.
- **Material looks unchanged after Reflow with Overwrite in place.** Overwriting only swaps parameter textures.
  On instances the plugin built, Normal and ORM are switched on with `UseNormal` / `UseORM`; on your own
  master material those switches are yours.

## Still stuck

Open an [issue](https://github.com/TypeDreamMoon/DreamTextureForUnreal/issues) with the editor log
(`Saved/Logs`, search `LogDreamTexture`), the *Environment check* result, and the backend's log.
