#include "frames.h"

#include "crypto.h"

#include <QtEndian>

namespace frames {

namespace {
constexpr int kOsTypeLinux = 100;

QByteArray gcmMetadata()
{
    // GcmMetadata { type = DEVICE_TO_DEVICE_MESSAGE(13), version = 1 }
    return pb::Writer().varint(1, 13).varint(2, 1).data();
}

QByteArray offlineV1(pb::Writer &v1)
{
    // OfflineFrame { version = V1(1), v1 = <v1> }
    return pb::Writer().varint(1, 1).bytes(2, v1.data()).data();
}
} // namespace

// ---------------- Stream framing ----------------

QByteArray lengthPrefixed(const QByteArray &frame)
{
    QByteArray out(4, '\0');
    qToBigEndian(quint32(frame.size()), out.data());
    out.append(frame);
    return out;
}

FrameRead takeFrame(QByteArray &buffer, QByteArray &frame)
{
    if (buffer.size() < 4)
        return FrameRead::Incomplete;
    const quint32 length = qFromBigEndian<quint32>(buffer.constData());
    if (length == 0 || length > kMaxFrameLength)
        return FrameRead::Invalid;
    if (buffer.size() < qsizetype(4 + length))
        return FrameRead::Incomplete;
    frame = buffer.mid(4, qsizetype(length));
    buffer.remove(0, qsizetype(4 + length));
    return FrameRead::Frame;
}

// ---------------- OfflineFrame ----------------

bool parseOfflineFrame(const QByteArray &data, OfflineFrame &out)
{
    QVector<pb::Field> root;
    if (!pb::parseMessage(data, root))
        return false;
    QByteArray v1raw;
    if (!pb::fieldBytes(root, 2, v1raw))
        return false;
    QVector<pb::Field> v1;
    if (!pb::parseMessage(v1raw, v1))
        return false;
    uint64_t type = 0;
    if (!pb::fieldVarint(v1, 1, type))
        return false;
    out.type = OfflineFrameType(type);

    switch (out.type) {
    case OfflineFrameType::ConnectionRequest: {
        QByteArray req;
        if (!pb::fieldBytes(v1, 2, req))
            return false;
        QVector<pb::Field> f;
        if (!pb::parseMessage(req, f))
            return false;
        if (!pb::fieldBytes(f, 6, out.endpointInfo))
            return false;
        return true;
    }
    case OfflineFrameType::ConnectionResponse: {
        QByteArray raw; QVector<pb::Field> fields; uint64_t response = 0, status = 1;
        if (!pb::fieldBytes(v1, 3, raw) || !pb::parseMessage(raw, fields)) return false;
        if (!pb::fieldVarint(fields, 3, response)) {
            if (pb::fieldVarint(fields, 1, status) && status == 0) response = 1;
        }
        out.connectionResponse = int(response); return true;
    }
    case OfflineFrameType::PayloadTransfer: {
        QByteArray pt;
        if (!pb::fieldBytes(v1, 4, pt))
            return false;
        QVector<pb::Field> f;
        if (!pb::parseMessage(pt, f))
            return false;
        QByteArray headerRaw, chunkRaw;
        if (!pb::fieldBytes(f, 2, headerRaw))
            return false;
        QVector<pb::Field> h;
        if (!pb::parseMessage(headerRaw, h))
            return false;
        uint64_t id = 0, ptype = 0, total = 0;
        if (!pb::fieldVarint(h, 1, id) || !pb::fieldVarint(h, 2, ptype))
            return false;
        pb::fieldVarint(h, 3, total);
        out.payloadHeader.id = qint64(id);
        out.payloadHeader.type = int(ptype);
        out.payloadHeader.totalSize = qint64(total);
        if (pb::fieldBytes(f, 3, chunkRaw)) {
            QVector<pb::Field> c;
            if (!pb::parseMessage(chunkRaw, c))
                return false;
            uint64_t flags = 0, offset = 0;
            if (!pb::fieldVarint(c, 1, flags) || !pb::fieldVarint(c, 2, offset))
                return false;
            out.payloadChunk.flags = int(flags);
            out.payloadChunk.offset = qint64(offset);
            out.payloadChunk.hasBody = pb::fieldBytes(c, 3, out.payloadChunk.body);
            out.hasPayloadChunk = true;
        }
        return true;
    }
    case OfflineFrameType::KeepAlive: {
        QByteArray ka;
        if (pb::fieldBytes(v1, 6, ka)) {
            QVector<pb::Field> f;
            if (pb::parseMessage(ka, f)) {
                uint64_t ack = 0;
                if (pb::fieldVarint(f, 1, ack))
                    out.keepAliveAck = ack != 0;
            }
        }
        return true;
    }
    default:
        return true;
    }
}

QByteArray buildConnectionResponseAccept()
{
    pb::Writer osInfo;
    osInfo.varint(1, kOsTypeLinux);
    pb::Writer resp;
    resp.varint(1, 0)          // legacy status
        .varint(3, 1)          // response = ACCEPT
        .bytes(4, osInfo.data());
    pb::Writer v1;
    v1.varint(1, int(OfflineFrameType::ConnectionResponse))
        .bytes(3, resp.data());
    return offlineV1(v1);
}

QByteArray buildKeepAliveOfflineFrame(bool ack)
{
    pb::Writer ka;
    ka.varint(1, ack ? 1 : 0);
    pb::Writer v1;
    v1.varint(1, int(OfflineFrameType::KeepAlive))
        .bytes(6, ka.data());
    return offlineV1(v1);
}

QByteArray buildDisconnectionOfflineFrame()
{
    pb::Writer disc; // empty DisconnectionFrame
    pb::Writer v1;
    v1.varint(1, int(OfflineFrameType::Disconnection))
        .bytes(7, disc.data());
    return offlineV1(v1);
}

QByteArray buildPayloadTransfer(const PayloadHeader &header, const PayloadChunk &chunk)
{
    pb::Writer h;
    h.svarint64(1, header.id)
        .varint(2, uint64_t(header.type))
        .svarint64(3, header.totalSize)
        .varint(4, 0); // is_sensitive = false
    pb::Writer c;
    c.varint(1, uint64_t(chunk.flags))
        .svarint64(2, chunk.offset);
    if (chunk.hasBody)
        c.bytes(3, chunk.body);
    pb::Writer pt;
    pt.varint(1, 1) // packet_type = DATA
        .bytes(2, h.data())
        .bytes(3, c.data());
    pb::Writer v1;
    v1.varint(1, int(OfflineFrameType::PayloadTransfer))
        .bytes(4, pt.data());
    return offlineV1(v1);
}

QVector<QByteArray> buildBytesPayloadTransfer(qint64 id, const QByteArray &body)
{
    PayloadHeader header;
    header.id = id;
    header.type = 1; // BYTES
    header.totalSize = body.size();
    PayloadChunk chunk;
    chunk.flags = 0;
    chunk.offset = 0;
    chunk.body = body;
    chunk.hasBody = true;
    QVector<QByteArray> out;
    out.append(buildPayloadTransfer(header, chunk));
    chunk.flags = 1; // LAST_CHUNK
    chunk.offset = body.size();
    chunk.body.clear();
    chunk.hasBody = false;
    out.append(buildPayloadTransfer(header, chunk));
    return out;
}

BytesPayloadAssembler::Result BytesPayloadAssembler::add(const PayloadHeader &header, const PayloadChunk &chunk, QByteArray &payload)
{
    if (header.totalSize < 0 || header.totalSize > kMaxFrameLength || (!m_buffers.contains(header.id) && m_buffers.size() >= m_maxPayloads))
        return Result::TooLarge;
    QByteArray &buffer = m_buffers[header.id];
    if (chunk.offset != buffer.size() || chunk.body.size() > header.totalSize - buffer.size()) {
        m_buffers.remove(header.id);
        return Result::Invalid;
    }
    buffer.append(chunk.body);
    if ((chunk.flags & 1) == 0) // LAST_CHUNK not yet reached
        return Result::Pending;
    payload = m_buffers.take(header.id);
    return payload.size() == header.totalSize ? Result::Complete : Result::Invalid;
}

// ---------------- UKEY2 ----------------

bool parseUkey2Message(const QByteArray &data, Ukey2Message &out)
{
    QVector<pb::Field> f;
    if (!pb::parseMessage(data, f))
        return false;
    uint64_t type = 0;
    if (!pb::fieldVarint(f, 1, type) || !pb::fieldBytes(f, 2, out.data))
        return false;
    out.type = int(type);
    return true;
}

bool parseUkey2ClientInit(const QByteArray &data, Ukey2ClientInit &out)
{
    QVector<pb::Field> f;
    if (!pb::parseMessage(data, f))
        return false;
    uint64_t version = 0;
    if (!pb::fieldVarint(f, 1, version))
        return false;
    out.version = int(version);
    if (!pb::fieldBytes(f, 2, out.random))
        return false;
    for (const QByteArray &commitRaw : pb::allBytesFields(f, 3)) {
        QVector<pb::Field> c;
        if (!pb::parseMessage(commitRaw, c))
            continue;
        uint64_t cipher = 0;
        QByteArray commitment;
        if (pb::fieldVarint(c, 1, cipher) && pb::fieldBytes(c, 2, commitment)
            && cipher == 100) { // P256_SHA512
            out.p256Commitment = commitment;
        }
    }
    if (!pb::fieldString(f, 4, out.nextProtocol))
        return false;
    return true;
}

bool parseUkey2ClientFinished(const QByteArray &data, QByteArray &peerX, QByteArray &peerY)
{
    QVector<pb::Field> f;
    if (!pb::parseMessage(data, f))
        return false;
    QByteArray pubKeyRaw;
    if (!pb::fieldBytes(f, 1, pubKeyRaw))
        return false;
    QVector<pb::Field> pk;
    if (!pb::parseMessage(pubKeyRaw, pk))
        return false;
    QByteArray ecRaw;
    if (!pb::fieldBytes(pk, 2, ecRaw))
        return false;
    QVector<pb::Field> ec;
    if (!pb::parseMessage(ecRaw, ec))
        return false;
    return pb::fieldBytes(ec, 1, peerX) && pb::fieldBytes(ec, 2, peerY);
}

QByteArray buildUkey2ServerInit(const QByteArray &signedX, const QByteArray &signedY)
{
    pb::Writer ec;
    ec.bytes(1, signedX).bytes(2, signedY);
    pb::Writer pubKey;
    pubKey.varint(1, 1) // EC_P256
        .bytes(2, ec.data());
    pb::Writer serverInit;
    serverInit.varint(1, 1) // version
        .bytes(2, crypto::randomBytes(32))
        .varint(3, 100) // P256_SHA512
        .bytes(4, pubKey.data());
    pb::Writer msg;
    msg.varint(1, int(Ukey2Type::ServerInit))
        .bytes(2, serverInit.data());
    return msg.data();
}

QByteArray buildUkey2Alert(int alertType)
{
    pb::Writer alert;
    alert.varint(1, uint64_t(alertType));
    pb::Writer msg;
    msg.varint(1, int(Ukey2Type::Alert))
        .bytes(2, alert.data());
    return msg.data();
}

bool deriveUkey2Keys(EVP_PKEY *ourKey,
                     const QByteArray &peerX,
                     const QByteArray &peerY,
                     const QByteArray &clientInit,
                     const QByteArray &serverInit,
                     Ukey2Keys &out)
{
    // Key schedule: docs/protocol.md
    const QByteArray shared = crypto::ecdhSharedX(ourKey, peerX, peerY);
    if (shared.size() != 32)
        return false;
    const QByteArray dhs = crypto::sha256(shared);
    const QByteArray info = clientInit + serverInit;
    out.authString = crypto::hkdfSha256(dhs, "UKEY2 v1 auth", info, 32);
    const QByteArray nextSecret = crypto::hkdfSha256(dhs, "UKEY2 v1 next", info, 32);

    const QByteArray d2dSalt = crypto::sha256("D2D");
    const QByteArray d2dClient = crypto::hkdfSha256(nextSecret, d2dSalt, "client", 32);
    const QByteArray d2dServer = crypto::hkdfSha256(nextSecret, d2dSalt, "server", 32);
    const QByteArray secureMessageSalt = crypto::sha256("SecureMessage");
    out.clientEncrypt = crypto::hkdfSha256(d2dClient, secureMessageSalt, "ENC:2", 32);
    out.clientHmac = crypto::hkdfSha256(d2dClient, secureMessageSalt, "SIG:1", 32);
    out.serverEncrypt = crypto::hkdfSha256(d2dServer, secureMessageSalt, "ENC:2", 32);
    out.serverHmac = crypto::hkdfSha256(d2dServer, secureMessageSalt, "SIG:1", 32);
    return true;
}

QString pinCode(const QByteArray &authString)
{
    int hash = 0;
    int multiplier = 1;
    for (const char c : authString) {
        const int byte = int(qint8(c)); // signed interpretation, per spec
        hash = (hash + byte * multiplier) % 9973;
        multiplier = (multiplier * 31) % 9973;
    }
    return QString::number(qAbs(hash)).rightJustified(4, QLatin1Char('0'));
}

// ---------------- SecureMessage ----------------

QByteArray encryptOfflineFrame(const QByteArray &serializedOfflineFrame, qint32 sequence,
                               const QByteArray &encryptKey, const QByteArray &sendHmacKey)
{
    // DeviceToDeviceMessage { message = 1, sequence_number = 2 }
    pb::Writer d2d;
    d2d.bytes(1, serializedOfflineFrame).varint(2, uint64_t(quint32(sequence)));

    const QByteArray iv = crypto::randomBytes(16);
    const QByteArray encrypted = crypto::aes256CbcEncrypt(encryptKey, iv, d2d.data());

    pb::Writer header;
    header.varint(1, 1) // HMAC_SHA256
        .varint(2, 2)   // AES_256_CBC
        .bytes(5, iv)
        .bytes(6, gcmMetadata());
    pb::Writer hab;
    hab.bytes(1, header.data()).bytes(2, encrypted);

    pb::Writer smsg;
    smsg.bytes(1, hab.data());
    smsg.bytes(2, crypto::hmacSha256(sendHmacKey, hab.data()));
    return smsg.data();
}

bool decryptSecureMessage(const QByteArray &data, const QByteArray &decryptKey, const QByteArray &recvHmacKey,
                          qint32 expectedSequence, QByteArray &offlineFrameOut)
{
    QVector<pb::Field> f;
    if (!pb::parseMessage(data, f))
        return false;
    QByteArray habRaw, signature;
    if (!pb::fieldBytes(f, 1, habRaw) || !pb::fieldBytes(f, 2, signature))
        return false;
    if (signature.size() != 32 || CRYPTO_memcmp(crypto::hmacSha256(recvHmacKey, habRaw).constData(), signature.constData(), 32) != 0)
        return false;

    QVector<pb::Field> hab;
    if (!pb::parseMessage(habRaw, hab))
        return false;
    QByteArray headerRaw, body;
    if (!pb::fieldBytes(hab, 1, headerRaw) || !pb::fieldBytes(hab, 2, body))
        return false;
    QVector<pb::Field> header;
    if (!pb::parseMessage(headerRaw, header))
        return false;
    uint64_t sigScheme = 0, encScheme = 0;
    if (!pb::fieldVarint(header, 1, sigScheme) || sigScheme != 1
        || !pb::fieldVarint(header, 2, encScheme) || encScheme != 2) return false;
    QByteArray iv;
    if (!pb::fieldBytes(header, 5, iv) || iv.size() != 16)
        return false;

    const QByteArray decrypted = crypto::aes256CbcDecrypt(decryptKey, iv, body);
    if (decrypted.isEmpty())
        return false;

    QVector<pb::Field> d2d;
    if (!pb::parseMessage(decrypted, d2d))
        return false;
    uint64_t seq = 0;
    if (!pb::fieldBytes(d2d, 1, offlineFrameOut) || !pb::fieldVarint(d2d, 2, seq))
        return false;
    return qint32(seq) == expectedSequence;
}

// ---------------- Sharing frames ----------------

bool parseSharingFrame(const QByteArray &data, SharingFrame &out)
{
    QVector<pb::Field> root;
    if (!pb::parseMessage(data, root))
        return false;
    QByteArray v1raw;
    if (!pb::fieldBytes(root, 2, v1raw))
        return false;
    QVector<pb::Field> v1;
    if (!pb::parseMessage(v1raw, v1))
        return false;
    uint64_t type = 0;
    if (!pb::fieldVarint(v1, 1, type))
        return false;
    out.type = SharingFrameType(type);

    if (out.type == SharingFrameType::Response) {
        QByteArray raw; QVector<pb::Field> fields; uint64_t status = 0;
        if (!pb::fieldBytes(v1, 3, raw) || !pb::parseMessage(raw, fields) || !pb::fieldVarint(fields, 1, status)) return false;
        out.responseStatus = int(status);
    } else if (out.type == SharingFrameType::Introduction) {
        QByteArray intro;
        if (!pb::fieldBytes(v1, 2, intro))
            return false;
        QVector<pb::Field> f;
        if (!pb::parseMessage(intro, f))
            return false;
        for (const QByteArray &fileRaw : pb::allBytesFields(f, 1)) {
            QVector<pb::Field> ff;
            if (!pb::parseMessage(fileRaw, ff))
                return false;
            FileMeta meta;
            uint64_t payloadId = 0, size = 0;
            if (!pb::fieldString(ff, 1, meta.name))
                return false;
            if (!pb::fieldVarint(ff, 3, payloadId) || !pb::fieldVarint(ff, 4, size))
                return false;
            meta.payloadId = qint64(payloadId);
            meta.size = qint64(size);
            pb::fieldString(ff, 5, meta.mimeType);
            out.files.append(meta);
        }
        for (const QByteArray &textRaw : pb::allBytesFields(f, 2)) {
            QVector<pb::Field> tf;
            if (!pb::parseMessage(textRaw, tf))
                return false;
            TextMeta meta;
            uint64_t payloadId = 0, size = 0, textType = 0;
            pb::fieldString(tf, 2, meta.title);
            pb::fieldVarint(tf, 3, textType);
            if (!pb::fieldVarint(tf, 4, payloadId) || !pb::fieldVarint(tf, 5, size))
                return false;
            meta.type = int(textType);
            meta.payloadId = qint64(payloadId);
            meta.size = qint64(size);
            out.texts.append(meta);
        }
    } else if (out.type == SharingFrameType::PairedKeyEncryption) {
        QByteArray pke;
        if (!pb::fieldBytes(v1, 4, pke))
            return false;
        out.hasPairedKeyEncryption = true;
    }
    return true;
}

static QByteArray sharingV1(pb::Writer &v1)
{
    return pb::Writer().varint(1, 1).bytes(2, v1.data()).data();
}

QByteArray buildPairedKeyEncryptionFrame(const QByteArray &qrSignature)
{
    pb::Writer pke;
    pke.bytes(1, crypto::randomBytes(72)) // signed_data
        .bytes(2, crypto::randomBytes(6)); // secret_id_hash
    if (!qrSignature.isEmpty()) pke.bytes(4, qrSignature);
    pb::Writer v1;
    v1.varint(1, int(SharingFrameType::PairedKeyEncryption))
        .bytes(4, pke.data());
    return sharingV1(v1);
}

QByteArray buildPairedKeyResultFrame()
{
    pb::Writer pkr;
    pkr.varint(1, 3); // status = UNABLE
    pb::Writer v1;
    v1.varint(1, int(SharingFrameType::PairedKeyResult))
        .bytes(5, pkr.data());
    return sharingV1(v1);
}

QByteArray buildSharingResponseFrame(int status)
{
    pb::Writer resp;
    resp.varint(1, uint64_t(status));
    pb::Writer v1;
    v1.varint(1, int(SharingFrameType::Response))
        .bytes(3, resp.data());
    return sharingV1(v1);
}

QByteArray buildCancelFrame()
{
    pb::Writer v1; // CANCEL carries no body
    v1.varint(1, int(SharingFrameType::Cancel));
    return sharingV1(v1);
}

} // namespace frames
