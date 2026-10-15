# Архитектура KQuickShare

## Решение: реализация с нуля на C++/Qt6, а не форк rquickshare

Рассматривались два варианта:

### Вариант 1: форк rquickshare (Rust)

rquickshare — рабочая двунаправленная реализация на Rust (ядро `rqs_lib` + UI на
Tauri/WebView). Против форка:

1. **UI не подлежит переиспользованию.** Требование — Kirigami/KDE-виджеты;
   у rquickshare интерфейс — веб-вью (Tauri). Значит, от форка осталось бы
   только ядро протокола.
2. **Склейка Rust↔Qt дороже переписывания.** Ядро на Tokio; встраивание в
   Qt event loop требует FFI-слоя, моста сигналов и обвязки сборки
   (Corrosion/cxx). cxx-qt — молодой стек, с Kirigami сочетается плохо.
3. **Протокол приёма компактен.** Приёмная сторона — mDNS-анонс, TCP-сервер,
   UKEY2-хендшейк и ~12 protobuf-сообщений. В NearDrop это ~1500 строк Swift.
   Целевой объём нашего ядра — ~2500 строк C++ без внешних зависимостей,
   кроме Qt6 и OpenSSL.
4. **Ресурсы.** Нативное Qt-приложение без WebView: заметно меньше память,
   быстрее старт — важно для фонового демона в трее.
5. **Лицензионная чистота.** rquickshare — GPL-3.0; протокол при этом — открытая
   спецификация, независимо реализованная в нескольких проектах. Мы пишем свой
   код по документированному протоколу (docs/protocol.md) и вольны в лицензии.

Когда форк rquickshare имел бы смысл: если бы требовалась двунаправленная
передача «здесь и сейчас» и устраивал WebView-интерфейс.

### Вариант 2 (выбран): C++20 + Qt6 + KDE Frameworks, с нуля

- **Qt6 Core/Network** — TCP-сервер, сокеты mDNS.
- **OpenSSL 3** — ECDH P-256, HKDF-SHA256, HMAC-SHA256, AES-256-CBC, SHA-512.
- **Собственный мини-кодек protobuf** (~200 строк): сообщений мало, схемы
  стабильны (proto-файлы Google не менялись годами). Это снимает зависимость
  от libprotobuf/protoc.
- **Собственный mDNS-респондер** (~300 строк, UDP multicast 224.0.0.251:5353).
  Плюсы: не нужен запущенный avahi-daemon (в KDE/Arch он часто выключен,
  включение требует root) и нет D-Bus-зависимости. Аналогично rquickshare
  (mdns-sd встроен в процесс).
- **GUI:** Kirigami (QML) — окно настроек; `KStatusNotifierItem` — трей;
  `KNotification` — запросы «принять/отклонить» с действиями.

## Структура

```
src/
  core/                    # библиотека протокола, без GUI (Qt6::Core, Qt6::Network, OpenSSL)
    protobuf.{h,cpp}       # мини-кодек protobuf wire format
    crypto.{h,cpp}         # обёртки OpenSSL
    endpointinfo.{h,cpp}   # сериализация/парсинг endpoint info
    frames.{h,cpp}         # конструкторы/парсеры сообщений протокола
    mdnsresponder.{h,cpp}  # встроенный mDNS-анонсер
    inboundconnection.{h,cpp}  # конечный автомат одного соединения
    nearbyserver.{h,cpp}   # TCP-listener + фабрика соединений (API для UI)
  cli/main.cpp             # kquickshare-cli — приёмник без GUI
  app/                     # kquickshare — GUI для Plasma
    main.cpp
    traycontroller.{h,cpp}
    qml/Main.qml
tests/fake_sender.py       # симуляция Android-отправителя (E2E-тест без железа)
packaging/PKGBUILD
docs/
```

## Поток данных (приём)

```
QTcpServer → InboundConnection (по одному на сокет)
  1. CONNECTION_REQUEST → имя/тип устройства
  2. UKEY2 хендшейк → ключи, PIN
  3. CONNECTION_RESPONSE (accept)
  4. PAIRED_KEY_ENCRYPTION → PAIRED_KEY_RESULT
  5. INTRODUCTION → сигнал transferRequested(метаданные + PIN)
  6. UI: accept(dir)/reject()
  7. FILE-чанки → файл(ы) на диске, сигналы прогресса
  8. LAST_CHUNK по всем файлам → DISCONNECTION → finished
```

Сигналы Qt между ядром и UI; ядро живёт в GUI-потоке (event-driven, без
блокировок), что для 1-2 одновременных передач оптимально по ресурсам.

## Что осознанно не делаем (v1)

- Отправка файлов с ПК (этап 2).
- BLE-триггер, Wi-Fi Direct, bandwidth upgrade — только Wi-Fi LAN.
- Видимость «только контакты» (требует серверов Google).
- QR-код спаривание.
