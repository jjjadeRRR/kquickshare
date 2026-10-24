#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -nE 's/^project\(kquickshare VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || { printf 'Cannot determine source version\n' >&2; exit 1; }
# Explicit allow-list: excludes build output, settings and local files.
tar -czf "$root/packaging/kquickshare-$version.tar.gz" \
    --sort=name --owner=0 --group=0 --numeric-owner \
    --transform "s,^,kquickshare-$version/," \
    --exclude='__pycache__' --exclude='*.pyc' \
    -C "$root" CMakeLists.txt LICENSE NOTICE.md README.md README.ru.md CONTRIBUTING.md install.sh src docs tests integration translations \
    packaging/make-source.sh packaging/org.kde.kquickshare.desktop.in \
    packaging/kquickshare.notifyrc packaging/kquickshare-send.desktop.in \
    packaging/org.kde.kquickshare.service.in packaging/org.kde.kquickshare.systemd.in \
    packaging/CPackConfig.cmake packaging/dependencies.sh packaging/firewalld packaging/org.kde.kquickshare.metainfo.xml
printf 'Created %s\n' "$root/packaging/kquickshare-$version.tar.gz"
