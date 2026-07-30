# Installation Guide

## Build dependencies

- **CMake** >= 3.22
- **Qt 6** >= 6.5.0 (Core, Widgets, Gui)
- **C++17** compiler (GCC 11+, Clang 14+)

### Debian/Ubuntu
```bash
sudo apt update
sudo apt install -y build-essential cmake qt6-base-dev
```

### Fedora
```bash
sudo dnf install -y gcc-c++ cmake qt6-qtbase-devel
```

### Arch Linux
```bash
sudo pacman -S base-devel cmake qt6-base
```

## Runtime dependencies (Wayland)

On Wayland the app shells out to two standard tools (it does not link them):

- **`grim`** — screen capture on wlroots compositors (Hyprland, Sway, …), used
  when Qt's own capture returns nothing on Wayland.
- **`wl-clipboard`** (`wl-copy`) — keeps the copied image on the clipboard after
  the process exits.

```bash
# Arch
sudo pacman -S grim wl-clipboard
# Debian/Ubuntu
sudo apt install -y grim wl-clipboard
# Fedora
sudo dnf install -y grim wl-clipboard
```

On X11 neither is required — Qt captures and writes the clipboard directly.

## Building

```bash
# Clone this repository
git clone <this-repo>
cd flameshotclone

# Build using the build script (recommended)
./build.sh

# Or manually
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make -j$(nproc)

# Install the `wlameshot` binary system-wide (optional)
sudo make install
```

To install for just your user instead (no sudo), copy the binary onto your PATH:

```bash
cp build/wlameshot ~/.local/bin/
```

## Usage

```bash
# Launch GUI capture mode (select region, then annotate)
wlameshot --gui

# Capture fullscreen and copy to clipboard
wlameshot --full

# Capture a specific screen by index (errors if the index doesn't exist)
wlameshot --screen 0

# Write to a file instead of the clipboard (works with --gui too)
wlameshot --full --output shot.png

# Show help / version
wlameshot --help
wlameshot --version
```

Exit status is `0` on success and `1` on failure (capture failed, bad
`--screen` index, clipboard handoff failed, or the file could not be written),
so it is safe to use in scripts.

## In-app shortcuts (GUI mode)

Drag to select a region (drag the handles to resize it, or drag inside with no
tool active to move it), then annotate:

| Key | Action |
| --- | --- |
| `A` | Arrow |
| `C` | Circle |
| `R` | Rectangle |
| `P` | Freehand pen |
| `T` | Text (click, type, `Enter` to commit) |
| `H` | Highlight |
| `N` | Numbered marker (click to place) |
| `B` | Blur / pixelate |
| `Ctrl+Z` | Undo last annotation |
| `Ctrl+Shift+Z` / `Ctrl+Y` | Redo |
| `Ctrl+S` | Save to PNG |
| `Enter` / `Ctrl+C` | Copy to clipboard |
| `Esc` | Cancel |
| Scroll wheel | Pen/line thickness (while a tool is active) |
| `Shift` + drag | Constrain a shape to a square/circle |
| Arrow keys | Nudge the selection (`Shift` = 10px) |
| Double-click | Copy the selection immediately |
| Right-click | Put the tool away, or open the color picker |
| `Shift+Enter` | New line inside a text box |

Pressing a tool key a second time puts the tool away, which is how you get back
to moving the selection by its interior. With the text tool active, clicking
existing text reopens it for editing.

## Desktop integration (Hyprland / omarchy)

### Print Screen keybind

Add to `~/.config/hypr/bindings.conf` (the `unbind` overrides any existing
`Print` binding, e.g. omarchy's default screenshot tool):

```
unbind = , PRINT
bindd = , PRINT, Screenshot (Wlameshot), exec, wlameshot --gui
```

Hyprland auto-reloads on save; validate with `hyprctl configerrors`.

### Waybar button

Reference `custom/screenshot` in a `modules-*` array in
`~/.config/waybar/config.jsonc`, then define it (the icon is the `nf-md-fire`
glyph from a Nerd Font):

```jsonc
"custom/screenshot": {
  "format": "󰈸",
  "on-click": "wlameshot --gui",
  "on-click-right": "wlameshot --full",
  "tooltip-format": "Wlameshot — screenshot tool"
}
```

Waybar does not auto-reload — apply with `omarchy restart waybar` (or
`killall -SIGUSR2 waybar`).
