#include "bletransport.h"
#include "frames.h"
#include "inboundconnection.h"
#include "weave.h"

#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class UpgradeTests : public QObject
{
    Q_OBJECT
private slots:
    void transfer_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("lan") << QStringLiteral("upgrade");
        QTest::newRow("bluetooth-fallback") << QStringLiteral("fallback");
        QTest::newRow("peer-declines") << QStringLiteral("decline");
        QTest::newRow("reject-wrong-endpoint") << QStringLiteral("wrong-id-first");
        QTest::newRow("multiple-files") << QStringLiteral("upgrade-multi");
    }
    void transfer()
    {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer peerServer;
        QVERIFY(peerServer.listen(QHostAddress::LocalHost));
        Weave peer(509);
        QByteArray reply, ignored, input;
        QCOMPARE(peer.receive(QByteArray::fromHex("800001000101fd"), reply, ignored), Weave::Result::Connected);
        auto *ble = new BleTransport(QStringLiteral("Bluetooth"), 509);
        QVERIFY(ble->receivePacket(QByteArray::fromHex("800001000101fd")));
        auto *connection = new InboundConnection(ble, this);
        connection->setLanUpgradeAddress(QHostAddress::LocalHost, 2000);
        QSignalSpy finished(connection, &InboundConnection::finished);
        connect(connection, &InboundConnection::transferRequested, this, [&](InboundConnection *conn, const TransferRequest &request) {
            const bool upgraded = mode == QLatin1String("upgrade") || mode == QLatin1String("upgrade-multi") || mode == QLatin1String("wrong-id-first");
            QCOMPARE(request.supportsLanTrust, upgraded);
            QCOMPARE(request.peerAddress, upgraded ? QStringLiteral("127.0.0.1") : QStringLiteral("Bluetooth"));
            conn->acceptTransfer(directory.path());
        });
        QPointer<QTcpSocket> peerSocket;
        connect(&peerServer, &QTcpServer::newConnection, this, [&] {
            peerSocket = peerServer.nextPendingConnection();
            connect(peerSocket, &QTcpSocket::readyRead, this, [&] {
                input += peerSocket->readAll();
                QByteArray frame;
                while (frames::takeFrame(input, frame) == frames::FrameRead::Frame) {
                    for (const auto &packet : peer.encode(frames::lengthPrefixed(frame)))
                        QVERIFY(ble->receivePacket(packet));
                }
            });
        });
        connect(ble, &BleTransport::packetReady, this, [&](const QByteArray &packet) {
            QByteArray response, data;
            const auto result = peer.receive(packet, response, data);
            if (result == Weave::Result::Data && peerSocket)
                peerSocket->write(data);
        });
        QProcess sender;
        sender.start(QStringLiteral(PYTHON_EXECUTABLE), {QStringLiteral(SOURCE_DIR "/tests/fake_upgrade_sender.py"), QString::number(peerServer.serverPort()), mode});
        QTRY_COMPARE_WITH_TIMEOUT(sender.state(), QProcess::NotRunning, 12000);
        QVERIFY2(sender.exitCode() == 0, sender.readAllStandardError().constData());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY2(finished.first().at(2).toString().isEmpty(), qPrintable(finished.first().at(2).toString()));
        QFile received(directory.filePath(QStringLiteral("upgrade.txt")));
        QVERIFY(received.open(QIODevice::ReadOnly));
        QCOMPARE(received.readAll(), QByteArray("hello over the selected transport"));
        if (mode == QLatin1String("upgrade-multi")) {
            QFile second(directory.filePath(QStringLiteral("second.txt")));
            QVERIFY(second.open(QIODevice::ReadOnly));
            QCOMPARE(second.readAll(), QByteArray("second file"));
        }
    }
};

QTEST_GUILESS_MAIN(UpgradeTests)
#include "upgrade_tests.moc"
