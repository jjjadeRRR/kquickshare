#pragma once

#include <QByteArray>

// Nearby Connections FEF3 service data. endpointInfo is the raw mDNS endpoint
// info. No classic Bluetooth address is advertised because this receiver only
// implements the BLE socket; claiming a classic address makes peers try it first.
QByteArray bleAdvertisement(const QByteArray &endpointId, QByteArray endpointInfo, const QByteArray &deviceToken);
