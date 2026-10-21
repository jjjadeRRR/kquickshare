# Clipboard action for Plasma 6.8

Adds **Send to phone with KQuickShare** to text entries in both the tray Clipboard
and the mouse-position popup opened with Meta+V. It uses the same Klipper backend
and history. Selected text is passed directly over the session D-Bus; it is never
interpolated into a command, logged, or copied over the current clipboard.

The interface has no external action provider, so this optional integration uses
a user QML import overlay, scoped to `plasma-plasmashell.service`:

- `~/.local/share/kquickshare/qml/org/kde/plasma/private/clipboard/`: QML interface
  plus a symlink to the installed native Klipper plugin.
- `~/.config/systemd/user/plasma-plasmashell.service.d/kquickshare-clipboard.conf`:
  prepends that import directory for Plasma Shell.

System files and the panel layout are unchanged. The QML was copied from the
installed KDE Plasma 6.8.0 module; original SPDX copyright and GPL-2.0-or-later
notices are preserved. The only interface change is an additional action in
`DelegateToolButtons.qml`; `qmldir` resolves QML from the overlay instead of the
native plugin's compiled resources. A test imports the real module URI and
checks that the new action sends the full selected text over D-Bus.

Install after installing KQuickShare:

```sh
./integration/install-clipboard.sh
systemctl --user restart plasma-plasmashell.service
```

Panels briefly restart. The installed copy of the script is also available under
`~/.local/share/kquickshare/integration/` for a user installation.

Remove with `./integration/uninstall-clipboard.sh`, then restart Plasma Shell.
After a major Plasma update, update or remove this version-specific overlay if
the clipboard interface changes.
