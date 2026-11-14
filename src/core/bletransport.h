#pragma once

#include "inboundtransport.h"
#include "weave.h"

#include <QQueue>
#include <QTimer>

// One Weave session. GATT owns the connection to BlueZ and feeds packets here.
class BleTransport final : public InboundTransport
{
    Q_OBJECT
public:
    explicit BleTransport(const QString &peer, quint16 maxPacketSize = 20, QObject *parent = nullptr);
    bool receivePacket(const QByteArray &packet);
    QByteArray readAll() override;
    qint64 write(const QByteArray &data) override;
    QString peerAddress() const override;
    bool supportsLanTrust() const override;
    void disconnectFromPeer() override;
    void abort() override;

signals:
    void established();
    void packetReady(const QByteArray &packet);

private:
    void sendNext();
    void closeNow();
    Weave m_weave;
    QString m_peer;
    QByteArray m_input;
    QQueue<QByteArray> m_output;
    QTimer m_outputTimer;
    qsizetype m_queuedBytes = 0;
    bool m_established = false;
    bool m_closing = false;
    bool m_closed = false;
};
