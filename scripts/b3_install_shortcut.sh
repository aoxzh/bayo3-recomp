#!/usr/bin/env bash
# Put a "Bayonetta 3" entry in the desktop menu and on the desktop, pointing at scripts/b3_launcher.sh.
#
#   scripts/b3_install_shortcut.sh [--remove]
#
# The entry is a .desktop file in ~/.local/share/applications (menu) and a copy on ~/Desktop. Nothing else is touched;
# --remove deletes both.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
APPS=${XDG_DATA_HOME:-$HOME/.local/share}/applications
ENTRY=$APPS/bayonetta3-recomp.desktop
DESKTOP_DIR=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
ICON=$ROOT/docs/icon.png
[[ -f $ICON ]] || ICON=applications-games

if [[ ${1:-} == --remove ]]; then
    rm -f -- "$ENTRY" "$DESKTOP_DIR/Bayonetta 3.desktop"
    printf 'removed %s\n' "$ENTRY"
    exit 0
fi

mkdir -p -- "$APPS"
cat >"$ENTRY" <<EOF
[Desktop Entry]
Type=Application
Name=Bayonetta 3
Comment=Bayonetta 3 recomp — launcher
Exec=$ROOT/scripts/b3_launcher.sh
Path=$ROOT
Icon=$ICON
Terminal=false
Categories=Game;
StartupWMClass=bayonetta3
EOF
chmod +x "$ENTRY"
if [[ -d $DESKTOP_DIR ]]; then
    cp -- "$ENTRY" "$DESKTOP_DIR/Bayonetta 3.desktop"
    chmod +x "$DESKTOP_DIR/Bayonetta 3.desktop"
fi
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
printf 'installed %s\n' "$ENTRY"
