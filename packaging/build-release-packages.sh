#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Debian 13, Fedora 43 and ALT p11 packages from the release source
# archive in rootless podman containers (tests run inside each build), then
# attaches them to the GitHub release with an updated SHA256SUMS.
#
# Usage: packaging/build-release-packages.sh [--no-upload]
# Run packaging/make-source.sh first; the archive must be the uploaded one.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -nE 's/^project\(kquickshare VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/CMakeLists.txt")
archive="$root/packaging/kquickshare-$version.tar.gz"
[ -f "$archive" ] || { printf 'Missing %s; run packaging/make-source.sh\n' "$archive" >&2; exit 1; }
upload=1
[ "${1:-}" != "--no-upload" ] || upload=0
out=$(mktemp -d "${TMPDIR:-/tmp}/kquickshare-packages.XXXXXX")
cp "$archive" "$root/packaging/alt/kquickshare.spec" "$out/"
printf 'Building into %s (logs: *.log)\n' "$out"

cat >"$out/inside.sh" <<EOF
#!/bin/sh
set -eu
cd /tmp && tar -xzf /out/kquickshare-$version.tar.gz && cd kquickshare-$version
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tests/fake_sender.py --binary build/kquickshare-cli
python3 tests/fake_receiver.py --binary build/kquickshare-send
(cd build && cpack -G "\$1")
cp build/kquickshare*.deb build/kquickshare*.rpm /out/ 2>/dev/null || true
EOF

podman run --rm -v "$out:/out:Z" docker.io/library/debian:13 sh -c 'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -qq -y --no-install-recommends ca-certificates file dpkg-dev desktop-file-utils build-essential cmake ninja-build qt6-base-dev qt6-svg-dev qt6-wayland qt6-tools-dev qt6-tools-dev-tools libssl-dev libkf6notifications-dev libkf6statusnotifieritem-dev libkf6prison-dev libkf6guiaddons-dev dbus-daemon python3-cryptography >/dev/null && sh /out/inside.sh DEB' >"$out/deb.log" 2>&1 &
deb=$!
podman run --rm -v "$out:/out:Z" registry.fedoraproject.org/fedora:43 sh -c 'dnf install -q -y desktop-file-utils gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwayland qt6-qttools-devel openssl-devel kf6-knotifications-devel kf6-kstatusnotifieritem-devel kf6-prison-devel kf6-kguiaddons-devel dbus-daemon python3-cryptography rpm-build >/dev/null && sh /out/inside.sh RPM' >"$out/rpm.log" 2>&1 &
rpm=$!
podman run --rm -v "$out:/out:Z" docker.io/alt:p11 sh -c "
set -e
apt-get update -qq && apt-get install -y -qq rpm-build rpm-macros-cmake cmake ctest ninja-build gcc-c++ qt6-base-devel qt6-tools-devel libssl-devel kf6-knotifications-devel kf6-kstatusnotifieritem-devel kf6-prison-devel kf6-kguiaddons-devel python3-module-cryptography dbus libqt6-svg qt6-wayland >/dev/null
useradd -m builder
mkdir -p /home/builder/RPM/SOURCES && cp /out/kquickshare-$version.tar.gz /home/builder/RPM/SOURCES/ && cp /out/kquickshare.spec /home/builder/ && chown -R builder /home/builder
cd /home/builder && HOME=/home/builder setpriv --reuid=builder --regid=builder --init-groups rpmbuild -ba kquickshare.spec
cp /home/builder/RPM/RPMS/*/*.rpm /home/builder/RPM/SRPMS/*.rpm /out/
" >"$out/alt.log" 2>&1 &
alt=$!

failed=0
wait $deb || { printf 'Debian build failed, see %s/deb.log\n' "$out" >&2; failed=1; }
wait $rpm || { printf 'Fedora build failed, see %s/rpm.log\n' "$out" >&2; failed=1; }
wait $alt || { printf 'ALT build failed, see %s/alt.log\n' "$out" >&2; failed=1; }
[ $failed -eq 0 ] || exit 1

packages="kquickshare_${version}_amd64.deb kquickshare-$version-1.x86_64.rpm kquickshare-$version-alt1.x86_64.rpm kquickshare-$version-alt1.src.rpm"
for p in $packages; do [ -f "$out/$p" ] || { printf 'Missing %s\n' "$p" >&2; exit 1; }; done
cp "$root/packaging/PKGBUILD" "$out/"
(cd "$out" && sha256sum "kquickshare-$version.tar.gz" $packages > SHA256SUMS && cat SHA256SUMS)
if [ $upload -eq 1 ]; then
    (cd "$out" && gh release upload "v$version" $packages SHA256SUMS --clobber)
    printf 'Uploaded to https://github.com/jjjadeRRR/kquickshare/releases/tag/v%s\n' "$version"
else
    printf 'Packages are in %s\n' "$out"
fi
