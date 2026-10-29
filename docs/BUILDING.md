# Building and installing KQuickShare

The normal desktop installation needs one command from an extracted source tree:

```sh
./install.sh
```

The installer detects Arch/derivatives, Debian/Ubuntu (with KF6 packages), Fedora
and openSUSE. It checks installed packages and uses the native package manager
with sudo when needed. It never adds a repository, upgrades the desktop, changes
the firewall or enables the private clipboard overlay. It builds with at most
four parallel jobs and installs into `~/.local`. Finish current transfers first.

Preview the dependency command without installing anything:

```sh
./install.sh --print-deps
```

`--distro debian|ubuntu|arch|fedora|opensuse-tumbleweed` explicitly selects the
package family, useful for derivatives. `--deps-only` installs only dependencies;
`--no-deps` skips package management. Debian needs KF6 packages (Debian 13+);
Ubuntu-based Plasma 5 systems such as Kubuntu 24.04 need a newer distribution,
not an extra repository silently added by this script.

See [COMPATIBILITY.md](COMPATIBILITY.md) for tested environments, Plasma versions,
firewall setup and the experimental clipboard-history overlay.

For an installation without starting or restarting the application:

```sh
./install.sh --no-start
```

For a custom installation, for example a packaging smoke test:

```sh
./install.sh --prefix /tmp/kquickshare-test --no-start --no-deps
```

Custom prefixes are not started automatically: configure desktop/D-Bus/systemd
search paths yourself, or use the standard `~/.local` prefix.
`KQUICKSHARE_BUILD_DIR` selects a separate build directory;
`KQUICKSHARE_BUILD_JOBS` selects a positive parallel job count, capped at four.

## Manual build

Build dependencies on Arch: `base-devel cmake ninja qt6-base qt6-svg qt6-tools
openssl knotifications kstatusnotifieritem prison kguiaddons`. Qt LinguistTools compiles
the English catalog into the application. Qt Core, Network, Widgets and D-Bus,
KDE Notifications, StatusNotifierItem, Prison and GuiAddons, and OpenSSL are required.
Minimum versions: Qt 6.8, KDE Frameworks 6.0, OpenSSL 3.0.

```sh
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX="$HOME/.local" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --parallel 4
cmake --install build
systemctl --user daemon-reload
systemctl --user restart org.kde.kquickshare.service
"$HOME/.local/bin/kquickshare"
```

Configure the install prefix before building: it is embedded in the D-Bus and
systemd service files. `cmake --install --prefix` alone does not rewrite those paths.

## Translations

Source strings are English. `translations/kquickshare_ru.ts` contains the Russian
translation and is compiled into a Qt resource. The application uses
the first supported language in Qt’s system UI language preferences, falling back
to English. Language is selected at startup. No translation files are downloaded.

Update the catalog after editing interface text:

```sh
cmake --build build --target update_translations
```

Translate new messages in Qt Linguist, then rebuild. Qt’s own dialogs can use the
system-provided `qtbase_ru` catalog when available. Dolphin uses the localized
`.desktop` action; the optional clipboard button uses Plasma’s UI language.

## Tests

Additional test dependencies: `qt6-declarative ki18n plasma-workspace dbus
python-cryptography`. Configure with `-DBUILD_TESTING=ON`, then run `ctest` and the
independent Python peers as described in the README. Tests load notification
configuration from the build tree and do not require a previous installation.

## Native packages

Build from a clean source tree on the distribution you are packaging for, using
`-DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON`, then run the tests. In the build
directory, `cpack -G DEB` produces a Debian/Ubuntu package with automatic shared
library dependencies (`file` and `dpkg-dev` are required); `cpack -G RPM` produces a Fedora RPM (`rpm-build` required).
Runtime Qt SVG/Wayland plugins are explicitly included in dependencies. These
packages are distribution builds, not universally portable binaries.

The Arch `PKGBUILD` downloads the matching release archive and verifies SHA-256.
The optional Plasma 6.8 clipboard-QML test requires extra packages and must be
explicitly enabled with `-DBUILD_PLASMA_CLIPBOARD_TESTS=ON`; normal GUI/core tests
only require Qt Test, a session D-Bus and Python 3 with `cryptography`.
