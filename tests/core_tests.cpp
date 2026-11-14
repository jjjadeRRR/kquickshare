#include "crypto.h"
#include "endpointinfo.h"
#include "frames.h"
#include "inboundconnection.h"
#include "nearbyserver.h"
#include "protobuf.h"
#include "weave.h"
#include "bleadvertisement.h"
#include "bletransport.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
class CoreTests : public QObject
{
    Q_OBJECT
private slots:
    void endpointStaysStableAcrossNetworkRestarts()
    {
        qputenv("KQUICKSHARE_DISABLE_BLE", "1");
        NearbyServer server;
        QSignalSpy started(&server, &NearbyServer::started);
        QVERIFY(server.start(QStringLiteral("Stable identity test"), DeviceType::Laptop, 0));
        QCOMPARE(started.size(), 1);
        const QString endpoint = started.first().at(0).toString();
        QVERIFY(server.start(QStringLiteral("Stable identity test"), DeviceType::Laptop, 0));
        QCOMPARE(started.size(), 2);
        QCOMPARE(started.last().at(0).toString(), endpoint);
        server.stop();
        qunsetenv("KQUICKSHARE_DISABLE_BLE");
    }
    void bandwidthUpgradeFrames()
    {
        const auto offer = frames::buildBandwidthUpgradeOffer(QByteArray::fromHex("c0a80184"), 12345);
        QCOMPARE(offer, QByteArray::fromHex("0801121708042a130801120f08051a090a04c0a8018410b9604801"));
        frames::OfflineFrame frame;
        QVERIFY(frames::parseOfflineFrame(offer, frame));
        QCOMPARE(frame.bandwidthEvent, 1);
        const auto intro = pb::Writer().varint(1, 1).bytes(2, pb::Writer().varint(1, 4)
            .bytes(5, pb::Writer().varint(1, 4).bytes(3, pb::Writer().bytes(1, "ABCD").data()).data()).data()).data();
        QVERIFY(frames::parseOfflineFrame(intro, frame));
        QCOMPARE(frame.bandwidthEvent, 4);
        QCOMPARE(frame.endpointId, QByteArray("ABCD"));
        QVERIFY(frames::buildBandwidthUpgradeOffer("bad", 123).isEmpty());
        QVERIFY(frames::parseOfflineFrame(frames::buildBandwidthUpgradeEvent(6), frame));
        QCOMPARE(frame.bandwidthEvent, 6);
        QCOMPARE(frames::buildDisconnectionOfflineFrame(), QByteArray::fromHex("0801120808063a0408001000"));
    }
    void bleStreamReachesInboundProtocol()
    {
        auto *transport = new BleTransport(QStringLiteral("Bluetooth"), 20);
        QSignalSpy packets(transport, &BleTransport::packetReady);
        QVERIFY(transport->receivePacket(QByteArray::fromHex("80000100010014")));
        QCOMPARE(packets.count(), 1);
        QCOMPARE(packets.takeFirst().at(0).toByteArray(), QByteArray::fromHex("8100010014"));
        QVERIFY(!transport->supportsLanTrust());
        auto *connection = new InboundConnection(transport);
        Weave client;
        QByteArray reply, ignored;
        QCOMPARE(client.receive(QByteArray::fromHex("80000100010014"), reply, ignored), Weave::Result::Connected);
        const QByteArray request = pb::Writer().varint(1, 1)
            .bytes(2, pb::Writer().varint(1, 1).bytes(2, pb::Writer().bytes(6, EndpointInfo::serialize("Bluetooth phone", DeviceType::Phone)).data()).data()).data();
        const auto incoming = client.encode(frames::lengthPrefixed(request));
        QVERIFY(incoming.size() > 1);
        for (const auto &packet : incoming)
            QVERIFY(transport->receivePacket(packet));
        QCOMPARE(connection->remoteDeviceName(), QString("Bluetooth phone"));
        delete connection;
    }
    void bleStreamDrainsBeforeClosing()
    {
        BleTransport transport(QStringLiteral("Bluetooth"));
        QVERIFY(transport.receivePacket(QByteArray::fromHex("80000100010014")));
        QSignalSpy packets(&transport, &BleTransport::packetReady);
        QSignalSpy disconnected(&transport, &BleTransport::disconnected);
        const QByteArray data = frames::lengthPrefixed(QByteArray(100, 'x'));
        QCOMPARE(transport.write(data), qint64(data.size()));
        transport.disconnectFromPeer();
        QCOMPARE(disconnected.count(), 0);
        QTRY_COMPARE(disconnected.count(), 1);
        QVERIFY(packets.size() > 1);
        QVERIFY(quint8(packets.last().at(0).toByteArray()[0]) & 0x04);
    }
    void bleAdvertisementFormat()
    {
        QByteArray info = QByteArray::fromHex("06") + QByteArray(16, '\0') + QByteArray::fromHex("0454657374");
        QCOMPARE(bleAdvertisement("ABCD", info, QByteArray::fromHex("1234")),
            QByteArray::fromHex("48fc9f5e0000002723fc9f5e41424344162600000000000000000000000000000000045465737400000000000000001234"));
        const auto longInfo = EndpointInfo::serialize(QString::fromUtf8("😀").repeated(100), DeviceType::Laptop);
        const auto result = bleAdvertisement("ABCD", longInfo, QByteArray(2, '\0'));
        const int infoSize = quint8(result[16]);
        QVERIFY(infoSize <= 131);
        EndpointInfo decoded;
        QVERIFY(EndpointInfo::parse(result.mid(17, infoSize), decoded));
        QVERIFY(!decoded.name.contains(QChar::ReplacementCharacter));
        QVERIFY(bleAdvertisement("bad", info, QByteArray(2, '\0')).isEmpty());
    }
    void weaveTransport()
    {
        Weave weave(509);
        QByteArray reply, data;
        using Result = Weave::Result;
        QCOMPARE(weave.receive(QByteArray::fromHex("800001000101fd"), reply, data), Result::Connected);
        QCOMPARE(reply, QByteArray::fromHex("81000101fd"));
        QCOMPARE(weave.receive(QByteArray::fromHex("0c0000000801"), reply, data), Result::Pending);
        const QByteArray stream = frames::lengthPrefixed(QByteArray(1200, 'x'));
        const auto packets = weave.encode(stream);
        QCOMPARE(packets.size(), 3);
        QCOMPARE(quint8(packets[0][0]), quint8(0x18));
        QCOMPARE(quint8(packets[1][0]), quint8(0x20));
        QCOMPARE(quint8(packets[2][0]), quint8(0x34));
        QCOMPARE(weave.receive(packets[0], reply, data), Result::Pending);
        QCOMPARE(weave.receive(packets[1], reply, data), Result::Pending);
        QCOMPARE(weave.receive(packets[2], reply, data), Result::Data);
        QCOMPARE(data, stream);
        QCOMPARE(weave.receive(QByteArray::fromHex("0c0000000802"), reply, data), Result::Disconnected);
    }
    void weaveRejectsMalformedPackets()
    {
        using Result = Weave::Result;
        QByteArray reply, data;
        Weave weave;
        QCOMPARE(weave.receive(QByteArray::fromHex("800002000201fd"), reply, data), Result::Invalid);
        QCOMPARE(weave.receive(QByteArray::fromHex("80000100010013"), reply, data), Result::Invalid);
        QCOMPARE(weave.receive(QByteArray::fromHex("80000100010014"), reply, data), Result::Connected);
        QCOMPARE(reply, QByteArray::fromHex("8100010014"));
        QCOMPARE(weave.receive(QByteArray::fromHex("0401"), reply, data), Result::Invalid);
        QCOMPARE(weave.receive(QByteArray::fromHex("0c01020300"), reply, data), Result::Invalid);
        QCOMPARE(weave.receive(QByteArray(21, 'x'), reply, data), Result::Invalid);
        Weave extended;
        QCOMPARE(extended.receive(QByteArray::fromHex("800001000101fdabcd"), reply, data), Result::Connected);
        QCOMPARE(reply, QByteArray::fromHex("8100010014"));
        QCOMPARE(extended.receive(QByteArray::fromHex("08fc9f5e00"), reply, data), Result::Pending);
        QCOMPARE(extended.receive(QByteArray::fromHex("0801"), reply, data), Result::Invalid);
    }
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
