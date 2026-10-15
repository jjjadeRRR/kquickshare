#include "protobuf.h"

#include <QtEndian>

namespace pb {

void appendVarint(QByteArray &out, uint64_t value)
{
    while (value >= 0x80) {
        out.append(char((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.append(char(value));
}

static bool readVarint(const QByteArray &data, qsizetype &pos, uint64_t &value)
{
    value = 0;
    int shift = 0;
    while (pos < data.size() && shift < 64) {
        const uint8_t b = uint8_t(data[pos++]);
        value |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80))
            return true;
        shift += 7;
    }
    return false;
}

bool parseMessage(const QByteArray &data, QVector<Field> &out)
{
    qsizetype pos = 0;
    while (pos < data.size()) {
        uint64_t tag = 0;
        if (!readVarint(data, pos, tag))
            return false;
        Field f;
        f.number = uint32_t(tag >> 3);
        f.wireType = uint8_t(tag & 7);
        if (f.number == 0)
            return false;
        switch (f.wireType) {
        case WT_Varint:
            if (!readVarint(data, pos, f.varint))
                return false;
            break;
        case WT_Fixed64:
            if (pos + 8 > data.size())
                return false;
            f.varint = qFromBigEndian<quint64>(reinterpret_cast<const uchar *>(data.constData() + pos));
            pos += 8;
            break;
        case WT_LengthDelimited: {
            uint64_t len = 0;
            if (!readVarint(data, pos, len))
                return false;
            if (len > uint64_t(data.size() - pos))
                return false;
            f.bytes = data.mid(pos, qsizetype(len));
            pos += qsizetype(len);
            break;
        }
        case WT_Fixed32:
            if (pos + 4 > data.size())
                return false;
            f.varint = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(data.constData() + pos));
            pos += 4;
            break;
        default:
            return false; // groups etc. are not used in this protocol
        }
        out.append(f);
    }
    return true;
}

bool fieldVarint(const QVector<Field> &fields, uint32_t number, uint64_t &value, int index)
{
    for (const Field &f : fields) {
        if (f.number == number && f.wireType == WT_Varint) {
            if (index-- == 0) {
                value = f.varint;
                return true;
            }
        }
    }
    return false;
}

bool fieldBytes(const QVector<Field> &fields, uint32_t number, QByteArray &value, int index)
{
    for (const Field &f : fields) {
        if (f.number == number && f.wireType == WT_LengthDelimited) {
            if (index-- == 0) {
                value = f.bytes;
                return true;
            }
        }
    }
    return false;
}

bool fieldString(const QVector<Field> &fields, uint32_t number, QString &value, int index)
{
    QByteArray raw;
    if (!fieldBytes(fields, number, raw, index))
        return false;
    value = QString::fromUtf8(raw);
    return true;
}

QVector<QByteArray> allBytesFields(const QVector<Field> &fields, uint32_t number)
{
    QVector<QByteArray> result;
    for (const Field &f : fields)
        if (f.number == number && f.wireType == WT_LengthDelimited)
            result.append(f.bytes);
    return result;
}

Writer &Writer::varint(uint32_t number, uint64_t value)
{
    appendVarint(m_data, uint64_t(number) << 3 | WT_Varint);
    appendVarint(m_data, value);
    return *this;
}

Writer &Writer::svarint64(uint32_t number, int64_t value)
{
    return varint(number, quint64(value)); // sign-extended 64-bit two's complement
}

Writer &Writer::bytes(uint32_t number, const QByteArray &value)
{
    appendVarint(m_data, uint64_t(number) << 3 | WT_LengthDelimited);
    appendVarint(m_data, uint64_t(value.size()));
    m_data.append(value);
    return *this;
}

Writer &Writer::string(uint32_t number, const QString &value)
{
    return bytes(number, value.toUtf8());
}

} // namespace pb
