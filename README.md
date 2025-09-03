# Tiler v3.1 — Pure Python, No External Tools, Faster Than Ever

> A complete rewrite of the tiling daemon in pure Python using python-xlib.  
> No xdotool or wmctrl — direct X11 calls provide instant, flicker-free tiling.

## What's New in v3

- **Zero external CLI dependencies** — No xdotool, no wmctrl, just python-xlib
- **Direct X11 calls** — Faster, smoother window moves and resizes
- **Same FIFO control interface** — Drop-in replacement for v2 scripts
- **Still lightweight** — Single Python process, minimal CPU usage

### What's Changed in v3.1

- **Improved logic** — In line 106, changed `geom["height"] <= half_height` to `geom["height"] >= half_height`

---

## Available Actions

- **center** — 3/4 of screen, centered  
- **fullscreen** — Usable screen area  
- **left** — Left half  
- **right** — Right half  
- **up** — Top half or quarter  
- **down** — Bottom half or quarter  

---

## Installation

### 1. Install Dependencies

```bash
sudo apt install python3-xlib
````

### 2. Make Files executable:

```bash
chmod +x ~/bin/tilerd
```
```bash
chmod +x ~/bin/tiler.sh
```

> **Tip:** Ensure `~/bin/tiler.sh` is in your PATH or bound to shortcuts.

---

## Usage

Start the daemon:

```bash
tilerd
```

Send commands to the daemon:

```bash
tiler.sh left
tiler.sh fullscreen
tiler.sh center
```

---

## Autostart with Trinity (TDE)

1. Install Autostart Manager:

```bash
sudo apt-get install kcontrol-autostart-trinity -y
```

2. Open **Control Panel → TDE Components → Autostart Manager**
3. Click **Add**, check **Run in terminal**, enter:

```bash
~/bin/tilerd
```

4. Press **Enter**, name it `tiler-daemon`, then confirm.

---

## Bind Tiling to Win+Key

Use TDE's Input Actions:

1. Open **Control Panel → Regional & Accessibility → Input Actions**
2. Click **New Group**, name it `Tiling`
3. Click **New Action**, name it e.g., `Tile Left`
4. Set **Action Type**: Keyboard Shortcut → Command/URL (simple)
5. On the **Keyboard Shortcut** tab, set your key (e.g., Win+Left)
6. On **Command/URL Settings**, enter:

```bash
~/bin/tiler.sh left
```

7. Repeat for each direction:

| Key        | Command                     |
| ---------- | --------------------------- |
| Win+Left   | `~/bin/tiler.sh left`       |
| Win+Right  | `~/bin/tiler.sh right`      |
| Win+Up     | `~/bin/tiler.sh up`         |
| Win+Down   | `~/bin/tiler.sh down`       |
| Win+Return | `~/bin/tiler.sh fullscreen` |
| Win+Space  | `~/bin/tiler.sh center`     |