#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=0.3.0
# Explicit allow-list: excludes build output, settings and local files.
tar -czf "$root/packaging/kquickshare-$version.tar.gz" \
    --sort=name --owner=0 --group=0 --numeric-owner \
    --transform "s,^,kquickshare-$version/," \
    --exclude='__pycache__' --exclude='*.pyc' \
    -C "$root" CMakeLists.txt LICENSE NOTICE.md README.md README.ru.md install.sh src docs tests integration translations \
    packaging/make-source.sh packaging/org.kde.kquickshare.desktop \
    packaging/kquickshare.notifyrc packaging/kquickshare-send.desktop \
    packaging/org.kde.kquickshare.service.in packaging/org.kde.kquickshare.systemd.in
printf 'Created %s\n' "$root/packaging/kquickshare-$version.tar.gz"
