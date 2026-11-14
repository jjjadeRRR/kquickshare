#include "bleadvertisement.h"

QByteArray bleAdvertisement(const QByteArray &endpointId, QByteArray endpointInfo, const QByteArray &deviceToken)
{
    if (endpointId.size() != 4 || endpointInfo.size() < 18 || deviceToken.size() != 2)
        return {};
    // Connections' non-fast advertisement limits endpoint info to 131 bytes.
    // Shorten only at a UTF-8 code point boundary.
    if (endpointInfo.size() > 131) {
        qsizetype end = 131;
        while (end > 18 && (quint8(endpointInfo[end]) & 0xc0) == 0x80)
            --end;
        endpointInfo.truncate(end);
        endpointInfo[17] = char(end - 18);
    }
    endpointInfo[0] = char(quint8(endpointInfo[0]) | 0x20); // version 1
    const QByteArray serviceHash = QByteArray::fromHex("fc9f5e");
    QByteArray inner = QByteArray::fromHex("23") + serviceHash + endpointId;
    inner.append(char(endpointInfo.size()));
    inner += endpointInfo;
    inner += QByteArray(6, '\0'); // no classic Bluetooth transport
    inner += QByteArray(2, '\0'); // no UWB address, no WebRTC
    QByteArray result = QByteArray::fromHex("48") + serviceHash;
    const quint32 length = inner.size();
    result.append(char(length >> 24));
    result.append(char(length >> 16));
    result.append(char(length >> 8));
    result.append(char(length));
    result += inner;
    result += deviceToken;
    return result;
}
