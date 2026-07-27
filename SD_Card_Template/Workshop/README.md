# Workshop

3D models for the Nomad's Workshop page. Drop files in here; the Workshop tile
appears on the menu as soon as this folder has anything in it.

Subfolders become collections — `Workshop/Repairs/Hose Clip.stl` shows up under
"Repairs". One level deep; anything deeper isn't scanned.

## What can be previewed

| Format | On the page |
|---|---|
| `.stl` (binary or ASCII) | Full 3D preview, dimensions, triangle count |
| `.obj` | Full 3D preview |
| `.3mf` | Full 3D preview, including multi-part plates |
| `.gcode`, `.step`, `.f3d`, `.scad`, `.zip`, and friends | Listed and downloadable, no preview |

The preview is drawn with plain WebGL off the card — nothing is fetched from the
internet, and no slicer is involved.

## Thumbnails and descriptions

Files that sit next to a model and share its name are picked up automatically:

```
Hose Clip.stl          the model
Hose Clip.jpg          its photo
Hose Clip.md           its description / print notes
README.md              a description shared by everything in this folder
```

A few common suffixes are understood too, so all of these attach to
`Hose Clip.stl` rather than being ignored:

```
Hose Clip-notes.md
Hose Clip.description.txt
Hose Clip print settings.txt
```

An exact name match always wins over a suffixed one. Images may be `.jpg`,
`.png`, `.webp`, `.gif` or `.bmp`; descriptions may be `.md`, `.txt` or `.nfo`,
and are shown as plain text, so write them however you like. A folder-wide
description can be called `README`, `notes` or `description`.

Where the photo shows up:

- in the grid, as the model's thumbnail
- for `.stl` / `.obj` / `.3mf`, behind a **Photo** button next to the 3D view,
  so you can compare the model with how it actually printed
- for everything else — `.gcode`, `.step` and friends — the photo *is* the
  preview, filling the space where the 3D view would be

## Print check

The model view compares the model's real bounding box against a build volume you
set once (stored in your browser, not on the card). It counts a part as fitting
if it fits either way round on the plate, since rotating 90° costs nothing.

Dimensions come from the model's own geometry, so this only applies to the
formats that get parsed — a `.gcode` or `.step` is reported as unknown rather
than guessed at.
