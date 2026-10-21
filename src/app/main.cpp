#include "receiver.h"
#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QIcon>
#include <QSocketNotifier>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
namespace {
int terminationPipe[2] = {-1,-1};
void handleTermination(int) { const char byte = 1; if (terminationPipe[1] >= 0) { const auto ignored = ::write(terminationPipe[1], &byte, 1); (void)ignored; } }
}

class Activation : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kquickshare")
  public:
    explicit Activation(Receiver *receiver) : QDBusAbstractAdaptor(receiver), m_receiver(receiver) {}
  public slots:
    QString SendStatus() const { return m_receiver->sendStatus(); }
    void Ping() {}
    void Show() { m_receiver->reveal(); }
    void SendFiles(const QStringList &paths) { m_receiver->sendFiles(paths); }
    void SendText(const QString &text) { m_receiver->sendText(text); }

  private:
    Receiver *m_receiver;
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("kquickshare");
    app.setOrganizationName("KQuickShare");
    app.setApplicationVersion("0.2.0");
    app.setDesktopFileName("org.kde.kquickshare");
    app.setWindowIcon(QIcon::fromTheme("org.kde.kquickshare"));
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("Quick Share для KDE Plasma — приём файлов с Android");
    parser.addOption({"daemon", "Внутренний запуск фоновой службы"});
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"background", "Запуск в трее без открытия окна"});
    parser.addOption({"send", "Отправить выбранные файлы на телефон"});
    parser.addOption({"clipboard", "Отправить текст из буфера обмена на телефон"});
    parser.addPositionalArgument("files", "Локальные файлы для отправки", "[files…]");
    parser.process(app);
    const QStringList files = parser.positionalArguments();
    const bool sending = parser.isSet("send") || !files.isEmpty();
    const bool clipboard = parser.isSet("clipboard");
    const QString text = clipboard ? QApplication::clipboard()->text() : QString();
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical("Нет соединения с D-Bus сеансом рабочего стола");
        return 1;
    }
    if (!parser.isSet("daemon")) {
        QDBusInterface existing("org.kde.kquickshare", "/Receiver", "org.kde.kquickshare", bus);
        if (!existing.isValid()) { qCritical("Не удалось запустить службу KQuickShare: %s", qPrintable(existing.lastError().message())); return 1; }
        QDBusMessage reply;
        if (sending) reply = existing.call("SendFiles", files);
        else if (clipboard) reply = existing.call("SendText", text);
        else reply = existing.call(parser.isSet("background") ? "Ping" : "Show");
        return reply.type() == QDBusMessage::ErrorMessage ? 1 : 0;
    }
    if (!bus.registerService("org.kde.kquickshare")) {
        QDBusInterface existing("org.kde.kquickshare", "/Receiver", "org.kde.kquickshare", bus);
        if (sending) existing.call("SendFiles", files);
        else if (clipboard) existing.call("SendText", text);
        else existing.call("Show");
        return 0;
    }
    QSocketNotifier *terminationNotifier = nullptr;
    if (::pipe2(terminationPipe, O_NONBLOCK | O_CLOEXEC) == 0) {
        terminationNotifier = new QSocketNotifier(terminationPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(terminationNotifier, &QSocketNotifier::activated, &app, [&app] { char data[32]; while (::read(terminationPipe[0], data, sizeof(data)) > 0) {} app.quit(); });
        struct sigaction action{}; action.sa_handler = handleTermination; sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr); sigaction(SIGINT, &action, nullptr);
    }
    Receiver receiver;
    new Activation(&receiver);
    bus.registerObject("/Receiver", &receiver, QDBusConnection::ExportAdaptors);
    if (sending) receiver.sendFiles(files);
    else if (clipboard) receiver.sendText(text);
    else if (!parser.isSet("background")) receiver.reveal();
    return app.exec();
}
#include "main.moc"
