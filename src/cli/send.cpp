#include "devicediscovery.h"
#include "outboundconnection.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("kquickshare-send");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"discover", "Find receiving phones for 8 seconds"});
    parser.addOption({"host", "IPv4 address", "address"});
    parser.addOption({"port", "TCP port", "port"});
    parser.addOption({"text", "Send text instead of files", "text"});
    parser.addPositionalArgument("files", "Local file paths");
    parser.process(app);
    if (parser.isSet("discover")) {
        DeviceDiscovery discovery;
        QObject::connect(&discovery, &DeviceDiscovery::changed, &app, [&discovery] {
            for (const auto &device : discovery.devices()) {
                const auto line
                    = QJsonDocument(QJsonObject{{"id", device.id}, {"name", device.name}, {"address", device.address.toString()}, {"port", device.port}})
                          .toJson(QJsonDocument::Compact);
                std::puts(line.constData());
                std::fflush(stdout);
            }
        });
        if (!discovery.start())
            return 1;
        QTimer::singleShot(8000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    OutboundConnection sender;
    QString error;
    const bool valid = parser.isSet("text") ? sender.setText(parser.value("text"), error) : sender.setFiles(parser.positionalArguments(), error);
    bool portOk = false;
    const uint port = parser.value("port").toUInt(&portOk);
    const QHostAddress host(parser.value("host"));
    if (!valid || !portOk || !port || port > 65535 || host.isNull()) {
        qCritical("%s", qPrintable(error.isEmpty() ? QString("Specify --host and --port") : error));
        return 1;
    }
    QObject::connect(&sender, &OutboundConnection::statusChanged, &app, [](const QString &s) {
        qInfo("%s", qPrintable(s));
    });
    QObject::connect(&sender, &OutboundConnection::finished, &app, [&app](const QString &error) {
        if (!error.isEmpty())
            qWarning("%s", qPrintable(error));
        app.exit(error.isEmpty() ? 0 : 1);
    });
    sender.start(host, quint16(port), QHostInfo::localHostName());
    return app.exec();
}
