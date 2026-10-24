# Contributing to KQuickShare

Bug reports, translations, distribution packaging and real-device testing are welcome.
This is a small AI-assisted community project; please include reproducible evidence
with code changes. The project is independent of Google and KDE.

Before reporting a bug, check existing issues and include:

- KQuickShare version and installation method.
- Distribution, Plasma, Qt and Wayland/X11 versions.
- Phone model, Android version, transfer direction and attachment type.
- Steps to reproduce, expected/actual result and a short relevant log excerpt.

Logs may contain IP addresses, device names and filenames. Remove private data.
Never upload clipboard history, tokens, personal photos or a full home-directory archive.

For development, read [BUILDING.md](docs/BUILDING.md). Run CTest and both Python
protocol peers. Fixes should include a regression test when practical. Core GUI
checks do not depend on Plasma Shell; the private clipboard-overlay test is a
separate opt-in check for Plasma 6.8 only.

For a compatibility report, use the same tests on your distribution and also try
both transfer directions with a real phone, tray startup, Dolphin, and received
text/image clipboard copying. A container build proves library compatibility;
it does not prove desktop integration or successful Android interoperability.

Translations live in `translations/kquickshare_en.ts`; Russian source strings use
Qt `tr()`. Preserve original copyright/license notices, particularly in the
optional KDE clipboard overlay.
