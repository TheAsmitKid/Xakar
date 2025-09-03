# Tiler v2 — FIFO-Based, Faster, Smarter Window Tiling

> A complete rewrite of the original xdotool-based Tiler, now using a persistent daemon and smarter tiling logic.

## What's New?

- **Persistent background process** — A lightweight daemon listens on `~/.tiler.sock` for commands
- **Faster performance** — Uses a more efficient Python implementation
- **Improved layout logic** — Smarter handling of window quarters and toggling between half/full dimensions
- **No need to pass border/titlebar dimensions manually** — Tiler now calculates frame overhead using `xwininfo`
- **Python 3 rewrite** — Cleaner, easier to maintain, and more hackable
- **New center behavior** — Places a 3/4 screen-sized window centered on the screen

## Installation

### Make Files executable:
```bash
chmod +x ~/bin/tilerd
```

```bash
chmod +x ~/bin/tiler.sh
```

> **Tip:** Ensure `~/bin/tiler.sh` is in your PATH or bound to shortcuts.

### 3. Dependencies

Install required tools:

```bash
sudo apt install python3 xdotool wmctrl x11-utils
```

---

## Usage

### Available Actions

* **center** — 3/4 of screen, centered
* **fullscreen** — Usable screen area
* **left** — Left half
* **right** — Right half
* **up** — Top half or quarter
* **down** — Bottom half or quarter

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
