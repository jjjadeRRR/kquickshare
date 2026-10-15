#include "nearbyserver.h"

#include "inboundconnection.h"
#include "mdnsresponder.h"

#include <QHostInfo>
#include <QRandomGenerator>
#include <QTcpServer>

namespace {
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
{
}

NearbyServer::~NearbyServer()
{
    stop();
}

bool NearbyServer::start(const QString &deviceName, DeviceType deviceType)
{
    stop();

    m_tcpServer = new QTcpServer(this);
    if (!m_tcpServer->listen(QHostAddress::AnyIPv4, 0)) {
        emit errorOccurred(QStringLiteral("Не удалось открыть TCP-порт: %1").arg(m_tcpServer->errorString()));
        stop();
        return false;
    }
    const quint16 port = m_tcpServer->serverPort();
    connect(m_tcpServer, &QTcpServer::newConnection, this, &NearbyServer::onNewConnection);

    // Service instance name: base64url(0x23 | endpointId | 0xFC 0x9F 0x5E | 0x00 0x00)
    QByteArray nameBytes;
    nameBytes.append(char(0x23));
    nameBytes.append(generateEndpointId());
    nameBytes.append(char(0xFC)).append(char(0x9F)).append(char(0x5E));
    nameBytes.append(char(0x00)).append(char(0x00));
    const QByteArray instanceName = base64UrlEncode(nameBytes);

    const QByteArray endpointInfo = base64UrlEncode(EndpointInfo::serialize(deviceName, deviceType));
    const QString hostName = sanitizeHostLabel(QHostInfo::localHostName()) + QStringLiteral(".local");

    m_mdns = new MdnsResponder(QStringLiteral("_FC9F5ED42C8A._tcp.local"),
                               QString::fromLatin1(instanceName), hostName, port, endpointInfo, this);
    if (!m_mdns->start()) {
        emit errorOccurred(QStringLiteral("Не удалось запустить mDNS-анонс"));
        stop();
        return false;
    }

    emit started(QString::fromLatin1(instanceName), port);
    return true;
}

void NearbyServer::stop()
{
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

void NearbyServer::onNewConnection()
{
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();
        auto *conn = new InboundConnection(socket, this);
        connect(conn, &InboundConnection::transferRequested, this, &NearbyServer::transferRequested);
        connect(conn, &InboundConnection::transferProgress, this, &NearbyServer::transferProgress);
        connect(conn, &InboundConnection::textReceived, this, &NearbyServer::textReceived);
        connect(conn, &InboundConnection::finished, this, &NearbyServer::connectionFinished);
    }
}
