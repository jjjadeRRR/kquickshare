<p align="center"><img src="src/app/icons/org.kde.kquickshare.svg" width="96" alt="KQuickShare icon"></p>

# KQuickShare

**Native Quick Share for KDE Plasma. Send files and text to Android, and receive files in your tray.**

**English** · [Русский](README.ru.md)

KQuickShare is a small C++ / Qt 6 application inspired by
[NearDrop](https://github.com/grishka/NearDrop). It uses Android's built-in
Quick Share over your local network; no companion Android app is needed.

> **This is a vibe-coded project.** It was developed with AI coding agents,
> with hands-on testing by the project owner on Arch Linux / KDE and an Android phone.

## Features

- Plasma tray icon, native KDE notifications and a Qt Widgets interface.
- Receive files into `~/Downloads`; accept or reject from a notification.
- Optional automatic reception from explicitly trusted devices.
- Send files from Dolphin's context menu, the application or the tray.
- Send text with a QR code, including an optional action in Plasma's clipboard history.
- Transfer progress, cancellation, startup at login and a user systemd service.
- Encrypted UKEY2 / P-256 / AES-256-CBC / HMAC-SHA256 transport.
- Partial-file cleanup and collision-safe filenames; existing files are preserved.

The interface follows the system’s language preferences: Russian or English,
with English as the fallback for other languages. Restart the application after
changing your desktop language. Tested on Arch Linux,
KDE Plasma 6.8 and a Pixel 7 Pro, in both directions. Other Android devices and
Plasma releases need wider testing.

## Arch Linux package

A reviewed `packaging/PKGBUILD` is included in the repository. It downloads the
versioned source archive from GitHub Releases and checks its SHA-256 checksum.

```sh
cd packaging
makepkg -si
```

The package has not been published on AUR. GitHub repository:
https://github.com/jjjadeRRR/kquickshare

## Install from source

Download and extract the source archive from [Releases](https://github.com/jjjadeRRR/kquickshare/releases),
open a terminal in the extracted folder and run **one command**:

```sh
./install.sh
```

On Arch Linux, the installer checks build dependencies and asks `pacman` to
install missing packages (using sudo when needed). It builds the application,
installs it for your user in `~/.local` and starts it in your desktop session.
Run it as your normal desktop user, without sudo. Installing missing dependencies
may ask for your administrator password; the application itself runs as your user.

KQuickShare appears in the KDE application menu and system tray. Enable startup
at login in its settings. Closing the window keeps the receiver in the tray;
choose Exit in the tray menu to stop it. Running `./install.sh` again updates the
application and restarts it; finish current transfers before updating.

The installer does not enable the optional clipboard overlay automatically.
Other distributions need Qt 6 and KDE Frameworks development packages installed
first. See [manual build instructions](docs/BUILDING.md) for custom prefixes,
headless installation and build dependencies.

## Receive from Android

1. Connect the computer and phone to the same LAN. Ethernet on the PC is fine.
2. Start KQuickShare and choose Share → Quick Share on Android.
3. Select the computer, compare the PIN and accept on the PC.
4. Files appear in `~/Downloads`. Received photos and text are also copied to the
   clipboard by default. Links are never opened automatically.

For automatic reception, check the trust option in the incoming transfer card
**before** accepting. Remove a device from the Trusted devices tab to revoke it.

**Trust limitation:** the allowlist currently matches the device's name, type
and IPv4 address, rather than a persistent cryptographic identity. Another device
on the LAN could imitate these values. Use automatic reception only on a LAN you
trust; an address change requires approval again.

## Send to Android

Select local files in Dolphin → right click → **Отправить на телефон через
KQuickShare…**, or use the file/text actions in the application or tray.
Scan the displayed QR with the phone camera, open Quick Share, select the
phone in the desktop dialog and send. Confirm reception on Android if requested.
The receive screen in Files by Google is another discovery option.

The transport is LAN-only. Bluetooth is not required; BLE activation, Wi-Fi
Direct, internet transfer and Google-account contact discovery are not implemented.
Wi-Fi client isolation, multicast filtering or a firewall can prevent discovery
or connections. Discovery uses UDP 5353 and transfers use a dynamically assigned TCP port.

## Received photos and text

The **Settings** tab has three independent options, all enabled by default:

- **Copy received photos to the clipboard**.
- **Copy received text to the clipboard**.
- **Save received text as a file**.

Click **Apply** to save your choices. Text includes Quick Share text messages
(such as sharing copied text on Android) and valid UTF-8 `.txt` attachments up to
5 MiB. Disable saving to keep these text files out of Downloads; disabling both
text options discards received text. Photos and other files always remain in
`~/Downloads`.

The clipboard changes only after a successful transfer. With multiple attachments,
the last processed eligible attachment remains in the clipboard. Images are copied
as images and text as text, rather than file paths. Images above 32 megapixels are
saved without clipboard copying; invalid or oversized `.txt` files are preserved
as ordinary files.

## Optional clipboard-history button (Plasma 6.8)

After installing to `~/.local`:

```sh
"$HOME/.local/share/kquickshare/integration/install-clipboard.sh"
systemctl --user restart plasma-plasmashell.service
```

For a system package, the script is at
`/usr/share/kquickshare/integration/install-clipboard.sh`.
Run it as your desktop user, without sudo. Panels briefly restart.

Hover a text entry in the tray clipboard or Meta+V history to reveal the send
button. Selected text is passed directly through session D-Bus, without a shell.

This is a user QML overlay for **Plasma 6.8**, not a public Klipper extension API.
It is optional and never enabled by the package installer. After a Plasma upgrade,
update or remove it if the clipboard interface changes. To remove it, run the
adjacent `uninstall-clipboard.sh` and restart Plasma Shell. See
[integration/README.md](integration/README.md) for details.

## Development and verification

```sh
sudo pacman -S --needed qt6-declarative ki18n plasma-workspace python-cryptography
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
python tests/fake_sender.py --binary build/kquickshare-cli -v
python tests/fake_receiver.py --binary build/kquickshare-send -v
```

Tests cover independent encrypted peers, file contents, empty files, collisions,
truncated transfers, malformed frames, authentication failures, delayed completion,
cancellation after 100%, trust decisions and the clipboard D-Bus action.
They supplement testing with real phones.

`kquickshare-cli` is a debugging receiver that **automatically accepts every
request**. It does not use the GUI trust list; use the GUI for everyday reception.
`kquickshare-send` is an uninstalled debugging sender built with the project.

Service logs:

```sh
journalctl --user -u org.kde.kquickshare.service
```

When reporting a problem, include the transfer direction, Android model/version,
Plasma version and the relevant log excerpt. Review logs before posting them:
they can contain local IP addresses, filenames and device information.

## License and credits

KQuickShare is licensed under **GPL-3.0-or-later**; see [LICENSE](LICENSE).
The optional clipboard integration includes KDE code under GPL-2.0-or-later,
with its original authorship notices preserved. See [NOTICE.md](NOTICE.md).

Protocol references include NearDrop and
[rquickshare](https://github.com/Martichou/rquickshare). See
[docs/protocol.md](docs/protocol.md) and [docs/architecture.md](docs/architecture.md).
Development included AI-assisted code generation and review; device testing was
performed by the project owner. This is an independent project, unaffiliated
with Google, KDE or NearDrop. Quick Share is a Google product name.
