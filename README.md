# Omacut

A dead-simple video trimmer. Open a video, drag the handles to pick a start and end, split it into clips, remove unwanted sections, preview, and export the resulting timeline. On Omarchy, the interface follows your theme's accent color.

Built using **Qt Quick (QML)** UI with the Material style — the same Qt stack Quickshell builds on — and **ffmpeg** for the cut. The C++ side compiles to a single executable; the QML is embedded in it via Qt resources.

<img width="3227" height="3227" alt="screenshot-2026-06-23_15-20-40" src="https://github.com/user-attachments/assets/c76047c8-618f-4c1c-91f9-e7024c4f953b" />

## Hotkeys

- *Space*: Start/stop timeline playback.
- *T*: Split at the playhead; select the right-hand clip.
- *Delete* or *Backspace*: Remove the selected clip and close the gap.
- *Left/Right*: Move the playhead by 1 second.
- *Shift+Left/Right*: Move the playhead by 5 seconds.
- *Alt+Left/Right*: Move the playhead by 0.2 seconds.
- *Ctrl+Space*: Move the selected clip's start to the playhead.
- *Alt+Space*: Move the selected clip's end to the playhead.
- *Z*: Zoom the selected clip; press again without changing the selection to zoom out.
- *Ctrl+O*: Open a new file to trim.
- *Ctrl+S*: Export the current timeline.
- *Q*: Quit (asks first if the timeline hasn't been exported).
- *?*: Show the hotkeys in the app.

## Split and delete

Click a clip to select it and place the playhead, then press **T** (or the Split button) to split. Each clip has its own start/end handles; the selected clip is highlighted. Press **Delete** or **Backspace** (or the trash button) to remove it. Preview skips removed material, and export joins the remaining clips without gaps.

Handles can expand into unused source material, but cannot overlap neighboring retained clips. Splits must leave at least 0.1 seconds on each side. **Z** focuses on the currently selected clip; a long timeline can be scrolled horizontally. The time display shows position and duration in the resulting video.

Deleting the last clip resets the timeline, unloads the video, and returns to the **Open a video** screen. There is no undo yet: reopen the source with **Ctrl+O** to start again. Edits do not alter the source unless you explicitly export over it.

To test this branch without replacing the installed application:

```sh
./bin/build
./bin/test
./build/omacut
```

For a quick check, split twice, select and delete the middle clip, then preview and export. The exported duration should equal the sum of the remaining clips (within frame rounding).

## Install

Install via the Omarchy Package Repository via the `omacut` package. It's installed by default in new installations of Omarchy (from Quattro forward).

## Requirements

- `xdg-desktop-portal` and a portal backend for the file picker
- `ffmpeg` and `ffprobe` on your PATH (used at runtime)

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
