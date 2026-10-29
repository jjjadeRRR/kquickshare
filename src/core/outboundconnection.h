#pragma once
#include "crypto.h"
#include "frames.h"
#include <QFile>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

class OutboundConnection : public QObject
{
    Q_OBJECT
public:
    explicit OutboundConnection(QObject *parent = nullptr);
    bool setFiles(const QStringList &paths, QString &error);
    bool setText(const QString &text, QString &error);
    void start(const QHostAddress &address, quint16 port, const QString &localName);
    void cancel();
    void setQrSigningKey(EVP_PKEY *key);
signals:
    void statusChanged(const QString &status);
    void verificationCode(const QString &pin);
    void progress(qint64 sent, qint64 total);
    void finished(const QString &error);

private:
    enum State { Idle, ServerInit, ConnectionResponse, PairedEncryption, PairedResult, Consent, Sending, Completing, Done };
    struct File
    {
        QFile *handle = nullptr;
        frames::FileMeta meta;
        qint64 offset = 0;
    };
    void read();
    void process(const QByteArray &data);
    void sharing(const QByteArray &data);
    void sendRaw(const QByteArray &data);
    void sendSecure(const QByteArray &data);
    void sendSharing(const QByteArray &data);
    void pump();
    void complete(const QString &error = {});
    bool derive(const QByteArray &serverMessage);
    QTcpSocket m_socket;
    QTimer m_timeout, m_keepAlive;
    State m_state = Idle;
    QByteArray m_buffer, m_init, m_finish;
    crypto::EvpPkeyPtr m_key{nullptr, &EVP_PKEY_free};
    crypto::EvpPkeyPtr m_qrKey{nullptr, &EVP_PKEY_free};
    QByteArray m_auth;
    QByteArray m_encryptKey, m_sendHmacKey, m_decryptKey, m_recvHmacKey;
    qint32 m_tx = 0, m_rx = 0;
    frames::BytesPayloadAssembler m_bytes{4};
    QVector<File> m_files;
    QByteArray m_text;
    bool m_isText = false, m_pumping = false, m_closing = false;
    qint64 m_textId = 0, m_total = 0, m_sent = 0;
    int m_current = 0;
};
