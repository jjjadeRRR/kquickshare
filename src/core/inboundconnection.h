// One inbound Quick Share connection: drives the protocol state machine
// (see docs/protocol.md) from TCP accept to finished file transfer.
#pragma once

#include "crypto.h"
#include "endpointinfo.h"
#include "frames.h"

#include <QHash>
#include <QObject>
#include <QPointer>

class QTcpSocket;
class QFile;
class QTimer;

struct IncomingFile {
    QString name;
    QString mimeType;
    qint64 payloadId = 0;
    qint64 size = 0;
};

struct TransferRequest {
    QString deviceName;
    QString peerAddress;
    DeviceType deviceType = DeviceType::Unknown;
    QString pinCode;
    QVector<IncomingFile> files;
    QString textTitle; // non-empty for URL/text transfers
    int textType = 0;
    qint64 totalBytes = 0;
};

class InboundConnection : public QObject {
    Q_OBJECT
public:
    explicit InboundConnection(QTcpSocket *socket, QObject *parent = nullptr);
    ~InboundConnection() override;

    QString remoteDeviceName() const { return m_deviceName; }

public slots:
    // Called by the UI in response to transferRequested.
    void acceptTransfer(const QString &destDir);
    void rejectTransfer();
    void cancelTransfer();

signals:
    void transferRequested(InboundConnection *self, const TransferRequest &request);
    void overallProgress(InboundConnection *self, qint64 received, qint64 total);
    void transferProgress(InboundConnection *self, const QString &fileName, qint64 received, qint64 total);
    void textReceived(InboundConnection *self, const QString &text, int type);
    // error is empty on success.
    void finished(InboundConnection *self, const QStringList &savedFiles, const QString &error);

private slots:
    void onReadyRead();

private:
    enum class State {
        Initial,
        ReceivedConnectionRequest,
        SentUkeyServerInit,
        ReceivedUkeyClientFinish,
        SentConnectionResponse,
        SentPairedKeyResult,
        ReceivedPairedKeyResult,
        WaitingForUserConsent,
        ReceivingFiles,
        Done,
    };

    struct ReceivingFile {
        IncomingFile meta;
        QString destPath;
        qint64 bytesTransferred = 0;
        QFile *file = nullptr;
    };

    void processFrame(const QByteArray &frameData);
    void processConnectionRequest(const QByteArray &frameData);
    void processUkey2ClientInit(const QByteArray &frameData);
    void processUkey2ClientFinish(const QByteArray &frameData);
    void processConnectionResponse(const QByteArray &frameData);
    void processSecureMessage(const QByteArray &frameData);
    void processPayloadTransfer(const frames::OfflineFrame &frame);
    void processSharingFrame(const QByteArray &data);
    void processFileChunk(const frames::OfflineFrame &frame);

    void sendRawFrame(const QByteArray &frame);
    void sendEncryptedOfflineFrame(const QByteArray &offlineFrame);
    void sendSharingFrame(const QByteArray &sharingFrame);
    void sendKeepAliveAck();
    void finishWithError(const QString &error);
    void finishTransfer(const QString &error = {});
    void sendDisconnectionAndClose();
    void cleanupFiles(bool deletePartial);

    QTcpSocket *m_socket;
    State m_state = State::Initial;
    QByteArray m_readBuffer;

    // Remote device
    QString m_deviceName;
    DeviceType m_deviceType = DeviceType::Unknown;

    // UKEY2 / crypto state
    crypto::EvpPkeyPtr m_keyPair{nullptr, &EVP_PKEY_free};
    QByteArray m_cipherCommitment;
    QByteArray m_clientInitRaw;
    QByteArray m_serverInitRaw;
    QByteArray m_decryptKey, m_encryptKey, m_recvHmacKey, m_sendHmacKey;
    qint32 m_serverSeq = 0;
    qint32 m_clientSeq = 0;
    QString m_pinCode;
    bool m_encryptionDone = false;

    // Transfer state
    QHash<qint64, QByteArray> m_bytesPayloadBuffers;
    QHash<qint64, ReceivingFile> m_files;
    qint64 m_textPayloadId = 0;
    TransferRequest m_pendingRequest;
    QStringList m_savedFiles;
    qint64 m_receivedBytes = 0;
    QString m_error;
    QTimer *m_consentTimer = nullptr;
    QTimer *m_idleTimer = nullptr;
    QTimer *m_keepAliveTimer = nullptr;
};
