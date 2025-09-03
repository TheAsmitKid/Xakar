import os

CONFIG_PATH = os.path.expanduser("~/.trinity/share/apps/khotkeys/setuzuna_xakar.khotkeys")
AUTOSTART_PATH = os.path.expanduser("~/.trinity/Autostart/xakar-daemon.desktop")
XCAPE_PATH = os.path.expanduser("~/.trinity/Autostart/xcape-daemon.desktop")

ACTIONS = [
    ("Up", "echo up > $HOME/.xakar.sock", "Win+Up"),
    ("Down", "echo down > $HOME/.xakar.sock", "Win+Down"),
    ("Left", "echo left > $HOME/.xakar.sock", "Win+Left"),
    ("Right", "echo right > $HOME/.xakar.sock", "Win+Right"),
    ("Center", "echo center > $HOME/.xakar.sock", "Win+Return"),
    ("Fullscreen", "echo fullscreen > $HOME/.xakar.sock", "Win+Space"),
    ("WM_Fullscreen", "echo wm_fullscreen > $HOME/.xakar.sock", "Win+Shift+Space"),
    ("PreserveGeomUp", "echo preserve_geom up > $HOME/.xakar.sock", "Win+Alt+Up"),
    ("PreserveGeomDown", "echo preserve_geom down > $HOME/.xakar.sock", "Win+Alt+Down"),
    ("PreserveGeomLeft", "echo preserve_geom left > $HOME/.xakar.sock", "Win+Alt+Left"),
    ("PreserveGeomRight", "echo preserve_geom right > $HOME/.xakar.sock", "Win+Alt+Right"),
    ("PreserveGeomCenter", "echo preserve_geom center > $HOME/.xakar.sock", "Win+Alt+Return"),
    ("PreserveGeomFullscreen", "echo preserve_geom fullscreen > $HOME/.xakar.sock", "Win+Alt+Space"),
    ("PreserveGeomWM_Fullscreen", "echo preserve_geom wm_fullscreen > $HOME/.xakar.sock", "Win+Alt+Shift+Space"),
    ("PreserveSizeUp", "echo preserve_size up > $HOME/.xakar.sock", "Win+Ctrl+Up"),
    ("PreserveSizeDown", "echo preserve_size down > $HOME/.xakar.sock", "Win+Ctrl+Down"),
    ("PreserveSizeLeft", "echo preserve_size left > $HOME/.xakar.sock", "Win+Ctrl+Left"),
    ("PreserveSizeRight", "echo preserve_size right > $HOME/.xakar.sock", "Win+Ctrl+Right"),
    ("PreserveSizeCenter", "echo preserve_size center > $HOME/.xakar.sock", "Win+Ctrl+Return"),
    ("PreserveSizeFullscreen", "echo preserve_size fullscreen > $HOME/.xakar.sock", "Win+Ctrl+Space"),
    ("PreserveSizeWM_Fullscreen", "echo preserve_size wm_fullscreen > $HOME/.xakar.sock", "Win+Ctrl+Shift+Space"),
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
    lines.append("Name=Setuzuna Xakar Bindings")
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


if __name__ == "__main__":
    install_shortcuts()
    install_autostart()
    install_xcape_autostart()
