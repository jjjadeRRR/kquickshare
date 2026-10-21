# Building and installing KQuickShare

The normal desktop installation needs one command from an extracted source tree:

```sh
./install.sh
```

On Arch Linux the script checks packages and installs missing dependencies using
`sudo pacman -S --needed`. It does not run a system upgrade or change the trust
list, autostart preference or optional clipboard overlay. It builds with at most
four parallel jobs, installs into `~/.local`, reloads the user systemd manager,
restarts KQuickShare and opens its window. Finish current transfers before updating.

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
openssl knotifications kstatusnotifieritem prison`. Qt LinguistTools compiles
the English catalog into the application. Qt Core, Network, Widgets and D-Bus,
KDE Notifications, StatusNotifierItem and Prison, and OpenSSL are required.

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

The original source strings are Russian. `translations/kquickshare_en.ts` contains
the English translation and is compiled into a Qt resource. The application uses
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
