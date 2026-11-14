#include "inboundconnection.h"
#include "inboundtransport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSet>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QTimer>
#include <limits>

namespace
{
using frames::kMaxFrameLength;
constexpr int kConsentTimeoutMs = 120'000;

constexpr qsizetype kMaxNameBytes = 255; // NAME_MAX on Linux file systems

// Shortens base so that base + tag + "." + suffix fits NAME_MAX bytes of UTF-8,
// cutting only at code point boundaries. Cyrillic names reach the limit at
// roughly 127 characters.
QString fittedName(QString base, const QString &tag, QString suffix)
{
    auto bytes = [](const QString &text) {
        return text.toUtf8().size();
    };
    if (bytes(suffix) > 32) // not a real extension; keep it as part of the name
        suffix.clear();
    const qsizetype reserved = bytes(tag) + (suffix.isEmpty() ? 0 : 1 + bytes(suffix));
    while (!base.isEmpty() && bytes(base) + reserved > kMaxNameBytes) {
        base.chop(1);
        if (!base.isEmpty() && base.back().isHighSurrogate())
            base.chop(1);
    }
    if (base.isEmpty())
        base = QStringLiteral("unnamed");
    QString name = base + tag;
    if (!suffix.isEmpty())
        name += QLatin1Char('.') + suffix;
    return name;
}

QString uniqueDestination(const QString &dir, const QString &fileName)
{
    QString cleanName = QFileInfo(fileName).fileName(); // strip any path components
    if (cleanName.isEmpty() || cleanName == "." || cleanName == "..")
        cleanName = QStringLiteral("unnamed");
    const QString base = QFileInfo(cleanName).completeBaseName();
    const QString suffix = QFileInfo(cleanName).suffix();
    if (cleanName.toUtf8().size() > kMaxNameBytes)
        cleanName = base.isEmpty() ? fittedName(cleanName, {}, {}) : fittedName(base, {}, suffix);
    QString dest = QDir(dir).filePath(cleanName);
    if (!QFileInfo::exists(dest))
        return dest;
    const QString uniqueBase = QFileInfo(cleanName).completeBaseName();
    const QString uniqueSuffix = QFileInfo(cleanName).suffix();
    for (int i = 1;; ++i) {
        const QString tag = QStringLiteral(" (%1)").arg(i);
        const QString candidate = uniqueBase.isEmpty() ? fittedName(cleanName, tag, {}) : fittedName(uniqueBase, tag, uniqueSuffix);
        dest = QDir(dir).filePath(candidate);
        if (!QFileInfo::exists(dest))
            return dest;
    }
}
} // namespace

InboundConnection::InboundConnection(QTcpSocket *socket, QObject *parent)
    : InboundConnection(new TcpInboundTransport(socket), parent)
{ }

InboundConnection::InboundConnection(InboundTransport *transport, QObject *parent)
    : QObject(parent)
    , m_transport(transport)
    , m_peerAddress(transport->peerAddress())
    , m_supportsLanTrust(transport->supportsLanTrust())
{
    attachTransport(m_transport);
    m_idleTimer = new QTimer(this);
    m_idleTimer->setInterval(180000);
    connect(m_idleTimer, &QTimer::timeout, this, [this] {
        finishWithError(tr("The device is not responding"));
    });
    m_idleTimer->start();
    m_keepAliveTimer = new QTimer(this);
    m_keepAliveTimer->setInterval(10000);
    connect(m_keepAliveTimer, &QTimer::timeout, this, [this] {
        if (m_encryptionDone && m_state != State::Done)
            sendEncryptedOfflineFrame(frames::buildKeepAliveOfflineFrame(false));
    });
    m_keepAliveTimer->start();
    // QTcpServer can hand us a socket whose readyRead was already emitted.
    // Defer processing so the owner can connect the transfer signals first.
    QTimer::singleShot(0, this, &InboundConnection::onReadyRead);
}

void InboundConnection::attachTransport(InboundTransport *transport)
{
    transport->setParent(this);
    connect(transport, &InboundTransport::readyRead, this, &InboundConnection::onReadyRead);
    connect(transport, &InboundTransport::disconnected, this, &InboundConnection::transportDisconnected);
}

void InboundConnection::transportDisconnected()
{
    if (m_upgradeState == UpgradeState::ClosingBluetooth && m_state != State::Done) {
        // Let BlueZ flush the final notification before using the new channel.
        QTimer::singleShot(200, this, &InboundConnection::finishUpgrade);
        return;
    }
    if (m_state != State::Done) {
        qWarning("Quick Share inbound disconnected: peer=%s state=%d", qPrintable(m_transport->peerAddress()), int(m_state));
        m_error = tr("Connection interrupted");
        m_state = State::Done;
        cleanupFiles(true);
    }
    emit finished(this, m_savedFiles, m_error);
    deleteLater();
}

void InboundConnection::offerLanUpgrade()
{
    m_upgradeServer = new QTcpServer(this);
    m_upgradeServer->setMaxPendingConnections(4);
    if (!m_upgradeServer->listen(m_upgradeAddress, 0)) {
        qInfo("Bluetooth receiver: LAN upgrade listener unavailable");
        m_upgradeServer->deleteLater();
        m_upgradeServer = nullptr;
        return;
    }
    connect(m_upgradeServer, &QTcpServer::newConnection, this, &InboundConnection::acceptUpgradeConnection);
    m_upgradeTimer = new QTimer(this);
    m_upgradeTimer->setSingleShot(true);
    connect(m_upgradeTimer, &QTimer::timeout, this, [this] {
        if (m_upgradeState == UpgradeState::Offered) {
            qInfo("Bluetooth receiver: LAN upgrade unavailable, continuing over Bluetooth");
            abandonUpgrade();
        } else if (m_upgradeState == UpgradeState::Draining || m_upgradeState == UpgradeState::ClosingBluetooth) {
            finishWithError(tr("The network connection could not be completed"));
        }
    });
    const quint32 ip = m_upgradeAddress.toIPv4Address();
    QByteArray ipBytes;
    ipBytes.append(char(ip >> 24)).append(char(ip >> 16)).append(char(ip >> 8)).append(char(ip));
    m_upgradeState = UpgradeState::Offered;
    m_upgradeTimer->start(m_upgradeTimeoutMs);
    sendEncryptedOfflineFrame(frames::buildBandwidthUpgradeOffer(ipBytes, m_upgradeServer->serverPort()));
    qInfo("Bluetooth receiver: offered encrypted LAN upgrade");
}

void InboundConnection::acceptUpgradeConnection()
{
    while (m_upgradeServer && m_upgradeServer->hasPendingConnections()) {
        auto *socket = m_upgradeServer->nextPendingConnection();
        if (m_upgradeState != UpgradeState::Offered || m_upgradeSocket) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setParent(this);
        socket->setReadBufferSize(frames::kMaxFrameLength + 4);
        m_upgradeSocket = socket;
        m_upgradeReadBuffer.clear();
        connect(socket, &QTcpSocket::readyRead, this, &InboundConnection::readUpgradeIntroduction);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            if (m_upgradeSocket != socket)
                return;
            m_upgradeSocket = nullptr;
            socket->deleteLater();
            m_upgradeReadBuffer.clear();
            if (m_upgradeState == UpgradeState::Draining || m_upgradeState == UpgradeState::ClosingBluetooth)
                finishWithError(tr("The network connection was interrupted"));
        });
        QTimer::singleShot(0, this, &InboundConnection::readUpgradeIntroduction);
    }
}

void InboundConnection::readUpgradeIntroduction()
{
    if (m_upgradeState != UpgradeState::Offered || !m_upgradeSocket)
        return;
    m_upgradeReadBuffer += m_upgradeSocket->readAll();
    if (m_upgradeReadBuffer.size() > 4096) {
        m_upgradeSocket->abort();
        return;
    }
    QByteArray intro;
    const auto result = frames::takeFrame(m_upgradeReadBuffer, intro);
    if (result == frames::FrameRead::Incomplete)
        return;
    frames::OfflineFrame frame;
    if (result == frames::FrameRead::Invalid || !frames::parseOfflineFrame(intro, frame)
        || frame.type != frames::OfflineFrameType::BandwidthUpgradeNegotiation || frame.bandwidthEvent != 4 || frame.endpointId != m_peerEndpointId) {
        m_upgradeSocket->abort();
        return;
    }
    const QByteArray ack = frames::lengthPrefixed(frames::buildBandwidthUpgradeEvent(6));
    if (m_upgradeSocket->write(ack) != ack.size()) {
        m_upgradeSocket->abort();
        return;
    }
    m_upgradeSocket->flush();
    m_upgradeServer->close();
    disconnect(m_upgradeSocket.data(), &QTcpSocket::readyRead, this, &InboundConnection::readUpgradeIntroduction);
    m_upgradeState = UpgradeState::Draining;
    m_upgradeTimer->start(8000);
    sendEncryptedOfflineFrameNow(frames::buildBandwidthUpgradeEvent(2)); // LAST_WRITE_TO_PRIOR_CHANNEL
    qInfo("Bluetooth receiver: LAN connected, negotiating transport switch");
}

void InboundConnection::handleUpgradeEvent(int event)
{
    if (m_upgradeState == UpgradeState::Offered && event == 5) {
        abandonUpgrade();
        return;
    }
    if (m_upgradeState != UpgradeState::Draining)
        return;
    if (event == 2 && !m_upgradeSentSafe) {
        m_upgradeSentSafe = true;
        sendEncryptedOfflineFrameNow(frames::buildBandwidthUpgradeEvent(3)); // SAFE_TO_CLOSE_PRIOR_CHANNEL
    } else if (event == 3 && m_upgradeSentSafe) {
        m_upgradeState = UpgradeState::ClosingBluetooth;
        sendRawFrame(frames::buildDisconnectionOfflineFrame()); // plaintext on the old channel
        m_transport->disconnectFromPeer();
    }
}

void InboundConnection::finishUpgrade()
{
    if (m_state == State::Done || m_upgradeState != UpgradeState::ClosingBluetooth)
        return;
    if (!m_upgradeSocket || m_upgradeSocket->state() != QAbstractSocket::ConnectedState || !m_readBuffer.isEmpty()) {
        finishWithError(tr("The network connection could not be completed"));
        return;
    }
    m_upgradeTimer->stop();
    auto *socket = m_upgradeSocket.data();
    m_peerAddress = socket->peerAddress().toString();
    m_supportsLanTrust = true;
    m_upgradeSocket = nullptr;
    disconnect(socket, nullptr, this, nullptr);
    disconnect(m_transport, nullptr, this, nullptr);
    m_transport->deleteLater();
    m_transport = new TcpInboundTransport(socket);
    attachTransport(m_transport);
    m_upgradeState = UpgradeState::Complete;
    if (m_requestDeferred) {
        m_pendingRequest.peerAddress = m_peerAddress;
        m_pendingRequest.supportsLanTrust = true;
    }
    m_readBuffer = std::move(m_upgradeReadBuffer);
    const auto queued = std::move(m_upgradeDeferredFrames);
    m_upgradeDeferredFrames.clear();
    for (const auto &frame : queued)
        sendEncryptedOfflineFrame(frame);
    qInfo("Bluetooth receiver: transfer upgraded to LAN");
    if (m_requestDeferred)
        presentTransferRequest();
    onReadyRead();
}

void InboundConnection::abandonUpgrade()
{
    if (m_upgradeTimer)
        m_upgradeTimer->stop();
    if (m_upgradeServer)
        m_upgradeServer->close();
    if (m_upgradeSocket) {
        auto *socket = m_upgradeSocket.data();
        m_upgradeSocket = nullptr;
        disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        socket->deleteLater();
    }
    m_upgradeReadBuffer.clear();
    m_upgradeState = UpgradeState::None;
    if (m_requestDeferred)
        presentTransferRequest();
}

void InboundConnection::presentTransferRequest()
{
    if (m_state != State::WaitingForUserConsent || m_consentTimer)
        return;
    m_requestDeferred = false;
    m_consentTimer = new QTimer(this);
    m_consentTimer->setSingleShot(true);
    m_consentTimer->setInterval(kConsentTimeoutMs);
    connect(m_consentTimer, &QTimer::timeout, this, [this] {
        sendSharingFrame(frames::buildSharingResponseFrame(5)); // TIMED_OUT
        finishWithError(tr("Timed out waiting for approval"));
    });
    m_consentTimer->start();
    emit transferRequested(this, m_pendingRequest);
}

InboundConnection::~InboundConnection()
{
    // Closing emits disconnected(); its handler must not run on a half-destroyed object.
    disconnect(m_transport, nullptr, this, nullptr);
    cleanupFiles(true);
    m_transport->abort();
}

void InboundConnection::onReadyRead()
{
    if (m_state == State::Done) {
        m_transport->readAll();
        return;
    }
    m_idleTimer->start();
    m_readBuffer.append(m_transport->readAll());
    QByteArray frame;
    for (;;) {
        switch (frames::takeFrame(m_readBuffer, frame)) {
        case frames::FrameRead::Incomplete:
            return;
        case frames::FrameRead::Invalid:
            finishWithError(tr("Frame is too large"));
            return;
        case frames::FrameRead::Frame:
            processFrame(frame);
            if (m_state == State::Done)
                return;
            break;
        }
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
    if (!frames::parseOfflineFrame(frameData, frame) || frame.type != frames::OfflineFrameType::ConnectionRequest) {
        finishWithError(tr("Expected CONNECTION_REQUEST"));
        return;
    }
    EndpointInfo info;
    if (!EndpointInfo::parse(frame.endpointInfo, info)) {
        finishWithError(tr("Invalid endpoint information"));
        return;
    }
    m_deviceName = info.name.isEmpty() ? QStringLiteral("Android") : info.name;
    m_deviceType = info.deviceType;
    m_peerEndpointId = frame.endpointId;
    m_state = State::ReceivedConnectionRequest;
}

void InboundConnection::processUkey2ClientInit(const QByteArray &frameData)
{
    frames::Ukey2Message msg;
    if (!frames::parseUkey2Message(frameData, msg) || msg.type != int(frames::Ukey2Type::ClientInit)) {
        sendRawFrame(frames::buildUkey2Alert(3)); // INCORRECT_MESSAGE
        finishWithError(tr("Expected UKEY2 CLIENT_INIT"));
        return;
    }
    frames::Ukey2ClientInit clientInit;
    if (!frames::parseUkey2ClientInit(msg.data, clientInit)) {
        sendRawFrame(frames::buildUkey2Alert(4)); // BAD_MESSAGE_DATA
        finishWithError(tr("Invalid CLIENT_INIT"));
        return;
    }
    if (clientInit.version != 1) {
        sendRawFrame(frames::buildUkey2Alert(100)); // BAD_VERSION
        finishWithError(tr("UKEY2: invalid version"));
        return;
    }
    if (clientInit.random.size() != 32) {
        sendRawFrame(frames::buildUkey2Alert(101)); // BAD_RANDOM
        finishWithError(tr("UKEY2: invalid random value"));
        return;
    }
    if (clientInit.p256Commitment.size() != 64) {
        sendRawFrame(frames::buildUkey2Alert(102)); // BAD_HANDSHAKE_CIPHER
        finishWithError(tr("UKEY2: missing P256_SHA512 commitment"));
        return;
    }
    if (clientInit.nextProtocol != QLatin1String("AES_256_CBC-HMAC_SHA256")) {
        sendRawFrame(frames::buildUkey2Alert(103)); // BAD_NEXT_PROTOCOL
        finishWithError(tr("UKEY2: invalid next_protocol"));
        return;
    }
    m_cipherCommitment = clientInit.p256Commitment;
    m_clientInitRaw = frameData;

    m_keyPair = crypto::generateP256();
    if (!m_keyPair) {
        finishWithError(tr("Could not generate a key"));
        return;
    }
    QByteArray x, y;
    if (!crypto::p256PublicXY(m_keyPair.get(), x, y)) {
        finishWithError(tr("Could not obtain the public key"));
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
        finishWithError(tr("Expected UKEY2 CLIENT_FINISH"));
        return;
    }
    if (crypto::sha512(frameData) != m_cipherCommitment) {
        finishWithError(tr("UKEY2: commitment mismatch"));
        return;
    }
    QByteArray peerX, peerY;
    if (!frames::parseUkey2ClientFinished(msg.data, peerX, peerY)) {
        finishWithError(tr("UKEY2: invalid client public key"));
        return;
    }

    frames::Ukey2Keys keys;
    if (!frames::deriveUkey2Keys(m_keyPair.get(), peerX, peerY, m_clientInitRaw, m_serverInitRaw, keys)) {
        finishWithError(tr("UKEY2: invalid device key"));
        return;
    }
    m_pinCode = frames::pinCode(keys.authString);
    // We are the UKEY2 server: decrypt with the client's keys, encrypt with ours.
    m_decryptKey = keys.clientEncrypt;
    m_recvHmacKey = keys.clientHmac;
    m_encryptKey = keys.serverEncrypt;
    m_sendHmacKey = keys.serverHmac;

    m_state = State::ReceivedUkeyClientFinish;
}

void InboundConnection::processConnectionResponse(const QByteArray &frameData)
{
    frames::OfflineFrame frame;
    if (!frames::parseOfflineFrame(frameData, frame) || frame.type != frames::OfflineFrameType::ConnectionResponse || frame.connectionResponse != 1) {
        finishWithError(tr("Expected CONNECTION_RESPONSE"));
        return;
    }
    sendRawFrame(frames::buildConnectionResponseAccept());
    m_encryptionDone = true;
    // The server speaks first in the payload layer: paired key encryption.
    sendSharingFrame(frames::buildPairedKeyEncryptionFrame());
    m_state = State::SentConnectionResponse;
    if (!m_supportsLanTrust && !m_upgradeAddress.isNull() && m_peerEndpointId.size() == 4)
        offerLanUpgrade();
}

void InboundConnection::processSecureMessage(const QByteArray &frameData)
{
    QByteArray offlineFrameRaw;
    ++m_clientSeq;
    if (!frames::decryptSecureMessage(frameData, m_decryptKey, m_recvHmacKey, m_clientSeq, offlineFrameRaw)) {
        finishWithError(tr("Could not decrypt the message"));
        return;
    }
    frames::OfflineFrame frame;
    if (!frames::parseOfflineFrame(offlineFrameRaw, frame)) {
        finishWithError(tr("Invalid offline frame"));
        return;
    }
    switch (frame.type) {
    case frames::OfflineFrameType::PayloadTransfer:
        processPayloadTransfer(frame);
        break;
    case frames::OfflineFrameType::KeepAlive:
        if (!frame.keepAliveAck)
            sendKeepAliveAck();
        break;
    case frames::OfflineFrameType::BandwidthUpgradeNegotiation:
        handleUpgradeEvent(frame.bandwidthEvent);
        break;
    case frames::OfflineFrameType::Disconnection:
        finishWithError(tr("The remote device disconnected"));
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
        QByteArray payload;
        switch (m_bytesPayloads.add(header, chunk, payload)) {
        case frames::BytesPayloadAssembler::Result::Pending:
            return;
        case frames::BytesPayloadAssembler::Result::TooLarge:
            finishWithError(tr("BYTES payload is too large"));
            return;
        case frames::BytesPayloadAssembler::Result::Invalid:
            finishWithError(tr("Invalid BYTES payload"));
            return;
        case frames::BytesPayloadAssembler::Result::Complete:
            break;
        }
        if (header.id == m_textPayloadId && m_state == State::ReceivingFiles) {
            if (payload.size() != m_pendingRequest.totalBytes) {
                finishWithError(tr("Invalid text size"));
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
        finishWithError(tr("Invalid sharing frame"));
        return;
    }
    // Android sends informational progress and response frames in addition to
    // the negotiated transfer. They do not change receiver consent or state.
    if ((m_state == State::WaitingForUserConsent || m_state == State::ReceivingFiles)
        && (frame.type == frames::SharingFrameType::ProgressUpdate || frame.type == frames::SharingFrameType::Response))
        return;
    if (frame.type == frames::SharingFrameType::Cancel) {
        finishWithError(tr("Cancelled by the sender"));
        return;
    }
    // During the handshake some phones interleave frames that carry no
    // decision (certificate info, progress, frame types newer than this
    // client). Failing on them aborted the first attempt of a share, which the
    // phone then had to retry. Skip them; the idle timer still ends a stalled peer.
    const bool informational = frame.type == frames::SharingFrameType::CertificateInfo || frame.type == frames::SharingFrameType::ProgressUpdate
        || frame.type == frames::SharingFrameType::Response || frame.type == frames::SharingFrameType::Unknown
        || int(frame.type) > int(frames::SharingFrameType::ProgressUpdate);
    const bool handshake = m_state == State::SentConnectionResponse || m_state == State::SentPairedKeyResult || m_state == State::ReceivedPairedKeyResult;
    if (handshake && informational) {
        qInfo("InboundConnection: skipped sharing frame type %d in state %d", int(frame.type), int(m_state));
        return;
    }
    switch (m_state) {
    case State::SentConnectionResponse:
        if (frame.type != frames::SharingFrameType::PairedKeyEncryption) {
            qWarning("InboundConnection: got sharing frame type %d instead of PAIRED_KEY_ENCRYPTION", int(frame.type));
            finishWithError(tr("Expected PAIRED_KEY_ENCRYPTION"));
            return;
        }
        sendSharingFrame(frames::buildPairedKeyResultFrame());
        m_state = State::SentPairedKeyResult;
        break;
    case State::SentPairedKeyResult:
        if (frame.type != frames::SharingFrameType::PairedKeyResult) {
            qWarning("InboundConnection: got sharing frame type %d instead of PAIRED_KEY_RESULT", int(frame.type));
            finishWithError(tr("Expected PAIRED_KEY_RESULT"));
            return;
        }
        m_state = State::ReceivedPairedKeyResult;
        break;
    case State::ReceivedPairedKeyResult: {
        if (frame.type != frames::SharingFrameType::Introduction) {
            qWarning("InboundConnection: got sharing frame type %d instead of INTRODUCTION", int(frame.type));
            finishWithError(tr("Expected INTRODUCTION"));
            return;
        }
        TransferRequest request;
        request.deviceName = m_deviceName;
        request.peerAddress = m_peerAddress;
        request.supportsLanTrust = m_supportsLanTrust;
        request.deviceType = m_deviceType;
        request.pinCode = m_pinCode;
        if (!frame.files.isEmpty() && frame.texts.isEmpty()) {
            QSet<qint64> ids;
            if (frame.files.size() > 1000) {
                finishWithError(tr("Too many files"));
                return;
            }
            for (const frames::FileMeta &meta : frame.files) {
                if (meta.size < 0 || meta.size > std::numeric_limits<qint64>::max() - request.totalBytes || ids.contains(meta.payloadId)
                    || meta.name.contains(QChar::Null)) {
                    finishWithError(tr("Invalid file metadata"));
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
        } else if (frame.texts.size() == 1 && frame.files.isEmpty() && frame.texts.first().size >= 0 && frame.texts.first().size <= kMaxFrameLength) {
            const frames::TextMeta &meta = frame.texts.first();
            request.textTitle = meta.title;
            request.textType = meta.type;
            m_textPayloadId = meta.payloadId;
            request.totalBytes = meta.size;
        } else {
            sendSharingFrame(frames::buildSharingResponseFrame(4)); // UNSUPPORTED_ATTACHMENT_TYPE
            finishWithError(tr("Unsupported attachment type"));
            return;
        }
        m_pendingRequest = request;
        m_state = State::WaitingForUserConsent;
        if (m_upgradeState == UpgradeState::Offered || m_upgradeState == UpgradeState::Draining || m_upgradeState == UpgradeState::ClosingBluetooth)
            m_requestDeferred = true;
        else
            presentTransferRequest();
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
        finishWithError(tr("Unexpected file chunk"));
        return;
    }
    const qint64 id = frame.payloadHeader.id;
    auto it = m_files.find(id);
    if (it == m_files.end()) {
        finishWithError(tr("Unknown payload ID %1").arg(id));
        return;
    }
    ReceivingFile &file = it.value();
    const frames::PayloadChunk &chunk = frame.payloadChunk;
    if (chunk.offset != file.bytesTransferred) {
        finishWithError(tr("Invalid file offset"));
        return;
    }
    if (file.bytesTransferred + chunk.body.size() > file.meta.size) {
        finishWithError(tr("File exceeds its declared size"));
        return;
    }
    if (!file.file) {
        auto *temporary = new QTemporaryFile(QDir(m_destDir).filePath(QStringLiteral(".kquickshare-XXXXXX.part")));
        temporary->setAutoRemove(false);
        if (!temporary->open()) {
            const QString reason = temporary->errorString();
            delete temporary;
            finishWithError(tr("Could not create file %1: %2").arg(file.meta.name, reason));
            return;
        }
        file.file = temporary;
    }
    if (chunk.hasBody && !chunk.body.isEmpty()) {
        if (file.file->write(chunk.body) != chunk.body.size()) {
            finishWithError(tr("Disk write error"));
            return;
        }
        file.bytesTransferred += chunk.body.size();
        m_receivedBytes += chunk.body.size();
        emit overallProgress(this, m_receivedBytes, m_pendingRequest.totalBytes);
        emit transferProgress(this, file.meta.name, file.bytesTransferred, file.meta.size);
    }
    if (chunk.flags & 1) {
        if (file.bytesTransferred != file.meta.size) {
            finishWithError(tr("The file is incomplete"));
            return;
        }
        if (!file.file->flush()) {
            finishWithError(tr("Could not save the file"));
            return;
        }
        file.file->close();
        // Publish only complete files, without ever replacing an existing path.
        bool renamed = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            file.destPath = uniqueDestination(m_destDir, file.meta.name);
            if (file.file->rename(file.destPath)) {
                renamed = true;
                break;
            }
            if (!QFileInfo::exists(file.destPath))
                break;
        }
        if (!renamed) {
            finishWithError(tr("Could not save file %1").arg(file.meta.name));
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
        finishWithError(tr("Could not create the downloads folder"));
        return;
    }
    if (!QFileInfo(destDir).isWritable()) {
        sendSharingFrame(frames::buildSharingResponseFrame(2)); // REJECT
        finishWithError(tr("The downloads folder is not writable: %1").arg(destDir));
        return;
    }
    const QStorageInfo storage(destDir);
    if (storage.isValid() && storage.bytesAvailable() < m_pendingRequest.totalBytes) {
        sendSharingFrame(frames::buildSharingResponseFrame(3));
        finishWithError(tr("Not enough disk space"));
        return;
    }
    // Temporary files are created when a payload's first chunk arrives. A batch
    // of up to 1000 attachments must not hold a descriptor per file (the usual
    // soft limit is 1024), and the phone sends payloads one after another.
    m_destDir = destDir;
    for (const IncomingFile &meta : m_pendingRequest.files) {
        ReceivingFile file;
        file.meta = meta;
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
    finishWithError(tr("Rejected by the user"));
}

void InboundConnection::cancelTransfer()
{
    finishWithError(tr("Transfer cancelled"));
}

void InboundConnection::sendRawFrame(const QByteArray &frame)
{
    const QByteArray data = frames::lengthPrefixed(frame);
    if (m_transport->write(data) != data.size() && m_state != State::Done) {
        // Defer the failure: callers may still be updating the protocol state.
        QTimer::singleShot(0, this, [this] { finishWithError(tr("Could not send data to the device")); });
    }
}

void InboundConnection::sendEncryptedOfflineFrame(const QByteArray &offlineFrame)
{
    if (m_upgradeState == UpgradeState::Draining || m_upgradeState == UpgradeState::ClosingBluetooth) {
        if (m_upgradeDeferredFrames.size() >= 64) {
            finishWithError(tr("Too many pending messages during network upgrade"));
            return;
        }
        m_upgradeDeferredFrames.append(offlineFrame);
        return;
    }
    sendEncryptedOfflineFrameNow(offlineFrame);
}

void InboundConnection::sendEncryptedOfflineFrameNow(const QByteArray &offlineFrame)
{
    ++m_serverSeq;
    sendRawFrame(frames::encryptOfflineFrame(offlineFrame, m_serverSeq, m_encryptKey, m_sendHmacKey));
}

void InboundConnection::sendSharingFrame(const QByteArray &sharingFrame)
{
    const qint64 id = qint64(QRandomGenerator::global()->generate64() & std::numeric_limits<qint64>::max());
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
    m_transport->disconnectFromPeer();
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
        qWarning("Quick Share inbound error: peer=%s state=%d error=%s", qPrintable(m_transport->peerAddress()), int(m_state), qPrintable(error));
    m_error = error;
    m_state = State::Done;
    m_idleTimer->stop();
    m_keepAliveTimer->stop();
    if (m_upgradeTimer)
        m_upgradeTimer->stop();
    if (m_upgradeServer)
        m_upgradeServer->close();
    if (m_upgradeSocket) {
        auto *socket = m_upgradeSocket.data();
        m_upgradeSocket = nullptr;
        disconnect(socket, nullptr, this, nullptr);
        socket->abort();
        socket->deleteLater();
    }
    cleanupFiles(true);
    // finished permits consumers (including --once) to quit/delete us. Emit it
    // from disconnected, after Qt has drained the final protocol frames.
    if (m_encryptionDone)
        sendDisconnectionAndClose();
    else
        m_transport->disconnectFromPeer();
    // A stalled peer must not hold a connection slot indefinitely.
    QTimer::singleShot(5000, this, [this] {
        m_transport->abort();
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
        if (!it->file)
            continue;
        it->file->close();
        if (deletePartial)
            QFile::remove(it->file->fileName());
        delete it->file;
    }
    m_files.clear();
}
