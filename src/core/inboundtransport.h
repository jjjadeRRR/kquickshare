// Byte stream presented to the incoming Nearby protocol state machine.
#pragma once

#include <QByteArray>
#include <QObject>

class QTcpSocket;

class InboundTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual QByteArray readAll() = 0;
    virtual qint64 write(const QByteArray &data) = 0;
    virtual QString peerAddress() const = 0;
    virtual bool supportsLanTrust() const = 0;
    // Graceful disconnect must deliver queued data before emitting disconnected.
    virtual void disconnectFromPeer() = 0;
    virtual void abort() = 0;

signals:
    void readyRead();
    void disconnected();
};

class TcpInboundTransport final : public InboundTransport
{
public:
    explicit TcpInboundTransport(QTcpSocket *socket);
    QByteArray readAll() override;
    qint64 write(const QByteArray &data) override;
    QString peerAddress() const override;
    bool supportsLanTrust() const override;
    void disconnectFromPeer() override;
    void abort() override;

private:
    QTcpSocket *m_socket;
};
