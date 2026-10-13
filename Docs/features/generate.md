# Generate

> [Reference](../index.md) » **Generate**

The **Generate** page makes new material sets without leaving the editor. It submits to the same backend queue
as the web UI and follows progress on the same event stream, so anything you generate here also appears in
the web UI and in the Library.

## The form

A material pipeline has two halves that you choose independently:

| Choice | What it is |
| :-- | :-- |
| **Image model** | Where the base image comes from: local SDXL (a realistic or a stylized style) or a cloud model. |
| **Decompose model** | How the base image is split into Normal, Roughness, Metallic, AO and Height: CHORD (one-step estimate, realistic) or Classic derivation (computed from the image). |

Other fields:

| Field | Notes |
| :-- | :-- |
| Prompt | The description. *Refine prompt* asks a text model to expand it into a prompt suited to tiling textures (top-down, flat lighting, seamless). The result is shown first and you choose *Use it* or *Discard*. A refine model can be picked when the backend lists more than one. |
| Resolution | Edge length in pixels. The options come from the chosen pipeline. CHORD works best at 1024. |
| Variants (1-8) | Generate several and pick. Variants run one after another, so more variants take longer. |
| Seed | *Random* picks a seed per variant. *Fixed* uses yours: variants in a batch use seed, seed+1, seed+2 and so on. The seed box accepts 0 up to 2^53-1; larger values would be silently altered by JSON on the way. |
| Import automatically when done | Import each variant when it finishes, through the same path as the Library's *Import*. Also in project settings. Off by default, since you generate several to pick one. It is checked at the moment a variant finishes, not afterwards. |

For a **cloud** image model there are two extra fields: *Cloud model* (the list is asked from the server) and
*Quality*. Higher quality costs more and is slower; `high` can take two or three minutes per image, so try
prompts at `medium` first. Cloud images are billed per call, and the form shows how many calls the batch makes. Set the
API key in the backend's web UI, then use *Reload* next to the model list.

## Licensing in the form

A combination whose output is not for commercial use (CHORD) is marked **before you submit**: a warning
under the form, and the button reads *Generate (output is non-commercial)*. If the pipeline has a commercial
alternative, the warning names it. Variants made with it carry a *Research use only* tag.

## Running

Click *Generate*. If the backend process is down:

- with *Backend Executable* set, the button reads *Start backend and generate*. It starts the backend,
  waits for it to answer, loads the workflow list, then submits. *Cancel* stops the submission but never
  stops the backend it started;
- without it, *Open Project Settings* takes you to where to set the path.

The batch list shows each variant as *Queued*, *Running* (with a percentage), *Done*, *Failed* (with the
backend's reason) or *Canceled*. Each row has its own *Cancel*; *Cancel all* cancels what has not finished.
Queued variants cancel immediately; a running one interrupts ComfyUI.

*Sync* asks the backend about every unfinished job. Progress arrives over the event stream and the stream
occasionally drops events, so use *Sync* if a row looks stuck. If the stream reconnects, the plugin catches
up on its own.

A finished variant has *Import* with a progress line (*Downloading 3/5*, *Writing to project...*) and
*Imported* when done. Importing again re-checks and downloads only what changed.

## What can go wrong

The form tells you which status level is at fault:

| Message | Level | Meaning |
| :-- | :-- | :-- |
| Backend process is not answering | 1 | Check `dreamtexture.exe` is running and the address is right. |
| Event stream is not connected | 2 | You can still submit. Progress catches up after it reconnects. |
| ComfyUI is stopped / not running | 3 | The backend is fine; do not restart it. Start ComfyUI from the **Connection Status** page. |
| ComfyUI is busy | 3 | The job queues and waits. |
| The backend did not build the "X x Y" combination | | A new segment was added but the backend has not reloaded its workflows. |
| Queue is full | | The whole batch is rejected (HTTP 429) and nothing stays queued. Try again later. |

See [Troubleshooting](../troubleshooting.md).
