# Xakar v5-rc1 — Native C++ X11 Tiling Daemon

> First release candidate for fifth-generation tiling daemon — rewritten in optimized C++17 with direct Xlib/Xinerama calls.  
> No Python, no external tools, no flicker. Fast, minimal, multi-monitor aware.
> No need for Xcape and added even more features, also needs installing .config at $HOME/.setuzuna/xakar/.config
---

## What's New in v4

- **Pure C++17 implementation** — replaces the Python daemon from v3.x
- **Direct Xlib/Xinerama** — zero dependencies beyond `libX11` and `libXinerama`
- **Multi-monitor smart tiling**:
  - Detects the monitor of the active window
  - Moves windows across monitors with preserved geometry or preserved size
  - Neighbor monitor detection (left/right/up/down aware)
- **New commands**:
  - `preserve_geom <dir>` → Move to another monitor, keep window position & size if possible
  - `preserve_size <dir>` → Move to another monitor, preserve size, reapply tiling mode
  - `wm_fullscreen` → Toggle WM-managed fullscreen (uses `_NET_WM_STATE_FULLSCREEN`)
- **KHotKeys integration** — automatic shortcut setup with `config.py`
- **Autostart support** — installs `.desktop` files into TDE's autostart folder

## What's New in v4.1
- Now with completely automatic uninstaller

---

## Installation

### 1. Install build dependencies

```bash
sudo apt update
sudo apt install -y build-essential libx11-dev libxinerama-dev
````

### 2. Clone and build

```bash
git clone https://codeberg.org/Setuzuna/Xakar.git
cd Xakar
make
```

### 3. Install

```bash
make install
```

This will:

* Install the binary `xakard` into `/usr/local/bin`
* Generate KHotKeys bindings (`~/.trinity/share/apps/khotkeys/setuzuna_xakar.khotkeys`)
* Create autostart entries for:

  * `xakard` (the tiling daemon)
  * `xcape` (optional Win key rebind)

### 4. Uninstall

```bash
make uninstall
```

This will remove:

* `/usr/local/bin/xakard`
* Autostart `.desktop` entries
* Custom KHotKeys config (`setuzuna_xakar`)

---

## Usage

Run manually:

```bash
xakard
```

It listens on:

```
$HOME/.xakar.sock
```

Send actions to it with:

```bash
echo left > ~/.xakar.sock
echo fullscreen > ~/.xakar.sock
echo preserve_geom right > ~/.xakar.sock
```

---

## Default Keybindings (via KHotKeys)

Installed automatically by `config.py`.

| Keys                   | Action                                   |
| ---------------------- | ---------------------------------------- |
| **Win+Up**             | `up` (top half / quarter)                |
| **Win+Down**           | `down` (bottom half / quarter)           |
| **Win+Left**           | `left` (left half / quarter)             |
| **Win+Right**          | `right` (right half / quarter)           |
| **Win+Return**         | `center` (3/4 centered)                  |
| **Win+Space**          | `fullscreen` (fill monitor)              |
| **Win+Shift+Space**    | `wm_fullscreen` (toggle WM fullscreen)   |
| **Win+Alt+\[Arrows]**  | `preserve_geom` move to neighbor monitor preserving geometry |
| **Win+Ctrl+\[Arrows]** | `preserve_size` move to neighbor monitor perserving tile mode|

---

## Example: Multi-monitor workflow

* **Win+Left** → Snap active window to left half of current monitor
* **Press again** → Push to the right half of neighbor monitor
* **Win+Alt+Right** → Move window to right monitor, preserving geometry
* **Win+Ctrl+Right** → Move window to right monitor, preserving size

---

## Development

Rebuild cleanly:

```bash
make clean && make
```

