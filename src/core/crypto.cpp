#include "crypto.h"

#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace crypto {

EvpPkeyPtr generateP256()
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx)
        return EvpPkeyPtr(nullptr, &EVP_PKEY_free);
    EVP_PKEY *pkey = nullptr;
    if (EVP_PKEY_keygen_init(ctx) == 1
        && EVP_PKEY_CTX_set_group_name(ctx, "prime256v1") == 1)
        EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);
    return EvpPkeyPtr(pkey, &EVP_PKEY_free);
}

bool p256PublicXY(EVP_PKEY *key, QByteArray &x, QByteArray &y)
{
    unsigned char buf[65];
    size_t len = 0;
    if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, buf, sizeof(buf), &len) != 1
        || len != 65 || buf[0] != 0x04)
        return false;
    x = QByteArray(reinterpret_cast<char *>(buf + 1), 32);
    y = QByteArray(reinterpret_cast<char *>(buf + 33), 32);
    return true;
}

QByteArray ecdhSharedX(EVP_PKEY *ourKey, QByteArray peerX, QByteArray peerY)
{
    // Take last 32 bytes (peer may send 33-byte signed two's complement);
    // left-pad with zeros if shorter.
    auto validCoord = [](const QByteArray &v) {
        return !v.isEmpty() && v.size() <= 33 && !(quint8(v[0]) & 0x80)
            && (v.size() != 33 || v[0] == '\0');
    };
    if (!validCoord(peerX) || !validCoord(peerY)) return {};
    auto norm32 = [](QByteArray v) {
        if (v.size() > 32)
            return v.right(32);
        return v.rightJustified(32, '\0');
    };
    peerX = norm32(std::move(peerX));
    peerY = norm32(std::move(peerY));

    unsigned char pub[65];
    pub[0] = 0x04;
    memcpy(pub + 1, peerX.constData(), 32);
    memcpy(pub + 33, peerY.constData(), 32);

    EVP_PKEY *peerRaw = nullptr;
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME,
                                         const_cast<char *>("prime256v1"), 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, pub, sizeof(pub)),
        OSSL_PARAM_construct_end(),
    };
    EVP_PKEY_CTX *fromCtx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    if (!fromCtx)
        return {};
    if (EVP_PKEY_fromdata_init(fromCtx) == 1)
        EVP_PKEY_fromdata(fromCtx, &peerRaw, EVP_PKEY_PUBLIC_KEY, params);
    EVP_PKEY_CTX_free(fromCtx);
    if (!peerRaw)
        return {};
    EvpPkeyPtr peer(peerRaw, &EVP_PKEY_free);

    EVP_PKEY_CTX *deriveCtx = EVP_PKEY_CTX_new(ourKey, nullptr);
    if (!deriveCtx)
        return {};
    QByteArray secret(32, '\0');
    size_t secretLen = size_t(secret.size());
    const bool ok = EVP_PKEY_derive_init(deriveCtx) == 1
        && EVP_PKEY_derive_set_peer(deriveCtx, peer.get()) == 1
        && EVP_PKEY_derive(deriveCtx, reinterpret_cast<unsigned char *>(secret.data()), &secretLen) == 1;
    EVP_PKEY_CTX_free(deriveCtx);
    if (!ok)
        return {};
    secret.resize(int(secretLen));
    return secret;
}

QByteArray sha256(const QByteArray &data)
{
    QByteArray out(SHA256_DIGEST_LENGTH, '\0');
    SHA256(reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()),
           reinterpret_cast<unsigned char *>(out.data()));
    return out;
}

QByteArray sha512(const QByteArray &data)
{
    QByteArray out(SHA512_DIGEST_LENGTH, '\0');
    SHA512(reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()),
           reinterpret_cast<unsigned char *>(out.data()));
    return out;
}

QByteArray hmacSha256(const QByteArray &key, const QByteArray &data)
{
    QByteArray out(SHA256_DIGEST_LENGTH, '\0');
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.constData(), int(key.size()),
         reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()),
         reinterpret_cast<unsigned char *>(out.data()), &len);
    out.resize(int(len));
    return out;
}

QByteArray hkdfSha256(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, int outLen)
{
    // Manual extract + expand to avoid EVP_KDF plumbing.
    QByteArray saltCopy = salt;
    if (saltCopy.isEmpty())
        saltCopy = QByteArray(SHA256_DIGEST_LENGTH, '\0');
    const QByteArray prk = hmacSha256(saltCopy, ikm);

    QByteArray okm, t;
    int counter = 1;
    while (okm.size() < outLen) {
        QByteArray round = t + info;
        round.append(char(counter++));
        t = hmacSha256(prk, round);
        okm.append(t);
    }
    okm.resize(outLen);
    return okm;
}

QByteArray aes256CbcEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &plain)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    QByteArray out(plain.size() + EVP_MAX_BLOCK_LENGTH, '\0');
    int len1 = 0, len2 = 0;
    const bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                                       reinterpret_cast<const unsigned char *>(key.constData()),
                                       reinterpret_cast<const unsigned char *>(iv.constData())) == 1
        && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len1,
                             reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) == 1
        && EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + len1, &len2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    out.resize(len1 + len2);
    return out;
}

QByteArray aes256CbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &cipher)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    QByteArray out(cipher.size(), '\0');
    int len1 = 0, len2 = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
                                       reinterpret_cast<const unsigned char *>(key.constData()),
                                       reinterpret_cast<const unsigned char *>(iv.constData())) == 1
        && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &len1,
                             reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size()) == 1
        && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + len1, &len2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {}; // bad padding / corrupt data
    out.resize(len1 + len2);
    return out;
}

QByteArray signP256(EVP_PKEY *key, const QByteArray &message)
{
    auto *ctx = EVP_MD_CTX_new(); if (!ctx || !key) { EVP_MD_CTX_free(ctx); return {}; }
    size_t size = 0;
    bool ok = EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1
        && EVP_DigestSign(ctx, nullptr, &size, reinterpret_cast<const unsigned char *>(message.constData()), message.size()) == 1;
    QByteArray der(qsizetype(size), '\0');
    ok = ok && EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(der.data()), &size, reinterpret_cast<const unsigned char *>(message.constData()), message.size()) == 1;
    EVP_MD_CTX_free(ctx); if (!ok) return {};
    const auto *ptr = reinterpret_cast<const unsigned char *>(der.constData());
    ECDSA_SIG *sig = d2i_ECDSA_SIG(nullptr, &ptr, long(size)); if (!sig) return {};
    QByteArray raw(64, '\0');
    ok = BN_bn2binpad(ECDSA_SIG_get0_r(sig), reinterpret_cast<unsigned char *>(raw.data()), 32) == 32
        && BN_bn2binpad(ECDSA_SIG_get0_s(sig), reinterpret_cast<unsigned char *>(raw.data())+32, 32) == 32;
    ECDSA_SIG_free(sig); return ok ? raw : QByteArray();
}

QByteArray aes128GcmDecrypt(const QByteArray &key, const QByteArray &combined, const QByteArray &aad)
{
    if (key.size() != 16 || combined.size() < 28) return {};
    auto *ctx = EVP_CIPHER_CTX_new(); if (!ctx) return {};
    const QByteArray ciphertext = combined.mid(12, combined.size()-28);
    QByteArray output(ciphertext.size()+16, '\0');
    QByteArray tag = combined.right(16); int length = 0, count = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(combined.constData())) == 1
        && EVP_DecryptUpdate(ctx, nullptr, &length, reinterpret_cast<const unsigned char *>(aad.constData()), int(aad.size())) == 1
        && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(output.data()), &count, reinterpret_cast<const unsigned char *>(ciphertext.constData()), int(ciphertext.size())) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1
        && EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(output.data())+count, &length) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};
    output.resize(count+length); return output;
}

QByteArray randomBytes(int n)
{
    QByteArray out(n, '\0');
    RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), n);
    return out;
}

QByteArray toSignedBytes(QByteArray coord)
{
    // Strip redundant leading zero bytes (keep at least one byte)...
    int i = 0;
    while (i < coord.size() - 1 && coord[i] == '\0')
        ++i;
    coord = coord.mid(i);
    // ...then prepend 0x00 if the top bit is set (two's complement stays positive).
    if (uint8_t(coord[0]) & 0x80)
        coord.prepend('\0');
    return coord;
}

} // namespace crypto
