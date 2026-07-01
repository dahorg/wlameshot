# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

A minimal Wayland-compatible screenshot tool inspired by Flameshot, written in C++17 with Qt 6 Widgets. Produces a single executable, `wlameshot`.

## Build & Run

```bash
./build.sh                       # configure + build into build/ (passes extra args to cmake)
./build.sh -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)   # rebuild after the first configure
sudo make -C build install       # optional system-wide install

# Run (from build/ after building)
./build/wlameshot --gui       # interactive rectangle selection, copies to clipboard
./build/wlameshot --full      # capture all screens, copy to clipboard
./build/wlameshot --screen 0  # capture a specific screen by index
```

There is no test suite, linter, or CI configured. Build dependencies: CMake >= 3.22, Qt6 >= 6.5.0 (Core, Widgets, Gui), C++17 compiler. See `INSTALL.md` for per-distro package names. **Runtime (Wayland only):** `grim` for capture and `wl-clipboard` (`wl-copy`) for the clipboard — both invoked as subprocesses, not linked.

**App identity is variablized in `CMakeLists.txt`** — `APP_ID` (binary/org/desktop basename, currently `wlameshot`), `APP_NAME`, and `APP_DESCRIPTION` are set once there and passed to the code via `target_compile_definitions` (plus `APP_VERSION` from `project(... VERSION)`). `main.cpp` reads those macros rather than hardcoding strings, and the executable target is `${APP_ID}`. To rename the app, change those CMake variables (and the `data/` asset filenames) — don't reintroduce literals in the source.

## Architecture

Three translation units under `src/`, wired together in `main.cpp`:

- **`ScreenshotGrabber`** (`screenshotgrabber.*`) — headless capture. Tries `QScreen::grabWindow(0)` first (works on X11); this returns a **null image on Wayland** (wlroots/GNOME block it), so it falls back to shelling out to the `grim` CLI (`grabViaGrim`, reads PNG from grim's stdout via `QProcess`). `grabFullscreen()` unions all screen geometries; `grabScreen(int)` captures one output (by `QScreen::name()`, e.g. `DP-1`, for grim's `-o`) and falls back to fullscreen on an out-of-range index.
- **`CaptureWidget`** (`capturewidget.*`) — a frameless, always-on-top full-screen `QWidget` (shown via `showFullScreen()`, since Wayland ignores client-set positions) that dims the desktop, lets the user rubber-band a bright selection region (resizable via 8 handles / movable when no tool is active — see `handleAt()` and the `Drag` state machine in the mouse handlers), then annotate it. Tools (`Tool` enum): arrow, circle, rectangle, freehand pen, text, highlight, numbered marker, blur. Each annotation is one `Annotation` struct (tool + point pair, plus `points` for pen, `text` for text, `number` for markers, and its own `color`), stored in a `QList` and repainted live by `drawAnnotation()`. Notable details: the highlighter uses `CompositionMode_Multiply` with a `paleColor()` tint so it darkens rather than covers; blur is a mosaic via `pixelated()` (downscale→upscale with `FastTransformation`); text is edited inline (`m_editingText`/`m_editIndex`, keystrokes captured in `keyPressEvent`). `renderResult()` flattens all annotations onto a copy of the screenshot and crops to the selection. A floating child-widget toolbar (`buildToolbar()`) holds tool buttons, color swatches (`m_colorGroup`) + a `QColorDialog`, and actions; buttons use `Qt::NoFocus` so keyboard focus stays on the canvas. Shortcuts: A/C/R/P/T/H/N/B, Ctrl+Z, Ctrl+S, Enter, Esc. Emits `captureCompleted(QImage)` or `captureAborted()`.
- **`main.cpp`** — parses CLI options (`QCommandLineParser`), always grabs a screenshot first, then either shows `CaptureWidget` (`--gui`) or copies the full image directly. In GUI mode the `captureCompleted(QImage)` handler just copies the already-finished image (cropping/annotation happen inside the widget). Clipboard writes go through `copyImageToClipboard()`, which pipes PNG bytes to `wl-copy` on Wayland (Qt's `QClipboard` loses ownership the instant the process exits on Wayland) and falls back to `QClipboard` when `wl-copy` is absent.

Control flow to keep in mind: capture happens **before** any UI is shown, so the overlay draws a static snapshot rather than the live desktop. Cropping and annotation flattening live in `CaptureWidget::renderResult()`; `main.cpp` only routes the finished image to the clipboard.

### Known constraints

- **Multi-monitor GUI mode**: the overlay covers the *primary* screen while `grabFullscreen()` returns the *union* of all screens, so overlay coordinates and the source image don't line up across monitors. Widget coordinates are assumed equal to screenshot pixel coordinates throughout `CaptureWidget` (selection crop, annotation positions) — valid only for a single monitor at scale factor 1.
- Annotations are **not clipped** to the selection while drawing (a stroke can extend past the region on screen); `renderResult()` crops to the selection at the end, so anything outside is dropped from the output regardless.
- The app registers **no global/system shortcuts** itself; the overlay handles keys only while focused. Global invocation (e.g. Print Screen → `wlameshot --gui`) is delegated to the compositor — see "Desktop integration" below.
- `ScreenshotGrabber::captureComplete` signal and `availableGeometries()` are declared but currently unused.

## Assets

`data/app/org.wlameshot.Wlameshot.desktop` and `data/img/app/org.wlameshot.Wlameshot.svg` are packaging assets. `.gitignore` ignores `*.desktop` globally but keeps `data/app/*.desktop` via a `!` exception. They are not referenced by `CMakeLists.txt` and are not installed by the build (only the `wlameshot` binary is, via the `install(TARGETS ...)` rule).

## Desktop integration

Global invocation is set up outside the repo, in the user's compositor config (documented in `README.md` / `INSTALL.md`): a Hyprland `Print` keybind (`~/.config/hypr/bindings.conf`, with an `unbind` to override omarchy's default) and a Waybar button (`custom/screenshot` in `~/.config/waybar/config.jsonc`), both running `wlameshot --gui`. These files live in `~/.config`, not this repo.

## Project metadata

MIT-licensed (`LICENSE`). The codebase was generated by Claude under human direction — the README carries an authorship disclosure. There is no per-file license header convention; keep new files consistent with that.
