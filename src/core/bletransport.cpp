#include "bletransport.h"

#include "frames.h"

namespace
{
constexpr qsizetype maxQueuedBytes = 2 * frames::kMaxFrameLength;
}

BleTransport::BleTransport(const QString &peer, quint16 maxPacketSize, QObject *parent)
    : InboundTransport(parent)
    , m_weave(maxPacketSize)
    , m_peer(peer)
{
    m_outputTimer.setSingleShot(true);
    connect(&m_outputTimer, &QTimer::timeout, this, &BleTransport::sendNext);
}

bool BleTransport::receivePacket(const QByteArray &packet)
{
    if (m_closed || m_closing)
        return false;
    QByteArray reply, data;
    switch (m_weave.receive(packet, reply, data)) {
    case Weave::Result::Connected:
        qInfo("Bluetooth receiver: Weave connection established");
        m_established = true;
        emit packetReady(reply);
        emit established();
        return true;
    case Weave::Result::Pending:
        return true;
    case Weave::Result::Data:
        if (data.size() > frames::kMaxFrameLength + 4 - m_input.size()) {
            closeNow();
            return false;
        }
        m_input.append(data);
        emit readyRead();
        return true;
    case Weave::Result::Disconnected:
        qInfo("Bluetooth receiver: Weave peer disconnected");
        closeNow();
        return false;
    case Weave::Result::Invalid:
        qWarning("Bluetooth receiver: invalid Weave packet (size=%lld)", qlonglong(packet.size()));
        closeNow();
        return false;
    }
    return false;
}

QByteArray BleTransport::readAll()
{
    QByteArray result;
    result.swap(m_input);
    return result;
}

qint64 BleTransport::write(const QByteArray &data)
{
    if (!m_established || m_closing || m_closed || data.size() > frames::kMaxFrameLength + 4)
        return -1;
    const auto packets = m_weave.encode(data);
    if (packets.isEmpty())
        return -1;
    qsizetype packetBytes = 0;
    for (const auto &packet : packets)
        packetBytes += packet.size();
    if (packetBytes > maxQueuedBytes - m_queuedBytes)
        return -1;
    for (const auto &packet : packets) {
        m_queuedBytes += packet.size();
        m_output.enqueue(packet);
    }
    if (!m_outputTimer.isActive())
        m_outputTimer.start(0);
    return data.size();
}

QString BleTransport::peerAddress() const { return m_peer; }
bool BleTransport::supportsLanTrust() const { return false; }

void BleTransport::disconnectFromPeer()
{
    if (m_closed)
        return;
    m_closing = true;
    if (m_output.isEmpty())
        closeNow();
}

void BleTransport::abort() { closeNow(); }

void BleTransport::sendNext()
{
    if (m_output.isEmpty()) {
        if (m_closing)
            closeNow();
        return;
    }
    const QByteArray packet = m_output.dequeue();
    m_queuedBytes -= packet.size();
    emit packetReady(packet);
    // Yield to BlueZ between notifications; do not close while messages remain.
    if (!m_closed)
        m_outputTimer.start(2);
}

void BleTransport::closeNow()
{
    if (m_closed)
        return;
    m_closed = true;
    m_outputTimer.stop();
    m_output.clear();
    m_queuedBytes = 0;
    emit disconnected();
}
