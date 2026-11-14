#include "nearbyserver.h"

#include "inboundconnection.h"
#include "inboundtransport.h"
#include "bletransport.h"
#include "bluetoothreceiver.h"
#include "mdnsresponder.h"

#include <QHostInfo>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>

namespace
{
QByteArray generateEndpointId()
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    QByteArray id(4, '\0');
    for (char &c : id)
        c = alphabet[QRandomGenerator::global()->bounded(int(sizeof(alphabet) - 1))];
    return id;
}

QString sanitizeHostLabel(const QString &hostName)
{
    QString out;
    for (const QChar c : hostName.toLower()) {
        if (c.isLetterOrNumber())
            out.append(c);
        else if (c == QLatin1Char('-') && !out.isEmpty())
            out.append(c);
    }
    if (out.isEmpty())
        out = QStringLiteral("linux-pc");
    return out;
}
} // namespace

NearbyServer::NearbyServer(QObject *parent)
    : QObject(parent)
    , m_endpointId(generateEndpointId())
{ }

NearbyServer::~NearbyServer()
{
    stop();
}

bool NearbyServer::start(const QString &deviceName, DeviceType deviceType, quint16 listenPort)
{
    stop();

    m_tcpServer = new QTcpServer(this);
    if (!m_tcpServer->listen(QHostAddress::AnyIPv4, listenPort)) {
        emit errorOccurred(tr("Could not open a TCP port: %1").arg(m_tcpServer->errorString()));
        stop();
        return false;
    }
    connect(m_tcpServer, &QTcpServer::newConnection, this, &NearbyServer::onNewConnection);
    m_deviceName = deviceName;
    m_deviceType = deviceType;
    const QString instanceName = refreshDiscovery();
    emit started(instanceName, m_tcpServer->serverPort());
    return true;
}

QString NearbyServer::refreshDiscovery()
{
    if (!m_tcpServer || !m_tcpServer->isListening())
        return {};
    if (m_bluetooth)
        disconnect(m_bluetooth, nullptr, this, nullptr);
    delete m_bluetooth;
    m_bluetooth = nullptr;
    if (m_mdns) {
        m_mdns->stop();
        m_mdns->deleteLater();
        m_mdns = nullptr;
    }

    // Service instance name: base64url(0x23 | endpointId | 0xFC 0x9F 0x5E | 0x00 0x00)
    const QByteArray endpointId = m_endpointId;
    QByteArray nameBytes;
    nameBytes.append(char(0x23));
    nameBytes.append(endpointId);
    nameBytes.append(char(0xFC)).append(char(0x9F)).append(char(0x5E));
    nameBytes.append(char(0x00)).append(char(0x00));
    const QByteArray instanceName = base64UrlEncode(nameBytes);

    const QByteArray rawEndpointInfo = EndpointInfo::serialize(m_deviceName, m_deviceType);
    const QByteArray endpointInfo = base64UrlEncode(rawEndpointInfo);
    // Use our own host label: answering for the machine's "<hostname>.local"
    // with cache-flush set conflicts with avahi-daemon on multi-homed hosts.
    const QString hostName
        = sanitizeHostLabel(QHostInfo::localHostName()) + QStringLiteral("-quickshare-") + QString::fromLatin1(endpointId).toLower() + QStringLiteral(".local");

    m_mdns = new MdnsResponder(QStringLiteral("_FC9F5ED42C8A._tcp.local"), QString::fromLatin1(instanceName), hostName, m_tcpServer->serverPort(), endpointInfo, this);
    if (!m_mdns->start())
        qWarning("Quick Share: LAN discovery unavailable; Bluetooth reception can still run");

    m_bluetooth = new BluetoothReceiver(endpointId, rawEndpointInfo, this);
    connect(m_bluetooth, &BluetoothReceiver::newConnection, this, [this](BleTransport *transport) { addConnection(transport); });
    m_bluetooth->start();
    return QString::fromLatin1(instanceName);
}

void NearbyServer::stop()
{
    const auto connections = findChildren<InboundConnection *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto *connection : connections)
        connection->cancelTransfer();
    if (m_bluetooth)
        disconnect(m_bluetooth, nullptr, this, nullptr);
    delete m_bluetooth;
    m_bluetooth = nullptr;
    if (m_mdns) {
        m_mdns->stop();
        m_mdns->deleteLater();
        m_mdns = nullptr;
    }
    if (m_tcpServer) {
        m_tcpServer->close();
        m_tcpServer->deleteLater();
        m_tcpServer = nullptr;
    }
}

bool NearbyServer::isRunning() const
{
    return m_tcpServer && m_tcpServer->isListening();
}

bool NearbyServer::hasActiveConnections() const
{
    return !findChildren<InboundConnection *>(QString(), Qt::FindDirectChildrenOnly).isEmpty();
}

void NearbyServer::onNewConnection()
{
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();
        qInfo("Quick Share inbound connecting: peer=%s", qPrintable(socket->peerAddress().toString()));
        addConnection(new TcpInboundTransport(socket));
    }
}

void NearbyServer::addConnection(InboundTransport *transport)
{
    if (findChildren<InboundConnection *>(QString(), Qt::FindDirectChildrenOnly).size() >= 8) {
        transport->abort();
        transport->deleteLater();
        return;
    }
    auto *conn = new InboundConnection(transport, this);
    if (!transport->supportsLanTrust() && m_mdns)
        conn->setLanUpgradeAddress(m_mdns->ipv4Address());
    connect(conn, &InboundConnection::transferRequested, this, &NearbyServer::transferRequested);
    connect(conn, &InboundConnection::transferProgress, this, &NearbyServer::transferProgress);
    connect(conn, &InboundConnection::textReceived, this, &NearbyServer::textReceived);
    connect(conn, &InboundConnection::finished, this, &NearbyServer::connectionFinished);
}
