#include "bluetoothreceiver.h"
#include "bletransport.h"
#include "endpointinfo.h"

#include <QDBusConnection>
#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QtTest>

using PropertiesByInterface = QMap<QString, QVariantMap>;
using ObjectsByPath = QMap<QDBusObjectPath, PropertiesByInterface>;
Q_DECLARE_METATYPE(PropertiesByInterface)
Q_DECLARE_METATYPE(ObjectsByPath)

class FakeRoot : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.DBus.ObjectManager")
public slots:
    ObjectsByPath GetManagedObjects() const
    {
        return {{QDBusObjectPath("/org/bluez/hci0"), {{"org.bluez.Adapter1", {{"Powered", true}, {"Address", "00:11:22:33:44:55"}}},
            {"org.bluez.GattManager1", {}}, {"org.bluez.LEAdvertisingManager1", {}}}}};
    }
};

class FakeGattManager : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.GattManager1")
public:
    explicit FakeGattManager(QObject *parent) : QDBusAbstractAdaptor(parent) { }
    QString sender;
    int registrations = 0;
public slots:
    void RegisterApplication(const QDBusObjectPath &, const QVariantMap &, const QDBusMessage &message) { sender = message.service(); ++registrations; }
};

class FakeAdvertisingManager : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.LEAdvertisingManager1")
public:
    explicit FakeAdvertisingManager(QObject *parent) : QDBusAbstractAdaptor(parent) { }
    int registrations = 0;
    int unregistrations = 0;
public slots:
    void RegisterAdvertisement(const QDBusObjectPath &, const QVariantMap &) { ++registrations; }
    void UnregisterAdvertisement(const QDBusObjectPath &) { ++unregistrations; }
};

class BluetoothTests : public QObject
{
    Q_OBJECT
private slots:
    void bluezSession()
    {
        qDBusRegisterMetaType<PropertiesByInterface>();
        qDBusRegisterMetaType<ObjectsByPath>();
        qRegisterMetaType<BleTransport *>("BleTransport*");
        auto bus = QDBusConnection::systemBus();
        QVERIFY(bus.isConnected());
        QVERIFY(bus.registerService("org.bluez"));
        FakeRoot root;
        QObject adapter;
        FakeGattManager gatt(&adapter);
        FakeAdvertisingManager advertising(&adapter);
        QVERIFY(bus.registerObject("/", &root, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/org/bluez/hci0", &adapter, QDBusConnection::ExportAdaptors));
        BluetoothReceiver receiver("ABCD", EndpointInfo::serialize("Test", DeviceType::Laptop));
        QSignalSpy connected(&receiver, &BluetoothReceiver::newConnection);
        receiver.start();
        QTRY_VERIFY_WITH_TIMEOUT(!gatt.sender.isEmpty(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(advertising.registrations, 1, 5000);
        QVERIFY(bus.connect(gatt.sender, "/org/kde/kquickshare/ble/service0/char2", "org.freedesktop.DBus.Properties", "PropertiesChanged",
            this, SLOT(propertiesChanged(QString,QVariantMap,QStringList))));
        auto write = QDBusMessage::createMethodCall(gatt.sender, "/org/kde/kquickshare/ble/service0/char1", "org.bluez.GattCharacteristic1", "WriteValue");
        write << QByteArray::fromHex("80000100010014")
              << QVariantMap{{"device", QVariant::fromValue(QDBusObjectPath("/org/bluez/hci0/dev_test"))}, {"mtu", QVariant::fromValue(quint16(23))}};
        QDBusPendingCallWatcher written(bus.asyncCall(write));
        QTRY_VERIFY_WITH_TIMEOUT(written.isFinished(), 5000);
        QVERIFY2(!QDBusPendingReply<>(written).isError(), qPrintable(QDBusPendingReply<>(written).error().message()));
        QTRY_COMPARE(connected.count(), 1);
        auto *transport = qvariant_cast<BleTransport *>(connected.first().at(0));
        QVERIFY(transport);
        QVERIFY(!transport->supportsLanTrust());
        auto subscribe = QDBusMessage::createMethodCall(gatt.sender, "/org/kde/kquickshare/ble/service0/char2", "org.bluez.GattCharacteristic1", "StartNotify");
        QDBusPendingCallWatcher subscribed(bus.asyncCall(subscribe));
        QTRY_VERIFY_WITH_TIMEOUT(subscribed.isFinished(), 5000);
        QVERIFY2(!QDBusPendingReply<>(subscribed).isError(), qPrintable(QDBusPendingReply<>(subscribed).error().message()));
        QTRY_COMPARE_WITH_TIMEOUT(m_value, QByteArray::fromHex("8100010014"), 5000);
        transport->abort();
        auto stop = QDBusMessage::createMethodCall(gatt.sender, "/org/kde/kquickshare/ble/service0/char2", "org.bluez.GattCharacteristic1", "StopNotify");
        QDBusPendingCallWatcher stopped(bus.asyncCall(stop));
        QTRY_VERIFY_WITH_TIMEOUT(stopped.isFinished(), 5000);
        QVERIFY2(!QDBusPendingReply<>(stopped).isError(), qPrintable(QDBusPendingReply<>(stopped).error().message()));
        QTRY_COMPARE_WITH_TIMEOUT(advertising.unregistrations, 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(advertising.registrations, 2, 5000);
        QCOMPARE(gatt.registrations, 1);
        bus.unregisterService("org.bluez");
    }
public slots:
    void propertiesChanged(const QString &interface, const QVariantMap &values, const QStringList &)
    {
        if (interface == QLatin1String("org.bluez.GattCharacteristic1"))
            m_value = values.value(QStringLiteral("Value")).toByteArray();
    }
private:
    QByteArray m_value;
};

QTEST_GUILESS_MAIN(BluetoothTests)
#include "bluetooth_tests.moc"
