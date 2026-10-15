# Протокол Google Quick Share (Nearby Share) — приёмная сторона

Документ описывает протокол так, как он реализован в KQuickShare. Источники:
[NearDrop](https://github.com/grishka/NearDrop) (PROTOCOL.md + Swift-код) и
[rquickshare](https://github.com/Martichou/rquickshare) (Rust-код). Описан только
транспорт Wi-Fi LAN (mDNS + TCP); BLE/Wi-Fi Direct не используются.

## Роли

- Приёмник — **сервер**: слушает TCP-порт, анонсирует mDNS-сервис.
- Отправитель (Android) — **клиент**: находит сервис, подключается по TCP.

Передача однонаправленная: клиент шлёт, сервер принимает.

## Обнаружение (mDNS)

Сервис: `_FC9F5ED42C8A._tcp.local.` (идентификатор — первые байты
`SHA256("NearbySharing")`), порт — произвольный TCP-порт сервера.

**Имя инстанса** — base64url (без padding) от 10 байт:

| Байты | Значение |
|-------|----------|
| 1     | `0x23` («PCP») |
| 4     | endpoint ID — 4 случайных символа `[0-9a-zA-Z]` |
| 3     | `0xFC 0x9F 0x5E` (service ID) |
| 2     | `0x00 0x00` |

**TXT-запись**: ключ `n`, значение — base64url (без padding) от «endpoint info»:

| Поле | Описание |
|------|----------|
| 1 байт | битовое поле: version(3) \| visibility(1, 0=видимый) \| deviceType(3) \| reserved(1). На практике = `deviceType << 1`. Типы: 0=unknown, 1=phone, 2=tablet, 3=laptop, 4=car, 5=foldable, 6=XR |
| 16 байт | случайные (salt + encrypted metadata key; без Google-серверов бессмысленны) |
| 1 байт + N байт | длина + имя устройства в UTF-8 (только если видимый) |
| TLV | опционально: type 1 = QR code data, type 2 = vendor ID |

Тот же формат «endpoint info» (без base64) приходит потом в `ConnectionRequestFrame.endpoint_info`.

Замечание из rquickshare: Android иногда не видит сервис, если он был запущен
*до* открытия шторки Quick Share. Лечится периодическими повторными анонсами
(мы шлём unsolicited announcement каждые 60 с) и ответами на query.

## Кадрирование TCP

Каждое protobuf-сообщение предваряется 4-байтовым big-endian целым — длиной
полезной нагрузки. Ограничение NearDrop: 5 МБ максимум на кадр.

## Общая последовательность (POV сервера)

```
Client -> Server: OfflineFrame CONNECTION_REQUEST          (plaintext)
Client -> Server: Ukey2Message CLIENT_INIT                 (plaintext)
Server -> Client: Ukey2Message SERVER_INIT                 (plaintext)
Client -> Server: Ukey2Message CLIENT_FINISH               (plaintext)
Client -> Server: OfflineFrame CONNECTION_RESPONSE         (plaintext)
Server -> Client: OfflineFrame CONNECTION_RESPONSE(accept) (plaintext)
--- далее всё внутри SecureMessage (AES-256-CBC + HMAC-SHA256) ---
Server -> Client: Sharing_Frame PAIRED_KEY_ENCRYPTION      (bytes-payload)
Client -> Server: PAIRED_KEY_ENCRYPTION
Server -> Client: PAIRED_KEY_RESULT(status=UNABLE)
Client -> Server: PAIRED_KEY_RESULT
Client -> Server: INTRODUCTION (список файлов)
       Server: спрашивает пользователя (показывает PIN)
Server -> Client: RESPONSE (ACCEPT / REJECT)
Client -> Server: FILE-payloads (чанки)
Client -> Server: (пустой LAST_CHUNK на файл)
Server -> Client: OfflineFrame DISCONNECTION
```

Служебное: клиент шлёт OfflineFrame KEEP_ALIVE ~раз в 10 с; сервер отвечает
KEEP_ALIVE с `ack=true` внутри шифрованного канала (или plaintext, если
шифрование ещё не установлено).

## UKEY2 (обмен ключами)

Протокол — Google UKEY2 (github.com/google/ukey2), режим `P256_SHA512`:
ECDH на NIST P-256, коммитмент = SHA-512.

1. **CLIENT_INIT** (`Ukey2Message{message_type=2, message_data=Ukey2ClientInit}`):
   - `version` == 1
   - `random` == 32 байта
   - `cipher_commitments[]`: ищем `handshake_cipher == 100 (P256_SHA512)`,
     запоминаем `commitment` (64 байта)
   - `next_protocol` == `"AES_256_CBC-HMAC_SHA256"`
   - **Сырое тело сообщения (без 4-байтовой длины) сохраняем** — нужно для
     вывода ключей.
2. Сервер генерирует пару P-256 и шлёт **SERVER_INIT**
   (`Ukey2Message{message_type=3, message_data=Ukey2ServerInit}`):
   - `version=1`, `random=32 байта`, `handshake_cipher=100`
   - `public_key` = сериализованный `GenericPublicKey{type=EC_P256(1),
     ec_p256_public_key{x, y}}`. Координаты — big-endian **two's complement**
     (обрезать ведущие нули; если старший бит установлен — добавить `0x00`,
     т.е. 33 байта).
   - Сырое сериализованное сообщение тоже сохраняем.
3. **CLIENT_FINISH**: проверяем `SHA512(сырое_сообщение) == commitment`.
   Парсим `Ukey2ClientFinished.public_key` → `GenericPublicKey` → x, y
   (если длиннее 32 — берём последние 32 байта).

### Вывод ключей

```
dhs          = SHA256( X-координата ECDH(наш_priv, их_pub) )   // 32 байта
ukeyInfo     = rawClientInit || rawServerInit
authString   = HKDF-SHA256(ikm=dhs, salt="UKEY2 v1 auth", info=ukeyInfo, 32)
nextSecret   = HKDF-SHA256(ikm=dhs, salt="UKEY2 v1 next", info=ukeyInfo, 32)

saltD2D      = 82AA55A0D397F88346CA1CEE8D3909B95F13FA7DEB1D4AB38376B8256DA85510  // SHA256("D2D")
d2dClientKey = HKDF(ikm=nextSecret, salt=saltD2D, info="client", 32)
d2dServerKey = HKDF(ikm=nextSecret, salt=saltD2D, info="server", 32)

smsgSalt     = SHA256("SecureMessage")
clientEnc    = HKDF(d2dClientKey, smsgSalt, "ENC:2", 32)
clientSig    = HKDF(d2dClientKey, smsgSalt, "SIG:1", 32)
serverEnc    = HKDF(d2dServerKey, smsgSalt, "ENC:2", 32)
serverSig    = HKDF(d2dServerKey, smsgSalt, "SIG:1", 32)

// Сервер:
decryptKey = clientEnc; recvHmacKey = clientSig
encryptKey = serverEnc; sendHmacKey = serverSig
```

### PIN-код

Из `authString` (показывается на обоих устройствах; у нас — в диалоге принятия):

```
hash = 0; multiplier = 1
для каждого байта b (ИНТЕРПРЕТИРУЕТСЯ КАК signed int8!):
    hash       = (hash + b * multiplier) mod 9973
    multiplier = (multiplier * 31) mod 9973
PIN = format("%04d", abs(hash))
```

## Уровень шифрования (securemessage + securegcm)

Каждое сообщение после хендшейка — `SecureMessage`:

```
SecureMessage {
  header_and_body = serialize(HeaderAndBody {
    header = Header {
      signature_scheme = HMAC_SHA256(1),
      encryption_scheme = AES_256_CBC(2),
      iv = 16 случайных байт,
      public_metadata = serialize(GcmMetadata { type = DEVICE_TO_DEVICE_MESSAGE(13), version = 1 })
    },
    body = AES-256-CBC-PKCS7(encryptKey, iv, serialize(DeviceToDeviceMessage {
             sequence_number = ++seq,      // свои счётчики у клиента и сервера, с 1
             message = serialize(OfflineFrame)
           }))
  }),
  signature = HMAC-SHA256(sendHmacKey, header_and_body)
}
```

Приём: проверка HMAC (recvHmacKey) → расшифровка (decryptKey) → проверка
`sequence_number == ++expectedSeq` → разбор OfflineFrame.

## Уровень payload (передача данных)

Все содержательные сообщения — OfflineFrame типа `PAYLOAD_TRANSFER(3)` с
`PayloadTransferFrame`:

- `payload_header`: `id` (int64, идентификатор payload в соединении),
  `type` (1=BYTES, 2=FILE), `total_size`.
- `payload_chunk`: `flags` (бит 0 = LAST_CHUNK), `offset`, `body`.

Особенность Android: каждое служебное сообщение шлётся двумя кадрами — первый
с данными, второй пустой с LAST_CHUNK. BYTES-payload'ы собираются в буфер по
`offset`; содержимое служебных — сериализованный `sharing.nearby.Frame`.
FILE-payload'ы пишутся в файл по `offset` (сверяем с ожидаемым).

## Служебные sharing-кадры (wire_format.proto)

Порядок (сервер): после plaintext CONNECTION_RESPONSE сервер первым шлёт
`PAIRED_KEY_ENCRYPTION{secret_id_hash: 6 rnd, signed_data: 72 rnd}` →
клиент отвечает своим `PAIRED_KEY_ENCRYPTION` → сервер шлёт
`PAIRED_KEY_RESULT{status=UNABLE(3)}` → клиент шлёт `PAIRED_KEY_RESULT` →
клиент шлёт `INTRODUCTION` (FileMetadata: name, payload_id, size, mime_type;
TextMetadata для текста/URL) → сервер спрашивает пользователя и шлёт
`RESPONSE{status=ACCEPT(1)|REJECT(2)|NOT_ENOUGH_SPACE(3)|
UNSUPPORTED_ATTACHMENT_TYPE(4)|TIMED_OUT(5)}`.

## Таймауты и завершение

- Все файлы получены (LAST_CHUNK по каждому payload_id) → сервер шлёт
  OfflineFrame DISCONNECTION и закрывает сокет.
- Отмена клиентом: Sharing_Frame v1.type == CANCEL(6) или разрыв TCP —
  незавершённые файлы удаляем.
- Отклонение: RESPONSE{REJECT} + DISCONNECTION.
