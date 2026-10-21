// OpenSSL 3 wrappers for the Quick Share crypto stack:
// P-256 key generation, ECDH, HKDF-SHA256, HMAC-SHA256, AES-256-CBC, SHA-256/512.
#pragma once

#include <QByteArray>
#include <memory>
#include <openssl/evp.h>

namespace crypto {

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

// Generates a new NIST P-256 key pair. nullptr on failure.
EvpPkeyPtr generateP256();

// Public key coordinates as unsigned big-endian, left-padded to exactly 32 bytes.
bool p256PublicXY(EVP_PKEY *key, QByteArray &x, QByteArray &y);

// ECDH shared secret (X coordinate, 32 bytes) with a peer public key given as
// big-endian X/Y (33-byte signed two's complement is handled: last 32 bytes used).
QByteArray ecdhSharedX(EVP_PKEY *ourKey, QByteArray peerX, QByteArray peerY);

QByteArray sha256(const QByteArray &data);
QByteArray sha512(const QByteArray &data);
QByteArray hmacSha256(const QByteArray &key, const QByteArray &data);
QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int outLen);

// AES-256-CBC with PKCS#7 padding. Decrypt returns empty array on failure.
QByteArray aes256CbcEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &plain);
QByteArray aes256CbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &cipher);

QByteArray signP256(EVP_PKEY *key, const QByteArray &message);
QByteArray aes128GcmDecrypt(const QByteArray &key, const QByteArray &combined, const QByteArray &aad);
QByteArray randomBytes(int n);

// Converts a 32-byte big-endian coordinate to protobuf "signed bytes"
// (strip leading zeros, prepend 0x00 if the top bit is set).
QByteArray toSignedBytes(QByteArray coord);

} // namespace crypto
