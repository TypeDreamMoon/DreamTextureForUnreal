# Library

> [Reference](../index.md) » **Library**

The **Library** page browses the backend's material library, imports a set into the project, and assigns
it to what is selected. Only the backend process has to be reachable; ComfyUI can be down.

## Browse

| Control | Does |
| :-- | :-- |
| Search box | Searches names, prompts and tags. Runs 0.3 seconds after you stop typing. |
| *All / Realistic / Stylized* | Filters by pipeline style. |
| *Favorites* | Shows only favorites. The star on a tile toggles it, and is restored if the backend refuses. |
| *Refresh* | Reloads the list. A material the web UI just finished appears by itself (on the `job.done` event); this is the manual fallback. |
| *Load more* | Pages in the next batch. |

If a refresh fails, the page keeps showing the last list and says the refresh failed. *Reconnect* retries the
connection without waiting for the backoff; use it after changing *Backend URL*.

## Details

Select a tile to see the prompt, negative prompt, seed, resolution, creation time, workflow, tags and the
material id. The details also say:

- whether the material is **tileable** (seamless) or "not guaranteed seamless";
- **Research use only / Not for commercial use**, for materials decomposed with CHORD. The reason from the
  manifest is shown with it;
- that the base image came from a **cloud model**, so the same parameters cannot reproduce it (cloud
  APIs take no seed);
- when the manifest cannot be read, a warning that the license is unknown. Treat that as not commercial until
  you have checked.

## Import

*Import* downloads the set and builds the assets. See [Getting started](../getting-started.md#import-a-set)
for the resulting folder.

How each channel is imported:

| Channel | Suffix | Compression | sRGB |
| :-- | :-- | :-- | :-- |
| Base Color | `_BC` | Default | on |
| Normal | `_N` | Normalmap | off |
| Packed ORM | `_ORM` | Masks | off |
| Height | `_H` | Grayscale | off |
| Roughness / Metallic / AO | `_R` / `_M` / `_AO` | Grayscale | off (only with *Import Separate Channels*) |

Behavior worth knowing:

- **Downloads are verified.** Each file is checked for size and SHA-256 (the first 16 hex digits of the
  whole file's SHA-256, as the backend computes it) before it is written. A partial or mismatched file is
  never used.
- **Importing again changes nothing.** Each texture records its source (material id, channel, sha256) in the
  package's editor-only metadata. Unchanged channels are skipped; changed ones are updated in place, so
  materials already using them stay connected. The records are not cooked.
- **16-bit grayscale PNGs stay 16-bit** (Height).
- **Normals are not flipped.** Backend manifests state DirectX normals (`y: directx`). The green channel is
  flipped only if a manifest says `opengl`.
- **No duplicate imports.** Importing a material that is already in the import queue does nothing and says
  so. The completion notification appears even if you closed the panel.
- Downloads go through `Saved/DreamTexture/Downloads/<id>/` in the project.

## Material instance

Unless *Create Material Instance* is off, every import builds or updates `MI_<name>_<id tail>` in the same
folder. Its parent is `M_DreamTexture_Master`, which the plugin **generates from code into your Import
Root** the first time it is needed. It is not stored in the plugin, so it works when the plugin is
disabled, and the repository has no binary assets.

The master's parameters are only ever added to, never renamed:

| Parameter | Does |
| :-- | :-- |
| `BaseColor`, `Normal`, `ORM`, `Height` | The textures. |
| `UseNormal`, `UseORM` | Static switches. A missing texture turns its switch off, so the material uses the scalar fallback instead of sampling a default texture. |
| `UsePOM` | Parallax occlusion mapping. Off by default; it is expensive. |
| `UVTiling`, `BaseColorTint` | Tiling and tint. |
| `NormalStrength`, `AOStrength`, `HeightRatio` | Strengths. |
| `RoughnessMin`, `RoughnessMax` | Roughness remap range. |
| `Roughness`, `Metallic` | Fallback values when ORM is missing. |

Things the plugin will not do to your work:

- If a master material exists at that path and the plugin did not generate it, it is left as it is and
  the plugin says so. It does not clear or rebuild it.
- If an instance exists at that name and the plugin did not build it for this material, it is not touched.
- When the plugin's own master graph changes in a new plugin version, it is rebuilt in place as the same
  asset, so existing instances keep working. Parameters you changed on your own instances are left alone.

## Assign to Selection

*Assign to Selection* (also offered by the import notification) puts the instance on what is selected:

- **Actors selected in the level:** every mesh component gets the material as an override. The mesh assets are
  not changed.
- **Nothing selected in the level, static meshes selected in the Content Browser:** the mesh asset's default
  material is changed. This affects every user of the asset that has no override, and marks the asset dirty.
  The dialog warns about it.
- **Engine meshes** (including ones inside engine plugins) are skipped and listed. Changing them would dirty
  engine packages.

The dialog states **how many slots will be replaced** before anything changes, lists the targets, and lets
you choose *All material slots* or one slot. The whole assignment is one transaction and Ctrl+Z undoes it all.
If a slot already holds that material, nothing is changed and it says so.
