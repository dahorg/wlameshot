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
./build/wlameshot --full -o s.png  # write to a file instead of the clipboard
```

There is no test suite, linter, or CI configured. Build dependencies: CMake >= 3.22, Qt6 >= 6.5.0 (Core, Widgets, Gui), C++17 compiler. See `INSTALL.md` for per-distro package names. **Runtime (Wayland only):** `grim` for capture and `wl-clipboard` (`wl-copy`) for the clipboard — both invoked as subprocesses, not linked.

**App identity is variablized in `CMakeLists.txt`** — `APP_ID` (binary/org/desktop basename, currently `wlameshot`), `APP_NAME`, and `APP_DESCRIPTION` are set once there and passed to the code via `target_compile_definitions` (plus `APP_VERSION` from `project(... VERSION)`). `main.cpp` reads those macros rather than hardcoding strings, and the executable target is `${APP_ID}`. To rename the app, change those CMake variables (and the `data/` asset filenames) — don't reintroduce literals in the source.

## Architecture

Three translation units under `src/`, wired together in `main.cpp`:

- **`ScreenshotGrabber`** (`screenshotgrabber.*`) — headless capture. Tries `QScreen::grabWindow(0)` first (works on X11); this returns a **null image on Wayland** (wlroots/GNOME block it), so it falls back to shelling out to the `grim` CLI (`grabViaGrim`, reads PNG from grim's stdout via `QProcess`). `grabFullscreen()` unions all screen geometries; `grabScreen(int)` captures one output (by `QScreen::name()`, e.g. `DP-1`, for grim's `-o`) and returns a **null image** on an out-of-range index — it deliberately does *not* silently substitute a different screen.
- **`CaptureWidget`** (`capturewidget.*`) — a frameless, always-on-top full-screen `QWidget` (shown via `showFullScreen()`, since Wayland ignores client-set positions) that dims the desktop, lets the user rubber-band a bright selection region (resizable via 8 handles / movable when no tool is active — see `handleAt()` and the `Drag` state machine in the mouse handlers), then annotate it. Tools (`Tool` enum): arrow, circle, rectangle, freehand pen, text, highlight, numbered marker, blur. Each annotation is one `Annotation` struct (tool + point pair, plus `points` for pen, `text` for text, `number` for markers, and its own `color`/`penWidth`), stored in a `QList`. A floating child-widget toolbar (`buildToolbar()`) holds tool buttons, color swatches (`m_colorGroup`) + a `QColorDialog`, and actions; buttons use `Qt::NoFocus` so keyboard focus stays on the canvas.

  Two things are worth knowing before touching this file:

  **Coordinates.** Interactive state (`m_selection`, every `Annotation` point) is in **widget logical units**; the screenshot is in **device pixels**. They differ by `m_scale` (the primary screen's `devicePixelRatio`) on a HiDPI display. `imageRect()`/`widgetRect()` convert between them and *everything* touching the screenshot goes through one of the two. `bakeAnnotation()` applies `painter.scale(m_scale, m_scale)` so annotation drawing code can stay in logical units. The constructor warns if the screenshot doesn't match `logical size × dpr` (multi-monitor or fractional scaling).

  **The flattened composite.** `m_flattened` is the screenshot with every *committed* annotation baked in, at device-pixel resolution, rebuilt by `rebuildFlattened()` whenever `m_annotations` changes. It is both what `paintEvent()` draws inside the selection and what `renderResult()` crops, so the overlay is a true preview of the output and there is only one compositing path. Consequences: blur (`mosaic()`, a downscale→upscale mosaic) samples `m_flattened` and therefore correctly obscures annotations beneath it rather than restoring original pixels; the annotation being text-edited is deliberately *excluded* from `m_flattened` so its caret never gets baked in; and `paintEvent()` draws the dimmed area from the pristine `m_screenshot`, so annotations only ever show through inside the selection. `renderResult()` is just a bounds-checked crop of `m_flattened`.

  Other details: the highlighter uses `CompositionMode_Multiply` with a blend-to-white tint so it darkens rather than covers; text is edited inline (`m_editingText`/`m_editIndex`, keystrokes in `keyPressEvent`) and supports newlines, `Ctrl+V`, and re-editing via `textAt()`. Selecting the active tool again sets `m_tool = Tool::None`, which is the only way back to moving the selection by its interior. Shortcuts: A/C/R/P/T/H/N/B, Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y, Ctrl+S, Ctrl+C, Enter, Esc, arrow keys; wheel sets pen width, or the blur mosaic block size when the blur tool is active; Shift constrains shapes. Both are stored per-`Annotation` (`penWidth`/`mosaicBlock`) and captured at press time, so scrolling never retroactively alters committed annotations. Emits `captureCompleted(QImage)`, `captureAborted()`, `captureSaved(QString)`, or `captureFailed(QString)`.
- **`main.cpp`** — parses CLI options (`QCommandLineParser`), always grabs a screenshot first, then either shows `CaptureWidget` (`--gui`) or copies/writes the full image directly. `--screen` is validated (non-numeric, negative, or out-of-range exits 1 with the available screen names). `--output`/`-o` writes a PNG instead of touching the clipboard, in both modes. Clipboard writes go through `copyImageToClipboard()`, which pipes PNG bytes to `wl-copy` on Wayland (Qt's `QClipboard` loses ownership the instant the process exits on Wayland) and falls back to `QClipboard` when `wl-copy` is absent; it returns false on failure and the caller exits 1. The widget's four outcome signals map to exit codes via `QCoreApplication::exit()`.

  **`main.cpp` installs a `qInstallMessageHandler`** that prints messages plainly to stdout/stderr. This is load-bearing, not cosmetic: several distros (Arch, Fedora) build Qt with journald support, which routes `qInfo`/`qWarning`/`qCritical` into the systemd journal, where a CLI user never sees them. Don't remove it, and don't use `printf` for user-facing messages — go through the `q*` macros so everything lands on one path.

Control flow to keep in mind: capture happens **before** any UI is shown, so the overlay draws a static snapshot rather than the live desktop. Cropping and annotation flattening live in `CaptureWidget`; `main.cpp` only routes the finished image to the clipboard or a file.

### Known constraints

- **Multi-monitor GUI mode**: the overlay covers the *primary* screen while `grabFullscreen()` returns the *union* of all screens, so overlay coordinates and the source image still don't line up across monitors. The constructor now warns when it detects this, and all crops are bounds-checked so the failure mode is a wrong-region capture rather than a garbage image, but the overlay itself is still single-screen. HiDPI *is* handled (see "Coordinates" above).
- In-progress strokes are clipped to the selection while drawing, but committed annotations are baked into `m_flattened` unclipped; since the output is cropped to the selection anyway, anything outside is dropped from the result regardless. This means annotations left over from a discarded selection stay in `m_flattened` and will reappear if a later selection overlaps them.
- The app registers **no global/system shortcuts** itself; the overlay handles keys only while focused. Global invocation (e.g. Print Screen → `wlameshot --gui`) is delegated to the compositor — see "Desktop integration" below.

## Assets

`data/app/org.wlameshot.Wlameshot.desktop` and `data/img/app/org.wlameshot.Wlameshot.svg` are packaging assets. `.gitignore` ignores `*.desktop` globally but keeps `data/app/*.desktop` via a `!` exception. They are not referenced by `CMakeLists.txt` and are not installed by the build (only the `wlameshot` binary is, via the `install(TARGETS ...)` rule).

## Desktop integration

Global invocation is set up outside the repo, in the user's compositor config (documented in `README.md` / `INSTALL.md`): a Hyprland `Print` keybind (`~/.config/hypr/bindings.conf`, with an `unbind` to override omarchy's default) and a Waybar button (`custom/screenshot` in `~/.config/waybar/config.jsonc`), both running `wlameshot --gui`. These files live in `~/.config`, not this repo.

## Project metadata

MIT-licensed (`LICENSE`). The codebase was generated by Claude under human direction — the README carries an authorship disclosure. There is no per-file license header convention; keep new files consistent with that.
