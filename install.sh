#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix="$HOME/.local"
start_app=1
install_deps=1
print_deps=0
deps_only=0
distro_override=""
while (($#)); do
    case "$1" in
        --prefix)
            [[ $# -ge 2 ]] || { printf 'Missing directory after --prefix\n' >&2; exit 2; }
            prefix=$2; shift 2 ;;
        --distro)
            [[ $# -ge 2 ]] || { printf 'Missing distribution after --distro\n' >&2; exit 2; }
            distro_override=$2; shift 2 ;;
        --print-deps) print_deps=1; shift ;;
        --deps-only) deps_only=1; shift ;;
        --no-start) start_app=0; shift ;;
        --no-deps) install_deps=0; shift ;;
        --help|-h)
            printf 'Usage: ./install.sh [--prefix DIRECTORY] [--no-start] [--no-deps] [--print-deps] [--deps-only] [--distro ID]\n'
            printf 'Build and install KQuickShare, then start it in your desktop session.\n'
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ "$prefix" = /* ]] || { printf 'The installation prefix must be an absolute path.\n' >&2; exit 2; }
if ((install_deps || print_deps || deps_only)); then
    # shellcheck source=packaging/dependencies.sh
    source "$root/packaging/dependencies.sh"
    ID=unknown
    ID_LIKE=""
    [[ ! -r /etc/os-release ]] || source /etc/os-release
    [[ -z "$distro_override" ]] || ID_LIKE=""
    configure_dependencies "${distro_override:-$ID}" "$ID_LIKE"
    if ((print_deps)); then
        printf '%q ' "${dependency_command[@]}" "${dependency_packages[@]}"
        printf '\n'
        exit 0
    fi
fi
if ((EUID == 0 && !deps_only)) && [[ "$prefix" = "$HOME/.local" ]]; then
    printf 'Run ./install.sh as your desktop user, without sudo.\n' >&2
    exit 1
fi
run_admin() {
    if ((EUID == 0)); then "$@"; else
        command -v sudo >/dev/null 2>&1 || { printf 'Install dependencies manually; sudo is unavailable.\n' >&2; exit 1; }
        sudo "$@"
    fi
}
if ((install_deps || deps_only)); then
    if [[ -e /run/ostree-booted ]]; then
        printf 'Immutable systems need a native package or development environment. See docs/BUILDING.md; use --no-deps after preparing dependencies.\n' >&2
        exit 1
    fi
    missing=()
    for package in "${dependency_packages[@]}"; do
        case "$dependency_family" in
            arch) pacman -Q "$package" >/dev/null 2>&1 || missing+=("$package") ;;
            debian) [[ $(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true) = 'install ok installed' ]] || missing+=("$package") ;;
            *) rpm -q "$package" >/dev/null 2>&1 || missing+=("$package") ;;
        esac
    done
    if ((${#missing[@]})); then
        [[ "$dependency_family" != debian ]] || run_admin apt-get update
        printf 'Installing missing dependencies: %s\n' "${missing[*]}"
        run_admin "${dependency_command[@]}" "${missing[@]}"
    fi
fi
((!deps_only)) || exit 0

for tool in cmake ninja c++; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'Missing build tool: %s. See README for dependencies.\n' "$tool" >&2; exit 1; }
done

build_dir="${KQUICKSHARE_BUILD_DIR:-$root/build-install}"
jobs="${KQUICKSHARE_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 2)}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { printf 'KQUICKSHARE_BUILD_JOBS must be a positive integer.\n' >&2; exit 2; }
# Avoid exhausting memory on laptops with many logical CPUs.
((jobs <= 4)) || jobs=4
printf 'Building KQuickShare…\n'
cmake -S "$root" -B "$build_dir" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel "$jobs"
cmake --install "$build_dir"

if ((start_app)) && [[ "$prefix" = "$HOME/.local" ]]; then
    if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
        systemctl --user daemon-reload
        systemctl --user restart org.kde.kquickshare.service
        "$prefix/bin/kquickshare"
        printf '\nKQuickShare is running. Look for it in the system tray.\n'
    else
        printf '\nInstalled. Log in to KDE, then start KQuickShare from the application menu.\n'
    fi
else
    printf '\nInstalled to %s\n' "$prefix"
fi
