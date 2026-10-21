#include "outboundconnection.h"
#include "endpointinfo.h"
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QSet>
#include <limits>

namespace {
qint64 payloadId() { return qint64(QRandomGenerator::global()->generate64() & 0x7fffffffffffffffULL); }
QByteArray wrap(int type, int field, const QByteArray &data) {
    return pb::Writer().varint(1, 1).bytes(2, pb::Writer().varint(1, type).bytes(field, data).data()).data();
}
QString pin(const QByteArray &key) {
    int hash = 0, multiplier = 1;
    for (char c : key) {
        hash = (hash + int(qint8(c)) * multiplier) % 9973;
        multiplier = multiplier * 31 % 9973;
    }
    return QString::number(qAbs(hash)).rightJustified(4, '0');
}
bool getFields(const QByteArray &data, QVector<pb::Field> &fields) { return pb::parseMessage(data, fields); }
} // namespace

OutboundConnection::OutboundConnection(QObject *parent) : QObject(parent) {
    m_socket.setReadBufferSize(5 * 1024 * 1024 + 4);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this,
            [this] { complete(tr("Телефон не ответил вовремя")); });
    connect(&m_keepAlive, &QTimer::timeout, this, [this] {
        if (m_tx && m_state != Done)
            sendSecure(frames::buildKeepAliveOfflineFrame(false));
    });
    connect(&m_socket, &QTcpSocket::readyRead, this, &OutboundConnection::read);
    connect(&m_socket, &QTcpSocket::bytesWritten, this, [this] { pump(); });
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (error != QAbstractSocket::RemoteHostClosedError)
            complete(m_socket.errorString());
    });
    connect(&m_socket, &QTcpSocket::disconnected, this, [this] {
        if (m_state == Completing)
            complete();
        else if (m_state != Done)
            complete(tr("Телефон прервал соединение"));
    });
}
bool OutboundConnection::setFiles(const QStringList &paths, QString &error) {
    if (paths.isEmpty() || paths.size() > 1000 || m_state != Idle || !m_files.isEmpty() || m_isText) {
        error = tr("Выберите от 1 до 1000 файлов");
        return false;
    }
    QMimeDatabase mime;
    QSet<qint64> ids;
    for (const QString &path : paths) {
        QFileInfo info(path);
        if (!info.isFile() || !info.isReadable() ||
            info.size() > std::numeric_limits<qint64>::max() - m_total) {
            error = tr("Файл недоступен: %1").arg(path);
            return false;
        }
        auto *file = new QFile(path, this);
        if (!file->open(QIODevice::ReadOnly)) {
            error = file->errorString();
            return false;
        }
        frames::FileMeta meta;
        meta.name = info.fileName();
        meta.mimeType = mime.mimeTypeForFile(info).name();
        meta.size = file->size();
        do {
            meta.payloadId = payloadId();
        } while (ids.contains(meta.payloadId));
        ids.insert(meta.payloadId);
        m_files.append({file, meta, 0});
        m_total += meta.size;
    }
    return true;
}
bool OutboundConnection::setText(const QString &text, QString &error) {
    if (text.isEmpty() || text.toUtf8().size() > 5 * 1024 * 1024 || m_state != Idle || !m_files.isEmpty()) {
        error = tr("Нужен текст размером до 5 МиБ");
        return false;
    }
    m_isText = true;
    m_text = text.toUtf8();
    m_total = m_text.size();
    m_textId = payloadId();
    return true;
}
void OutboundConnection::setQrSigningKey(EVP_PKEY *key) {
    if (key && EVP_PKEY_up_ref(key) == 1)
        m_qrKey.reset(key);
}
void OutboundConnection::start(const QHostAddress &address, quint16 port, const QString &localName) {
    if (m_state != Idle)
        return;
    qInfo("Quick Share outbound connecting: peer=%s port=%u", qPrintable(address.toString()), port);
    m_state = ServerInit;
    m_timeout.start(15000);
    emit statusChanged(tr("Подключение к телефону…"));
    connect(&m_socket, &QTcpSocket::connected, this, [this, localName] {
        m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_key = crypto::generateP256();
        QByteArray x, y;
        if (!m_key || !crypto::p256PublicXY(m_key.get(), x, y)) {
            complete(tr("Не удалось создать ключ шифрования"));
            return;
        }
        const QByteArray pub =
            pb::Writer()
                .varint(1, 1)
                .bytes(
                    2,
                    pb::Writer().bytes(1, crypto::toSignedBytes(x)).bytes(2, crypto::toSignedBytes(y)).data())
                .data();
        m_finish = pb::Writer().varint(1, 4).bytes(2, pb::Writer().bytes(1, pub).data()).data();
        const QByteArray commitment = pb::Writer().varint(1, 100).bytes(2, crypto::sha512(m_finish)).data();
        m_init = pb::Writer()
                     .varint(1, 2)
                     .bytes(2, pb::Writer()
                                   .varint(1, 1)
                                   .bytes(2, crypto::randomBytes(32))
                                   .bytes(3, commitment)
                                   .string(4, "AES_256_CBC-HMAC_SHA256")
                                   .data())
                     .data();
        const QByteArray request = pb::Writer()
                                       .string(1, QString::fromLatin1(crypto::randomBytes(3).toHex().left(4)))
                                       .string(2, localName)
                                       .varint(4, QRandomGenerator::global()->generate())
                                       .varint(5, 5)
                                       .bytes(6, EndpointInfo::serialize(localName, DeviceType::Laptop))
                                       .varint(8, 10000)
                                       .varint(9, 30000)
                                       .data();
        sendRaw(wrap(1, 2, request));
        sendRaw(m_init);
    });
    m_socket.connectToHost(address, port);
}
void OutboundConnection::sendRaw(const QByteArray &data) {
    QByteArray packet(4, '\0');
    quint32 n = quint32(data.size());
    packet[0] = char(n >> 24);
    packet[1] = char(n >> 16);
    packet[2] = char(n >> 8);
    packet[3] = char(n);
    packet += data;
    m_socket.write(packet);
}
void OutboundConnection::sendSecure(const QByteArray &data) {
    sendRaw(frames::encryptOfflineFrame(data, ++m_tx, m_enc, m_mac));
}
void OutboundConnection::sendSharing(const QByteArray &data) {
    for (const auto &frame : frames::buildBytesPayloadTransfer(payloadId(), data))
        sendSecure(frame);
}
void OutboundConnection::read() {
    if (m_state == Done)
        return;
    m_timeout.start(m_state == Consent ? 120000 : 30000);
    m_buffer += m_socket.readAll();
    while (m_buffer.size() >= 4) {
        const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData());
        const quint32 n = quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | quint32(p[3]);
        if (n == 0 || n > 5 * 1024 * 1024) {
            complete(tr("Недопустимый размер кадра"));
            return;
        }
        if (m_buffer.size() < n + 4)
            return;
        const QByteArray data = m_buffer.mid(4, n);
        m_buffer.remove(0, n + 4);
        process(data);
        if (m_state == Done)
            return;
    }
}
bool OutboundConnection::derive(const QByteArray &raw) {
    frames::Ukey2Message message;
    if (!frames::parseUkey2Message(raw, message) || message.type != 3)
        return false;
    QVector<pb::Field> f;
    uint64_t version = 0, cipher = 0;
    QByteArray random, pub;
    if (!getFields(message.data, f) || !pb::fieldVarint(f, 1, version) || version != 1 ||
        !pb::fieldVarint(f, 3, cipher) || cipher != 100 || !pb::fieldBytes(f, 2, random) ||
        random.size() != 32 || !pb::fieldBytes(f, 4, pub))
        return false;
    QByteArray x, y;
    if (!frames::parseUkey2ClientFinished(pb::Writer().bytes(1, pub).data(), x, y))
        return false;
    const QByteArray shared = crypto::ecdhSharedX(m_key.get(), x, y);
    if (shared.size() != 32)
        return false;
    const QByteArray dhs = crypto::sha256(shared), info = m_init + raw;
    const QByteArray auth = crypto::hkdfSha256(dhs, "UKEY2 v1 auth", info, 32);
    const QByteArray next = crypto::hkdfSha256(dhs, "UKEY2 v1 next", info, 32);
    const auto salt = crypto::sha256("D2D");
    const auto client = crypto::hkdfSha256(next, salt, "client", 32),
               server = crypto::hkdfSha256(next, salt, "server", 32);
    const auto sm = crypto::sha256("SecureMessage");
    m_enc = crypto::hkdfSha256(client, sm, "ENC:2", 32);
    m_mac = crypto::hkdfSha256(client, sm, "SIG:1", 32);
    m_dec = crypto::hkdfSha256(server, sm, "ENC:2", 32);
    m_verify = crypto::hkdfSha256(server, sm, "SIG:1", 32);
    m_auth = auth;
    emit verificationCode(pin(auth));
    return true;
}
void OutboundConnection::process(const QByteArray &data) {
    if (m_state == ServerInit) {
        if (!derive(data)) {
            complete(tr("Не удалось согласовать шифрование с телефоном"));
            return;
        }
        m_state = ConnectionResponse;
        sendRaw(m_finish);
        sendRaw(frames::buildConnectionResponseAccept());
        return;
    }
    frames::OfflineFrame frame;
    if (m_state == ConnectionResponse) {
        if (!frames::parseOfflineFrame(data, frame) ||
            frame.type != frames::OfflineFrameType::ConnectionResponse || frame.connectionResponse != 1) {
            complete(tr("Телефон отклонил подключение"));
            return;
        }
        m_state = PairedEncryption;
        sendSharing(frames::buildPairedKeyEncryptionFrame(m_qrKey ? crypto::signP256(m_qrKey.get(), m_auth)
                                                                  : QByteArray()));
        m_keepAlive.start(10000);
        return;
    }
    QByteArray plain;
    if (!frames::decryptSecureMessage(data, m_dec, m_verify, ++m_rx, plain) ||
        !frames::parseOfflineFrame(plain, frame)) {
        complete(tr("Ошибка проверки зашифрованного сообщения"));
        return;
    }
    if (frame.type == frames::OfflineFrameType::KeepAlive) {
        if (!frame.keepAliveAck)
            sendSecure(frames::buildKeepAliveOfflineFrame(true));
        return;
    }
    if (frame.type == frames::OfflineFrameType::Disconnection) {
        complete(m_state == Completing ? QString() : tr("Телефон прервал передачу"));
        return;
    }
    if (frame.type != frames::OfflineFrameType::PayloadTransfer || !frame.hasPayloadChunk ||
        frame.payloadHeader.type != 1)
        return;
    const auto &h = frame.payloadHeader;
    const auto &c = frame.payloadChunk;
    if (h.totalSize < 0 || h.totalSize > 5 * 1024 * 1024 ||
        (!m_bytes.contains(h.id) && m_bytes.size() >= 4)) {
        complete(tr("Недопустимый размер сообщения"));
        return;
    }
    auto &buffer = m_bytes[h.id];
    if (c.offset != buffer.size() || c.body.size() > h.totalSize - buffer.size()) {
        complete(tr("Повреждённое сообщение телефона"));
        return;
    }
    buffer += c.body;
    if (c.flags & 1) {
        if (buffer.size() != h.totalSize) {
            complete(tr("Неполное сообщение телефона"));
            return;
        }
        const QByteArray payload = buffer;
        m_bytes.remove(h.id);
        sharing(payload);
    }
}
void OutboundConnection::sharing(const QByteArray &data) {
    frames::SharingFrame frame;
    if (!frames::parseSharingFrame(data, frame)) {
        complete(tr("Повреждённое сообщение Quick Share"));
        return;
    }
    if (frame.type == frames::SharingFrameType::Cancel) {
        complete(tr("Отменено на телефоне"));
        return;
    }
    if (m_state == PairedEncryption && frame.type == frames::SharingFrameType::PairedKeyEncryption) {
        m_state = PairedResult;
        sendSharing(frames::buildPairedKeyResultFrame());
    } else if (m_state == PairedResult && frame.type == frames::SharingFrameType::PairedKeyResult) {
        QByteArray intro;
        if (m_isText)
            intro = pb::Writer()
                        .bytes(2, pb::Writer()
                                      .string(2, QString::fromUtf8(m_text).left(80))
                                      .varint(3, 1)
                                      .svarint64(4, m_textId)
                                      .svarint64(5, m_text.size())
                                      .svarint64(6, m_textId)
                                      .data())
                        .data();
        else
            for (const auto &file : m_files) {
                const auto &f = file.meta;
                const int type = f.mimeType.startsWith("image/")   ? 1
                                 : f.mimeType.startsWith("video/") ? 2
                                 : f.mimeType.startsWith("audio/") ? 4
                                                                   : 0;
                intro += pb::Writer()
                             .bytes(1, pb::Writer()
                                           .string(1, f.name)
                                           .varint(2, type)
                                           .svarint64(3, f.payloadId)
                                           .svarint64(4, f.size)
                                           .string(5, f.mimeType)
                                           .svarint64(6, f.payloadId)
                                           .data())
                             .data();
            }
        m_state = Consent;
        sendSharing(wrap(1, 2, intro));
        m_timeout.start(120000);
        emit statusChanged(tr("Подтвердите приём на телефоне"));
    } else if (m_state == Consent && frame.type == frames::SharingFrameType::Response) {
        if (frame.responseStatus != 1) {
            const QString error = frame.responseStatus == 3   ? tr("На телефоне недостаточно места")
                                  : frame.responseStatus == 5 ? tr("Время подтверждения истекло")
                                                              : tr("Телефон отклонил передачу");
            complete(error);
            return;
        }
        m_state = Sending;
        emit statusChanged(tr("Отправка…"));
        m_timeout.start(30000);
        pump();
    }
}
void OutboundConnection::pump() {
    if ((m_state != Sending && m_state != Completing) || m_pumping)
        return;
    QScopedValueRollback<bool> guard(m_pumping, true);
    if (m_state == Sending && m_isText) {
        for (const auto &frame : frames::buildBytesPayloadTransfer(m_textId, m_text))
            sendSecure(frame);
        m_sent = m_total;
        emit progress(m_sent, m_total);
        m_state = Completing;
    }
    while (m_state == Sending && m_socket.bytesToWrite() < 512 * 1024) {
        if (m_current == m_files.size()) {
            m_state = Completing;
            break;
        }
        auto &file = m_files[m_current];
        const auto &meta = file.meta;
        const QByteArray body = file.handle->read(qMin<qint64>(65536, meta.size - file.offset));
        if (body.isEmpty() && file.offset < meta.size) {
            complete(tr("Ошибка чтения файла: %1").arg(meta.name));
            return;
        }
        frames::PayloadHeader header{meta.payloadId, 2, meta.size};
        frames::PayloadChunk chunk{0, file.offset, body, true};
        if (!body.isEmpty()) {
            sendSecure(frames::buildPayloadTransfer(header, chunk));
            file.offset += body.size();
            m_sent += body.size();
            emit progress(m_sent, m_total);
        }
        if (file.offset == meta.size) {
            chunk = {1, file.offset, {}, false};
            sendSecure(frames::buildPayloadTransfer(header, chunk));
            file.handle->close();
            ++m_current;
        }
    }
    if (m_state == Completing && !m_closing && m_socket.bytesToWrite() == 0) {
        m_closing = true;
        emit statusChanged(tr("Завершение передачи…"));
        // Queued bytes reaching the kernel does not mean that Android has
        // committed the attachment. The receiver owns completion; continue
        // reading its result (including Cancel) until it disconnects.
        m_timeout.start(30000);
    }
}
void OutboundConnection::cancel() {
    if (m_state >= PairedEncryption && m_state != Done)
        sendSharing(wrap(6, 7, {}));
    complete(tr("Передача отменена"));
}
void OutboundConnection::complete(const QString &error) {
    if (!error.isEmpty())
        qWarning("Quick Share outbound error in state %d: %s", int(m_state), qPrintable(error));
    if (m_state == Done)
        return;
    m_state = Done;
    m_timeout.stop();
    m_keepAlive.stop();
    for (auto &file : m_files)
        file.handle->close();
    m_socket.abort();
    emit finished(error);
}
