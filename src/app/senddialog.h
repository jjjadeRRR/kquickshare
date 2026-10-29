#pragma once
#include "crypto.h"
#include "devicediscovery.h"
#include <QDialog>
#include <QPointer>
class OutboundConnection;
class QListWidget;
class QLabel;
class QPushButton;
class QProgressBar;
class SendDialog : public QDialog
{
    Q_OBJECT
public:
    SendDialog(const QStringList &files, const QString &text, QWidget *parent = nullptr);
    ~SendDialog() override;
    QString status() const;

protected:
    void reject() override;

private:
    void updateDevices();
    void send();
    crypto::EvpPkeyPtr m_qrKey{nullptr, &EVP_PKEY_free};
    DeviceDiscovery m_discovery;
    QStringList m_files;
    QString m_text;
    QString m_lastError;
    bool m_shuttingDown = false;
    QList<NearbyDevice> m_devices;
    QListWidget *m_list;
    QLabel *m_status, *m_pin;
    QPushButton *m_send;
    QProgressBar *m_progress;
    QPointer<OutboundConnection> m_connection;
};
