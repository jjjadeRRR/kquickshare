#include "crypto.h"
#include "endpointinfo.h"
#include "frames.h"
#include "inboundconnection.h"
#include "nearbyserver.h"
#include "protobuf.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
class CoreTests : public QObject
{
    Q_OBJECT
private slots:
    void bufferedHandshake()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTcpSocket peer;
        peer.connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(peer.waitForConnected());
        QVERIFY(server.waitForNewConnection());
        auto *socket = server.nextPendingConnection();
        const auto request
            = pb::Writer()
                  .varint(1, 1)
                  .bytes(2, pb::Writer().varint(1, 1).bytes(2, pb::Writer().bytes(6, EndpointInfo::serialize("Prebuffered", DeviceType::Phone)).data()).data())
                  .data();
        QByteArray packet(4, '\0');
        const quint32 n = request.size();
        packet[0] = char(n >> 24);
        packet[1] = char(n >> 16);
        packet[2] = char(n >> 8);
        packet[3] = char(n);
        peer.write(packet + request);
        peer.flush();
        QVERIFY(socket->waitForReadyRead());
        auto *connection = new InboundConnection(socket);
        QTRY_COMPARE(connection->remoteDeviceName(), QString("Prebuffered"));
        delete connection;
    }
    void drainBeforeFinished()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QTcpSocket peer;
        peer.setReadBufferSize(1024);
        peer.connectToHost(QHostAddress::LocalHost, server.serverPort());
        QVERIFY(peer.waitForConnected());
        QVERIFY(server.waitForNewConnection());
        auto *socket = server.nextPendingConnection();
        auto *connection = new InboundConnection(socket);
        QSignalSpy finished(connection, &InboundConnection::finished);
        // Force pending writes; finished must not permit callers to destroy the
        // transport while its final bytes are still queued.
        socket->write(QByteArray(8 * 1024 * 1024, 'x'));
        connection->cancelTransfer();
        QCOMPARE(finished.count(), 0);
        qint64 received = 0;
        connect(&peer, &QTcpSocket::readyRead, this, [&] {
            received += peer.readAll().size();
        });
        peer.setReadBufferSize(0);
        received += peer.readAll().size();
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(received, qint64(8 * 1024 * 1024), 4000);
    }
    void endpointTypes()
    {
        for (int t = 0; t <= 6; ++t) {
            EndpointInfo info;
            QVERIFY(EndpointInfo::parse(EndpointInfo::serialize("Телефон", DeviceType(t)), info));
            QCOMPARE(info.name, QString("Телефон"));
            QCOMPARE(int(info.deviceType), t);
        }
        EndpointInfo info;
        QVERIFY(!EndpointInfo::parse(QByteArray(17, '\0'), info));
    }
    void longUtf8Name()
    {
        const QString name = QString::fromUtf8("😀").repeated(100);
        const auto data = EndpointInfo::serialize(name, DeviceType::Laptop);
        QVERIFY(base64UrlEncode(data).size() + 2 <= 255);
        EndpointInfo info;
        QVERIFY(EndpointInfo::parse(data, info));
        QVERIFY(!info.name.contains(QChar::ReplacementCharacter));
    }
    void protobufInvalid()
    {
        QVector<pb::Field> fields;
        QVERIFY(!pb::parseMessage(QByteArray::fromHex("0880808080808080808002"), fields));
        fields.clear();
        QVERIFY(!pb::parseMessage(QByteArray::fromHex("12056162"), fields));
    }
    void shortCoordinate()
    {
        auto a = crypto::generateP256();
        auto b = crypto::generateP256();
        QVERIFY(a);
        QVERIFY(b);
        QByteArray x, y;
        QVERIFY(crypto::p256PublicXY(b.get(), x, y));
        const QByteArray shared = crypto::ecdhSharedX(a.get(), crypto::toSignedBytes(x), crypto::toSignedBytes(y));
        QVERIFY(crypto::p256PublicXY(a.get(), x, y));
        QCOMPARE(shared, crypto::ecdhSharedX(b.get(), crypto::toSignedBytes(x), crypto::toSignedBytes(y)));
        QVERIFY(crypto::ecdhSharedX(a.get(), QByteArray(34, 'a'), y).isEmpty());
        // Find a valid point with a shortened signed coordinate and verify that
        // it derives the same secret as the peer (left padding, not right padding).
        bool checkedShort = false;
        for (int attempt = 0; attempt < 4096; ++attempt) {
            b = crypto::generateP256();
            QVERIFY(crypto::p256PublicXY(b.get(), x, y));
            const auto sx = crypto::toSignedBytes(x);
            const auto sy = crypto::toSignedBytes(y);
            if (sx.size() >= 32 && sy.size() >= 32)
                continue;
            const auto shortSecret = crypto::ecdhSharedX(a.get(), sx, sy);
            QVERIFY(crypto::p256PublicXY(a.get(), x, y));
            QCOMPARE(shortSecret, crypto::ecdhSharedX(b.get(), crypto::toSignedBytes(x), crypto::toSignedBytes(y)));
            checkedShort = true;
            break;
        }
        QVERIFY(checkedShort);
    }
    void secureTampering()
    {
        const QByteArray enc(32, 'e'), mac(32, 'm');
        const auto plain = frames::buildKeepAliveOfflineFrame(false);
        auto packet = frames::encryptOfflineFrame(plain, 1, enc, mac);
        QByteArray out;
        QVERIFY(frames::decryptSecureMessage(packet, enc, mac, 1, out));
        QCOMPARE(out, plain);
        QVERIFY(!frames::decryptSecureMessage(packet, enc, mac, 2, out));
        packet[packet.size() - 1] ^= 1;
        QVERIFY(!frames::decryptSecureMessage(packet, enc, mac, 1, out));
    }
    void streamFraming()
    {
        QByteArray buffer = frames::lengthPrefixed("abc") + frames::lengthPrefixed("de").left(5);
        QByteArray frame;
        QCOMPARE(frames::takeFrame(buffer, frame), frames::FrameRead::Frame);
        QCOMPARE(frame, QByteArray("abc"));
        QCOMPARE(frames::takeFrame(buffer, frame), frames::FrameRead::Incomplete);
        buffer += 'e';
        QCOMPARE(frames::takeFrame(buffer, frame), frames::FrameRead::Frame);
        QCOMPARE(frame, QByteArray("de"));
        QVERIFY(buffer.isEmpty());
        buffer = QByteArray::fromHex("00000000");
        QCOMPARE(frames::takeFrame(buffer, frame), frames::FrameRead::Invalid);
        buffer = QByteArray::fromHex("00500001");
        QCOMPARE(frames::takeFrame(buffer, frame), frames::FrameRead::Invalid);
    }
    void bytesAssembler()
    {
        using Result = frames::BytesPayloadAssembler::Result;
        frames::BytesPayloadAssembler assembler(2);
        const frames::PayloadHeader header{7, 1, 5};
        QByteArray payload;
        QCOMPARE(assembler.add(header, {0, 0, "abc", true}, payload), Result::Pending);
        QCOMPARE(assembler.add(header, {0, 3, "de", true}, payload), Result::Pending);
        QCOMPARE(assembler.add(header, {1, 5, {}, false}, payload), Result::Complete);
        QCOMPARE(payload, QByteArray("abcde"));
        QCOMPARE(assembler.add(header, {0, 1, "x", true}, payload), Result::Invalid); // wrong offset
        QCOMPARE(assembler.add(header, {1, 0, "abcdef", true}, payload), Result::Invalid); // overflow
        QCOMPARE(assembler.add(header, {1, 0, "ab", true}, payload), Result::Invalid); // truncated
        QCOMPARE(assembler.add({1, 1, frames::kMaxFrameLength + 1}, {}, payload), Result::TooLarge);
        QCOMPARE(assembler.add({1, 1, 1}, {}, payload), Result::Pending);
        QCOMPARE(assembler.add({2, 1, 1}, {}, payload), Result::Pending);
        QCOMPARE(assembler.add({3, 1, 1}, {}, payload), Result::TooLarge); // concurrency limit
    }
    void ukey2Agreement()
    {
        auto client = crypto::generateP256(), server = crypto::generateP256();
        QByteArray cx, cy, sx, sy;
        QVERIFY(crypto::p256PublicXY(client.get(), cx, cy));
        QVERIFY(crypto::p256PublicXY(server.get(), sx, sy));
        frames::Ukey2Keys a, b;
        using crypto::toSignedBytes; // coordinates travel as protobuf signed bytes
        QVERIFY(frames::deriveUkey2Keys(client.get(), toSignedBytes(sx), toSignedBytes(sy), "init", "reply", a));
        QVERIFY(frames::deriveUkey2Keys(server.get(), toSignedBytes(cx), toSignedBytes(cy), "init", "reply", b));
        QCOMPARE(a.authString, b.authString);
        QCOMPARE(a.clientEncrypt, b.clientEncrypt);
        QCOMPARE(a.serverHmac, b.serverHmac);
        QVERIFY(a.clientEncrypt != a.serverEncrypt);
        QCOMPARE(frames::pinCode(a.authString).size(), 4);
        QCOMPARE(frames::pinCode(QByteArray()), QString("0000"));
    }
    void cancelFrame()
    {
        frames::SharingFrame frame;
        QVERIFY(frames::parseSharingFrame(frames::buildCancelFrame(), frame));
        QCOMPARE(frame.type, frames::SharingFrameType::Cancel);
    }
    void endpointEmptyTrailingRecord()
    {
        QByteArray data = EndpointInfo::serialize("Phone", DeviceType::Phone);
        data += QByteArray::fromHex("0100"); // QR record with an empty value
        EndpointInfo info;
        QVERIFY(EndpointInfo::parse(data + QByteArray::fromHex("0102abcd"), info));
        QCOMPARE(info.qrCodeData, QByteArray::fromHex("abcd"));
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
