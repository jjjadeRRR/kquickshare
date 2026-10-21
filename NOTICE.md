# Licensing and attribution

## KQuickShare

Original application code, tests, documentation and icons are licensed under
GPL-3.0-or-later, unless a file states otherwise. Copyright 2026 KQuickShare
contributors. The license text is in `LICENSE`.

## Optional Plasma clipboard integration

`integration/qml/org/kde/plasma/private/clipboard/` contains QML from KDE Plasma
Workspace 6.8.0, with a KQuickShare action added to `DelegateToolButtons.qml` and
a local-import adjustment in `qmldir`.

Upstream: https://invent.kde.org/plasma/plasma-workspace/-/tree/Plasma/6.8/klipper

Original per-file SPDX copyright notices are preserved. These files are licensed
under GPL-2.0-or-later; the text is in `integration/COPYING`. The native Klipper
plugin is not redistributed here: the optional installer links to the installed
system plugin. The overlay is not enabled by the package installation.

## Protocol references

NearDrop by Grishka: https://github.com/grishka/NearDrop (Unlicense).
rquickshare by Martichou and contributors:
https://github.com/Martichou/rquickshare (GPL-3.0).

These projects informed protocol research. KQuickShare uses its own C++ transport
implementation, rather than bundling their application binaries or source trees.
Qt, KDE Frameworks and OpenSSL are system dependencies under their own licenses.
