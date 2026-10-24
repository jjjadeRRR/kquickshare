#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# This overlay replaces private QML, so refuse unverified Plasma series before
# writing anything. Ordinary sending/receiving does not require this integration.
version=$(plasmashell --version 2>/dev/null | awk '$1 == "plasmashell" { print $2 }')
case "$version" in
    6.8.[0-9]*) ;;
    *) printf 'The clipboard-history overlay supports Plasma 6.8.x only (found: %s). Use the tray/Dolphin actions instead.\n' "${version:-unknown}" >&2; exit 1 ;;
esac
target="${XDG_DATA_HOME:-$HOME/.local/share}/kquickshare/qml"
module="$target/org/kde/plasma/private/clipboard"
if command -v qtpaths6 >/dev/null 2>&1; then
    qtqml=$(qtpaths6 --query QT_INSTALL_QML)
else
    qtqml=$(qmake6 -query QT_INSTALL_QML)
fi
plugin="$qtqml/org/kde/plasma/private/clipboard/libklipperplugin.so"
[ -f "$plugin" ] || { printf 'Plasma Clipboard QML module not found: %s\n' "$plugin" >&2; exit 1; }
mkdir -p "$module"
cp "$base/qml/org/kde/plasma/private/clipboard/"*.qml "$module/"
cp "$base/qml/org/kde/plasma/private/clipboard/qmldir" "$module/"
ln -sfn "$plugin" "$module/libklipperplugin.so"
dropin="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/plasma-plasmashell.service.d/kquickshare-clipboard.conf"
mkdir -p "$(dirname -- "$dropin")"
if [ -f "$dropin" ]; then
    IFS= read -r first < "$dropin"
    [ "$first" = '# Managed by KQuickShare clipboard integration' ] || { printf 'Refusing to overwrite %s\n' "$dropin" >&2; exit 1; }
fi
printf '# Managed by KQuickShare clipboard integration\n[Service]\nEnvironment="QML_IMPORT_PATH=%s"\n' "$target${QML_IMPORT_PATH:+:$QML_IMPORT_PATH}" > "$dropin"
systemctl --user daemon-reload
printf 'Clipboard action installed for both the tray and Meta+V.\nRestart plasma-plasmashell.service to load it.\n'
