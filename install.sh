#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix="$HOME/.local"
start_app=1
install_deps=1
while (($#)); do
    case "$1" in
        --prefix)
            [[ $# -ge 2 ]] || { printf 'Missing directory after --prefix\n' >&2; exit 2; }
            prefix=$2; shift 2 ;;
        --no-start) start_app=0; shift ;;
        --no-deps) install_deps=0; shift ;;
        --help|-h)
            printf 'Usage: ./install.sh [--prefix DIRECTORY] [--no-start] [--no-deps]\n'
            printf 'Build and install KQuickShare, then start it in your desktop session.\n'
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ "$prefix" = /* ]] || { printf 'The installation prefix must be an absolute path.\n' >&2; exit 2; }
if ((EUID == 0)) && [[ "$prefix" = "$HOME/.local" ]]; then
    printf 'Run ./install.sh as your desktop user, without sudo.\n' >&2
    exit 1
fi

if ((install_deps)) && command -v pacman >/dev/null 2>&1; then
    required=(base-devel cmake ninja qt6-base qt6-svg qt6-tools openssl knotifications kstatusnotifieritem prison)
    missing=()
    while IFS= read -r package; do
        [[ -z "$package" ]] || missing+=("$package")
    done < <(pacman -T "${required[@]}" || true)
    if ((${#missing[@]})); then
        printf 'Installing missing dependencies: %s\n' "${missing[*]}"
        if ((EUID == 0)); then
            pacman -S --needed "${missing[@]}"
        else
            command -v sudo >/dev/null 2>&1 || { printf 'Install the listed dependencies first; sudo is unavailable.\n' >&2; exit 1; }
            sudo pacman -S --needed "${missing[@]}"
        fi
    fi
fi
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
