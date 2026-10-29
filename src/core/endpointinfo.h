// Endpoint info: the structure carried in the mDNS TXT record (base64url)
// and in ConnectionRequestFrame.endpoint_info (raw).
#pragma once

#include <QByteArray>
#include <QString>

enum class DeviceType : int {
    Unknown = 0,
    Phone = 1,
    Tablet = 2,
    Laptop = 3,
};

struct EndpointInfo
{
    DeviceType deviceType = DeviceType::Unknown;
    QString name; // empty if the device is hidden
    QByteArray qrCodeData;

    // byte0 = deviceType << 1 (version 0, visible), 16 random bytes, len + name
    static QByteArray serialize(const QString &name, DeviceType type);
    static bool parse(const QByteArray &data, EndpointInfo &out);
};

// URL-safe base64 without padding (as used by Android/rquickshare).
QByteArray base64UrlEncode(const QByteArray &data);
QByteArray base64UrlDecode(const QByteArray &data);
