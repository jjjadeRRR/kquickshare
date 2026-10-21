#pragma once
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>
struct NearbyDevice {
    QString id, name;
    QHostAddress address;
    quint16 port = 0;
};
class DeviceDiscovery : public QObject {
    Q_OBJECT
  public:
    explicit DeviceDiscovery(QObject *parent = nullptr);
    bool start();
    void stop();
    void setQrKey(const QByteArray &key);
    QList<NearbyDevice> devices() const;
    static bool decodeName(const QByteArray &packet, int &pos, QString &name);
    void ingest(const QByteArray &packet); // Also used by deterministic packet tests.
  signals:
    void changed();
    void errorOccurred(const QString &error);

  private:
    struct Record {
        QString host, name;
        quint16 port = 0;
        qint64 expires = 0;
        bool phone = true;
    };
    void query();
    void question(const QString &name, quint16 type);
    void publish();
    QUdpSocket m_socket;
    QTimer m_timer;
    QHash<QString, Record> m_services;
    QHash<QString, QHostAddress> m_hosts;
    QList<NearbyDevice> m_devices;
    QByteArray m_qrToken, m_nameKey;
};
