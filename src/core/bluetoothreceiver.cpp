#include "bluetoothreceiver.h"

#include "bleadvertisement.h"
#include "bletransport.h"
#include "crypto.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <functional>

using InterfaceProperties = QMap<QString, QVariantMap>;
using ManagedObjects = QMap<QDBusObjectPath, InterfaceProperties>;
Q_DECLARE_METATYPE(InterfaceProperties)
Q_DECLARE_METATYPE(ManagedObjects)

namespace
{
const QString serviceUuid = QStringLiteral("0000fef3-0000-1000-8000-00805f9b34fb");
const QString slotUuid = QStringLiteral("00000000-0000-3000-8000-000000000000");
const QString writeUuid = QStringLiteral("00000100-0004-1000-8000-001a11000101");
const QString notifyUuid = QStringLiteral("00000100-0004-1000-8000-001a11000102");
const QString rootPath = QStringLiteral("/org/kde/kquickshare/ble");
const QString servicePath = rootPath + QStringLiteral("/service0");
const QString slotPath = servicePath + QStringLiteral("/char0");
const QString writePath = servicePath + QStringLiteral("/char1");
const QString notifyPath = servicePath + QStringLiteral("/char2");
const QString adPath = rootPath + QStringLiteral("/advertisement");
const QString charInterface = QStringLiteral("org.bluez.GattCharacteristic1");
const QString serviceInterface = QStringLiteral("org.bluez.GattService1");

QDBusPendingCall call(const QDBusConnection &bus, const QString &path, const QString &interface, const QString &method, const QVariantList &arguments = {})
{
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), path, interface, method);
    msg.setArguments(arguments);
    return bus.asyncCall(msg);
}
} // namespace

class GattManagerObject : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.DBus.ObjectManager")
public:
    ManagedObjects objects;
public slots:
    ManagedObjects GetManagedObjects() const { return objects; }
};

class GattServiceObject : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.GattService1")
    Q_PROPERTY(QString UUID READ uuid CONSTANT)
    Q_PROPERTY(bool Primary READ primary CONSTANT)
public:
    QString uuid() const { return serviceUuid; }
    bool primary() const { return true; }
};

class GattCharacteristicObject : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.GattCharacteristic1")
    Q_PROPERTY(QString UUID READ uuid CONSTANT)
    Q_PROPERTY(QDBusObjectPath Service READ service CONSTANT)
    Q_PROPERTY(QStringList Flags READ flags CONSTANT)
    Q_PROPERTY(QByteArray Value READ value)
    Q_PROPERTY(bool Notifying READ notifying)
public:
    GattCharacteristicObject(const QDBusConnection &bus, const QString &path, const QString &uuid, const QStringList &flags, const QString &bluezOwner)
        : m_bus(bus), m_path(path), m_uuid(uuid), m_flags(flags), m_bluezOwner(bluezOwner) { }
    QString uuid() const { return m_uuid; }
    QDBusObjectPath service() const { return QDBusObjectPath(servicePath); }
    QStringList flags() const { return m_flags; }
    QByteArray value() const { return m_value; }
    bool notifying() const { return m_notifying; }
    void setValue(const QByteArray &value) { m_value = value; }
    std::function<bool(const QByteArray &, const QVariantMap &)> writeHandler;
    std::function<void(bool)> notifyHandler;

    void notify(const QByteArray &packet)
    {
        if (!m_notifying)
            return;
        m_value = packet;
        auto signal = QDBusMessage::createSignal(m_path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
        signal << charInterface << QVariantMap{{QStringLiteral("Value"), packet}} << QStringList{};
        m_bus.send(signal);
    }

public slots:
    QByteArray ReadValue(const QVariantMap &options)
    {
        qInfo("Bluetooth receiver: GATT read, uuid=%s", qPrintable(m_uuid));
        if (!authorized() || !m_flags.contains(QStringLiteral("read"))) {
            sendErrorReply(QStringLiteral("org.bluez.Error.NotPermitted"), QStringLiteral("Read not permitted"));
            return {};
        }
        const uint offset = options.value(QStringLiteral("offset"), 0).toUInt();
        if (offset > uint(m_value.size())) {
            sendErrorReply(QStringLiteral("org.bluez.Error.InvalidOffset"), QStringLiteral("Invalid offset"));
            return {};
        }
        return m_value.mid(offset);
    }
    void WriteValue(const QByteArray &value, const QVariantMap &options)
    {
        if (!authorized() || !writeHandler || !writeHandler(value, options))
            sendErrorReply(QStringLiteral("org.bluez.Error.Failed"), QStringLiteral("Invalid or unavailable BLE session"));
    }
    void StartNotify()
    {
        qInfo("Bluetooth receiver: GATT notification subscription");
        if (!authorized() || !m_flags.contains(QStringLiteral("notify"))) {
            sendErrorReply(QStringLiteral("org.bluez.Error.NotPermitted"), QStringLiteral("Notifications unavailable"));
            return;
        }
        m_notifying = true;
        if (notifyHandler)
            notifyHandler(true);
    }
    void StopNotify()
    {
        qInfo("Bluetooth receiver: GATT notification stopped");
        if (!authorized()) {
            sendErrorReply(QStringLiteral("org.bluez.Error.NotPermitted"), QStringLiteral("Not authorized"));
            return;
        }
        m_notifying = false;
        if (notifyHandler)
            notifyHandler(false);
    }
    void Confirm() { }
private:
    bool authorized() const { return calledFromDBus() && message().service() == m_bluezOwner; }
    QDBusConnection m_bus;
    QString m_path, m_uuid;
    QStringList m_flags;
    QString m_bluezOwner;
    QByteArray m_value;
    bool m_notifying = false;
};

class AdvertisementObject : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.LEAdvertisement1")
    Q_PROPERTY(QString Type READ type CONSTANT)
    Q_PROPERTY(QVariantMap ServiceData READ serviceData CONSTANT)
    Q_PROPERTY(QString SecondaryChannel READ secondaryChannel CONSTANT)
    Q_PROPERTY(uint MinInterval READ minInterval CONSTANT)
    Q_PROPERTY(uint MaxInterval READ maxInterval CONSTANT)
public:
    QByteArray advertisement;
    QString type() const { return QStringLiteral("peripheral"); }
    QString secondaryChannel() const { return QStringLiteral("1M"); }
    uint minInterval() const { return 100; }
    uint maxInterval() const { return 150; }
    QVariantMap serviceData() const { return {{serviceUuid, advertisement}}; }
public slots:
    void Release() { emit released(); }
signals:
    void released();
};

class BluetoothReceiverImpl : public QObject
{
    Q_OBJECT
public:
    BluetoothReceiverImpl(BluetoothReceiver *owner, QByteArray endpointId, QByteArray endpointInfo)
        : QObject(owner), m_owner(owner), m_endpointId(std::move(endpointId)), m_endpointInfo(std::move(endpointInfo)),
          m_connectionName(QStringLiteral("kquickshare-ble-%1").arg(quintptr(this))),
          m_bus(QDBusConnection::connectToBus(QDBusConnection::SystemBus, m_connectionName))
    {
        m_advertisementTimer.setSingleShot(true);
        connect(&m_advertisementTimer, &QTimer::timeout, this, &BluetoothReceiverImpl::refreshAdvertisement);
    }
    ~BluetoothReceiverImpl() override
    {
        if (m_session)
            m_session->abort();
        QDBusConnection::disconnectFromBus(m_connectionName);
    }

    void start()
    {
        if (qEnvironmentVariableIsSet("KQUICKSHARE_DISABLE_BLE"))
            return;
        if (!m_bus.isConnected()) {
            emit failed();
            return;
        }
        auto *serviceWatcher = new QDBusServiceWatcher(QStringLiteral("org.bluez"), m_bus, QDBusServiceWatcher::WatchForUnregistration, this);
        connect(serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] { emit failed(); });
        qDBusRegisterMetaType<InterfaceProperties>();
        qDBusRegisterMetaType<ManagedObjects>();
        auto *watch = new QDBusPendingCallWatcher(call(m_bus, QStringLiteral("/"), QStringLiteral("org.freedesktop.DBus.ObjectManager"), QStringLiteral("GetManagedObjects")), this);
        connect(watch, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
            static bool reportedUnavailable = false;
            static bool reportedNoAdapter = false;
            QDBusPendingReply<ManagedObjects> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError()) {
                if (!reportedUnavailable)
                    qWarning("Bluetooth receiver: BlueZ unavailable: %s", qPrintable(reply.error().message()));
                reportedUnavailable = true;
                emit failed();
                return;
            }
            reportedUnavailable = false;
            const auto objects = reply.value();
            for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
                const auto adapter = it.value().value(QStringLiteral("org.bluez.Adapter1"));
                if (!adapter.value(QStringLiteral("Powered")).toBool() || !it.value().contains(QStringLiteral("org.bluez.GattManager1"))
                    || !it.value().contains(QStringLiteral("org.bluez.LEAdvertisingManager1")))
                    continue;
                m_adapter = it.key().path();
                reportedNoAdapter = false;
                setup();
                return;
            }
            if (!reportedNoAdapter)
                qInfo("Bluetooth receiver: no powered BlueZ adapter with GATT and advertising support");
            reportedNoAdapter = true;
            emit failed();
        });
    }

    bool hasActiveSession() const { return !m_session.isNull(); }

signals:
    void failed();

private:
    void setup()
    {
        const auto owner = m_bus.interface()->serviceOwner(QStringLiteral("org.bluez"));
        if (!owner.isValid()) {
            qWarning("Bluetooth receiver: could not identify BlueZ");
            emit failed();
            return;
        }
        const QByteArray adv = bleAdvertisement(m_endpointId, m_endpointInfo, crypto::randomBytes(2));
        if (adv.isEmpty())
            return;
        m_manager = new GattManagerObject;
        m_service = new GattServiceObject;
        m_slot = new GattCharacteristicObject(m_bus, slotPath, slotUuid, {QStringLiteral("read")}, owner.value());
        m_write = new GattCharacteristicObject(m_bus, writePath, writeUuid, {QStringLiteral("write")}, owner.value());
        m_notify = new GattCharacteristicObject(m_bus, notifyPath, notifyUuid, {QStringLiteral("notify"), QStringLiteral("indicate")}, owner.value());
        m_advertisement = new AdvertisementObject;
        for (QObject *object : QList<QObject *>{m_manager, m_service, m_slot, m_write, m_notify, m_advertisement})
            object->setParent(this);
        m_slot->setValue(adv);
        m_advertisement->advertisement = adv;
        connect(m_advertisement, &AdvertisementObject::released, this, [this] {
            if (!m_refreshingAdvertisement)
                emit failed();
        });

        m_manager->objects.insert(QDBusObjectPath(servicePath), {{serviceInterface, {{QStringLiteral("UUID"), serviceUuid}, {QStringLiteral("Primary"), true}}}});
        const auto addChar = [this](const QString &path, GattCharacteristicObject *c) {
            m_manager->objects.insert(QDBusObjectPath(path), {{charInterface, {{QStringLiteral("UUID"), c->uuid()},
                {QStringLiteral("Service"), QVariant::fromValue(QDBusObjectPath(servicePath))}, {QStringLiteral("Flags"), c->flags()}}}});
        };
        addChar(slotPath, m_slot);
        addChar(writePath, m_write);
        addChar(notifyPath, m_notify);
        m_notify->notifyHandler = [this](bool enabled) {
            if (enabled) {
                while (!m_pendingPackets.isEmpty())
                    m_notify->notify(m_pendingPackets.dequeue());
            } else if (m_session) {
                m_session->abort();
            }
            if (!enabled && m_advertised && !m_refreshingAdvertisement)
                m_advertisementTimer.start(250);
        };
        m_write->writeHandler = [this](const QByteArray &packet, const QVariantMap &options) { return receive(packet, options); };

        const auto flags = QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties;
        if (!m_bus.registerObject(rootPath, m_manager, flags) || !m_bus.registerObject(servicePath, m_service, flags)
            || !m_bus.registerObject(slotPath, m_slot, flags) || !m_bus.registerObject(writePath, m_write, flags)
            || !m_bus.registerObject(notifyPath, m_notify, flags) || !m_bus.registerObject(adPath, m_advertisement, flags)) {
            qWarning("Bluetooth receiver: could not export GATT objects: %s", qPrintable(m_bus.lastError().message()));
            emit failed();
            return;
        }
        auto *watch = new QDBusPendingCallWatcher(call(m_bus, m_adapter, QStringLiteral("org.bluez.GattManager1"), QStringLiteral("RegisterApplication"),
            {QVariant::fromValue(QDBusObjectPath(rootPath)), QVariantMap{}}), this);
        connect(watch, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError()) {
                qWarning("Bluetooth receiver: could not register GATT service: %s", qPrintable(reply.error().message()));
                emit failed();
                return;
            }
            registerAdvertisement();
        });
    }

    void registerAdvertisement()
    {
        auto *watch = new QDBusPendingCallWatcher(call(m_bus, m_adapter, QStringLiteral("org.bluez.LEAdvertisingManager1"), QStringLiteral("RegisterAdvertisement"),
            {QVariant::fromValue(QDBusObjectPath(adPath)), QVariantMap{}}), this);
        connect(watch, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            const bool renewed = m_refreshingAdvertisement;
            m_refreshingAdvertisement = false;
            if (reply.isError()) {
                qWarning("Bluetooth receiver: could not advertise: %s", qPrintable(reply.error().message()));
                emit failed();
            } else {
                m_advertised = true;
                m_advertisementTimer.start(30000);
                if (!renewed)
                    qInfo("Bluetooth receiver: advertising and GATT service registered");
            }
        });
    }

    void refreshAdvertisement()
    {
        if (!m_advertised || m_refreshingAdvertisement)
            return;
        if (m_session) {
            m_advertisementTimer.start(10000);
            return;
        }
        m_refreshingAdvertisement = true;
        m_advertised = false;
        auto *watch = new QDBusPendingCallWatcher(call(m_bus, m_adapter, QStringLiteral("org.bluez.LEAdvertisingManager1"),
            QStringLiteral("UnregisterAdvertisement"), {QVariant::fromValue(QDBusObjectPath(adPath))}), this);
        connect(watch, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<> reply = *watcher;
            watcher->deleteLater();
            if (reply.isError() && reply.error().name() != QLatin1String("org.bluez.Error.DoesNotExist")) {
                m_refreshingAdvertisement = false;
                qWarning("Bluetooth receiver: could not refresh advertising: %s", qPrintable(reply.error().message()));
                emit failed();
                return;
            }
            registerAdvertisement();
        });
    }

    bool receive(const QByteArray &packet, const QVariantMap &options)
    {
        if (options.value(QStringLiteral("offset"), 0).toUInt() != 0) {
            qWarning("Bluetooth receiver: invalid write offset");
            return false;
        }
        const auto device = qvariant_cast<QDBusObjectPath>(options.value(QStringLiteral("device"))).path();
        if (device.isEmpty() || device == QStringLiteral("/")) {
            qWarning("Bluetooth receiver: write did not identify a device");
            return false;
        }
        if (m_session && device != m_device)
            return false;
        if (!m_session) {
            // Late data or error packets from a completed session must not
            // create another session while the phone closes its old channel.
            if (packet.size() < 7 || packet.size() > 20 || (quint8(packet[0]) & 0x8f) != 0x80)
                return false;
            qInfo("Bluetooth receiver: starting Weave session (first packet %lld bytes)", qlonglong(packet.size()));
            // The request write reports the negotiated ATT MTU. Its value is
            // three bytes larger than the largest notification payload.
            const uint mtu = options.value(QStringLiteral("mtu"), 23).toUInt();
            const quint16 packetSize = quint16(qBound(20u, mtu > 3 ? mtu - 3 : 20, 509u));
            m_device = device;
            auto *transport = new BleTransport(QStringLiteral("Bluetooth"), packetSize, this);
            m_session = transport;
            connect(transport, &BleTransport::packetReady, this, [this](const QByteArray &outgoing) {
                if (m_notify->notifying())
                    m_notify->notify(outgoing);
                else if (m_pendingPackets.size() < 32)
                    m_pendingPackets.enqueue(outgoing);
                else if (m_session)
                    m_session->abort();
            });
            connect(transport, &BleTransport::established, this, [this, transport] { emit m_owner->newConnection(transport); });
            connect(transport, &BleTransport::disconnected, this, [this, transport] {
                if (m_session == transport) {
                    m_session = nullptr;
                    m_device.clear();
                    m_pendingPackets.clear();
                }
                if (transport->parent() == this)
                    transport->deleteLater();
            });
        }
        return m_session->receivePacket(packet);
    }

    BluetoothReceiver *m_owner;
    QByteArray m_endpointId, m_endpointInfo;
    QString m_connectionName;
    QDBusConnection m_bus;
    QString m_adapter, m_device;
    QPointer<BleTransport> m_session;
    QQueue<QByteArray> m_pendingPackets;
    QTimer m_advertisementTimer;
    bool m_advertised = false;
    bool m_refreshingAdvertisement = false;
    GattManagerObject *m_manager = nullptr;
    GattServiceObject *m_service = nullptr;
    GattCharacteristicObject *m_slot = nullptr, *m_write = nullptr, *m_notify = nullptr;
    AdvertisementObject *m_advertisement = nullptr;
};

BluetoothReceiver::BluetoothReceiver(const QByteArray &endpointId, const QByteArray &endpointInfo, QObject *parent)
    : QObject(parent), m_endpointId(endpointId), m_endpointInfo(endpointInfo), m_impl(nullptr), m_retryTimer(new QTimer(this))
{
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_impl && m_impl->hasActiveSession()) {
            m_retryTimer->start(10000);
            return;
        }
        delete m_impl;
        createImpl();
        m_impl->start();
    });
    createImpl();
}
BluetoothReceiver::~BluetoothReceiver() = default;
void BluetoothReceiver::start() { m_impl->start(); }
void BluetoothReceiver::createImpl()
{
    m_impl = new BluetoothReceiverImpl(this, m_endpointId, m_endpointInfo);
    connect(m_impl, &BluetoothReceiverImpl::failed, this, [this] {
        if (!m_retryTimer->isActive())
            m_retryTimer->start(10000);
    });
}

#include "bluetoothreceiver.moc"
