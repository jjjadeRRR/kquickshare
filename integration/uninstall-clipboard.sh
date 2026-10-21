#!/bin/sh
set -eu
dropin="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/plasma-plasmashell.service.d/kquickshare-clipboard.conf"
if [ -f "$dropin" ]; then
    IFS= read -r first < "$dropin"
    [ "$first" = '# Managed by KQuickShare clipboard integration' ] || { printf 'Refusing to remove unmanaged %s\n' "$dropin" >&2; exit 1; }
    rm "$dropin"
fi
systemctl --user daemon-reload
printf 'Native clipboard interface restored. Restart plasma-plasmashell.service to load it.\nThe inactive overlay files and clipboard history are preserved.\n'
