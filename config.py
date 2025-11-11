import argparse
import configparser
import os
import re
import shutil
import sys
from datetime import datetime
from tempfile import NamedTemporaryFile

CONFIG_PATH = os.path.expanduser("~/.trinity/share/apps/khotkeys/setuzuna_xakar.khotkeys")
EMPTY_PATH = os.path.expanduser("~/.trinity/share/apps/khotkeys/setuzuna_empty.khotkeys")
AUTOSTART_PATH = os.path.expanduser("~/.trinity/Autostart/xakar-daemon.desktop")
XCAPE_PATH = os.path.expanduser("~/.trinity/Autostart/xcape-daemon.desktop")
KHOTKEYS_PATH = os.path.expanduser("~/.trinity/share/config/khotkeysrc")
TARGET_NAME = "Setuzuna Xakar Bindings"

ACTIONS = [
    ("Up", "echo tile up > $HOME/.setuzuna/xakar/internal.sock", "Win+Up"),
    ("Down", "echo tile down > $HOME/.setuzuna/xakar/internal.sock", "Win+Down"),
    ("Left", "echo tile left > $HOME/.setuzuna/xakar/internal.sock", "Win+Left"),
    ("Right", "echo tile right > $HOME/.setuzuna/xakar/internal.sock", "Win+Right"),
    ("Center", "echo tile center > $HOME/.setuzuna/xakar/internal.sock", "Win+Return"),
    ("Fullscreen", "echo tile fullscreen > $HOME/.setuzuna/xakar/internal.sock", "Win+Space"),
    ("WM_Fullscreen", "echo tile wm_fullscreen > $HOME/.setuzuna/xakar/internal.sock", "Win+Shift+Space"),
    ("PreserveGeomUp", "echo tile up preserve_geom > $HOME/.setuzuna/xakar/internal.sock", "Win+Alt+Up"),
    ("PreserveGeomDown", "echo tile down preserve_geom > $HOME/.setuzuna/xakar/internal.sock", "Win+Alt+Down"),
    ("PreserveGeomLeft", "echo tile left preserve_geom > $HOME/.setuzuna/xakar/internal.sock", "Win+Alt+Left"),
    ("PreserveGeomRight", "echo tile right preserve_geom > $HOME/.setuzuna/xakar/internal.sock", "Win+Alt+Right"),
    ("PreserveGeomCenter", "echo tile center preserve_geom > $HOME/.setuzuna/xakar/internal.sock", "Win+Alt+Return"),
    ("PreserveGeomFullscreen", "echo tile fullscreen preserve_geom > $HOME/.setuzuna/xakar/internal.sock", "Win+Alt+Space"),
    ("PreserveGeomWM_Fullscreen", "echo tile wm_fullscreen preserve_geom > $HOME/.setuzuna/xakar/internal.sock", "Win+Alt+Shift+Space"),
    ("PreserveSizeUp", "echo tile up preserve_mode > $HOME/.setuzuna/xakar/internal.sock", "Win+Ctrl+Up"),
    ("PreserveSizeDown", "echo tile down preserve_mode > $HOME/.setuzuna/xakar/internal.sock", "Win+Ctrl+Down"),
    ("PreserveSizeLeft", "echo tile left preserve_mode > $HOME/.setuzuna/xakar/internal.sock", "Win+Ctrl+Left"),
    ("PreserveSizeRight", "echo tile right preserve_mode > $HOME/.setuzuna/xakar/internal.sock", "Win+Ctrl+Right"),
    ("PreserveSizeCenter", "echo tile center preserve_mode > $HOME/.setuzuna/xakar/internal.sock", "Win+Ctrl+Return"),
    ("PreserveSizeFullscreen", "echo tile fullscreen preserve_mode > $HOME/.setuzuna/xakar/internal.sock", "Win+Ctrl+Space"),
    ("PreserveSizeWM_Fullscreen", "echo tile wm_fullscreen preserve_mode > $HOME/.setuzuna/xakar/internal.sock", "Win+Ctrl+Shift+Space"),
]

def generate_block() -> str:
    lines = []

    # Header
    lines.append("[Data]")
    lines.append("DataCount=1\n")
    lines.append("[Main]")
    lines.append("ImportId=setuzuna_xakar")
    lines.append("Version=2\n")

    # Main group
    lines.append("[Data_1]")
    lines.append("Comment=")
    lines.append(f"DataCount={len(ACTIONS)}")
    lines.append("Enabled=true")
    lines.append(f"Name={TARGET_NAME}")
    lines.append("SystemGroup=0")
    lines.append("Type=ACTION_DATA_GROUP\n")

    lines.append("[Data_1Conditions]")
    lines.append("Comment=")
    lines.append("ConditionsCount=0\n")

    # Actions
    for idx, (name, cmd, key) in enumerate(ACTIONS, start=1):
        lines.append(f"[Data_1_{idx}]")
        lines.append("Comment=")
        lines.append("Enabled=true")
        lines.append(f"Name={name}")
        lines.append("Type=COMMAND_URL_SHORTCUT_ACTION_DATA\n")

        lines.append(f"[Data_1_{idx}Actions]")
        lines.append("ActionsCount=1\n")

        lines.append(f"[Data_1_{idx}Actions0]")
        lines.append(f"CommandURL={cmd}")
        lines.append("Type=COMMAND_URL\n")

        lines.append(f"[Data_1_{idx}Conditions]")
        lines.append("Comment=")
        lines.append("ConditionsCount=0\n")

        lines.append(f"[Data_1_{idx}Triggers]")
        lines.append("Comment=Simple_action")
        lines.append("TriggersCount=1\n")

        lines.append(f"[Data_1_{idx}Triggers0]")
        lines.append(f"Key={key}")
        lines.append("Type=SHORTCUT\n")

    return "\n".join(lines)


def install_shortcuts():
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w") as f:
        f.write(generate_block())
    print(f"[xakar] KHotKeys config written to {CONFIG_PATH}")


def install_autostart():
    os.makedirs(os.path.dirname(AUTOSTART_PATH), exist_ok=True)
    desktop_entry = """
[Desktop Entry]
Exec=xakard
Type=Application
StartupNotify=true
Terminal=false
"""
    with open(AUTOSTART_PATH, "w") as f:
        f.write(desktop_entry)
    print(f"[xakar] Autostart entry created at {AUTOSTART_PATH}")


def install_xcape_autostart():
    os.makedirs(os.path.dirname(XCAPE_PATH), exist_ok=True)
    desktop_entry = """
[Desktop Entry]
Exec=xcape -e 'Super_L=Control_L|Super_L|Z'
Type=Application
StartupNotify=true
Terminal=false
"""
    with open(XCAPE_PATH, "w") as f:
        f.write(desktop_entry)
    print(f"[xakar] Autostart entry created at {XCAPE_PATH}")

def install_empty_config():
    os.makedirs(os.path.dirname(EMPTY_PATH), exist_ok=True)
    empty_content = """[Data]
DataCount=1

[Main]
ImportId=setuzuna_empty
Version=2
"""
    with open(EMPTY_PATH, "w") as f:
        f.write(empty_content)
    print(f"[xakar] KHotKeys empty config written to {EMPTY_PATH}")

def uninstall_shortcuts():
    parser = configparser.RawConfigParser(strict=False, allow_no_value = True)
    parser.optionxform = str
    if os.path.exists(KHOTKEYS_PATH):
        try:
            with open(KHOTKEYS_PATH, "r", encoding="utf-8") as f:
                parser.read_file(f)
        except Exception:
            parser.read(KHOTKEYS_PATH, encoding="utf-8")
    all_sections = parser.sections()
    prefixes = set()
    for sec in parser.sections():
        try:
            if parser.has_option(sec, "Name"):
                val = parser.get(sec, "Name")
                if val == TARGET_NAME:
                    prefixes.add(sec)
        except Exception:
            continue

    if not prefixes:
        print("[xakar] No sections with Name={} found. Nothing to do.".format(repr(TARGET_NAME)))
        return 0

    to_remove = set()
    for prefix in prefixes:
        pat = re.compile(r"^" + re.escape(prefix) + r"($|[^0-9])")
        for sec in all_sections:
            if pat.match(sec):
                to_remove.add(sec)

    to_remove = sorted(to_remove)
    
    if not to_remove:
        print("[xakar] Found prefixes:", prefixes)
        print("[xakar] But no matching sections to remove (after applying digit-boundary rule).")
        return 0

    print("[xakar] Found prefixes (sections with Name={}):".format(repr(TARGET_NAME)))
    for p in sorted(prefixes):
        print("[xakar]  -", p)

    dirname = os.path.dirname(KHOTKEYS_PATH) or "."
    base = os.path.basename(KHOTKEYS_PATH)
    ts = datetime.now().strftime("%Y%m%dT%H%M%S")
    bak = os.path.join(dirname, f"{base}.bak.{ts}")
    shutil.copy2(KHOTKEYS_PATH, bak)

    if bak:
        print("[xakar] Backup created:", bak)
    else:
        print("[xakar] No backup created (file did not exist? unexpected).")

    for s in to_remove:
        try:
            parser.remove_section(s)
        except Exception as e:
            print(f"[xakar] Warning: failed to remove section {s}: {e}", file=sys.stderr)

    dirn = os.path.dirname(KHOTKEYS_PATH) or "."
    with NamedTemporaryFile("w", encoding="utf-8", delete=False, dir=dirn) as tf:
        parser.write(tf)
        tmpname = tf.name
    os.replace(tmpname, KHOTKEYS_PATH)
    print("[xakar] Wrote modified file:", KHOTKEYS_PATH)
    print("[xakar] Removed {} sections.".format(len(to_remove)))
    return 0

def main():
    parser = argparse.ArgumentParser(description="Manage installation/uninstallation of shortcuts and autostart configs.")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--install", action="store_true", help="Install shortcuts and autostart configs")
    group.add_argument("--uninstall", action="store_true", help="Uninstall shortcuts and restore empty config")

    args = parser.parse_args()

    if args.install:
        #install_shortcuts()
        install_autostart()
        #install_xcape_autostart()
    elif args.uninstall:
        install_empty_config()
        uninstall_shortcuts()

if __name__ == "__main__":
    main()
