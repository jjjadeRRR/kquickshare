// Manual hardware harness. Auto-accepts into the supplied temporary directory.
#include "bletransport.h"
#include "bluetoothreceiver.h"
#include "endpointinfo.h"
#include "inboundconnection.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) {
        qWarning("Usage: ble-test-receiver LAN_IPV4 DESTINATION");
        return 2;
    }
    const QHostAddress address(app.arguments().at(1));
    const QString destination = app.arguments().at(2);
    BluetoothReceiver receiver("b123", EndpointInfo::serialize(QStringLiteral("KQS LAN Handoff"), DeviceType::Laptop));
    QObject::connect(&receiver, &BluetoothReceiver::newConnection, &app, [&](BleTransport *transport) {
        auto *connection = new InboundConnection(transport, &app);
        connection->setLanUpgradeAddress(address);
        QObject::connect(connection, &InboundConnection::transferRequested, &app, [destination](InboundConnection *conn, const TransferRequest &) {
            qInfo("Test: accepting incoming transfer");
            conn->acceptTransfer(destination);
        });
        QObject::connect(connection, &InboundConnection::finished, &app, [&](InboundConnection *, const QStringList &files, const QString &error) {
            qInfo("Test: finished, files=%lld error=%s", qlonglong(files.size()), qPrintable(error));
            QTimer::singleShot(0, &app, &QCoreApplication::quit);
        });
    });
    receiver.start();
    return app.exec();
}
