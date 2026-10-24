# Compatibility and testing

KQuickShare is a native Linux **Qt 6 / KDE Frameworks 6** application. The main
application does not import Plasma Shell QML. Build requirements are Qt >= 6.8,
KDE Frameworks >= 6.0, OpenSSL >= 3.0 and a C++20 compiler.

## What the checks prove

| Environment | Scope |
|---|---|
| Arch Linux, Plasma 6.8, Wayland, Pixel 7 Pro | Real desktop and phone testing in both directions; background text/image clipboard checks |
| Debian 13, Qt 6.8, KF 6.13 | Clean container build, GUI/core/installer tests and independent encrypted protocol peers |
| Fedora 43 | Clean container build, GUI/core/installer tests, protocol peers and native RPM packaging |
| Ubuntu 26.04 | CI build/test target; desktop/phone reports welcome |
| openSUSE Tumbleweed / Leap 16 | Dependency plan provided; currently needs wider build and desktop verification |

See [GitHub Actions](https://github.com/jjjadeRRR/kquickshare/actions) for the
current state of the automated matrix. These are separate from real desktop tests:
a successful offscreen/container test does **not** verify every Plasma release,
a physical phone, host firewall, Wayland compositor or distribution theme.

Plasma **6.x** is the target. The public tray, notification and Qt Widgets APIs
are independent of the private clipboard overlay. Older Plasma 6 releases with
suitable libraries can build the main application; real-device reports are welcome.
Plasma 5 and distributions shipping only KDE Frameworks 5 (for example an
unmodified Kubuntu 24.04 installation) are not supported. The installer never adds
third-party repositories or upgrades the desktop to satisfy dependencies.

## Desktop integration

- A desktop session D-Bus and a StatusNotifierItem host are needed for normal use.
- Plasma Wayland background clipboard access uses KDE's `KSystemClipboard`.
  On compositors that do not expose the required data-control protocol, background
  clipboard access can be unavailable. Other desktop environments are not certified.
- The application can be activated by session D-Bus. systemd-user is used for
  service supervision and automatic restart when available.
- Dolphin actions use a standard service-menu desktop file.
- The optional **history send button** replaces private Klipper QML and is only
  intended for **Plasma 6.8.x**. Its installer refuses other series. Do not enable
  it on an unverified version; remove it before changing Plasma series. Receiving
  into the clipboard, sending from the application and Dolphin do not need it.
- Immutable variants (Kinoite, Aeon, etc.) need distribution-specific packaging;
  the dependency installer intentionally refuses to modify an OSTree deployment.
- The provided Fedora RPM is for Fedora; it is not an openSUSE RPM or a universal
  Linux binary. Debian packages must likewise match the release they were built for.

## Network and firewall

Both devices must share a LAN which allows multicast and connections between
clients. The default receive port is **TCP 53318**; discovery uses **UDP 5353**.
The receive port can be changed in Settings; `0` chooses a random port. A busy
configured port produces an error, rather than silently selecting another one.
Sending connects to the port advertised by the phone, which may be dynamic.

A firewalld service is installed by system packages. It is never enabled
automatically. On a trusted home-network zone, review the zone and enable it:

```sh
firewall-cmd --get-active-zones
sudo firewall-cmd --permanent --zone=home --add-service=kquickshare
sudo firewall-cmd --reload
```

Use your actual trusted zone, not `public`. Do not move an untrusted interface to
`home` just to make sharing work. A user-local installation puts the definition
under `~/.local/lib/firewalld/services/kquickshare.xml`; an administrator can copy
it to `/etc/firewalld/services/` and reload firewalld before enabling it.
The provided rule matches the default receive port; update it if you change the
port in the application. For UFW or another firewall, permit UDP 5353 and inbound
TCP 53318 **only from your trusted LAN/subnet**. Router client isolation and
multicast filtering need to be disabled for the relevant trusted network.

## Help verify a system

Try a normal and empty file, several files, Unicode text shared from Android,
a `.txt` file and a photo in both directions. Check accepting/rejecting, trusted
autoreceive, cancellation, duplicate filenames, tray-only startup, Dolphin and
clipboard options. Include distribution/Plasma/Qt/Android versions and session
type in a [bug report](https://github.com/jjjadeRRR/kquickshare/issues/new/choose).
Do not post private clipboard contents or personal device information.
