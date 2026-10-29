Name: kquickshare
Version: 0.5.0
Release: alt1

Summary: Send and receive Android Quick Share files and text on KDE Plasma
License: GPL-3.0-or-later and GPL-2.0-or-later
Group: Networking/File transfer
Url: https://github.com/jjjadeRRR/kquickshare

Source: %name-%version.tar.gz

BuildRequires(pre): rpm-macros-cmake
BuildRequires: cmake ninja-build gcc-c++
BuildRequires: qt6-base-devel qt6-tools-devel libssl-devel
BuildRequires: kf6-knotifications-devel kf6-kstatusnotifieritem-devel kf6-prison-devel kf6-kguiaddons-devel
# %%check: encrypted transfer peers and the GUI tests on a private session bus
BuildRequires: python3-module-cryptography dbus

# Loaded as plugins, so automatic library dependencies do not find them.
Requires: libqt6-svg qt6-wayland

%description
Native Quick Share for KDE Plasma 6. Send files and text to Android phones
and receive them in the tray over the local network, without a companion
Android application. Uses encrypted UKEY2 / AES-256-CBC transport.

%prep
%setup

%build
%cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
%cmake_build

%install
%cmake_install

%check
cd %_cmake__builddir
ctest --output-on-failure
python3 ../tests/fake_sender.py --binary ./kquickshare-cli
python3 ../tests/fake_receiver.py --binary ./kquickshare-send

%files
%_bindir/kquickshare
%_bindir/kquickshare-cli
%_desktopdir/org.kde.kquickshare.desktop
%_datadir/kio/servicemenus/kquickshare-send.desktop
%_datadir/knotifications6/kquickshare.notifyrc
%_datadir/dbus-1/services/org.kde.kquickshare.service
%_datadir/systemd/user/org.kde.kquickshare.service
%_datadir/metainfo/org.kde.kquickshare.metainfo.xml
%_iconsdir/hicolor/*/apps/org.kde.kquickshare*.svg
%_datadir/kquickshare
%_datadir/licenses/kquickshare
%_docdir/kquickshare
%dir %_prefix/lib/firewalld
%dir %_prefix/lib/firewalld/services
%_prefix/lib/firewalld/services/kquickshare.xml

%changelog
* Thu Oct 30 2026 jjjadeRRR <jjjadeRRR@users.noreply.github.com> 0.5.0-alt1
- Initial build for ALT Linux.
