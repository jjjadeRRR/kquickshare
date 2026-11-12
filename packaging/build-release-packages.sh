#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Debian 13, Fedora 43 and ALT p11 packages from the release source
# archive in rootless podman containers (tests run inside each build), then
# attaches them to the GitHub release with an updated SHA256SUMS.
#
# Usage: packaging/build-release-packages.sh [--no-upload] [deb] [rpm] [alt]
# Without targets all three are built. Packages that build are uploaded even
# if another target fails; SHA256SUMS keeps the entries already published.
# Run packaging/make-source.sh first; the archive must be the uploaded one.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -nE 's/^project\(kquickshare VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$root/CMakeLists.txt")
archive="$root/packaging/kquickshare-$version.tar.gz"
[ -f "$archive" ] || { printf 'Missing %s; run packaging/make-source.sh\n' "$archive" >&2; exit 1; }
upload=1
targets=""
for arg in "$@"; do
    case "$arg" in
        --no-upload) upload=0 ;;
        deb | rpm | alt) targets="$targets $arg" ;;
        *) printf 'Unknown argument: %s\n' "$arg" >&2; exit 2 ;;
    esac
done
[ -n "$targets" ] || targets="deb rpm alt"
wants() { case " $targets " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }
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

deb=""; rpm=""
wants deb && { podman run --rm -v "$out:/out:Z" docker.io/library/debian:13 sh -c 'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -qq -y --no-install-recommends ca-certificates file dpkg-dev desktop-file-utils build-essential cmake ninja-build qt6-base-dev qt6-svg-dev qt6-wayland qt6-tools-dev qt6-tools-dev-tools libssl-dev libkf6notifications-dev libkf6statusnotifieritem-dev libkf6prison-dev libkf6guiaddons-dev dbus-daemon python3-cryptography >/dev/null && sh /out/inside.sh DEB' >"$out/deb.log" 2>&1 & deb=$!; }
wants rpm && { podman run --rm -v "$out:/out:Z" registry.fedoraproject.org/fedora:43 sh -c 'dnf install -q -y desktop-file-utils gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwayland qt6-qttools-devel openssl-devel kf6-knotifications-devel kf6-kstatusnotifieritem-devel kf6-prison-devel kf6-kguiaddons-devel dbus-daemon python3-cryptography rpm-build >/dev/null && sh /out/inside.sh RPM' >"$out/rpm.log" 2>&1 & rpm=$!; }

failed=""
built=""
if [ -n "$deb" ]; then
    if wait "$deb"; then built="$built kquickshare_${version}_amd64.deb"; else failed="$failed deb"; fi
fi
if [ -n "$rpm" ]; then
    if wait "$rpm"; then built="$built kquickshare-$version-1.x86_64.rpm"; else failed="$failed rpm"; fi
fi

# ALT runs after the others: its rpm refuses to install when it thinks the
# container root is short of space (seen with parallel builds), so the size
# check is also disabled; a real lack of space still fails the build later.
if wants alt; then
    if podman run --rm -v "$out:/out:Z" docker.io/alt:p11 sh -c "
set -e
apt-get update -qq && apt-get install -y -qq -o RPM::Install-Options::=--ignoresize rpm-build rpm-macros-cmake cmake ctest ninja-build gcc-c++ qt6-base-devel qt6-tools-devel libssl-devel kf6-knotifications-devel kf6-kstatusnotifieritem-devel kf6-prison-devel kf6-kguiaddons-devel python3-module-cryptography dbus libqt6-svg qt6-wayland >/dev/null
useradd -m builder
mkdir -p /home/builder/RPM/SOURCES && cp /out/kquickshare-$version.tar.gz /home/builder/RPM/SOURCES/ && cp /out/kquickshare.spec /home/builder/ && chown -R builder /home/builder
cd /home/builder && HOME=/home/builder setpriv --reuid=builder --regid=builder --init-groups rpmbuild -ba kquickshare.spec
cp /home/builder/RPM/RPMS/*/*.rpm /home/builder/RPM/SRPMS/*.rpm /out/
" >"$out/alt.log" 2>&1; then
        built="$built kquickshare-$version-alt1.x86_64.rpm kquickshare-$version-alt1.src.rpm"
    else
        failed="$failed alt"
    fi
fi

for target in $failed; do printf '%s build failed, see %s/%s.log\n' "$target" "$out" "$target" >&2; done
for p in $built; do [ -f "$out/$p" ] || { printf 'Missing %s\n' "$p" >&2; exit 1; }; done
[ -n "$built" ] || exit 1

cd "$out"
if [ $upload -eq 1 ]; then
    # Keep checksums of assets published earlier, replacing the rebuilt ones.
    gh -R jjjadeRRR/kquickshare release download "v$version" -p SHA256SUMS -O published.sha256 2>/dev/null || : >published.sha256
    sha256sum "kquickshare-$version.tar.gz" $built >new.sha256
    { cat new.sha256; awk 'NR == FNR { seen[$2] = 1; next } !($2 in seen)' new.sha256 published.sha256; } | sort -k2 >SHA256SUMS
    cat SHA256SUMS
    gh -R jjjadeRRR/kquickshare release upload "v$version" $built SHA256SUMS --clobber
    printf 'Uploaded to https://github.com/jjjadeRRR/kquickshare/releases/tag/v%s\n' "$version"
else
    sha256sum "kquickshare-$version.tar.gz" $built
    printf 'Packages are in %s\n' "$out"
fi
[ -z "$failed" ]
