// Minimal embedded mDNS responder (RFC 6762 subset).
// Advertises the Quick Share service on 224.0.0.251:5353 without avahi-daemon:
// answers PTR/SRV/TXT/A queries and sends periodic unsolicited announcements.
#pragma once

#include <QHostAddress>
#include <QObject>

class QUdpSocket;
class QTimer;

class MdnsResponder : public QObject {
    Q_OBJECT
public:
    // serviceType: "_FC9F5ED42C8A._tcp.local"
    // instanceName: base64url-encoded endpoint id blob
    // hostName: e.g. "mypc.local" (must resolve to ipv4)
    MdnsResponder(QString serviceType, QString instanceName, QString hostName,
                  quint16 port, const QByteArray &txtValue, QObject *parent = nullptr);
    ~MdnsResponder() override;

    bool start();
    void stop();

private:
    void onReadyRead();
    void announce();
    QByteArray buildResponse(quint32 ttl = 120) const;
    void sendResponse(const QHostAddress &to, quint16 port);
    static QString decodeName(const QByteArray &packet, int &pos, bool *ok);
    static void encodeName(QByteArray &out, const QString &name);

    QString m_serviceType;
    QString m_instanceFqdn;
    QString m_hostName;
    quint16 m_port = 0;
    QByteArray m_txtValue;
    QHostAddress m_ipv4;
    QUdpSocket *m_socket = nullptr;
    QTimer *m_announceTimer = nullptr;
    int m_startupAnnounces = 0;
};
