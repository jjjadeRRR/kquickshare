#include "receiver.h"
#include "senddialog.h"
#include <QClipboard>
#include <KNotification>
#include <KStatusNotifierItem>
#include <QApplication>
#include <QCheckBox>
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
#include <QNetworkInterface>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>

namespace {
QString sizeText(qint64 size) { return QLocale().formattedDataSize(size); }
QLabel *label(const QString &text, QWidget *parent = nullptr) {
    auto *l = new QLabel(text, parent);
    l->setTextFormat(Qt::PlainText);
    l->setWordWrap(true);
    return l;
}
QString networkSignature() {
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
    : m_destination(destinationOverride.isEmpty() ? QDir::homePath() + "/Downloads" : destinationOverride),
      m_server(this), m_tray(new KStatusNotifierItem("kquickshare", this)) {
    setWindowTitle("KQuickShare");
    resize(640, 590);
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
    m_empty = label("Готово к передаче\n\nНа телефоне выберите файл → Поделиться "
                    "→ Quick Share → имя этого компьютера.\n\nТелефон и "
                    "компьютер должны быть в одной локальной сети.");
    m_transfers->addWidget(m_empty);
    scroll->setWidget(receive);
    tabs->addTab(scroll, QIcon::fromTheme("folder-download"), "Передачи");
    auto *settings = new QWidget;
    auto *form = new QFormLayout(settings);
    form->setSpacing(14);
    m_enabled = new QCheckBox("Принимать файлы");
    m_enabled->setChecked(m_settings.value("enabled", true).toBool());
    form->addRow(m_enabled);
    m_name = new QLineEdit(m_settings.value("name", QHostInfo::localHostName()).toString());
    m_name->setMaxLength(60);
    form->addRow("Имя компьютера:", m_name);
    auto *folderRow = new QWidget;
    auto *folderLayout = new QHBoxLayout(folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    m_folder = new QLineEdit(destination());
    m_folder->setReadOnly(true);
    folderLayout->addWidget(m_folder);
    form->addRow("Сохранять в:", folderRow);
    m_autostart = new QCheckBox("Запускать при входе в KDE");
    const QString autostartPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
                                  "/autostart/org.kde.kquickshare.desktop";
    m_autostart->setChecked(QFileInfo::exists(autostartPath));
    form->addRow(m_autostart);
    auto *apply = new QPushButton(QIcon::fromTheme("dialog-ok-apply"), "Применить");
    form->addRow(apply);
    form->addRow(label("Компьютер виден всем в локальной сети, пока приём включён. Для "
                       "незнакомых устройств всегда требуется подтверждение с PIN-кодом."));
    tabs->addTab(settings, QIcon::fromTheme("configure"), "Настройки");
    auto *trustedPage = new QWidget;
    auto *trustedLayout = new QVBoxLayout(trustedPage);
    trustedLayout->addWidget(label("Доверенные устройства"));
    trustedLayout->addWidget(label("Автоприём привязан к имени, типу устройства и IP-адресу. Это удобство "
                                   "для домашней сети, а не проверка ключа телефона. Адрес и имя могут быть "
                                   "подменены. При смене IP потребуется новое подтверждение."));
    m_trusted = new QListWidget;
    trustedLayout->addWidget(m_trusted, 1);
    auto *remove = new QPushButton(QIcon::fromTheme("list-remove"), "Удалить из доверенных");
    trustedLayout->addWidget(remove);
    tabs->addTab(trustedPage, QIcon::fromTheme("security-high"), "Доверенные");
    refreshTrusted();
    auto *sendRow = new QHBoxLayout;
    auto *sendFilesButton = new QPushButton(QIcon::fromTheme("document-send"), "Отправить файлы…");
    auto *sendTextButton = new QPushButton(QIcon::fromTheme("edit-paste"), "Отправить текст из буфера…");
    sendRow->addWidget(sendFilesButton); sendRow->addWidget(sendTextButton); layout->addLayout(sendRow);
    connect(sendFilesButton, &QPushButton::clicked, this, [this] {
        const auto paths = QFileDialog::getOpenFileNames(this, "Отправить на телефон", QDir::homePath());
        if (!paths.isEmpty()) sendFiles(paths);
    });
    connect(sendTextButton, &QPushButton::clicked, this, [this] { sendText(QApplication::clipboard()->text()); });
    auto *footer = new QHBoxLayout;
    auto *open = new QPushButton(QIcon::fromTheme("folder-download"), "Открыть папку загрузок");
    footer->addWidget(open);
    footer->addStretch();
    layout->addLayout(footer);
    connect(open, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(destination())); });
    connect(apply, &QPushButton::clicked, this, [this, autostartPath] {
        if (m_server.hasActiveConnections()) {
            notify("error", "Дождитесь завершения передач",
                   "Настройки можно изменить после завершения или отмены текущих "
                   "передач.");
            return;
        }
        const QString name = m_name->text().trimmed();
        if (name.isEmpty()) {
            m_name->setFocus();
            return;
        }
        m_settings.setValue("name", name);

        m_settings.setValue("enabled", m_enabled->isChecked());
        if (m_autostart->isChecked()) {
            QDir().mkpath(QFileInfo(autostartPath).absolutePath());
            QSaveFile f(autostartPath);
            const QByteArray content = "[Desktop Entry]\nType=Application\nName=KQuickShare\nExec=systemctl --user start org.kde.kquickshare.service\nIcon=org.kde.kquickshare\nTerminal=false\n";
            if (!f.open(QIODevice::WriteOnly) || f.write(content) != content.size() || !f.commit())
                notify("error", "Не удалось включить автозапуск", f.errorString());
        } else if (QFileInfo::exists(autostartPath) && !QFile::remove(autostartPath))
            notify("error", "Не удалось отключить автозапуск", autostartPath);
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
    menu->addAction(QIcon::fromTheme("org.kde.kquickshare"), "Передачи и настройки…", this, &Receiver::reveal);
    menu->addAction("Открыть папку загрузок", this,
                    [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(destination())); });
    auto *pause = menu->addAction("Принимать файлы");
    pause->setCheckable(true);
    pause->setChecked(m_enabled->isChecked());
    connect(pause, &QAction::triggered, this, [this](bool enabled) {
        m_enabled->setChecked(enabled);
        m_settings.setValue("enabled", enabled);
        startReceiver();
    });
    connect(menu, &QMenu::aboutToShow, this,
            [this, pause] { pause->setChecked(m_settings.value("enabled", true).toBool()); });
    menu->addAction(QIcon::fromTheme("document-send"), "Отправить файлы…", sendFilesButton, &QPushButton::click);
    menu->addAction(QIcon::fromTheme("edit-paste"), "Отправить текст из буфера…", sendTextButton, &QPushButton::click);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme("application-exit"), "Выйти", qApp, &QApplication::quit);
    m_tray->setContextMenu(menu);
    connect(m_tray, &KStatusNotifierItem::activateRequested, this, [this] { reveal(); });
    connect(&m_server, &NearbyServer::transferRequested, this, &Receiver::incoming);
    connect(&m_server, &NearbyServer::connectionFinished, this, &Receiver::finish);
    connect(&m_server, &NearbyServer::textReceived, this,
            [this](InboundConnection *conn, const QString &text, int) {
                // Store shared text as data; never launch received URLs
                // automatically.
                QDir().mkpath(destination());
                QString path =
                    QDir(destination())
                        .filePath("Quick Share " +
                                  QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss-zzz") + ".txt");
                QSaveFile f(path);
                const QByteArray bytes = text.toUtf8();
                if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
                    if (m_active.contains(conn))
                        m_active[conn].textSaveFailed = true;
                    notify("error", "Не удалось сохранить текст", f.errorString());
                }
            });
    connect(&m_server, &NearbyServer::errorOccurred, this, [this](const QString &error) {
        m_lastError = error;
        refreshState();
    });
    auto *network = new QTimer(this);
    network->setInterval(5000);
    connect(network, &QTimer::timeout, this, [this, signature = networkSignature()]() mutable {
        const QString current = networkSignature();
        if (!m_server.hasActiveConnections() && m_settings.value("enabled", true).toBool() &&
            (current != signature || !m_server.isRunning())) {
            signature = current;
            startReceiver();
        }
    });
    network->start();
    startReceiver();
}
Receiver::~Receiver() { m_server.stop(); }
void Receiver::closeEvent(QCloseEvent *e) {
    hide();
    e->ignore();
}
void Receiver::reveal() {
    show();
    raise();
    activateWindow();
}
QString Receiver::destination() const { return m_destination; }
QString Receiver::trustKey(const TransferRequest &r) const {
    return QString::fromLatin1(
        QCryptographicHash::hash(
            (r.deviceName + '\n' + r.peerAddress + '\n' + QString::number(int(r.deviceType))).toUtf8(),
            QCryptographicHash::Sha256)
            .toHex());
}
void Receiver::refreshTrusted() {
    m_trusted->clear();
    m_settings.beginGroup("trusted");
    for (const QString &key : m_settings.childKeys()) {
        auto *item = new QListWidgetItem(m_settings.value(key).toString(), m_trusted);
        item->setData(Qt::UserRole, key);
    }
    m_settings.endGroup();
}
void Receiver::startReceiver() {
    m_lastError.clear();
    if (m_settings.value("enabled", true).toBool())
        m_server.start(m_settings.value("name", QHostInfo::localHostName()).toString());
    else
        m_server.stop();
    refreshState();
}
void Receiver::refreshState() {
    const QString text = !m_lastError.isEmpty()  ? m_lastError
                         : !m_server.isRunning() ? "Приём приостановлен"
                         : m_active.isEmpty()
                             ? "В сети · " + m_settings.value("name", QHostInfo::localHostName()).toString()
                             : QString("Активных передач: %1").arg(m_active.size());
    m_state->setText(text);
    m_tray->setToolTip("folder-download", "KQuickShare", text);
    bool pending = false;
    for (const auto &t : m_active)
        pending |= !t.accepted;
    m_tray->setStatus(pending ? KStatusNotifierItem::NeedsAttention : KStatusNotifierItem::Active);
    m_tray->setOverlayIconByName(m_server.isRunning() ? QString() : "media-playback-pause");
}
KNotification *Receiver::notify(const QString &event, const QString &title, const QString &body,
                                bool persistent) {
    auto *n = new KNotification(event, persistent ? KNotification::Persistent : KNotification::CloseOnTimeout,
                                this);
    n->setComponentName("kquickshare");
    n->setTitle(title);
    n->setText(body.toHtmlEscaped());
    n->setIconName("org.kde.kquickshare");
    connect(n->addDefaultAction("Открыть KQuickShare"), &KNotificationAction::activated, this,
            &Receiver::reveal);
    QTimer::singleShot(0, n, &KNotification::sendEvent);
    return n;
}
void Receiver::incoming(InboundConnection *conn, const TransferRequest &r) {
    qInfo("Quick Share request: peer=%s files=%lld bytes=%lld", qPrintable(r.peerAddress),
          qlonglong(r.files.size()), r.totalBytes);
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
        label(r.files.isEmpty()
                  ? r.textTitle
                  : names.mid(0, 5).join('\n') +
                        (names.size() > 5 ? QString("\n…и ещё %1").arg(names.size() - 5) : QString())));
    t.status = label("Сверьте PIN-код с телефоном и подтвердите приём.");
    v->addWidget(t.status);
    t.progress = new QProgressBar;
    t.progress->setRange(0, 1000);
    t.progress->setValue(0);
    t.progress->hide();
    v->addWidget(t.progress);
    auto *consent = new QWidget;
    auto *cv = new QVBoxLayout(consent);
    cv->setContentsMargins(0, 0, 0, 0);
    t.remember = new QCheckBox("Доверять этому устройству в домашней сети");
    t.remember->setToolTip("Автоприём по имени, типу и IP-адресу. Без проверки "
                           "постоянного ключа телефона.");
    cv->addWidget(t.remember);
    auto *buttons = new QHBoxLayout;
    auto *yes = new QPushButton(QIcon::fromTheme("dialog-ok"), "Принять");
    auto *no = new QPushButton(QIcon::fromTheme("dialog-cancel"), "Отклонить");
    buttons->addWidget(yes);
    buttons->addWidget(no);
    buttons->addStretch();
    cv->addLayout(buttons);
    t.consent = consent;
    v->addWidget(consent);
    auto *cancel = new QPushButton("Отменить передачу");
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
    connect(conn, &InboundConnection::overallProgress, this,
            [this, conn](InboundConnection *, qint64 received, qint64 total) {
                auto it = m_active.find(conn);
                if (it == m_active.end())
                    return;
                it->progress->setValue(int(1000.0 * received / qMax(qint64(1), total)));
                it->status->setText(QString("Получено %1 из %2").arg(sizeText(received), sizeText(total)));
            });
    if (m_settings.contains("trusted/" + trustKey(r))) {
        accept(conn);
        cancel->show();
        auto *n = notify("receiving", "Приём от «" + r.deviceName + "»",
                         QString("Автоматический приём · %1").arg(sizeText(r.totalBytes)));
    } else {
        auto *n = notify(
            "request", "«" + r.deviceName + "» отправляет файлы",
            QString("PIN %1 · %2\nСверьте код на телефоне.").arg(r.pinCode, sizeText(r.totalBytes)), true);
        m_active[conn].notification = n;
        connect(n->addAction("Принять"), &KNotificationAction::activated, this, [this, safe, cancel] {
            if (safe) {
                accept(safe);
                cancel->show();
            }
        });
        connect(n->addAction("Отклонить"), &KNotificationAction::activated, this, [safe] {
            if (safe)
                safe->rejectTransfer();
        });
    }
    refreshState();
}
void Receiver::accept(InboundConnection *conn) {
    auto it = m_active.find(conn);
    if (it == m_active.end() || it->accepted)
        return;
    if (it->remember->isChecked()) {
        m_settings.setValue("trusted/" + trustKey(it->request),
                            it->request.deviceName + " · " + it->request.peerAddress);
        m_settings.sync();
        refreshTrusted();
    }
    it->accepted = true;
    it->consent->hide();
    it->progress->show();
    it->status->setText("Получение…");
    if (it->notification)
        it->notification->close();
    conn->acceptTransfer(destination());
    refreshState();
}
void Receiver::finish(InboundConnection *conn, const QStringList &files, const QString &incomingError) {
    auto it = m_active.find(conn);
    if (it == m_active.end())
        return;
    const QString error = it->textSaveFailed ? QString("Не удалось сохранить текст на диск") : incomingError;
    qInfo("Quick Share finished: files=%lld error=%s", qlonglong(files.size()), qPrintable(error));
    const QString device = it->request.deviceName;
    if (it->notification)
        it->notification->close();
    it->consent->hide();
    if (error.isEmpty()) {
        it->progress->setValue(1000);
        it->status->setText(files.isEmpty() ? "Текст получен и сохранён"
                                            : QString("Сохранено файлов: %1").arg(files.size()));
    } else {
        it->progress->hide();
        it->status->setText(error);
    }
    auto *n = notify(
        error.isEmpty() ? "complete" : "error",
        error.isEmpty() ? "Получено от «" + device + "»" : "Передача от «" + device + "» завершена",
        error.isEmpty() ? (files.isEmpty() ? "Текст сохранён в папке загрузок"
                                           : QString("Файлов: %1 · %2").arg(files.size()).arg(destination()))
                        : error);
    if (error.isEmpty())
        connect(n->addAction("Открыть папку"), &KNotificationAction::activated, this,
                [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(destination())); });
    m_active.erase(it);
    refreshState();
}

void Receiver::sendFiles(const QStringList &paths) {
    QStringList local;
    for (const auto &path : paths) {
        const QUrl url = QUrl::fromUserInput(path, QDir::currentPath(), QUrl::AssumeLocalFile);
        if (!url.isLocalFile()) { notify("error", "Нужны локальные файлы", "Сначала сохраните файл на компьютер."); return; }
        local << url.toLocalFile();
    }
    if (local.isEmpty()) return;
    auto *dialog = new SendDialog(local, {}, this); dialog->show(); dialog->raise(); dialog->activateWindow();
}
void Receiver::sendText(const QString &text) {
    if (text.isEmpty()) { notify("error", "Буфер обмена пуст", "Скопируйте текст перед отправкой."); return; }
    auto *dialog = new SendDialog({}, text, this); dialog->show(); dialog->raise(); dialog->activateWindow();
}

QString Receiver::sendStatus() const {
    QStringList status;
    for (auto *dialog : findChildren<SendDialog *>()) status << dialog->status();
    return status.join('\n');
}
