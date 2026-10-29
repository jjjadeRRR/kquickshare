#include "receiver.h"
#include "senddialog.h"
#include <KNotification>
#include <KStatusNotifierItem>
#include <KSystemClipboard>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QNetworkInterface>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

namespace
{
QString sizeText(qint64 size)
{
    return QLocale().formattedDataSize(size);
}
QLabel *label(const QString &text, QWidget *parent = nullptr)
{
    auto *l = new QLabel(text, parent);
    l->setTextFormat(Qt::PlainText);
    l->setWordWrap(true);
    return l;
}
QString networkSignature()
{
    QStringList addresses;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : iface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                addresses << iface.name() + entry.ip().toString();
    }
    addresses.sort();
    return addresses.join(';');
}
} // namespace

Receiver::Receiver(const QString &destinationOverride)
    : m_destination(destinationOverride.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : destinationOverride)
    , m_server(this)
    , m_tray(new KStatusNotifierItem("kquickshare", this))
{
    setWindowTitle("KQuickShare");
    resize(680, 660);
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(16);
    setCentralWidget(central);
    auto *heading = label("KQuickShare");
    auto font = heading->font();
    font.setPointSize(font.pointSize() + 8);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    m_state = label("");
    layout->addWidget(m_state);
    auto *tabs = new QTabWidget;
    layout->addWidget(tabs, 1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *receive = new QWidget;
    m_transfers = new QVBoxLayout(receive);
    m_transfers->setAlignment(Qt::AlignTop);
    m_empty = label(tr(
        "Ready to share\n\nOn your phone, select a file → Share → Quick Share → this computer’s name.\n\nThe phone and computer must be on the same local network."));
    m_transfers->addWidget(m_empty);
    scroll->setWidget(receive);
    tabs->addTab(scroll, QIcon::fromTheme("folder-download"), tr("Transfers"));
    auto *settings = new QWidget;
    auto *form = new QFormLayout(settings);
    form->setSpacing(14);
    m_enabled = new QCheckBox(tr("Receive files"));
    m_enabled->setChecked(m_settings.value("enabled", true).toBool());
    form->addRow(m_enabled);
    m_name = new QLineEdit(m_settings.value("name", QHostInfo::localHostName()).toString());
    m_name->setMaxLength(60);
    form->addRow(tr("Computer name:"), m_name);
    auto *folderRow = new QWidget;
    auto *folderLayout = new QHBoxLayout(folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    m_folder = new QLineEdit(destination());
    m_folder->setObjectName("downloadDirectory");
    m_folder->setReadOnly(true);
    folderLayout->addWidget(m_folder);
    form->addRow(tr("Save to:"), folderRow);
    m_port = new QSpinBox;
    m_port->setObjectName("listenPort");
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(tr("Automatic"));
    m_port->setValue(m_settings.value("listenPort", 53318).toInt());
    m_port->setToolTip(tr("Allow inbound TCP on this port and mDNS (UDP 5353) in your firewall only on a trusted network. 0 selects a random port."));
    form->addRow(tr("Receive TCP port:"), m_port);
    m_autostart = new QCheckBox(tr("Start when logging in to KDE"));
    const QString autostartPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/org.kde.kquickshare.desktop";
    m_autostart->setChecked(QFileInfo::exists(autostartPath));
    form->addRow(m_autostart);
    auto option = [this, form](const QString &title, const char *key) {
        auto *box = new QCheckBox(title);
        box->setObjectName(QString::fromLatin1(key));
        box->setChecked(m_settings.value(key, true).toBool());
        form->addRow(box);
        return box;
    };
    m_copyText = option(tr("Copy received text to the clipboard"), "copyReceivedText");
    m_saveText = option(tr("Save received text as a file"), "saveReceivedText");
    m_saveText->setToolTip(tr(
        "Shared Quick Share text and UTF-8 .txt files up to 5 MiB. Photos are always saved in Downloads. If both text options are disabled, received text is discarded."));
    auto *apply = new QPushButton(QIcon::fromTheme("dialog-ok-apply"), tr("Apply"));
    apply->setObjectName("applySettings");
    form->addRow(apply);
    form->addRow(
        label(tr("The computer is visible to everyone on the local network while reception is enabled. Unknown devices always require approval with a PIN.")));
    auto *settingsScroll = new QScrollArea;
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setWidget(settings);
    form->setSizeConstraint(QLayout::SetMinimumSize);
    tabs->addTab(settingsScroll, QIcon::fromTheme("configure"), tr("Settings"));
    auto *trustedPage = new QWidget;
    auto *trustedLayout = new QVBoxLayout(trustedPage);
    trustedLayout->addWidget(label(tr("Trusted devices")));
    trustedLayout->addWidget(label(tr(
        "Automatic reception matches the device name, type and IP address. This is a convenience for a trusted LAN, not verification of the phone’s key. Names and addresses can be spoofed. A new IP address requires approval again.")));
    m_trusted = new QListWidget;
    trustedLayout->addWidget(m_trusted, 1);
    auto *remove = new QPushButton(QIcon::fromTheme("list-remove"), tr("Remove from trusted devices"));
    trustedLayout->addWidget(remove);
    tabs->addTab(trustedPage, QIcon::fromTheme("security-high"), tr("Trusted"));
    refreshTrusted();
    auto *sendRow = new QHBoxLayout;
    auto *sendFilesButton = new QPushButton(QIcon::fromTheme("document-send"), tr("Send files…"));
    auto *sendTextButton = new QPushButton(QIcon::fromTheme("edit-paste"), tr("Send clipboard text…"));
    sendRow->addWidget(sendFilesButton);
    sendRow->addWidget(sendTextButton);
    layout->addLayout(sendRow);
    connect(sendFilesButton, &QPushButton::clicked, this, [this] {
        const auto paths = QFileDialog::getOpenFileNames(this, tr("Send to phone"), QDir::homePath());
        if (!paths.isEmpty())
            sendFiles(paths);
    });
    connect(sendTextButton, &QPushButton::clicked, this, [this] {
        sendText(KSystemClipboard::instance()->text(QClipboard::Clipboard));
    });
    auto *footer = new QHBoxLayout;
    auto *open = new QPushButton(QIcon::fromTheme("folder-download"), tr("Open downloads folder"));
    footer->addWidget(open);
    footer->addStretch();
    layout->addLayout(footer);
    connect(open, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(destination()));
    });
    connect(apply, &QPushButton::clicked, this, [this, autostartPath] {
        if (m_server.hasActiveConnections()) {
            notify("error", tr("Wait for transfers to finish"), tr("You can change settings after finishing or cancelling current transfers."));
            return;
        }
        const QString name = m_name->text().trimmed();
        if (name.isEmpty()) {
            m_name->setFocus();
            return;
        }
        m_settings.setValue("name", name);
        m_settings.setValue("listenPort", m_port->value());

        m_settings.setValue("enabled", m_enabled->isChecked());
        m_settings.setValue("copyReceivedText", m_copyText->isChecked());
        m_settings.setValue("saveReceivedText", m_saveText->isChecked());
        if (m_autostart->isChecked()) {
            QDir().mkpath(QFileInfo(autostartPath).absolutePath());
            QSaveFile f(autostartPath);
            const QByteArray content
                = "[Desktop Entry]\nType=Application\nName=KQuickShare\nExec=systemctl --user start org.kde.kquickshare.service\nIcon=org.kde.kquickshare\nTerminal=false\n";
            if (!f.open(QIODevice::WriteOnly) || f.write(content) != content.size() || !f.commit())
                notify("error", tr("Could not enable startup at login"), f.errorString());
        } else if (QFileInfo::exists(autostartPath) && !QFile::remove(autostartPath))
            notify("error", tr("Could not disable startup at login"), autostartPath);
        m_settings.sync();
        startReceiver();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (auto *item = m_trusted->currentItem()) {
            m_settings.remove("trusted/" + item->data(Qt::UserRole).toString());
            m_settings.sync();
            refreshTrusted();
        }
    });
    m_tray->setTitle("KQuickShare");
    m_tray->setCategory(KStatusNotifierItem::Communications);
    m_tray->setIconByName("org.kde.kquickshare-symbolic");
    m_tray->setStandardActionsEnabled(false);
    auto *menu = new QMenu(this);
    menu->addAction(QIcon::fromTheme("org.kde.kquickshare"), tr("Transfers and settings…"), this, &Receiver::reveal);
    menu->addAction(tr("Open downloads folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(destination()));
    });
    auto *pause = menu->addAction(tr("Receive files"));
    pause->setCheckable(true);
    pause->setChecked(m_enabled->isChecked());
    connect(pause, &QAction::triggered, this, [this](bool enabled) {
        m_enabled->setChecked(enabled);
        m_settings.setValue("enabled", enabled);
        startReceiver();
    });
    connect(menu, &QMenu::aboutToShow, this, [this, pause] {
        pause->setChecked(m_settings.value("enabled", true).toBool());
    });
    menu->addAction(QIcon::fromTheme("document-send"), tr("Send files…"), sendFilesButton, &QPushButton::click);
    menu->addAction(QIcon::fromTheme("edit-paste"), tr("Send clipboard text…"), sendTextButton, &QPushButton::click);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme("application-exit"), tr("Exit"), qApp, &QApplication::quit);
    m_tray->setContextMenu(menu);
    connect(m_tray, &KStatusNotifierItem::activateRequested, this, [this] {
        reveal();
    });
    connect(&m_server, &NearbyServer::transferRequested, this, &Receiver::incoming);
    connect(&m_server, &NearbyServer::connectionFinished, this, &Receiver::finish);
    connect(&m_server, &NearbyServer::textReceived, this, [this](InboundConnection *conn, const QString &text, int) {
        auto it = m_active.find(conn);
        if (it != m_active.end())
            it->text = text;
    });
    connect(&m_server, &NearbyServer::errorOccurred, this, [this](const QString &error) {
        m_lastError = error;
        refreshState();
    });
    auto *network = new QTimer(this);
    network->setInterval(5000);
    connect(network, &QTimer::timeout, this, [this, signature = networkSignature()]() mutable {
        const QString current = networkSignature();
        if (!m_server.hasActiveConnections() && m_settings.value("enabled", true).toBool() && (current != signature || !m_server.isRunning())) {
            signature = current;
            startReceiver();
        }
    });
    network->start();
    startReceiver();
}
Receiver::~Receiver()
{
    m_server.stop();
}
void Receiver::closeEvent(QCloseEvent *e)
{
    hide();
    e->ignore();
}
void Receiver::reveal()
{
    show();
    raise();
    activateWindow();
}
QString Receiver::destination() const
{
    return m_destination;
}
QString Receiver::trustKey(const TransferRequest &r) const
{
    return QString::fromLatin1(
        QCryptographicHash::hash((r.deviceName + '\n' + r.peerAddress + '\n' + QString::number(int(r.deviceType))).toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}
void Receiver::refreshTrusted()
{
    m_trusted->clear();
    m_settings.beginGroup("trusted");
    for (const QString &key : m_settings.childKeys()) {
        auto *item = new QListWidgetItem(m_settings.value(key).toString(), m_trusted);
        item->setData(Qt::UserRole, key);
    }
    m_settings.endGroup();
}
void Receiver::startReceiver()
{
    m_lastError.clear();
    if (m_settings.value("enabled", true).toBool())
        m_server.start(
            m_settings.value("name", QHostInfo::localHostName()).toString(), DeviceType::Laptop, quint16(m_settings.value("listenPort", 53318).toUInt()));
    else
        m_server.stop();
    refreshState();
}
void Receiver::refreshState()
{
    const QString text = !m_lastError.isEmpty() ? m_lastError
        : !m_server.isRunning()                 ? tr("Reception paused")
        : m_active.isEmpty()                    ? tr("Online · %1").arg(m_settings.value("name", QHostInfo::localHostName()).toString())
                                                : tr("Active transfers: %1").arg(m_active.size());
    m_state->setText(text);
    m_tray->setToolTip("org.kde.kquickshare", "KQuickShare", text);
    bool pending = false;
    for (const auto &t : m_active)
        pending |= !t.accepted;
    m_tray->setStatus(pending ? KStatusNotifierItem::NeedsAttention : KStatusNotifierItem::Active);
    m_tray->setOverlayIconByName(m_server.isRunning() ? QString() : "media-playback-pause");
}
KNotification *Receiver::notify(const QString &event, const QString &title, const QString &body, bool persistent)
{
    auto *n = new KNotification(event, persistent ? KNotification::Persistent : KNotification::CloseOnTimeout, this);
    n->setComponentName("kquickshare");
    n->setTitle(title);
    n->setText(body.toHtmlEscaped());
    n->setIconName("org.kde.kquickshare");
    connect(n->addDefaultAction(tr("Open KQuickShare")), &KNotificationAction::activated, this, &Receiver::reveal);
    QTimer::singleShot(0, n, &KNotification::sendEvent);
    return n;
}
void Receiver::incoming(InboundConnection *conn, const TransferRequest &r)
{
    qInfo("Quick Share request: peer=%s files=%lld bytes=%lld", qPrintable(r.peerAddress), qlonglong(r.files.size()), r.totalBytes);
    m_empty->hide();
    // Keep a bounded session history.
    for (int i = m_transfers->count() - 1; i >= 0 && m_transfers->count() > 30; --i) {
        QWidget *w = m_transfers->itemAt(i)->widget();
        bool active = false;
        for (const auto &entry : m_active)
            active |= entry.card == w;
        if (w != m_empty && !active) {
            delete m_transfers->takeAt(i);
            delete w;
        }
    }
    Transfer t;
    t.request = r;
    auto *card = new QGroupBox(r.deviceName);
    t.card = card;
    auto *v = new QVBoxLayout(card);
    v->addWidget(label(QString("%1 · %2 · PIN %3").arg(r.peerAddress, sizeText(r.totalBytes), r.pinCode)));
    QStringList names;
    for (const auto &f : r.files)
        names << f.name;
    v->addWidget(
        label(r.files.isEmpty() ? r.textTitle : names.mid(0, 5).join('\n') + (names.size() > 5 ? tr("\n…and %1 more").arg(names.size() - 5) : QString())));
    t.status = label(tr("Compare the PIN with your phone and approve reception."));
    v->addWidget(t.status);
    t.progress = new QProgressBar;
    t.progress->setRange(0, 1000);
    t.progress->setValue(0);
    t.progress->hide();
    v->addWidget(t.progress);
    auto *consent = new QWidget;
    auto *cv = new QVBoxLayout(consent);
    cv->setContentsMargins(0, 0, 0, 0);
    t.remember = new QCheckBox(tr("Trust this device on my home network"));
    t.remember->setToolTip(tr("Automatic reception matches the name, type and IP address. The phone’s persistent key is not verified."));
    cv->addWidget(t.remember);
    auto *buttons = new QHBoxLayout;
    auto *yes = new QPushButton(QIcon::fromTheme("dialog-ok"), tr("Accept"));
    auto *no = new QPushButton(QIcon::fromTheme("dialog-cancel"), tr("Reject"));
    buttons->addWidget(yes);
    buttons->addWidget(no);
    buttons->addStretch();
    cv->addLayout(buttons);
    t.consent = consent;
    v->addWidget(consent);
    auto *cancel = new QPushButton(tr("Cancel transfer"));
    cancel->hide();
    v->addWidget(cancel);
    QPointer<InboundConnection> safe(conn);
    connect(yes, &QPushButton::clicked, this, [this, safe, cancel] {
        if (safe) {
            accept(safe);
            cancel->show();
        }
    });
    connect(no, &QPushButton::clicked, this, [safe] {
        if (safe)
            safe->rejectTransfer();
    });
    connect(cancel, &QPushButton::clicked, this, [safe] {
        if (safe)
            safe->cancelTransfer();
    });
    connect(conn, &QObject::destroyed, cancel, &QWidget::hide);
    m_transfers->insertWidget(0, card);
    m_active.insert(conn, t);
    connect(conn, &InboundConnection::overallProgress, this, [this, conn](InboundConnection *, qint64 received, qint64 total) {
        auto it = m_active.find(conn);
        if (it == m_active.end())
            return;
        it->progress->setValue(int(1000.0 * received / qMax(qint64(1), total)));
        it->status->setText(tr("Received %1 of %2").arg(sizeText(received), sizeText(total)));
    });
    if (m_settings.contains("trusted/" + trustKey(r))) {
        accept(conn);
        cancel->show();
        notify("receiving", tr("Receiving from «%1»").arg(r.deviceName), tr("Automatic reception · %1").arg(sizeText(r.totalBytes)));
    } else {
        auto *n = notify("request",
            tr("«%1» is sending files").arg(r.deviceName),
            tr("PIN %1 · %2\nCompare the code on your phone.").arg(r.pinCode, sizeText(r.totalBytes)),
            true);
        m_active[conn].notification = n;
        connect(n->addAction(tr("Accept")), &KNotificationAction::activated, this, [this, safe, cancel] {
            if (safe) {
                accept(safe);
                cancel->show();
            }
        });
        connect(n->addAction(tr("Reject")), &KNotificationAction::activated, this, [safe] {
            if (safe)
                safe->rejectTransfer();
        });
    }
    refreshState();
}
void Receiver::accept(InboundConnection *conn)
{
    auto it = m_active.find(conn);
    if (it == m_active.end() || it->accepted)
        return;
    if (it->remember->isChecked()) {
        m_settings.setValue("trusted/" + trustKey(it->request), it->request.deviceName + " · " + it->request.peerAddress);
        m_settings.sync();
        refreshTrusted();
    }
    it->accepted = true;
    it->consent->hide();
    it->progress->show();
    it->status->setText(tr("Receiving…"));
    if (it->notification)
        it->notification->close();
    conn->acceptTransfer(destination());
    refreshState();
}
void Receiver::finish(InboundConnection *conn, const QStringList &files, const QString &incomingError)
{
    auto it = m_active.find(conn);
    if (it == m_active.end())
        return;
    QString error = incomingError;
    QStringList savedFiles = files;
    bool copied = false;
    if (error.isEmpty()) {
        const bool copyText = m_settings.value("copyReceivedText", true).toBool();
        const bool saveText = m_settings.value("saveReceivedText", true).toBool();
        auto copy = [&](const QString &text) {
            if (copyText) {
                auto *mime = new QMimeData;
                mime->setText(text);
                KSystemClipboard::instance()->setMimeData(mime, QClipboard::Clipboard);
                copied = true;
            }
        };
        if (it->text) {
            copy(*it->text);
            if (saveText) {
                QDir().mkpath(destination());
                QTemporaryFile file(
                    QDir(destination()).filePath("Quick Share " + QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss") + "-XXXXXX.txt"));
                const auto bytes = it->text->toUtf8();
                if (!file.open() || file.write(bytes) != bytes.size() || !file.flush()) {
                    error = tr("Could not save the text to disk");
                } else {
                    file.setAutoRemove(false);
                    savedFiles.append(file.fileName());
                }
            }
        }
        for (const auto &path : files) {
            const QFileInfo info(path);
            if (info.suffix().compare("txt", Qt::CaseInsensitive) == 0 && info.size() <= 5 * 1024 * 1024) {
                QFile file(path);
                if (file.open(QIODevice::ReadOnly)) {
                    const auto bytes = file.read(5 * 1024 * 1024 + 1);
                    QStringDecoder decoder(QStringDecoder::Utf8);
                    const QString text = decoder.decode(bytes);
                    const bool valid = !decoder.hasError() && !text.contains(QChar::Null) && bytes.size() == info.size();
                    file.close();
                    if (valid) {
                        copy(text);
                        if (!saveText) {
                            if (QFile::remove(path))
                                savedFiles.removeOne(path);
                            else
                                error = tr("Could not remove text file: %1").arg(info.fileName());
                        }
                        continue;
                    }
                }
            }
        }
    }
    QString result = tr("Received. Files saved: %1").arg(savedFiles.size());
    if (copied)
        result += tr(" · Copied to the clipboard");
    qInfo("Quick Share finished: files=%lld error=%s", qlonglong(files.size()), qPrintable(error));
    const QString device = it->request.deviceName;
    if (it->notification)
        it->notification->close();
    it->consent->hide();
    if (error.isEmpty()) {
        it->progress->setValue(1000);
        it->status->setText(result);
    } else {
        it->progress->hide();
        it->status->setText(error);
    }
    auto *n = notify(error.isEmpty() ? "complete" : "error",
        error.isEmpty() ? tr("Received from «%1»").arg(device) : tr("Transfer from «%1» ended").arg(device),
        error.isEmpty() ? result : error);
    if (error.isEmpty() && !savedFiles.isEmpty())
        connect(n->addAction(tr("Open folder")), &KNotificationAction::activated, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(destination()));
        });
    m_active.erase(it);
    refreshState();
}

void Receiver::sendFiles(const QStringList &paths)
{
    QStringList local;
    for (const auto &path : paths) {
        const QUrl url = QUrl::fromUserInput(path, QDir::currentPath(), QUrl::AssumeLocalFile);
        if (!url.isLocalFile()) {
            notify("error", tr("Local files are required"), tr("Save the file to your computer first."));
            return;
        }
        local << url.toLocalFile();
    }
    if (local.isEmpty())
        return;
    auto *dialog = new SendDialog(local, {}, this);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
void Receiver::sendText(const QString &text)
{
    if (text.isEmpty()) {
        notify("error", tr("The clipboard is empty"), tr("Copy some text before sending."));
        return;
    }
    auto *dialog = new SendDialog({}, text, this);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

QString Receiver::sendStatus() const
{
    QStringList status;
    for (auto *dialog : findChildren<SendDialog *>())
        status << dialog->status();
    return status.join('\n');
}
