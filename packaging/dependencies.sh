# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by install.sh. All package names come from official distro repositories.
configure_dependencies() {
    local distro=${1:-} like=${2:-}
    case " $distro $like " in
        *' arch '*|*' manjaro '*|*' endeavouros '*|*' cachyos '*)
            dependency_family=arch
            dependency_command=(pacman -S --needed)
            dependency_packages=(base-devel cmake ninja qt6-base qt6-svg qt6-wayland qt6-tools openssl knotifications kstatusnotifieritem prison kguiaddons)
            ;;
        *' debian '*|*' ubuntu '*|*' neon '*)
            dependency_family=debian
            dependency_command=(apt-get install)
            dependency_packages=(build-essential cmake ninja-build qt6-base-dev qt6-svg-dev qt6-wayland qt6-tools-dev qt6-tools-dev-tools libssl-dev libkf6notifications-dev libkf6statusnotifieritem-dev libkf6prison-dev libkf6guiaddons-dev)
            ;;
        *' fedora '*)
            dependency_family=fedora
            dependency_command=(dnf install)
            dependency_packages=(gcc-c++ cmake ninja-build qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwayland qt6-qttools-devel openssl-devel kf6-knotifications-devel kf6-kstatusnotifieritem-devel kf6-prison-devel kf6-kguiaddons-devel)
            ;;
        *' opensuse '*|*' opensuse-tumbleweed '*|*' opensuse-leap '*)
            dependency_family=opensuse
            dependency_command=(zypper install)
            dependency_packages=(gcc-c++ cmake ninja qt6-base-devel qt6-svg-devel qt6-wayland qt6-linguist-devel libopenssl-devel kf6-knotifications-devel kf6-kstatusnotifieritem-devel kf6-prison-devel kf6-kguiaddons-devel)
            ;;
        *)
            printf 'Unsupported distribution: %s. Install the dependencies in docs/BUILDING.md, then use ./install.sh --no-deps.\n' "$distro" >&2
            return 1 ;;
    esac
}
