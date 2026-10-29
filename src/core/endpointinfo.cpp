#include "endpointinfo.h"

#include "crypto.h"

QByteArray EndpointInfo::serialize(const QString &name, DeviceType type)
{
    QByteArray out;
    out.append(char(int(type) << 1));
    out.append(crypto::randomBytes(16));
    QByteArray nameUtf8 = name.toUtf8();
    // TXT strings are limited to 255 bytes including "n=" and base64.
    // Keep endpoint info <= 189 bytes and preserve complete UTF-8 codepoints.
    if (nameUtf8.size() > 168) {
        int end = 168;
        while (end > 0 && (quint8(nameUtf8[end]) & 0xc0) == 0x80) --end;
        nameUtf8.truncate(end);
    }
    out.append(char(nameUtf8.size()));
    out.append(nameUtf8);
    return out;
}

bool EndpointInfo::parse(const QByteArray &data, EndpointInfo &out)
{
    if (data.size() < 17)
        return false;
    const uint8_t b0 = uint8_t(data[0]);
    const bool hasName = (b0 & 0x10) == 0; // visibility bit: 0 = visible
    out.deviceType = DeviceType((b0 >> 1) & 7);
    int offset = 17;
    if (hasName) {
        if (data.size() < 18) return false;
        const int nameLen = uint8_t(data[offset]);
        if (data.size() < offset + 1 + nameLen)
            return false;
        out.name = QString::fromUtf8(data.mid(offset + 1, nameLen));
        offset += 1 + nameLen;
    }
    // TLV records (type 1 = QR code data)
    while (data.size() - offset >= 2) {
        const uint8_t type = uint8_t(data[offset]);
        const int len = uint8_t(data[offset + 1]);
        offset += 2;
        if (data.size() - offset < len)
            break;
        if (type == 1)
            out.qrCodeData = data.mid(offset, len);
        offset += len;
    }
    return true;
}

QByteArray base64UrlEncode(const QByteArray &data)
{
    QByteArray out = data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return out;
}

QByteArray base64UrlDecode(const QByteArray &data)
{
    return QByteArray::fromBase64(data, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}
