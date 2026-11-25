// Entry point of the core library: TCP listener, mDNS and optional BLE discovery.
// Owns all active inbound connections and re-emits their signals.
#pragma once

#include "endpointinfo.h"

#include <QObject>
#include <QTimer>

class QTcpServer;
class MdnsResponder;
class InboundConnection;
class InboundTransport;
class BluetoothReceiver;
struct TransferRequest;

class NearbyServer : public QObject
{
    Q_OBJECT
public:
    explicit NearbyServer(QObject *parent = nullptr);
    ~NearbyServer() override;

    // Starts the TCP listener on the requested port (0 = automatic) and advertises it via mDNS.
    bool start(const QString &deviceName, DeviceType deviceType = DeviceType::Laptop, quint16 listenPort = 0, bool enableBluetooth = true);
    void stop();
    bool isRunning() const;
    bool hasActiveConnections() const;

signals:
    void transferRequested(InboundConnection *connection, const TransferRequest &request);
    void transferProgress(InboundConnection *connection, const QString &fileName, qint64 received, qint64 total);
    void textReceived(InboundConnection *connection, const QString &text, int type);
    // error is empty on success; connection object is deleted right after.
    void connectionFinished(InboundConnection *connection, const QStringList &savedFiles, const QString &error);
    void started(const QString &serviceName, quint16 port);
    void errorOccurred(const QString &message);

private slots:
    void onNewConnection();

private:
    void addConnection(InboundTransport *transport);
    QString startDiscovery(const QString &deviceName, DeviceType deviceType, bool enableBluetooth);
    QTcpServer *m_tcpServer = nullptr;
    MdnsResponder *m_mdns = nullptr;
    QTimer m_mdnsRetry;
    BluetoothReceiver *m_bluetooth = nullptr;
    QByteArray m_endpointId;
};
