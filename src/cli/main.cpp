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
    parser.setApplicationDescription(QStringLiteral("Приёмник Quick Share (Nearby Share) без GUI"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption destOption({QStringLiteral("d"), QStringLiteral("dest")},
                                        QStringLiteral("Папка для сохранения файлов"), QStringLiteral("dir"),
                                        QDir::currentPath());
    const QCommandLineOption nameOption({QStringLiteral("n"), QStringLiteral("name")},
                                        QStringLiteral("Видимое имя устройства"), QStringLiteral("name"),
                                        QHostInfo::localHostName());
    const QCommandLineOption onceOption(QStringLiteral("once"),
                                        QStringLiteral("Завершиться после первой передачи"));
    parser.addOption(destOption);
    parser.addOption(nameOption);
    parser.addOption(onceOption);
    parser.process(app);

    const QString destDir = parser.value(destOption);
    const bool once = parser.isSet(onceOption);

    NearbyServer server;
    QObject::connect(&server, &NearbyServer::started, &app, [](const QString &service, quint16 port) {
        qInfo("Анонсирован сервис %s на порту %u — устройство видно в Quick Share",
              qPrintable(service), port);
    });
    QObject::connect(&server, &NearbyServer::errorOccurred, &app, [](const QString &message) {
        qWarning("Ошибка: %s", qPrintable(message));
    });
    QObject::connect(&server, &NearbyServer::transferRequested, &app,
                     [&server, destDir](InboundConnection *conn, const TransferRequest &request) {
                         if (!request.files.isEmpty()) {
                             qInfo("Входящая передача от %s (PIN %s):", qPrintable(request.deviceName),
                                   qPrintable(request.pinCode));
                             for (const IncomingFile &f : request.files)
                                 qInfo("  %s (%lld байт, %s)", qPrintable(f.name), f.size,
                                       qPrintable(f.mimeType));
                         } else {
                             qInfo("Входящий текст от %s (PIN %s): %s", qPrintable(request.deviceName),
                                   qPrintable(request.pinCode), qPrintable(request.textTitle));
                         }
                         conn->acceptTransfer(destDir);
                     });
    QObject::connect(&server, &NearbyServer::transferProgress, &app,
                     [](InboundConnection *, const QString &fileName, qint64 received, qint64 total) {
                         qInfo("  %s: %lld/%lld", qPrintable(fileName), received, total);
                     });
    QObject::connect(&server, &NearbyServer::textReceived, &app,
                     [](InboundConnection *, const QString &text, int) {
                         qInfo("Получен текст: %s", qPrintable(text));
                     });
    QObject::connect(&server, &NearbyServer::connectionFinished, &app,
                     [&app, once](InboundConnection *, const QStringList &savedFiles, const QString &error) {
                         if (error.isEmpty()) {
                             for (const QString &path : savedFiles)
                                 qInfo("Сохранено: %s", qPrintable(path));
                         } else {
                             qWarning("Передача завершилась: %s", qPrintable(error));
                         }
                         if (once)
                             QTimer::singleShot(0, &app, &QCoreApplication::quit);
                     });

    if (!server.start(parser.value(nameOption)))
        return 1;
    return app.exec();
}
