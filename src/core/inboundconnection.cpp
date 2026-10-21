#include "inboundconnection.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryFile>
#include <QStorageInfo>
#include <QSet>
#include <limits>
#include <QTcpSocket>
#include <QTimer>

namespace {
constexpr qsizetype kMaxFrameLength = 5 * 1024 * 1024;
constexpr int kConsentTimeoutMs = 120'000;

QString pinCodeFromAuthKey(const QByteArray &key)
{
    int hash = 0;
    int multiplier = 1;
    for (const char c : key) {
        const int byte = int(int8_t(c)); // signed interpretation, per spec
        hash = (hash + byte * multiplier) % 9973;
        multiplier = (multiplier * 31) % 9973;
    }
    return QString::number(std::abs(hash)).rightJustified(4, QLatin1Char('0'));
}

QString uniqueDestination(const QString &dir, const QString &fileName)
{
    QString cleanName = QFileInfo(fileName).fileName(); // strip any path components
    if (cleanName.isEmpty() || cleanName == "." || cleanName == "..")
        cleanName = QStringLiteral("unnamed");
    QString dest = QDir(dir).filePath(cleanName);
    if (!QFileInfo::exists(dest))
        return dest;
    const QString base = QFileInfo(cleanName).completeBaseName();
    const QString suffix = QFileInfo(cleanName).suffix();
    for (int i = 1;; ++i) {
        QString candidate = base + QStringLiteral(" (%1)").arg(i);
        if (!suffix.isEmpty())
            candidate += QLatin1Char('.') + suffix;
        dest = QDir(dir).filePath(candidate);
        if (!QFileInfo::exists(dest))
            return dest;
    }
}
} // namespace

InboundConnection::InboundConnection(QTcpSocket *socket, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
{
    m_socket->setParent(this);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setReadBufferSize(kMaxFrameLength + 4);
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(180000);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        finishWithError(QStringLiteral("Устройство не отвечает"));
    });
    m_idleTimer->start();
    m_keepAliveTimer = new QTimer(this);
    m_keepAliveTimer->setInterval(10000);
    connect(m_keepAliveTimer, &QTimer::timeout, this, [this] {
        if (m_encryptionDone && m_state != State::Done)
            sendEncryptedOfflineFrame(frames::buildKeepAliveOfflineFrame(false));
    });
    m_keepAliveTimer->start();
    connect(m_socket, &QTcpSocket::readyRead, this, &InboundConnection::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        if (m_state != State::Done) {
            qWarning("Quick Share inbound disconnected: peer=%s state=%d",
                     qPrintable(m_socket->peerAddress().toString()), int(m_state));
            m_error = QStringLiteral("Соединение прервано");
            m_state = State::Done;
            cleanupFiles(true);
        }
        emit finished(this, m_savedFiles, m_error);
        deleteLater();
    });
    // QTcpServer can hand us a socket whose readyRead was already emitted.
    // Defer processing so the owner can connect the transfer signals first.
    QTimer::singleShot(0, this, &InboundConnection::onReadyRead);
}

InboundConnection::~InboundConnection()
{
    cleanupFiles(true);
    m_socket->close();
}

void InboundConnection::onReadyRead()
{
    if (m_state == State::Done) {
        m_socket->readAll();
        return;
    }
    m_idleTimer->start();
    m_readBuffer.append(m_socket->readAll());
    while (m_readBuffer.size() >= 4) {
        const auto *p = reinterpret_cast<const uchar *>(m_readBuffer.constData());
        const quint32 frameLength = quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | quint32(p[3]);
        if (frameLength == 0 || frameLength > kMaxFrameLength) {
            finishWithError(QStringLiteral("Слишком большой кадр"));
            return;
        }
        if (m_readBuffer.size() < qsizetype(4 + frameLength))
            return; // wait for more data
        const QByteArray frame = m_readBuffer.mid(4, qsizetype(frameLength));
        m_readBuffer.remove(0, qsizetype(4 + frameLength));
        processFrame(frame);
        if (m_state == State::Done)
            return;
    }
}

void InboundConnection::processFrame(const QByteArray &frameData)
{
    switch (m_state) {
    case State::Initial:
        processConnectionRequest(frameData);
        break;
    case State::ReceivedConnectionRequest:
        processUkey2ClientInit(frameData);
        break;
    case State::SentUkeyServerInit:
        processUkey2ClientFinish(frameData);
        break;
    case State::ReceivedUkeyClientFinish:
        processConnectionResponse(frameData);
        break;
    default:
        processSecureMessage(frameData);
        break;
    }
}

void InboundConnection::processConnectionRequest(const QByteArray &frameData)
{
    frames::OfflineFrame frame;
    if (!frames::parseOfflineFrame(frameData, frame)
        || frame.type != frames::OfflineFrameType::ConnectionRequest) {
        finishWithError(QStringLiteral("Ожидался CONNECTION_REQUEST"));
        return;
    }
    EndpointInfo info;
    if (!EndpointInfo::parse(frame.endpointInfo, info)) {
        finishWithError(QStringLiteral("Некорректный endpoint info"));
        return;
    }
    m_deviceName = info.name.isEmpty() ? QStringLiteral("Android") : info.name;
    m_deviceType = info.deviceType;
    m_state = State::ReceivedConnectionRequest;
}

void InboundConnection::processUkey2ClientInit(const QByteArray &frameData)
{
    frames::Ukey2Message msg;
    if (!frames::parseUkey2Message(frameData, msg) || msg.type != int(frames::Ukey2Type::ClientInit)) {
        sendRawFrame(frames::buildUkey2Alert(3)); // INCORRECT_MESSAGE
        finishWithError(QStringLiteral("Ожидался UKEY2 CLIENT_INIT"));
        return;
    }
    frames::Ukey2ClientInit clientInit;
    if (!frames::parseUkey2ClientInit(msg.data, clientInit)) {
        sendRawFrame(frames::buildUkey2Alert(4)); // BAD_MESSAGE_DATA
        finishWithError(QStringLiteral("Некорректный CLIENT_INIT"));
        return;
    }
    if (clientInit.version != 1) {
        sendRawFrame(frames::buildUkey2Alert(100)); // BAD_VERSION
        finishWithError(QStringLiteral("UKEY2: неверная версия"));
        return;
    }
    if (clientInit.random.size() != 32) {
        sendRawFrame(frames::buildUkey2Alert(101)); // BAD_RANDOM
        finishWithError(QStringLiteral("UKEY2: неверный random"));
        return;
    }
    if (clientInit.p256Commitment.size() != 64) {
        sendRawFrame(frames::buildUkey2Alert(102)); // BAD_HANDSHAKE_CIPHER
        finishWithError(QStringLiteral("UKEY2: нет коммитмента P256_SHA512"));
        return;
    }
    if (clientInit.nextProtocol != QLatin1String("AES_256_CBC-HMAC_SHA256")) {
        sendRawFrame(frames::buildUkey2Alert(103)); // BAD_NEXT_PROTOCOL
        finishWithError(QStringLiteral("UKEY2: неверный next_protocol"));
        return;
    }
    m_cipherCommitment = clientInit.p256Commitment;
    m_clientInitRaw = frameData;

    m_keyPair = crypto::generateP256();
    if (!m_keyPair) {
        finishWithError(QStringLiteral("Не удалось сгенерировать ключ"));
        return;
    }
    QByteArray x, y;
    if (!crypto::p256PublicXY(m_keyPair.get(), x, y)) {
        finishWithError(QStringLiteral("Не удалось получить публичный ключ"));
        return;
    }
    const QByteArray serverInit = frames::buildUkey2ServerInit(crypto::toSignedBytes(x), crypto::toSignedBytes(y));
    m_serverInitRaw = serverInit;
    sendRawFrame(serverInit);
    m_state = State::SentUkeyServerInit;
}

void InboundConnection::processUkey2ClientFinish(const QByteArray &frameData)
{
    frames::Ukey2Message msg;
    if (!frames::parseUkey2Message(frameData, msg) || msg.type != int(frames::Ukey2Type::ClientFinish)) {
        finishWithError(QStringLiteral("Ожидался UKEY2 CLIENT_FINISH"));
        return;
    }
    if (crypto::sha512(frameData) != m_cipherCommitment) {
        finishWithError(QStringLiteral("UKEY2: коммитмент не совпал"));
        return;
    }
    QByteArray peerX, peerY;
    if (!frames::parseUkey2ClientFinished(msg.data, peerX, peerY)) {
        finishWithError(QStringLiteral("UKEY2: неверный публичный ключ клиента"));
        return;
    }

    // Key derivation (docs/protocol.md)
    const QByteArray shared = crypto::ecdhSharedX(m_keyPair.get(), peerX, peerY);
    if (shared.size() != 32) {
        finishWithError(QStringLiteral("UKEY2: недопустимый ключ устройства"));
        return;
    }
    const QByteArray dhs = crypto::sha256(shared);
    const QByteArray ukeyInfo = m_clientInitRaw + m_serverInitRaw;
    const QByteArray authString = crypto::hkdfSha256(dhs, "UKEY2 v1 auth", ukeyInfo, 32);
    const QByteArray nextSecret = crypto::hkdfSha256(dhs, "UKEY2 v1 next", ukeyInfo, 32);
    m_pinCode = pinCodeFromAuthKey(authString);

    const QByteArray d2dSalt = QByteArray::fromHex(
        "82AA55A0D397F88346CA1CEE8D3909B95F13FA7DEB1D4AB38376B8256DA85510");
    const QByteArray d2dClientKey = crypto::hkdfSha256(nextSecret, d2dSalt, "client", 32);
    const QByteArray d2dServerKey = crypto::hkdfSha256(nextSecret, d2dSalt, "server", 32);
    const QByteArray smsgSalt = crypto::sha256("SecureMessage");
    m_decryptKey = crypto::hkdfSha256(d2dClientKey, smsgSalt, "ENC:2", 32);
    m_recvHmacKey = crypto::hkdfSha256(d2dClientKey, smsgSalt, "SIG:1", 32);
    m_encryptKey = crypto::hkdfSha256(d2dServerKey, smsgSalt, "ENC:2", 32);
    m_sendHmacKey = crypto::hkdfSha256(d2dServerKey, smsgSalt, "SIG:1", 32);

    m_state = State::ReceivedUkeyClientFinish;
}

void InboundConnection::processConnectionResponse(const QByteArray &frameData)
{
    frames::OfflineFrame frame;
    if (!frames::parseOfflineFrame(frameData, frame)
        || frame.type != frames::OfflineFrameType::ConnectionResponse
        || frame.connectionResponse != 1) {
        finishWithError(QStringLiteral("Ожидался CONNECTION_RESPONSE"));
        return;
    }
    sendRawFrame(frames::buildConnectionResponseAccept());
    m_encryptionDone = true;
    // The server speaks first in the payload layer: paired key encryption.
    sendSharingFrame(frames::buildPairedKeyEncryptionFrame());
    m_state = State::SentConnectionResponse;
}

void InboundConnection::processSecureMessage(const QByteArray &frameData)
{
    QByteArray offlineFrameRaw;
    ++m_clientSeq;
    if (!frames::decryptSecureMessage(frameData, m_decryptKey, m_recvHmacKey, m_clientSeq, offlineFrameRaw)) {
        finishWithError(QStringLiteral("Не удалось расшифровать сообщение"));
        return;
    }
    frames::OfflineFrame frame;
    if (!frames::parseOfflineFrame(offlineFrameRaw, frame)) {
        finishWithError(QStringLiteral("Некорректный offline frame"));
        return;
    }
    switch (frame.type) {
    case frames::OfflineFrameType::PayloadTransfer:
        processPayloadTransfer(frame);
        break;
    case frames::OfflineFrameType::KeepAlive:
        if (!frame.keepAliveAck) sendKeepAliveAck();
        break;
    case frames::OfflineFrameType::Disconnection:
        finishWithError(QStringLiteral("Удалённая сторона разорвала соединение"));
        break;
    default:
        qDebug("InboundConnection: ignored encrypted frame type %d", int(frame.type));
        break;
    }
}

void InboundConnection::processPayloadTransfer(const frames::OfflineFrame &frame)
{
    const frames::PayloadHeader &header = frame.payloadHeader;
    const frames::PayloadChunk &chunk = frame.payloadChunk;
    if (!frame.hasPayloadChunk)
        return;

    if (header.type == 1) { // BYTES
        if (header.totalSize < 0 || header.totalSize > kMaxFrameLength
            || (!m_bytesPayloadBuffers.contains(header.id) && m_bytesPayloadBuffers.size() >= 16)) {
            finishWithError(QStringLiteral("Слишком большой BYTES-payload"));
            return;
        }
        QByteArray &buffer = m_bytesPayloadBuffers[header.id];
        if (chunk.offset != buffer.size()) {
            finishWithError(QStringLiteral("Неверное смещение BYTES-payload"));
            return;
        }
        if (chunk.body.size() > header.totalSize - buffer.size()) {
            finishWithError(QStringLiteral("Размер BYTES-payload превышен"));
            return;
        }
        if (chunk.hasBody)
            buffer.append(chunk.body);
        if ((chunk.flags & 1) == 0)
            return;
        if (buffer.size() != header.totalSize) {
            finishWithError(QStringLiteral("Неполный BYTES-payload"));
            return;
        }
        const QByteArray payload = buffer;
        m_bytesPayloadBuffers.remove(header.id);
        if (header.id == m_textPayloadId && m_state == State::ReceivingFiles) {
            if (payload.size() != m_pendingRequest.totalBytes) {
                finishWithError(QStringLiteral("Неверный размер текста"));
                return;
            }
            emit textReceived(this, QString::fromUtf8(payload), m_pendingRequest.textType);
            finishTransfer();
            return;
        }
        processSharingFrame(payload);
    } else if (header.type == 2) { // FILE
        processFileChunk(frame);
    }
}

void InboundConnection::processSharingFrame(const QByteArray &data)
{
    frames::SharingFrame frame;
    if (!frames::parseSharingFrame(data, frame)) {
        finishWithError(QStringLiteral("Некорректный sharing frame"));
        return;
    }
    // Android sends informational progress and response frames in addition to
    // the negotiated transfer. They do not change receiver consent or state.
    if ((m_state == State::WaitingForUserConsent || m_state == State::ReceivingFiles)
        && (frame.type == frames::SharingFrameType::ProgressUpdate
            || frame.type == frames::SharingFrameType::Response)) return;
    if (frame.type == frames::SharingFrameType::Cancel) {
        finishWithError(QStringLiteral("Отменено отправителем"));
        return;
    }
    switch (m_state) {
    case State::SentConnectionResponse:
        if (frame.type != frames::SharingFrameType::PairedKeyEncryption) {
            finishWithError(QStringLiteral("Ожидался PAIRED_KEY_ENCRYPTION"));
            return;
        }
        sendSharingFrame(frames::buildPairedKeyResultFrame());
        m_state = State::SentPairedKeyResult;
        break;
    case State::SentPairedKeyResult:
        if (frame.type != frames::SharingFrameType::PairedKeyResult) {
            finishWithError(QStringLiteral("Ожидался PAIRED_KEY_RESULT"));
            return;
        }
        m_state = State::ReceivedPairedKeyResult;
        break;
    case State::ReceivedPairedKeyResult: {
        if (frame.type != frames::SharingFrameType::Introduction) {
            finishWithError(QStringLiteral("Ожидался INTRODUCTION"));
            return;
        }
        TransferRequest request;
        request.deviceName = m_deviceName;
        request.peerAddress = m_socket->peerAddress().toString();
        request.deviceType = m_deviceType;
        request.pinCode = m_pinCode;
        if (!frame.files.isEmpty() && frame.texts.isEmpty()) {
            QSet<qint64> ids;
            if (frame.files.size() > 1000) {
                finishWithError(QStringLiteral("Слишком много файлов"));
                return;
            }
            for (const frames::FileMeta &meta : frame.files) {
                if (meta.size < 0 || meta.size > std::numeric_limits<qint64>::max() - request.totalBytes
                    || ids.contains(meta.payloadId) || meta.name.contains(QChar::Null)) {
                    finishWithError(QStringLiteral("Недопустимые метаданные файлов"));
                    return;
                }
                ids.insert(meta.payloadId);
                IncomingFile file;
                file.name = meta.name;
                file.mimeType = meta.mimeType;
                file.payloadId = meta.payloadId;
                file.size = meta.size;
                request.files.append(file);
                request.totalBytes += meta.size;
            }
        } else if (frame.texts.size() == 1 && frame.files.isEmpty()
                   && frame.texts.first().size >= 0 && frame.texts.first().size <= kMaxFrameLength) {
            const frames::TextMeta &meta = frame.texts.first();
            request.textTitle = meta.title;
            request.textType = meta.type;
            m_textPayloadId = meta.payloadId;
            request.totalBytes = meta.size;
        } else {
            sendSharingFrame(frames::buildSharingResponseFrame(4)); // UNSUPPORTED_ATTACHMENT_TYPE
            finishWithError(QStringLiteral("Неподдерживаемый тип вложения"));
            return;
        }
        m_pendingRequest = request;
        m_state = State::WaitingForUserConsent;
        m_consentTimer = new QTimer(this);
        m_consentTimer->setSingleShot(true);
        m_consentTimer->setInterval(kConsentTimeoutMs);
        connect(m_consentTimer, &QTimer::timeout, this, [this] {
            sendSharingFrame(frames::buildSharingResponseFrame(5)); // TIMED_OUT
            finishWithError(QStringLiteral("Истёк таймаут ожидания ответа пользователя"));
        });
        m_consentTimer->start();
        emit transferRequested(this, request);
        break;
    }
    default:
        qWarning("InboundConnection: unexpected sharing frame %d in state %d", int(frame.type), int(m_state));
        break;
    }
}

void InboundConnection::processFileChunk(const frames::OfflineFrame &frame)
{
    if (m_state != State::ReceivingFiles) {
        finishWithError(QStringLiteral("Неожиданный файловый чанк"));
        return;
    }
    const qint64 id = frame.payloadHeader.id;
    auto it = m_files.find(id);
    if (it == m_files.end()) {
        finishWithError(QStringLiteral("Неизвестный payload ID %1").arg(id));
        return;
    }
    ReceivingFile &file = it.value();
    const frames::PayloadChunk &chunk = frame.payloadChunk;
    if (chunk.offset != file.bytesTransferred) {
        finishWithError(QStringLiteral("Неверное смещение в файле"));
        return;
    }
    if (file.bytesTransferred + chunk.body.size() > file.meta.size) {
        finishWithError(QStringLiteral("Размер файла превышен"));
        return;
    }
    if (chunk.hasBody && !chunk.body.isEmpty()) {
        if (file.file->write(chunk.body) != chunk.body.size()) {
            finishWithError(QStringLiteral("Ошибка записи на диск"));
            return;
        }
        file.bytesTransferred += chunk.body.size();
        m_receivedBytes += chunk.body.size();
        emit overallProgress(this, m_receivedBytes, m_pendingRequest.totalBytes);
        emit transferProgress(this, file.meta.name, file.bytesTransferred, file.meta.size);
    }
    if (chunk.flags & 1) {
        if (file.bytesTransferred != file.meta.size) {
            finishWithError(QStringLiteral("Файл получен не полностью"));
            return;
        }
        if (!file.file->flush()) {
            finishWithError(QStringLiteral("Не удалось сохранить файл"));
            return;
        }
        file.file->close();
        // Publish only complete files, without ever replacing an existing path.
        bool renamed = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            file.destPath = uniqueDestination(QFileInfo(file.destPath).absolutePath(), file.meta.name);
            if (file.file->rename(file.destPath)) { renamed = true; break; }
            if (!QFileInfo::exists(file.destPath)) break;
        }
        if (!renamed) {
            finishWithError(QStringLiteral("Не удалось сохранить файл %1").arg(file.meta.name));
            return;
        }
        m_savedFiles.append(file.destPath);
        delete file.file;
        m_files.erase(it);
        if (m_files.isEmpty()) {
            finishTransfer();
        }
    }
}

void InboundConnection::acceptTransfer(const QString &destDir)
{
    if (m_state != State::WaitingForUserConsent)
        return;
    if (m_consentTimer) {
        m_consentTimer->stop();
        m_consentTimer->deleteLater();
        m_consentTimer = nullptr;
    }
    if (m_pendingRequest.files.isEmpty()) {
        // Text/URL transfer: nothing to prepare, the payload arrives as BYTES.
        sendSharingFrame(frames::buildSharingResponseFrame(1)); // ACCEPT
        m_state = State::ReceivingFiles;
        return;
    }
    if (!QDir().mkpath(destDir)) {
        finishWithError(QStringLiteral("Не удалось создать папку загрузок"));
        return;
    }
    const QStorageInfo storage(destDir);
    if (storage.isValid() && storage.bytesAvailable() < m_pendingRequest.totalBytes) {
        sendSharingFrame(frames::buildSharingResponseFrame(3));
        finishWithError(QStringLiteral("Недостаточно места на диске"));
        return;
    }
    for (const IncomingFile &meta : m_pendingRequest.files) {
        ReceivingFile file;
        file.meta = meta;
        file.destPath = uniqueDestination(destDir, meta.name);
        auto *temporary = new QTemporaryFile(QDir(destDir).filePath(QStringLiteral(".kquickshare-XXXXXX.part")));
        temporary->setAutoRemove(false);
        file.file = temporary;
        if (!temporary->open()) {
            delete temporary;
            finishWithError(QStringLiteral("Не удалось создать файл %1").arg(file.destPath));
            return;
        }
        m_files.insert(meta.payloadId, file);
    }
    sendSharingFrame(frames::buildSharingResponseFrame(1)); // ACCEPT
    m_state = State::ReceivingFiles;
}

void InboundConnection::rejectTransfer()
{
    if (m_state != State::WaitingForUserConsent)
        return;
    sendSharingFrame(frames::buildSharingResponseFrame(2)); // REJECT
    finishWithError(QStringLiteral("Отклонено пользователем"));
}

void InboundConnection::cancelTransfer()
{
    finishWithError(QStringLiteral("Передача отменена"));
}

void InboundConnection::sendRawFrame(const QByteArray &frame)
{
    QByteArray out(4, '\0');
    const quint32 len = quint32(frame.size());
    out[0] = char(len >> 24);
    out[1] = char(len >> 16);
    out[2] = char(len >> 8);
    out[3] = char(len);
    out.append(frame);
    m_socket->write(out);
}

void InboundConnection::sendEncryptedOfflineFrame(const QByteArray &offlineFrame)
{
    ++m_serverSeq;
    sendRawFrame(frames::encryptOfflineFrame(offlineFrame, m_serverSeq, m_encryptKey, m_sendHmacKey));
}

void InboundConnection::sendSharingFrame(const QByteArray &sharingFrame)
{
    qint64 id = qint64(quint64(QDateTime::currentMSecsSinceEpoch()) << 32 | quint32(QRandomGenerator::global()->generate()));
    for (const QByteArray &transferFrame : frames::buildBytesPayloadTransfer(id, sharingFrame))
        sendEncryptedOfflineFrame(transferFrame);
}

void InboundConnection::sendKeepAliveAck()
{
    const QByteArray frame = frames::buildKeepAliveOfflineFrame(true);
    if (m_encryptionDone)
        sendEncryptedOfflineFrame(frame);
    else
        sendRawFrame(frame);
}

void InboundConnection::sendDisconnectionAndClose()
{
    const QByteArray frame = frames::buildDisconnectionOfflineFrame();
    if (m_encryptionDone)
        sendEncryptedOfflineFrame(frame);
    else
        sendRawFrame(frame);
    m_socket->flush();
    m_socket->disconnectFromHost();
}

void InboundConnection::finishWithError(const QString &error)
{
    finishTransfer(error);
}

void InboundConnection::finishTransfer(const QString &error)
{
    if (m_state == State::Done)
        return;
    if (!error.isEmpty())
        qWarning("Quick Share inbound error: peer=%s state=%d error=%s",
                 qPrintable(m_socket->peerAddress().toString()), int(m_state), qPrintable(error));
    m_error = error;
    m_state = State::Done;
    m_idleTimer->stop();
    m_keepAliveTimer->stop();
    cleanupFiles(true);
    // finished permits consumers (including --once) to quit/delete us. Emit it
    // from disconnected, after Qt has drained the final protocol frames.
    if (m_encryptionDone)
        sendDisconnectionAndClose();
    else
        m_socket->disconnectFromHost();
    // A stalled peer must not hold a connection slot indefinitely.
    QTimer::singleShot(5000, this, [this] {
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->abort();
    });
}

void InboundConnection::cleanupFiles(bool deletePartial)
{
    if (m_consentTimer) {
        m_consentTimer->stop();
        m_consentTimer->deleteLater();
        m_consentTimer = nullptr;
    }
    for (auto it = m_files.begin(); it != m_files.end(); ++it) {
        it->file->close();
        if (deletePartial)
            QFile::remove(it->file->fileName());
        delete it->file;
    }
    m_files.clear();
}
