// kquickshare-cli: headless Quick Share receiver (prototype / debugging).
// Auto-accepts incoming transfers into the destination directory.
#include "../core/inboundconnection.h"
#include "../core/nearbyserver.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QHostInfo>
#include <QTimer>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kquickshare-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KQUICKSHARE_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Headless Quick Share (Nearby Share) receiver"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption destOption(
        {QStringLiteral("d"), QStringLiteral("dest")}, QStringLiteral("Directory for received files"), QStringLiteral("dir"), QDir::currentPath());
    const QCommandLineOption nameOption(
        {QStringLiteral("n"), QStringLiteral("name")}, QStringLiteral("Visible device name"), QStringLiteral("name"), QHostInfo::localHostName());
    const QCommandLineOption onceOption(QStringLiteral("once"), QStringLiteral("Exit after the first transfer"));
    parser.addOption(destOption);
    parser.addOption(nameOption);
    parser.addOption(onceOption);
    parser.process(app);

    const QString destDir = parser.value(destOption);
    const bool once = parser.isSet(onceOption);

    NearbyServer server;
    QObject::connect(&server, &NearbyServer::started, &app, [](const QString &service, quint16 port) {
        qInfo("Advertising service %s on port %u; the device is visible in Quick Share", qPrintable(service), port);
    });
    QObject::connect(&server, &NearbyServer::errorOccurred, &app, [](const QString &message) {
        qWarning("Error: %s", qPrintable(message));
    });
    QObject::connect(&server, &NearbyServer::transferRequested, &app, [&server, destDir](InboundConnection *conn, const TransferRequest &request) {
        if (!request.files.isEmpty()) {
            qInfo("Incoming transfer from %s (PIN %s):", qPrintable(request.deviceName), qPrintable(request.pinCode));
            for (const IncomingFile &f : request.files)
                qInfo("  %s (%lld bytes, %s)", qPrintable(f.name), f.size, qPrintable(f.mimeType));
        } else {
            qInfo("Incoming text from %s (PIN %s): %s", qPrintable(request.deviceName), qPrintable(request.pinCode), qPrintable(request.textTitle));
        }
        conn->acceptTransfer(destDir);
    });
    QObject::connect(&server, &NearbyServer::transferProgress, &app, [](InboundConnection *, const QString &fileName, qint64 received, qint64 total) {
        qInfo("  %s: %lld/%lld", qPrintable(fileName), received, total);
    });
    QObject::connect(&server, &NearbyServer::textReceived, &app, [](InboundConnection *, const QString &text, int) {
        qInfo("Received text: %s", qPrintable(text));
    });
    QObject::connect(&server, &NearbyServer::connectionFinished, &app, [&app, once](InboundConnection *, const QStringList &savedFiles, const QString &error) {
        if (error.isEmpty()) {
            for (const QString &path : savedFiles)
                qInfo("Saved: %s", qPrintable(path));
        } else {
            qWarning("Transfer failed: %s", qPrintable(error));
        }
        if (once)
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
    });

    if (!server.start(parser.value(nameOption)))
        return 1;
    return app.exec();
}
