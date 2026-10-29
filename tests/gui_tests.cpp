#include "receiver.h"
#include "localization.h"
#include <QTabWidget>
#include "senddialog.h"
#include "outboundconnection.h"
#include <QApplication>
#include <QClipboard>
#include <KSystemClipboard>
#include <QMimeData>
#include <QImage>
#include <QCheckBox>
#include <QDBusConnection>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QProcess>
#include <QPushButton>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QVariantMap>

class Notifications : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
  public:
    uint next = 0;
    QStringList actions;
    QString title;
  public slots:
    QStringList GetCapabilities() { return {"actions", "body"}; }
    QString GetServerInformation(QString &vendor, QString &version, QString &spec) {
        vendor = "KQuickShare Tests";
        version = "1";
        spec = "1.2";
        return "Test Notifications";
    }
    uint Notify(const QString &, uint, const QString &, const QString &summary, const QString &,
                const QStringList &buttons, const QVariantMap &, int) {
        actions = buttons;
        title = summary;
        return ++next;
    }
    void CloseNotification(uint id) { emit NotificationClosed(id, 3); }
  signals:
    void ActionInvoked(uint id, const QString &action);
    void NotificationClosed(uint id, uint reason);
};
static Notifications *notifications;

namespace {
QByteArray receiverAnnouncement(quint16 port) {
    auto name = [](const QString &value) {
        QByteArray result;
        for (const auto &label : value.split('.')) {
            const auto bytes = label.toUtf8();
            result.append(char(bytes.size()));
            result += bytes;
        }
        result.append('\0');
        return result;
    };
    auto u16 = [](quint16 value) {
        QByteArray result;
        result.append(char(value >> 8));
        result.append(char(value));
        return result;
    };
    const QString instance = "retry._FC9F5ED42C8A._tcp.local";
    QByteArray packet(12, '\0');
    packet[2] = char(0x84);
    packet[7] = 3;
    auto record = [&](const QString &owner, quint16 type, const QByteArray &body) {
        packet += name(owner) + u16(type) + u16(1) + QByteArray::fromHex("00000078") +
                  u16(quint16(body.size())) + body;
    };
    record("retry.local", 1, QByteArray::fromHex("7f000001"));
    record(instance, 33, QByteArray(4, '\0') + u16(port) + name("retry.local"));
    const auto txt = "n=" + base64UrlEncode(EndpointInfo::serialize("Retry Phone", DeviceType::Phone));
    record(instance, 16, QByteArray(1, char(txt.size())) + txt);
    return packet;
}
} // namespace

class GuiTests : public QObject {
    Q_OBJECT
  private slots:
    void cleanup() {
        // KSystemClipboard keeps its Wayland read lock until the next event-loop
        // iteration. Drain it before qExec returns and QApplication is destroyed.
        QCoreApplication::processEvents();
    }
    void localizedInterface_data() {
        QTest::addColumn<QStringList>("languages");
        QTest::addColumn<QString>("tab");
        QTest::addColumn<QString>("sendButton");
        QTest::newRow("russian") << QStringList{"ru-RU"} << QString("Передачи") << QString("Отправить файлы…");
        QTest::newRow("english") << QStringList{"en-US"} << QString("Transfers") << QString("Send files…");
        QTest::newRow("fallback") << QStringList{"fr-FR"} << QString("Transfers") << QString("Send files…");
        QTest::newRow("preference") << QStringList{"en-US", "ru-RU"} << QString("Transfers") << QString("Send files…");
    }
    void localizedInterface() {
        QFETCH(QStringList, languages);
        QFETCH(QString, tab);
        QFETCH(QString, sendButton);
        QTranslator translator;
        QVERIFY(localization::load(translator, languages));
        QTemporaryDir downloads;
        QSettings().setValue("name", "Linux PC");
        Receiver receiver(downloads.path());
        auto *tabs = receiver.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), tab);
        bool found = false;
        for (auto *button : receiver.findChildren<QPushButton *>()) found |= button->text() == sendButton;
        QVERIFY(found);
        SendDialog dialog({}, "Localization fixture");
        QCOMPARE(dialog.windowTitle(), localization::language(languages) == "ru"
                     ? QString("Отправить через KQuickShare") : QString("Send with KQuickShare"));
        QString error;
        OutboundConnection connection;
        QVERIFY(!connection.setText({}, error));
        QCOMPARE(error, localization::language(languages) == "ru"
                     ? QString("Нужен текст размером до 5 МиБ") : QString("Text must be at most 5 MiB"));
        receiver.show();
        receiver.grab().save(QString(SOURCE_DIR) + "/build/locale-" + localization::language(languages) + ".png");
        tabs->setCurrentIndex(1);
        // Use generic display values in documentation screenshots, never host data.
        receiver.findChild<QLineEdit *>("downloadDirectory")->setText("~/Downloads");
        receiver.findChild<QSpinBox *>("listenPort")->setValue(53318);
        receiver.grab().save(QString(SOURCE_DIR) + "/build/settings-" + localization::language(languages) + ".png");
    }
    void receivedClipboard_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("copy");
        QTest::addColumn<bool>("save");
        for (const auto &kind : {QString("text"), QString("txt"), QString("image")}) {
            for (bool copy : {false, true}) {
                for (bool save : {false, true}) {
                    const auto name = kind + (copy ? "-copy" : "-no-copy") + (save ? "-save" : "-no-save");
                    QTest::newRow(qPrintable(name)) << kind << copy << save;
                }
            }
        }
        QTest::newRow("invalid-utf8-preserved") << QString("invalid") << true << false;
        QTest::newRow("failed-transfer-keeps-clipboard") << QString("truncated") << true << false;
    }
    void receivedClipboard() {
        QFETCH(QString, kind);
        QFETCH(bool, copy);
        QFETCH(bool, save);
        QTemporaryDir downloads;
        QSettings settings;
        settings.clear();
        settings.setValue("listenPort", 0);
        // Legacy preference must not re-enable image clipboard handling.
        settings.setValue("copyReceivedImages", true);
        settings.setValue("copyReceivedText", kind == "image" ? !copy : copy);
        settings.setValue("saveReceivedText", save);
        settings.sync();
        auto *initial = new QMimeData;
        initial->setText("existing clipboard");
        KSystemClipboard::instance()->setMimeData(initial, QClipboard::Clipboard);
        QTRY_COMPARE(KSystemClipboard::instance()->text(QClipboard::Clipboard), QString("existing clipboard"));
        QByteArray payload = QString("Привет с телефона 🌍\nВторая строка").toUtf8();
        const QString text = QString::fromUtf8(payload);
        QString filename = "shared.txt";
        if (kind == "image") {
            QImage image(3, 2, QImage::Format_RGB32);
            image.fill(QColor(20, 100, 180));
            QVERIFY(image.save(downloads.filePath("fixture.png")));
            QFile fixture(downloads.filePath("fixture.png"));
            QVERIFY(fixture.open(QIODevice::ReadOnly));
            payload = fixture.readAll();
            fixture.close();
            QFile::remove(fixture.fileName());
            filename = "photo.png";
        } else if (kind == "invalid") {
            payload = QByteArray::fromHex("fffe00fe");
        }
        if (kind == "txt") {
            QFile existing(downloads.filePath("shared.txt"));
            QVERIFY(existing.open(QIODevice::WriteOnly));
            QCOMPARE(existing.write("keep existing"), 13);
        }
        Receiver receiver(downloads.path());
        auto *tcp = receiver.findChild<QTcpServer *>();
        QVERIFY(tcp && tcp->isListening());
        QSignalSpy completed(receiver.findChild<NearbyServer *>(), &NearbyServer::connectionFinished);
        const QString intro = kind == "text"
            ? QString("s.send_sharing(sharing(1,2,b(2,b(2,'Shared text')+v(3,1)+v(4,42)+v(5,%1)))); s.recv_sharing()") .arg(payload.size())
            : QString("s.introduce([(42,'%1',%2)])").arg(filename).arg(payload.size());
        const QString script = QString(
            "import sys; sys.path.insert(0,'%1/tests'); from fake_sender import Sender,sharing,b,v; "
            "s=Sender('127.0.0.1',%2,'Clipboard Android'); %3; "
            "s.payload(42,%4,%5,0,bytes.fromhex('%6'),True); s.close()")
            .arg(SOURCE_DIR).arg(tcp->serverPort()).arg(intro)
            .arg(kind == "text" ? 1 : 2).arg(payload.size())
            .arg(QString::fromLatin1((kind == "truncated" ? payload.left(2) : payload).toHex()));
        QProcess sender;
        sender.start(QStringLiteral(PYTHON_EXECUTABLE), {"-c", script});
        QPushButton *accept = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (auto *button : receiver.findChildren<QPushButton *>())
                if (button->text() == "Accept") { accept = button; return true; }
            return false;
        })(), 8000);
        QTest::mouseClick(accept, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(sender.state(), QProcess::NotRunning, 8000);
        QCOMPARE(sender.exitCode(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 8000);
        QCOMPARE(completed.first().at(2).toString().isEmpty(), kind != "truncated");
        QCOMPARE(KSystemClipboard::instance()->text(QClipboard::Clipboard),
            copy && (kind == "text" || kind == "txt") ? text : QString("existing clipboard"));
        auto files = QDir(downloads.path()).entryList(QDir::Files | QDir::NoDotAndDotDot);
        if (kind == "txt") {
            QVERIFY(files.removeOne("shared.txt"));
            QFile existing(downloads.filePath("shared.txt"));
            QVERIFY(existing.open(QIODevice::ReadOnly));
            QCOMPARE(existing.readAll(), QByteArray("keep existing"));
        }
        const bool shouldSave = kind == "image" || kind == "invalid" ||
                                (save && (kind == "text" || kind == "txt"));
        QCOMPARE(files.size(), shouldSave ? 1 : 0);
        if (shouldSave) {
            QFile file(downloads.filePath(files.first()));
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), payload);
        }
    }
    void clipboardSettingsPersistIndependently() {
        QTemporaryDir downloads;
        QSettings settings;
        settings.clear();
        settings.setValue("listenPort", 0);
        const QStringList keys{"copyReceivedText", "saveReceivedText"};
        {
            Receiver defaults(downloads.path());
            for (const auto &key : keys)
                QVERIFY(defaults.findChild<QCheckBox *>(key)->isChecked());
        }
        for (int enabled = 0; enabled < keys.size(); ++enabled) {
            {
                Receiver receiver(downloads.path());
                for (int i = 0; i < keys.size(); ++i) {
                    auto *box = receiver.findChild<QCheckBox *>(keys[i]);
                    QVERIFY(box);
                    box->setChecked(i == enabled);
                }
                auto *apply = receiver.findChild<QPushButton *>("applySettings");
                QVERIFY(apply);
                QTest::mouseClick(apply, Qt::LeftButton);
            }
            Receiver restarted(downloads.path());
            for (int i = 0; i < keys.size(); ++i) {
                auto *box = restarted.findChild<QCheckBox *>(keys[i]);
                QVERIFY(box);
                QCOMPARE(box->isChecked(), i == enabled);
            }
        }
    }
    void configuredListeningPort() {
        QTemporaryDir downloads;
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 port = probe.serverPort();
        probe.close();
        QSettings settings;
        settings.clear();
        settings.setValue("listenPort", port);
        Receiver receiver(downloads.path());
        auto *tcp = receiver.findChild<QTcpServer *>();
        QVERIFY(tcp && tcp->isListening());
        QCOMPARE(tcp->serverPort(), port);
    }
    void occupiedListeningPortDoesNotSilentlyMove() {
        QTemporaryDir downloads;
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::AnyIPv4));
        QSettings settings;
        settings.clear();
        settings.setValue("listenPort", occupied.serverPort());
        Receiver receiver(downloads.path());
        auto *tcp = receiver.findChild<QTcpServer *>();
        QVERIFY(!tcp || !tcp->isListening());
    }
    void trustedRoundTrip() {
        QTemporaryDir downloads;
        QVERIFY(downloads.isValid());
        QSettings settings;
        settings.clear();
        settings.setValue("listenPort", 0);
        settings.setValue("destination", downloads.path());
        settings.sync();
        Receiver receiver(downloads.path());
        receiver.reveal();
        auto *tcp = receiver.findChild<QTcpServer *>();
        QVERIFY(tcp);
        QVERIFY(tcp->isListening());
        const quint16 port = tcp->serverPort();
        auto send = [port](QProcess &process, const QString &name) {
            const QString script = QString("import sys; sys.path.insert(0, %1); from fake_sender import "
                                           "Sender; s=Sender('127.0.0.1', %2, %3); s.introduce([(42, "
                                           "'received.txt', 5)]); s.payload(42, 2, 5, 0, b'hello', "
                                           "True); s.close()")
                                       .arg(QString("'%1/tests'").arg(SOURCE_DIR))
                                       .arg(port)
                                       .arg("'" + name + "'");
            process.start(QStringLiteral(PYTHON_EXECUTABLE), {"-c", script});
        };
        QProcess first;
        send(first, "Test Android");
        QGroupBox *card = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     for (auto *g : receiver.findChildren<QGroupBox *>())
                                         if (g->title() == "Test Android") {
                                             card = g;
                                             return true;
                                         }
                                     return false;
                                 })(),
                                 8000);
        QVERIFY(card);
        auto *trust = card->findChild<QCheckBox *>();
        QVERIFY(trust);
        QVERIFY(!trust->isChecked());
        trust->setChecked(true);
        receiver.grab().save(QString(SOURCE_DIR) + "/build/preview.png");
        QPushButton *accept = nullptr;
        for (auto *button : card->findChildren<QPushButton *>())
            if (button->text() == "Accept")
                accept = button;
        QVERIFY(accept);
        QTRY_VERIFY_WITH_TIMEOUT(notifications->title.contains("is sending files"), 8000);
        const int buttonIndex = notifications->actions.indexOf("Accept");
        QVERIFY(buttonIndex > 0);
        emit notifications->ActionInvoked(notifications->next, notifications->actions[buttonIndex - 1]);
        QTRY_COMPARE_WITH_TIMEOUT(first.state(), QProcess::NotRunning, 8000);
        QCOMPARE(first.exitCode(), 0);
        QTRY_VERIFY(QFileInfo::exists(downloads.filePath("received.txt")));
        settings.sync();
        settings.beginGroup("trusted");
        QCOMPARE(settings.childKeys().size(), 1);
        settings.endGroup();
        QProcess second;
        send(second, "Test Android");
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), QProcess::NotRunning, 8000);
        QCOMPARE(second.exitCode(), 0);
        QVERIFY(QFileInfo::exists(downloads.filePath("received (1).txt")));
        // A different name at the same address must still ask for consent.
        QProcess stranger;
        send(stranger, "Other Android");
        QGroupBox *unknown = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     for (auto *g : receiver.findChildren<QGroupBox *>())
                                         if (g->title() == "Other Android") {
                                             unknown = g;
                                             return true;
                                         }
                                     return false;
                                 })(),
                                 8000);
        QVERIFY(!QFileInfo::exists(downloads.filePath("received (2).txt")));
        QPushButton *reject = nullptr;
        for (auto *button : unknown->findChildren<QPushButton *>())
            if (button->text() == "Reject")
                reject = button;
        QVERIFY(reject);
        QTest::mouseClick(reject, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(stranger.state(), QProcess::NotRunning, 8000);
        receiver.close();
        QVERIFY(!receiver.isVisible());
    }
    void retryRejectedOutgoingTransfer() {
        auto startReceiver = [](QProcess &process, const QString &mode) {
            const QString script =
                QString("import sys,socket; sys.path.insert(0, '%1/tests'); from fake_receiver import "
                        "Receiver; s=socket.socket(); s.bind(('127.0.0.1',0)); s.listen(); "
                        "print(s.getsockname()[1],flush=True); r=Receiver(s,'%2'); print(r.text,flush=True)")
                    .arg(SOURCE_DIR)
                    .arg(mode);
            process.start(QStringLiteral(PYTHON_EXECUTABLE), {"-c", script});
            if (!process.waitForReadyRead(5000))
                return quint16(0);
            return process.readLine().trimmed().toUShort();
        };
        QProcess rejected;
        const auto firstPort = startReceiver(rejected, "reject");
        QVERIFY(firstPort);
        QPointer<SendDialog> dialog = new SendDialog({}, "Retry fixture text");
        dialog->show();
        auto *discovery = dialog->findChild<DeviceDiscovery *>();
        QVERIFY(discovery);
        auto *list = dialog->findChild<QListWidget *>("nearbyDevices");
        QVERIFY(list);
        auto *send = dialog->findChild<QPushButton *>("sendButton");
        QVERIFY(send);
        discovery->ingest(receiverAnnouncement(firstPort));
        auto entries = list->findItems("Retry Phone", Qt::MatchStartsWith);
        QVERIFY(!entries.isEmpty());
        list->setCurrentItem(entries.first());
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->status().contains("rejected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(rejected.state(), QProcess::NotRunning, 5000);
        QProcess accepted;
        const auto secondPort = startReceiver(accepted, "accept");
        QVERIFY(secondPort);
        discovery->ingest(receiverAnnouncement(secondPort));
        entries = list->findItems("Retry Phone", Qt::MatchStartsWith);
        QVERIFY(!entries.isEmpty());
        list->setCurrentItem(entries.first());
        QVERIFY(send->isEnabled());
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->status().startsWith("Sent to"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(accepted.state(), QProcess::NotRunning, 5000);
        QCOMPARE(accepted.exitCode(), 0);
        QCOMPARE(accepted.readAllStandardOutput().trimmed(), QByteArray("Retry fixture text"));
        dialog->close();
        QTRY_VERIFY(dialog.isNull());
    }
};
int main(int argc, char **argv) {
    QTemporaryDir config;
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    QApplication app(argc, argv);
    app.setApplicationName("kquickshare-tests");
    app.setOrganizationName("KQuickShareTests");
    app.setQuitOnLastWindowClosed(false);
    Notifications notificationService;
    notifications = &notificationService;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.freedesktop.Notifications") ||
        !bus.registerObject("/org/freedesktop/Notifications", notifications,
                            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals))
        return 1;
    QSettings().setValue("listenPort", 0);
    GuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "gui_tests.moc"
