#include "mdnsresponder.h"

#include <QNetworkInterface>
#include <QTimer>
#include <QUdpSocket>
#include <QVariant>
#include <QtEndian>

namespace
{
constexpr quint16 kMdnsPort = 5353;
const QHostAddress kMdnsGroup(QStringLiteral("224.0.0.251"));
constexpr int kAnnounceIntervalMs = 60'000;

// Picks the primary LAN IPv4: up & running, not loopback, not point-to-point,
// not link-local. Prefers private RFC1918 addresses.
QPair<QHostAddress, QNetworkInterface> pickPrimaryIpv4()
{
    QPair<QHostAddress, QNetworkInterface> fallback;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) || flags.testFlag(QNetworkInterface::IsLoopBack)
            || flags.testFlag(QNetworkInterface::IsPointToPoint))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLinkLocal() || addr.isLoopback())
                continue;
            QPair<QHostAddress, QNetworkInterface> candidate{addr, iface};
            const quint32 v = addr.toIPv4Address();
            const bool isPrivate = (v >> 24) == 10 || (v >> 16) == 0xC0A8 || (v >> 20) == 0xAC1;
            if (isPrivate)
                return candidate;
            if (fallback.first.isNull())
                fallback = candidate;
        }
    }
    return fallback;
}
} // namespace

MdnsResponder::MdnsResponder(QString serviceType, QString instanceName, QString hostName, quint16 port, const QByteArray &txtValue, QObject *parent)
    : QObject(parent)
    , m_serviceType(std::move(serviceType))
    , m_instanceFqdn(std::move(instanceName) + QLatin1Char('.') + m_serviceType)
    , m_hostName(std::move(hostName))
    , m_port(port)
    , m_txtValue(txtValue)
{ }

MdnsResponder::~MdnsResponder()
{
    stop();
}

bool MdnsResponder::start()
{
    const auto [ipv4, iface] = pickPrimaryIpv4();
    if (ipv4.isNull()) {
        qWarning("MdnsResponder: no suitable IPv4 interface found");
        return false;
    }
    m_ipv4 = ipv4;

    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, kMdnsPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning("MdnsResponder: bind failed: %s", qPrintable(m_socket->errorString()));
        stop();
        return false;
    }
    if (!m_socket->joinMulticastGroup(kMdnsGroup, iface)) {
        qWarning("MdnsResponder: joinMulticastGroup failed: %s", qPrintable(m_socket->errorString()));
        stop();
        return false;
    }
    m_socket->setMulticastInterface(iface);
    m_socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    // Useful for local self-tests; harmless otherwise.
    m_socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, QVariant(1));
    connect(m_socket, &QUdpSocket::readyRead, this, &MdnsResponder::onReadyRead);

    // RFC 6762: announce a few times at startup, then re-announce periodically
    // (Android sometimes misses services started before the share sheet opens).
    m_startupAnnounces = 0;
    m_announceTimer = new QTimer(this);
    m_announceTimer->setInterval(kAnnounceIntervalMs);
    connect(m_announceTimer, &QTimer::timeout, this, &MdnsResponder::announce);
    m_announceTimer->start();
    announce();
    QTimer::singleShot(1000, this, &MdnsResponder::announce);
    QTimer::singleShot(2000, this, &MdnsResponder::announce);
    return true;
}

void MdnsResponder::stop()
{
    if (m_announceTimer) {
        m_announceTimer->stop();
        m_announceTimer->deleteLater();
        m_announceTimer = nullptr;
    }
    if (m_socket) {
        // Expire this instance immediately rather than leaving ghost entries
        // in Android's device list until the 120-second TTL elapses.
        m_socket->writeDatagram(buildResponse(0), kMdnsGroup, kMdnsPort);
        m_socket->leaveMulticastGroup(kMdnsGroup);
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void MdnsResponder::encodeName(QByteArray &out, const QString &name)
{
    const QStringList labels = name.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString &label : labels) {
        const QByteArray l = label.toUtf8().left(63);
        out.append(char(l.size()));
        out.append(l);
    }
    out.append('\0');
}

QString MdnsResponder::decodeName(const QByteArray &packet, int &pos, bool *ok)
{
    QStringList labels;
    int p = pos;
    int jumps = 0;
    bool jumped = false;
    *ok = false;
    while (p < packet.size()) {
        const uint8_t len = uint8_t(packet[p]);
        if ((len & 0xC0) == 0xC0) { // compression pointer
            if (p + 1 >= packet.size() || ++jumps > 8)
                return {};
            const int target = ((len & 0x3F) << 8) | uint8_t(packet[p + 1]);
            if (!jumped)
                pos = p + 2;
            jumped = true;
            p = target;
            continue;
        }
        if (len == 0) {
            if (!jumped)
                pos = p + 1;
            *ok = true;
            return labels.join(QLatin1Char('.'));
        }
        p += 1;
        if (p + len > packet.size())
            return {};
        labels.append(QString::fromUtf8(packet.mid(p, len)));
        p += len;
    }
    return {};
}

void MdnsResponder::onReadyRead()
{
    while (m_socket->hasPendingDatagrams()) {
        QByteArray packet(int(m_socket->pendingDatagramSize()), '\0');
        QHostAddress srcAddr;
        quint16 srcPort = 0;
        if (m_socket->readDatagram(packet.data(), packet.size(), &srcAddr, &srcPort) < 0)
            continue;
        if (packet.size() < 12)
            continue;

        const quint16 flags = (quint16(uint8_t(packet[2])) << 8) | uint8_t(packet[3]);
        if (flags & 0x8000) // not a query
            continue;
        const int qdCount = (int(uint8_t(packet[4])) << 8) | uint8_t(packet[5]);

        int pos = 12;
        bool matches = false;
        bool wantsUnicast = false;
        for (int i = 0; i < qdCount; ++i) {
            bool ok = false;
            const QString qname = decodeName(packet, pos, &ok);
            if (!ok || pos + 4 > packet.size())
                break;
            const quint16 qtype = (quint16(uint8_t(packet[pos])) << 8) | uint8_t(packet[pos + 1]);
            const quint16 qclass = (quint16(uint8_t(packet[pos + 2])) << 8) | uint8_t(packet[pos + 3]);
            pos += 4;
            wantsUnicast = wantsUnicast || (qclass & 0x8000); // QU bit
            const QString lower = qname.toLower();
            if (lower == m_serviceType.toLower() || lower == m_instanceFqdn.toLower() || lower == m_hostName.toLower()) {
                if (qtype == 12 || qtype == 33 || qtype == 16 || qtype == 1 || qtype == 255) // PTR/SRV/TXT/A/ANY
                    matches = true;
            }
        }
        if (!matches)
            continue;
        if (wantsUnicast && srcPort != 0)
            sendResponse(srcAddr, srcPort);
        else
            sendResponse(kMdnsGroup, kMdnsPort);
    }
}

QByteArray MdnsResponder::buildResponse(quint32 ttl) const
{
    QByteArray out;
    out.append(QByteArray(12, '\0'));
    out[2] = char(0x84); // QR=1, AA=1
    out[7] = 4; // ANCOUNT = 4

    auto appendCommon = [&out, ttl](const QString &name, quint16 type) {
        encodeName(out, name);
        out.append(char(type >> 8)).append(char(type & 0xff));
        out.append(char(type == 12 ? 0 : 0x80)).append(char(0x01)); // shared PTR has no cache-flush
        out.append(char(ttl >> 24)).append(char(ttl >> 16)).append(char(ttl >> 8)).append(char(ttl));
    };
    auto appendRdata = [&out](const QByteArray &rdata) {
        out.append(char(rdata.size() >> 8)).append(char(rdata.size() & 0xff));
        out.append(rdata);
    };

    // PTR: service -> instance (no cache-flush for shared records, but harmless)
    appendCommon(m_serviceType, 12);
    {
        QByteArray rdata;
        encodeName(rdata, m_instanceFqdn);
        appendRdata(rdata);
    }
    // SRV: instance -> host:port
    appendCommon(m_instanceFqdn, 33);
    {
        QByteArray rdata;
        rdata.append(QByteArray(4, '\0')); // priority=0, weight=0
        rdata.append(char(m_port >> 8)).append(char(m_port & 0xff));
        encodeName(rdata, m_hostName);
        appendRdata(rdata);
    }
    // TXT: instance, "n=<base64>"
    appendCommon(m_instanceFqdn, 16);
    {
        QByteArray str = QByteArray("n=") + m_txtValue;
        QByteArray rdata;
        rdata.append(char(str.size()));
        rdata.append(str);
        appendRdata(rdata);
    }
    // A: host -> ipv4
    appendCommon(m_hostName, 1);
    {
        QByteArray rdata(4, '\0');
        const quint32 v = qToBigEndian(m_ipv4.toIPv4Address());
        memcpy(rdata.data(), &v, 4);
        appendRdata(rdata);
    }
    return out;
}

void MdnsResponder::sendResponse(const QHostAddress &to, quint16 port)
{
    if (!m_socket)
        return;
    const QByteArray response = buildResponse();
    m_socket->writeDatagram(response, to, port);
}

void MdnsResponder::announce()
{
    sendResponse(kMdnsGroup, kMdnsPort);
}
