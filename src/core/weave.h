// Framing for Nearby Connections' BLE Weave socket. This class has no Bluetooth
// dependencies; the GATT layer supplies packets and sends the returned packets.
#pragma once

#include <QByteArray>
#include <QList>

class Weave
{
public:
    enum class Result { Pending, Connected, Data, Disconnected, Invalid };

    // The default fits the minimum ATT MTU. Pass a larger value only when the
    // GATT backend knows the actual maximum notification size for this peer.
    explicit Weave(quint16 maxPacketSize = 20);

    // Processes one GATT write. On Connected, reply is the confirmation packet;
    // on Data, data is a byte stream containing length-prefixed Nearby frames.
    // The session owner must discard this instance on Disconnected or Invalid.
    Result receive(const QByteArray &packet, QByteArray &reply, QByteArray &data);
    // Encodes one complete length-prefixed Nearby frame for GATT notification.
    QList<QByteArray> encode(const QByteArray &frame);

private:
    bool m_connected = false;
    bool m_receiving = false;
    quint16 m_maxPacketSize;
    quint16 m_packetSize = 0;
    quint8 m_sendCounter = 1; // confirmation uses counter zero
    QByteArray m_message;
};
