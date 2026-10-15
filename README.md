# Omacut

A dead-simple video editor. Open a video, trim either end, split it into clips and cut pieces out, add more videos and put the clips in any order, preview the result, and export. On Omarchy, the interface follows your theme's accent color.

Built using **Qt Quick (QML)** UI with the Material style — the same Qt stack Quickshell builds on — and **ffmpeg** for the cut. The C++ side compiles to a single executable; the QML is embedded in it via Qt resources.

<img width="3227" height="3227" alt="screenshot-2026-06-23_15-20-40" src="https://github.com/user-attachments/assets/c76047c8-618f-4c1c-91f9-e7024c4f953b" />

## Editing

The timeline shows your clips back to back, in the order they play, each framed with a handle at both ends over its own filmstrip. Whatever isn't in a clip is cut.

- **Double-click a clip** to split it there.
- **Drag a handle** to trim that clip. A trimmed clip can grow back into its video, up to either end.
- **Double-click the line** between two pieces of the same stretch of video to join them again.
- **Hold a clip, then drag it** to move it before or after the others.
- **Hover a clip** and click its **×** to remove it.
- **Click +** to add another video after the clip under the playhead.

Handles catch on neighbouring clip edges and the playhead.

## Hotkeys

- *Space*: Start/stop video playback.
- *0–9*: Jump to 0%–90% of the edit.
- *Left/Right*: Move the playhead by 1 second.
- *Shift+Left/Right*: Move the playhead by 5 seconds.
- *Alt+Left/Right*: Move the playhead by 0.2 seconds.
- *[ / ]*: Jump to the previous / next clip edge.
- *S*: Split the clip at the playhead.
- *,* / *.*: Play the clip under the playhead at half / double speed (0.5×, 1×, or 2×).
- *X, Delete*: Remove the clip under the playhead.
- *Alt+[ / Alt+]*: Move the clip under the playhead earlier / later.
- *Ctrl+Space*: Move the start of the clip under the playhead to the playhead.
- *Alt+Space*: Move the end of the clip under the playhead to the playhead.
- *Ctrl+Z / Ctrl+Shift+Z*: Undo / redo.
- *Z*: Zoom to the clip under the playhead for fine tuning (Z again zooms back out).
- *F*: Save the frame under the playhead as a PNG next to its video.
- *Ctrl+O*: Open a new file to edit.
- *Ctrl+Shift+O*: Add a video after the clip under the playhead.
- *Ctrl+S*: Export the current edit.
- *Q*: Quit (asks first if the edit hasn't been exported).
- *?*: Show the hotkeys in the app.

## Install

Install via the Omarchy Package Repository via the `omacut` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Requirements

- `xdg-desktop-portal` and a portal backend for the file picker
- `ffmpeg` and `ffprobe` on your PATH (used at runtime)

Exports are always written as MP4 files, regardless of the input video's container. The export dialog offers Original/1080p/720p quality — never upscaling, and always preserving the aspect ratio. When clips come from different videos, the export takes the frame size of the first clip's video; clips of another shape are letterboxed into it, and videos without sound get silence.

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

## License

MIT. See `LICENSE`.
