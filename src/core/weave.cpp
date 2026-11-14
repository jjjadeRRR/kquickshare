#include "weave.h"

#include "frames.h"

#include <algorithm>

namespace
{
const QByteArray serviceHash = QByteArray::fromHex("fc9f5e");
constexpr qsizetype maxMessageSize = frames::kMaxFrameLength + 7;
}

Weave::Weave(quint16 maxPacketSize)
    : m_maxPacketSize(std::clamp<quint16>(maxPacketSize, 20, 509))
{ }

Weave::Result Weave::receive(const QByteArray &packet, QByteArray &reply, QByteArray &data)
{
    reply.clear();
    data.clear();
    if (packet.isEmpty())
        return Result::Invalid;
    const auto header = quint8(packet[0]);
    if (header & 0x80) {
        if ((header & 0x0f) == 2)
            return Result::Disconnected;
        if (m_connected || (header & 0x0f) != 0 || packet.size() < 7 || packet.size() > 20)
            return Result::Invalid;
        const quint16 minVersion = (quint8(packet[1]) << 8) | quint8(packet[2]);
        const quint16 maxVersion = (quint8(packet[3]) << 8) | quint8(packet[4]);
        const quint16 peerSize = (quint8(packet[5]) << 8) | quint8(packet[6]);
        if (minVersion > 1 || maxVersion < 1 || peerSize < 20)
            return Result::Invalid;
        m_packetSize = std::min(peerSize, m_maxPacketSize);
        reply = QByteArray::fromHex("810001");
        reply.append(char(m_packetSize >> 8));
        reply.append(char(m_packetSize));
        m_connected = true;
        return Result::Connected;
    }
    if (!m_connected || packet.size() > m_packetSize || packet.size() < 2)
        return Result::Invalid;
    const bool first = header & 0x08;
    const bool last = header & 0x04;
    if (first == m_receiving)
        return Result::Invalid;
    if (first) {
        m_message.clear();
        m_receiving = true;
    }
    if (m_message.size() + packet.size() - 1 > maxMessageSize)
        return Result::Invalid;
    m_message.append(packet.constData() + 1, packet.size() - 1);
    if (!last)
        return Result::Pending;
    m_receiving = false;
    if (m_message.size() < 3)
        return Result::Invalid;
    const QByteArray hash = m_message.left(3);
    if (hash == QByteArray(3, '\0')) {
        // SocketControlFrame: type 1 is introduction; type 2 is disconnection.
        if (m_message.size() < 5 || quint8(m_message[3]) != 0x08)
            return Result::Invalid;
        if (quint8(m_message[4]) == 1)
            return Result::Pending;
        return quint8(m_message[4]) == 2 ? Result::Disconnected : Result::Invalid;
    }
    if (hash != serviceHash)
        return Result::Invalid;
    data = m_message.mid(3);
    return Result::Data;
}

QList<QByteArray> Weave::encode(const QByteArray &frame)
{
    QList<QByteArray> packets;
    if (!m_connected || frame.isEmpty() || frame.size() > maxMessageSize - 3)
        return packets;
    const QByteArray message = serviceHash + frame;
    const qsizetype payloadSize = m_packetSize - 1;
    for (qsizetype offset = 0; offset < message.size(); offset += payloadSize) {
        const qsizetype size = std::min(payloadSize, message.size() - offset);
        quint8 header = (m_sendCounter++ & 7) << 4;
        if (offset == 0)
            header |= 0x08;
        if (offset + size == message.size())
            header |= 0x04;
        QByteArray packet(1, char(header));
        packet.append(message.constData() + offset, size);
        packets.append(packet);
    }
    return packets;
}
