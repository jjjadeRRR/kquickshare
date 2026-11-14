#include "inboundtransport.h"

#include "frames.h"

#include <QTcpSocket>

TcpInboundTransport::TcpInboundTransport(QTcpSocket *socket)
    : m_socket(socket)
{
    socket->setParent(this);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->setReadBufferSize(frames::kMaxFrameLength + 4);
    connect(socket, &QTcpSocket::readyRead, this, &InboundTransport::readyRead);
    connect(socket, &QTcpSocket::disconnected, this, &InboundTransport::disconnected);
}

QByteArray TcpInboundTransport::readAll() { return m_socket->readAll(); }
qint64 TcpInboundTransport::write(const QByteArray &data) { return m_socket->write(data); }
QString TcpInboundTransport::peerAddress() const { return m_socket->peerAddress().toString(); }
bool TcpInboundTransport::supportsLanTrust() const { return true; }
void TcpInboundTransport::disconnectFromPeer() { m_socket->flush(); m_socket->disconnectFromHost(); }
void TcpInboundTransport::abort() { m_socket->abort(); }
