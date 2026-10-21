// Builders and parsers for the Quick Share protocol messages.
// Field numbers follow Google's protos (see docs/protocol.md).
#pragma once

#include "endpointinfo.h"
#include "protobuf.h"

#include <QVector>

namespace frames {

// ---------------- OfflineFrame (location.nearby.connections) ----------------

enum class OfflineFrameType {
    Unknown = 0,
    ConnectionRequest = 1,
    ConnectionResponse = 2,
    PayloadTransfer = 3,
    BandwidthUpgradeNegotiation = 4,
    KeepAlive = 5,
    Disconnection = 6,
    PairedKeyEncryption = 7,
    AuthenticationMessage = 8,
    AuthenticationResult = 9,
    AutoResume = 10,
    AutoReconnect = 11,
    BandwidthUpgradeRetry = 12,
};

struct PayloadHeader {
    qint64 id = 0;
    int type = 0; // 1 = BYTES, 2 = FILE
    qint64 totalSize = 0;
};

struct PayloadChunk {
    int flags = 0; // bit 0 = LAST_CHUNK
    qint64 offset = 0;
    QByteArray body;
    bool hasBody = false;
};

struct OfflineFrame {
    OfflineFrameType type = OfflineFrameType::Unknown;
    QByteArray endpointInfo;   // ConnectionRequest
    PayloadHeader payloadHeader;
    PayloadChunk payloadChunk;
    bool hasPayloadChunk = false;
    bool keepAliveAck = false;
    int connectionResponse = 0;
};

bool parseOfflineFrame(const QByteArray &data, OfflineFrame &out);

QByteArray buildPayloadTransfer(const PayloadHeader &header, const PayloadChunk &chunk);
QByteArray buildConnectionResponseAccept();
QByteArray buildKeepAliveOfflineFrame(bool ack);
QByteArray buildDisconnectionOfflineFrame();
// Two-frames-per-message payload, mirroring Android: data frame + empty LAST_CHUNK frame.
QVector<QByteArray> buildBytesPayloadTransfer(qint64 id, const QByteArray &body);

// ---------------- UKEY2 (securegcm) ----------------

enum class Ukey2Type {
    Alert = 1,
    ClientInit = 2,
    ServerInit = 3,
    ClientFinish = 4,
};

struct Ukey2Message {
    int type = 0;
    QByteArray data;
};

struct Ukey2ClientInit {
    int version = 0;
    QByteArray random;
    QByteArray p256Commitment; // empty if no P256_SHA512 commitment found
    QString nextProtocol;
};

bool parseUkey2Message(const QByteArray &data, Ukey2Message &out);
bool parseUkey2ClientInit(const QByteArray &data, Ukey2ClientInit &out);
// Extracts the peer's public key coordinates from Ukey2ClientFinished.message_data.
bool parseUkey2ClientFinished(const QByteArray &data, QByteArray &peerX, QByteArray &peerY);
// Full serialized Ukey2Message of type SERVER_INIT.
QByteArray buildUkey2ServerInit(const QByteArray &signedX, const QByteArray &signedY);
QByteArray buildUkey2Alert(int alertType);

// ---------------- SecureMessage (securemessage/securegcm) ----------------

QByteArray encryptOfflineFrame(const QByteArray &serializedOfflineFrame, qint32 sequence,
                               const QByteArray &encryptKey, const QByteArray &sendHmacKey);
// Verifies HMAC, decrypts, checks the sequence number, returns the inner OfflineFrame bytes.
bool decryptSecureMessage(const QByteArray &data, const QByteArray &decryptKey, const QByteArray &recvHmacKey,
                          qint32 expectedSequence, QByteArray &offlineFrameOut);

// ---------------- Sharing frames (sharing.nearby, wire_format.proto) ----------------

enum class SharingFrameType {
    Unknown = 0,
    Introduction = 1,
    Response = 2,
    PairedKeyEncryption = 3,
    PairedKeyResult = 4,
    CertificateInfo = 5,
    Cancel = 6,
    ProgressUpdate = 7,
};

struct FileMeta {
    QString name;
    QString mimeType;
    qint64 payloadId = 0;
    qint64 size = 0;
};

struct TextMeta {
    QString title;
    int type = 0; // 1 = TEXT, 2 = URL, 3 = ADDRESS, 4 = PHONE_NUMBER
    qint64 payloadId = 0;
    qint64 size = 0;
};

struct SharingFrame {
    SharingFrameType type = SharingFrameType::Unknown;
    QVector<FileMeta> files;          // Introduction
    QVector<TextMeta> texts;          // Introduction
    bool hasPairedKeyEncryption = false;
    int responseStatus = 0;
};

bool parseSharingFrame(const QByteArray &data, SharingFrame &out);

QByteArray buildPairedKeyEncryptionFrame(const QByteArray &qrSignature = {});
QByteArray buildPairedKeyResultFrame(); // status = UNABLE
// status: 1 = ACCEPT, 2 = REJECT, 3 = NOT_ENOUGH_SPACE, 4 = UNSUPPORTED_ATTACHMENT_TYPE, 5 = TIMED_OUT
QByteArray buildSharingResponseFrame(int status);

} // namespace frames
