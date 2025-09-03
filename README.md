# Tiler v1

Tiler is a shell script that uses **xdotool** to move and resize your active window into preset layouts — similar to a tiling window manager. It works well on traditional desktops like Trinity (TDE) or any X11-based environment.

## Features
- Tiling: *left, right, up, down, center, fullscreen*
- Auto-detects screen resolution or accepts manual input
- Supports user-defined borders and title bar height
- Keyboard shortcut integration (e.g., Win+Arrow keys)

## Installation

Download the script and make it executable:

```bash
chmod +x ~/bin/tiler.sh
````
> **Tip:** Add `~/bin` to your PATH if it's not already.

Make sure `xdotool` is installed:

```bash
sudo apt install xdotool
```

### How to Find Border and Title Bar Values

You must know your window's decoration sizes for accurate tiling.

If you're using **TDE with IceWM themes**:

1. Open **Control Panel → Appearance & Themes → Window Decorations**
2. Click **"Open TDE's IceWM Theme Folder"**
3. Navigate into the folder of the theme you are using
4. Open `default.theme`
5. Look for these lines:

   ```text
   BorderSizeX=
   BorderSizeY=
   TitleBarHeight=
   ```
6. Note the values and pass them as arguments to the script.

If you're using a **system-wide IceWM theme**:

```text
/opt/trinity/share/apps/twin/icewm-themes/
```

Then repeat the same steps in the theme’s `default.theme` file.

## Usage

```bash
tiler.sh ACTION [--width WIDTH] [--height HEIGHT] [--border-x PX] [--border-y PX] [--titlebar PX]
```

### Examples

```bash
tiler.sh left
tiler.sh --width 1366 --height 768 --border-x 2 --border-y 2 --titlebar 24 fullscreen
```

## Available Actions

* **fullscreen** – Maximize to usable area
* **left** – Left half of the screen
* **right** – Right half
* **up** – Top half or top quarter (depending on width)
* **down** – Bottom half or quarter
* **center** – 1024×600 window centered
