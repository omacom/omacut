# Omacut

A dead-simple video editor. Open a video, trim either end, split it into clips and cut pieces out, add more videos and put the clips in any order, preview the result, and export. On Omarchy, the interface follows your theme's accent color.

Built using **Qt Quick (QML)** UI with the Material style — the same Qt stack Quickshell builds on — and **ffmpeg** for the cut. The C++ side compiles to a single executable; the QML is embedded in it via Qt resources.

<img width="3227" height="3227" alt="screenshot-2026-06-23_15-20-40" src="https://github.com/user-attachments/assets/c76047c8-618f-4c1c-91f9-e7024c4f953b" />

## Editing

The timeline shows your clips in the order they play, each framed with a handle at both ends over its own filmstrip. Clips sit over their own footage, so what you cut stays on the timeline, dimmed, between them, and nothing moves when you trim: dragging a handle back over the dimmed footage restores it. Playback and export skip whatever isn't in a clip. The timeline only changes scale when you zoom, or add, remove or reorder clips.

- **Double-click a clip** to split it there.
- **Drag a handle** to trim that clip. A trimmed clip can grow back into its video, up to either end.
- **Double-click the line** between two pieces of the same stretch of video to join them again.
- **Hold a clip, then drag it** to move it before or after the others.
- **Hover a clip** and click its **×** to remove it.
- **Click +** to add another video after the clip under the playhead.

Handles catch on neighbouring clip edges and the playhead.

Confirmations are answered from the keyboard: the arrows and Tab move between the buttons, Enter presses one, a button's underlined letter presses that one (with or without Alt), and Escape backs out.

## Hotkeys

- *Space*: Start/stop video playback.
- *0–9*: Jump to 0%–90% of the edit.
- *Left/Right*: Move the playhead by 1 second.
- *Shift+Left/Right*: Move the playhead by 5 seconds.
- *Alt+Left/Right*: Pause and move the playhead by one video frame. Hold to move it by 0.2 seconds at a time.
- *[ / ]*: Jump to the previous / next clip edge.
- *S*: Split the clip at the playhead.
- *X, Delete*: Remove the clip under the playhead.
- *Alt+[ / Alt+]*: Move the clip under the playhead earlier / later.
- *Ctrl+Space*: Move the start of the clip under the playhead to the playhead.
- *Ctrl+Shift+Space*: Move the end of the clip under the playhead to the playhead.
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

Built with CMake:

```bash
bin/build
```

This produces a single `omacut` binary in `build/`.

Requirements: a C++17 compiler, `cmake`, and Qt 6 (`qt6-base`, `qt6-declarative`, `qt6-multimedia`). The package's PKGBUILD in `omacom/omarchy-pkgs` lists everything, and CI installs from it.

## Test

```bash
bin/test
```

Runs the Qt Test suite through `ctest`; it needs `ffmpeg` to make its sample videos. The tests run offscreen with Qt's software renderer, so no windows open and screenshots hold real pixels. Any QML warning fails the test that caused it.

`qmlTimelineViewDependsOnlyOnTheEdit` makes random edits the way a person might (splits, trims, handle drags, joins, moves, removals, added videos, zooms, undo and redo) and after each one checks the timeline against a freshly opened editor showing the same edit, down to the pixel. It also checks that the zoomed-out track is covered end to end, that trimming a clip shorter moves no other clip, and that nothing in the filmstrip reloads or moves during a drag. When it fails, it prints the edits that led there and saves both pictures to `/tmp`. Run it longer, or against a real recording, before a release:

```bash
OMACUT_FUZZ_SEEDS=50 OMACUT_FUZZ_VIDEO=~/Videos/some-recording.mp4 build/tests/backend_tests qmlTimelineViewDependsOnlyOnTheEdit
```

Set `OMACUT_TEST_ON_SCREEN=1` to watch the tests in real windows instead.

## License

MIT. See `LICENSE`.
