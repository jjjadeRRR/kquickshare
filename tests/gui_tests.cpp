#include "receiver.h"
#include "senddialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QDBusConnection>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
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
    void trustedRoundTrip() {
        QTemporaryDir downloads;
        QVERIFY(downloads.isValid());
        QSettings settings;
        settings.clear();
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
            process.start("python", {"-c", script});
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
            if (button->text() == "Принять")
                accept = button;
        QVERIFY(accept);
        QTRY_VERIFY_WITH_TIMEOUT(notifications->title.contains("отправляет"), 8000);
        const int buttonIndex = notifications->actions.indexOf("Принять");
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
            if (button->text() == "Отклонить")
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
            process.start("python", {"-c", script});
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
        QTRY_VERIFY_WITH_TIMEOUT(dialog->status().contains("отклонил"), 8000);
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
        QTRY_VERIFY_WITH_TIMEOUT(dialog->status().startsWith("Отправлено"), 8000);
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
    GuiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "gui_tests.moc"
