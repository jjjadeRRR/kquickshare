#include "localization.h"
#include "receiver.h"

#include <QApplication>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDir>
#include <QIcon>
#include <QLibraryInfo>
#include <QSocketNotifier>
#include <QUrl>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace
{
constexpr auto kService = "org.kde.kquickshare";
constexpr auto kPath = "/Receiver";

int terminationPipe[2] = {-1, -1};

void handleTermination(int)
{
    const char byte = 1;
    if (terminationPipe[1] >= 0) {
        const auto ignored = ::write(terminationPipe[1], &byte, 1);
        (void)ignored;
    }
}

// SIGTERM/SIGINT are delivered to the Qt event loop through a self-pipe so the
// receiver can remove unfinished files before exiting.
void installTerminationHandler(QCoreApplication &app)
{
    if (::pipe2(terminationPipe, O_NONBLOCK | O_CLOEXEC) != 0)
        return;
    auto *notifier = new QSocketNotifier(terminationPipe[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app] {
        char data[32];
        while (::read(terminationPipe[0], data, sizeof(data)) > 0) { }
        app.quit();
    });
    struct sigaction action{};
    action.sa_handler = handleTermination;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

// The service may run with another working directory; send it absolute paths.
QStringList absolutePaths(const QStringList &paths)
{
    QStringList result;
    for (const QString &path : paths) {
        const QUrl url = QUrl::fromUserInput(path, QDir::currentPath(), QUrl::AssumeLocalFile);
        result << (url.isLocalFile() ? url.toLocalFile() : path);
    }
    return result;
}
} // namespace

class Activation : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kquickshare")
public:
    explicit Activation(Receiver *receiver)
        : QDBusAbstractAdaptor(receiver)
        , m_receiver(receiver)
    { }

public Q_SLOTS:
    QString SendStatus() const
    {
        return m_receiver->sendStatus();
    }
    void Ping()
    { }
    void Show()
    {
        m_receiver->reveal();
    }
    void SendFiles(const QStringList &paths)
    {
        m_receiver->sendFiles(paths);
    }
    void SendText(const QString &text)
    {
        m_receiver->sendText(text);
    }

private:
    Receiver *m_receiver;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTranslator appTranslation, qtTranslation;
    if (!localization::load(appTranslation)) {
        qCritical("Could not load the embedded application translations");
        return 1;
    }
    if (localization::language() == QLatin1String("ru") && qtTranslation.load(QStringLiteral("qtbase_ru"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslation);
    app.setApplicationName(QStringLiteral("kquickshare"));
    app.setOrganizationName(QStringLiteral("KQuickShare"));
    app.setApplicationVersion(QStringLiteral(KQUICKSHARE_VERSION));
    app.setDesktopFileName(QStringLiteral("org.kde.kquickshare"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.kquickshare")));
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate("Main", "Quick Share for KDE Plasma — Android file transfers"));
    parser.addOption({QStringLiteral("daemon"), QCoreApplication::translate("Main", "Internal background service mode")});
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("background"), QCoreApplication::translate("Main", "Start in the tray without opening a window")});
    parser.addOption({QStringLiteral("send"), QCoreApplication::translate("Main", "Send the selected files to a phone")});
    parser.addOption({QStringLiteral("clipboard"), QCoreApplication::translate("Main", "Send clipboard text to a phone")});
    parser.addPositionalArgument(QStringLiteral("files"), QCoreApplication::translate("Main", "Local files to send"), QStringLiteral("[files…]"));
    parser.process(app);

    const QStringList files = absolutePaths(parser.positionalArguments());
    const bool sending = parser.isSet(QStringLiteral("send")) || !files.isEmpty();
    const bool clipboard = parser.isSet(QStringLiteral("clipboard"));
    const bool background = parser.isSet(QStringLiteral("background"));
    const QString text = clipboard ? QApplication::clipboard()->text() : QString();

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical("No connection to the desktop session D-Bus");
        return 1;
    }

    // Forwards the request to the running (or D-Bus-activated) service.
    auto forward = [&](bool showByDefault) {
        QDBusInterface service(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kService), bus);
        if (!service.isValid()) {
            qCritical("Could not start the KQuickShare service: %s", qPrintable(service.lastError().message()));
            return 1;
        }
        QDBusMessage reply;
        if (sending)
            reply = service.call(QStringLiteral("SendFiles"), files);
        else if (clipboard)
            reply = service.call(QStringLiteral("SendText"), text);
        else
            reply = service.call(showByDefault ? QStringLiteral("Show") : QStringLiteral("Ping"));
        return reply.type() == QDBusMessage::ErrorMessage ? 1 : 0;
    };

    if (!parser.isSet(QStringLiteral("daemon")))
        return forward(!background);
    if (bus.interface()->isServiceRegistered(QLatin1String(kService)))
        return forward(true);

    installTerminationHandler(app);
    Receiver receiver;
    new Activation(&receiver);
    // Export the object before taking the name: calls queued during D-Bus
    // activation are dispatched as soon as the name is owned.
    if (!bus.registerObject(QLatin1String(kPath), &receiver, QDBusConnection::ExportAdaptors)) {
        qCritical("Could not export the KQuickShare D-Bus object");
        return 1;
    }
    if (!bus.registerService(QLatin1String(kService)))
        return forward(true); // another instance won the race

    if (sending)
        receiver.sendFiles(files);
    else if (clipboard)
        receiver.sendText(text);
    else if (!background)
        receiver.reveal();
    return app.exec();
}

#include "main.moc"
