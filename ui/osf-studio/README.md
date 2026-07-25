# OSF Studio

Local-first web authoring for OSF Animation scene files.

## Current milestone

- Import one or more `*.osf.json` files.
- Open or drag local GLB/GLTF clips into the animation viewer.
- Decode Starfield `.af` clips locally using a user-selected matching `skeleton.rig`.
- Preview embedded meshes or skeleton-only animation files with orbit controls.
- Select embedded animations, scrub time, pause, restart, frame, and change playback speed.
- Edit scene identity, policy, roles, placement, stages, timing, and clips.
- Edit the complete document in JSON mode.
- Validate common structural and cross-file problems.
- Preserve fields not yet represented by the form editor.
- Undo/redo, browser-local drafts, and JSON export.

Graph scenes are detected and preserved but currently use JSON mode. The file access boundary is
deliberately browser-local so a chosen-folder workspace or live OSF bridge can be added without
changing the editor model.

## Development

```powershell
npm install
npm run dev
```

The development server uses `http://127.0.0.1:8792`. Run `npm run verify` before shipping changes.
