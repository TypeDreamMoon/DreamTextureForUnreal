# Reflow

> [Reference](../index.md) » **Reflow**

Reflow takes a material that is already in your project, sends its Base Color to the backend, and brings
the result back. Use it to **fill in channels the material is missing** (Normal, Roughness, Metallic, AO,
Height) or to **restyle** it from a prompt.

It needs a backend that supports it; see [Backend](../backend.md#which-backend-version). On an older one,
the page says which segment is missing.

## 1. Pick a material

Select one of these, then click *Get Selection*:

- a material or material instance in the Content Browser;
- an actor in the level (its material slots are listed and you pick one).

The page lists every texture the material uses, which channel each one is, and whether it can be exported.

### How channels are recognized

For each texture the plugin looks at, in order from most to least reliable:

1. **Provenance record**, written by this plugin when it imported the texture;
2. **Material graph**: what the sampler's outputs are wired into;
3. **Parameter name**;
4. **Asset name**;
5. **Sampler type** (a guess).

The *Evidence* column shows which one decided. Roles: Base Color, Normal, Roughness, Metallic, Ambient
Occlusion, Height, Packed, Emissive, Opacity, Specular, Other (ignored), Unrecognized. A packed
texture shows what R, G and B hold.

If a result is wrong, or the plugin asks you to assign a role ("manual assignment"), choose the right
role in the dropdown. Your choice is remembered **per material, outside your assets**, in
`Saved/DreamTexture/ReflowOverrides.json` in the project. *Auto (undo my assignment)* removes it.

### Export

*Export N PNG(s)* writes each exportable texture to a new folder under `Saved/DreamTexture/Reflow/` in the project, with
*Open folder* to show it. A texture that cannot be exported says why; the plugin never falls back to a
compressed mip. The export comes from the texture's **source data**, keeps bit depth, and does no sRGB
conversion. *Max edge* shrinks the long edge before export (never enlarges), for decompose models that cannot
take a huge image.

## 2. Pick what to do

### Fill missing channels

Only the **decompose** step runs. Your Base Color goes to the backend unchanged, and the Normal, Roughness,
Metallic, AO and Height your material lacked come back. The picture does not change.

- The Base Color is not regenerated (the backend's copy is resized and re-encoded). It cannot be ticked, and
  the new instance's Base Color points at **your original texture**.
- If the material has several Base Colors, *Base Color to send* chooses which one to send.
- Channels the material already has are not ticked by default. Tick one to replace it. *Reset to default* restores the
  defaults: missing ticked, existing not.

### Restyle

The existing Base Color is a **reference image** for a local image-to-image run, redrawn from your prompt, then
the result is decomposed into a full set.

| Field | Notes |
| :-- | :-- |
| Prompt | What it should become. Required. |
| Image model | Local image models only (Realistic / Stylized decides the look). Cloud models have no denoise strength and are not offered. |
| Denoise strength | How far from the original. Default 0.55 tiled, 0.30 UV-unwrapped. The text beside the slider describes the effect: up to 0.35 almost a copy with refined detail; up to 0.55 keeps the structure and changes the surface; up to 0.75 borrows the tones and layout and regenerates; above that largely ignores the original. |
| Resolution | Edge of the repainted image. CHORD works best at 1024. |

### Decompose model

The same choice as on the Generate page. A model marked *Research use only* produces results that
cannot be used commercially.

## 3. Tileable or UV-unwrapped

| | **Tileable** (default) | **UV-unwrapped** |
| :-- | :-- | :-- |
| For | Ground, walls, rock: seamless tiling materials | Characters, props, trim sheets: one image maps to one mesh's unwrap |
| Request | Seamless processing on | `tileable=false`: no seamless processing at all |
| Default decompose model | As on Generate | **Classic derivation**. CHORD still available, marked *Out of distribution, quality not guaranteed*. |
| Restyle denoise | No cap | **Hard cap 0.35**; more would destroy the UV correspondence |
| Preview | 3x3 tiled preview | Side by side: *original | result* |

The choice is remembered per material in `Saved/DreamTexture/ReflowOverrides.json` (under `uv_kinds`), not
written into your assets. If the material is not a project asset (created at runtime), the choice
holds for this session and is not remembered.

The cap is enforced in three places: the slider, whether the run can start, and the start of the run.

## 4. Where the result goes

| | Does |
| :-- | :-- |
| **Create new** (default) | New texture set and a new `MI_<name>` instance. The original material is not touched at all. |
| **Overwrite in place** | Changes the texture parameters of the **selected material instance**, only for the channels ticked, in one transaction (Ctrl+Z). Not available when the selection is a master material: textures are hard-wired in the graph and there is no parameter to change. |

Notes on overwriting:

- A channel counts as "already there" if it is a texture parameter of the instance, even when the plugin could
  not resolve it through the graph (an instance this plugin built with `UsePOM` off still has its `Height`
  parameter).
- Channels inside a packed texture are replaced as a **whole texture** or not at all. There is no per-pixel
  repacking. If the packing is not the standard ORM layout, or the alpha channel holds something else,
  the plugin does not touch it.
- On an instance this plugin built, putting a new Normal or ORM in also switches `UseNormal` / `UseORM`
  on, in the same transaction, since the material would never sample it otherwise. Switches on your own
  master material are left alone.

## 5. Run

*Start* runs these steps and shows the current one: start backend (if needed), export, upload, submit,
generate, download, import, and (when overwriting) apply to the instance. A failure says which step failed:
*Failed at "Upload": ...*. *Cancel* interrupts the job.

The run keeps going if you close the panel, and imports automatically. Reflow jobs are tagged `reflow` and
are not picked up by *Import automatically when done*, so a reflow is never imported as an ordinary material.

When finished, *Show in Content Browser* selects the result.
