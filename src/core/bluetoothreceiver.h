#pragma once

#include <QObject>

class BleTransport;
class BluetoothReceiverImpl;
class QTimer;

// Optional BlueZ receiver. A missing, powered-off, or unsupported adapter does
// not prevent the LAN receiver from working.
class BluetoothReceiver : public QObject
{
    Q_OBJECT
public:
    BluetoothReceiver(const QByteArray &endpointId, const QByteArray &endpointInfo, QObject *parent = nullptr);
    ~BluetoothReceiver() override;
    void start();

signals:
    void newConnection(BleTransport *transport);

private:
    void createImpl();
    QByteArray m_endpointId;
    QByteArray m_endpointInfo;
    BluetoothReceiverImpl *m_impl;
    QTimer *m_retryTimer;
};
