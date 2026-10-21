#include "devicediscovery.h"
#include "crypto.h"
#include "endpointinfo.h"
#include <QDateTime>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>
namespace {
const QString service = QStringLiteral("_fc9f5ed42c8a._tcp.local");
quint16 u16(const QByteArray &p, int n) { return quint16(quint8(p[n])) << 8 | quint8(p[n + 1]); }
quint32 u32(const QByteArray &p, int n) { return quint32(u16(p, n)) << 16 | u16(p, n + 2); }
} // namespace
DeviceDiscovery::DeviceDiscovery(QObject *parent) : QObject(parent) {
    m_timer.setInterval(3000);
    connect(&m_timer, &QTimer::timeout, this, &DeviceDiscovery::query);
    connect(&m_socket, &QUdpSocket::readyRead, this, [this] {
        while (m_socket.hasPendingDatagrams()) {
            const auto datagram = m_socket.receiveDatagram(65536);
            ingest(datagram.data());
        }
    });
}
bool DeviceDiscovery::start() {
    stop();
    if (!m_socket.bind(QHostAddress::AnyIPv4, 5353,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit errorOccurred(m_socket.errorString());
        return false;
    }
    bool joined = false;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::CanMulticast) ||
            (flags & QNetworkInterface::IsLoopBack) || (flags & QNetworkInterface::IsPointToPoint))
            continue;
        joined |= m_socket.joinMulticastGroup(QHostAddress("224.0.0.251"), iface);
    }
    if (!joined) {
        emit errorOccurred(tr("Нет доступной локальной сети"));
        stop();
        return false;
    }
    m_socket.setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    m_timer.start();
    query();
    return true;
}
void DeviceDiscovery::stop() {
    m_timer.stop();
    m_socket.close();
    m_services.clear();
    m_hosts.clear();
    m_devices.clear();
}
QList<NearbyDevice> DeviceDiscovery::devices() const { return m_devices; }
void DeviceDiscovery::setQrKey(const QByteArray &key) {
    m_qrToken = crypto::hkdfSha256(key, {}, "advertisingContext", 16);
    m_nameKey = crypto::hkdfSha256(key, {}, "encryptionKey", 16);
}
void DeviceDiscovery::question(const QString &name, quint16 type) {
    if (m_socket.state() != QAbstractSocket::BoundState)
        return;
    QByteArray packet(12, '\0');
    packet[5] = 1;
    for (const auto &label : name.split('.')) {
        const auto bytes = label.toUtf8();
        packet.append(char(bytes.size()));
        packet += bytes;
    }
    packet.append('\0');
    packet.append(char(type >> 8)).append(char(type)).append(char(0)).append(char(1));
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::CanMulticast) ||
            (flags & QNetworkInterface::IsLoopBack) || (flags & QNetworkInterface::IsPointToPoint))
            continue;
        m_socket.setMulticastInterface(iface);
        m_socket.writeDatagram(packet, QHostAddress("224.0.0.251"), 5353);
    }
}
void DeviceDiscovery::query() {
    question("_FC9F5ED42C8A._tcp.local", 12);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_services.begin(); it != m_services.end();) {
        if (it->expires < now)
            it = m_services.erase(it);
        else
            ++it;
    }
    publish();
}
bool DeviceDiscovery::decodeName(const QByteArray &packet, int &pos, QString &name) {
    int p = pos, hops = 0;
    bool jumped = false;
    QStringList labels;
    while (p >= 0 && p < packet.size() && ++hops <= 128) {
        const quint8 len = quint8(packet[p++]);
        if ((len & 0xc0) == 0xc0) {
            if (p >= packet.size())
                return false;
            const int target = ((len & 0x3f) << 8) | quint8(packet[p++]);
            if (!jumped)
                pos = p;
            jumped = true;
            p = target;
            continue;
        }
        if (len & 0xc0 || p + len > packet.size())
            return false;
        if (!len) {
            if (!jumped)
                pos = p;
            name = labels.join('.').toLower();
            return true;
        }
        labels << QString::fromUtf8(packet.mid(p, len));
        p += len;
    }
    return false;
}
void DeviceDiscovery::ingest(const QByteArray &p) {
    if (p.size() < 12 || !(u16(p, 2) & 0x8000))
        return;
    int pos = 12;
    const int questions = u16(p, 4), records = u16(p, 6) + u16(p, 8) + u16(p, 10);
    if (questions > 256 || records > 1024)
        return;
    for (int i = 0; i < questions; ++i) {
        QString unused;
        if (!decodeName(p, pos, unused) || pos + 4 > p.size())
            return;
        pos += 4;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < records; ++i) {
        QString name;
        if (!decodeName(p, pos, name) || pos + 10 > p.size())
            return;
        const auto type = u16(p, pos);
        const auto ttl = u32(p, pos + 4);
        const int size = u16(p, pos + 8);
        pos += 10;
        if (pos + size > p.size())
            return;
        const bool instance = name.endsWith('.' + service);
        if (type == 12 && name == service) {
            int offset = pos;
            QString target;
            if (!decodeName(p, offset, target) || !target.endsWith('.' + service))
                return;
            if (!ttl)
                m_services.remove(target);
            else if (m_services.size() < 128 || m_services.contains(target)) {
                auto &record = m_services[target];
                record.expires = now + qMin(ttl, quint32(120)) * 1000;
                if (!record.port)
                    question(target, 33);
                if (record.name.isEmpty())
                    question(target, 16);
            }
        } else if (type == 1 && size == 4) {
            if (m_hosts.size() < 512 || m_hosts.contains(name))
                m_hosts[name] = QHostAddress(u32(p, pos));
        } else if (instance && (type == 33 || type == 16)) {
            if (!ttl) {
                m_services.remove(name);
                pos += size;
                continue;
            }
            if (m_services.size() >= 128 && !m_services.contains(name)) {
                pos += size;
                continue;
            }
            auto &r = m_services[name];
            r.expires = now + qMin(ttl, quint32(120)) * 1000;
            if (type == 33 && size >= 7) {
                int offset = pos + 6;
                r.port = u16(p, pos + 4);
                if (!decodeName(p, offset, r.host))
                    return;
                if (!m_hosts.contains(r.host))
                    question(r.host, 1);
            }
            if (type == 16) {
                int at = pos;
                while (at < pos + size) {
                    const int n = quint8(p[at++]);
                    if (at + n > pos + size)
                        return;
                    const auto value = p.mid(at, n);
                    at += n;
                    if (value.startsWith("n=")) {
                        EndpointInfo info;
                        if (EndpointInfo::parse(base64UrlDecode(value.mid(2)), info)) {
                            if (info.name.isEmpty() && !m_nameKey.isEmpty() && info.qrCodeData.size() > 28) {
                                const auto decoded =
                                    crypto::aes128GcmDecrypt(m_nameKey, info.qrCodeData, m_qrToken);
                                if (!decoded.isEmpty())
                                    info.name = QString::fromUtf8(decoded);
                            }
                            // Hidden devices are offered only after our QR key
                            // authenticates their encrypted discovery metadata.
                            r.name = info.name;
                            r.phone = info.deviceType != DeviceType::Laptop;
                        }
                    }
                }
            }
        }
        pos += size;
    }
    publish();
}
void DeviceDiscovery::publish() {
    QList<NearbyDevice> devices;
    for (auto it = m_services.cbegin(); it != m_services.cend(); ++it) {
        const auto &r = it.value();
        const auto address = m_hosts.value(r.host);
        if (r.port && !r.name.isEmpty() && r.phone && !address.isNull())
            devices.append({it.key(), r.name, address, r.port});
    }
    std::sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    bool same = devices.size() == m_devices.size();
    for (int i = 0; same && i < devices.size(); ++i) {
        const auto &a = devices[i], &b = m_devices[i];
        same &= a.id == b.id && a.name == b.name && a.address == b.address && a.port == b.port;
    }
    m_devices = devices;
    if (!same)
        emit changed();
}
