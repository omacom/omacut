# Omacut

A dead-simple video trimmer. Open a video, trim either end, split it into clips and cut ranges out of the middle, preview the result, and export. On Omarchy, the interface follows your theme's accent color.

Built using **Qt Quick (QML)** UI with the Material style — the same Qt stack Quickshell builds on — and **ffmpeg** for the cut. The C++ side compiles to a single executable; the QML is embedded in it via Qt resources.

<img width="3227" height="3227" alt="screenshot-2026-06-23_15-20-40" src="https://github.com/user-attachments/assets/c76047c8-618f-4c1c-91f9-e7024c4f953b" />

## Editing

The timeline shows your video as one or more clips, each framed with a handle at both ends. Whatever falls outside every clip is cut. Playback plays the clips in order and skips the gaps.

- **Double-click a clip** to split it there.
- **Drag a handle** to trim that clip. Dragging the handles at a split apart removes the part between them.
- **Double-click a gap**, or the line between two touching clips, to join them again.
- **Hover a clip** and click its **×** to remove it.

Handles catch on neighbouring clips and the playhead.

## Hotkeys

- *Space*: Start/stop video playback.
- *Left/Right*: Move the playhead by 1 second.
- *Shift+Left/Right*: Move the playhead by 5 seconds.
- *Alt+Left/Right*: Move the playhead by 0.2 seconds.
- *[ / ]*: Jump to the previous / next clip edge.
- *S*: Split the clip at the playhead.
- *X, Delete*: Remove the clip under the playhead; in a gap, restore it.
- *Ctrl+Space*: Move the start of the clip under the playhead to the playhead.
- *Alt+Space*: Move the end of the clip under the playhead to the playhead.
- *Ctrl+Z / Ctrl+Shift+Z*: Undo / redo.
- *Z*: Zoom to the clip under the playhead for fine tuning (Z again zooms back out).
- *Ctrl+O*: Open a new file to trim.
- *Ctrl+S*: Export the current edit.
- *Q*: Quit (asks first if the edit hasn't been exported).
- *?*: Show the hotkeys in the app.

## Install

Install via the Omarchy Package Repository via the `omacut` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Requirements

- `xdg-desktop-portal` and a portal backend for the file picker
- `ffmpeg` and `ffprobe` on your PATH (used at runtime)

Exports run in the background: keep editing, previewing, or opening another recording while they encode. Each export keeps the source and clips selected when its save dialog opened. Further exports queue and run one at a time; the export panel shows progress, saved files, errors, and cancellation. Keep Omacut open until the queue finishes. Closing with exports pending asks before cancelling them.

Exports are always written as MP4 files, regardless of the input video's container. The export dialog offers Original/1080p/720p quality — never upscaling, and always preserving the aspect ratio.

## Build

Uses Qt's own build tool, `qmake6` (no cmake needed):

```bash
./bin/build
```

This produces a single `omacut` binary in `build/`.

Requirements:

- A C++17 compiler and Qt6: `qt6-base`, `qt6-declarative` (Qt Quick + Controls),
  `qt6-multimedia`

## Test

```bash
./bin/test
```

## Package

Build and install the local Arch package:

```bash
./bin/install
```

This runs `./bin/build`, then `makepkg -fsi` from `pkgbuild/` so same-version local packages are rebuilt and reinstalled. Extra arguments are passed through to `makepkg`, for example `./bin/install --clean`. The package installs the binary, desktop entry, app icon, and MIT license. Local package outputs such as `pkgbuild/pkg/`, `pkgbuild/src/`, and `*.pkg.tar.*` are ignored.

## License

MIT. See `LICENSE`.
