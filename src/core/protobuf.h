// Minimal protobuf wire-format codec for the Quick Share protocol.
// Supports varint, fixed64, length-delimited and fixed32 fields.
// Not a general protobuf replacement — just enough for our message set.
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>

namespace pb
{

enum WireType : uint8_t {
    WT_Varint = 0,
    WT_Fixed64 = 1,
    WT_LengthDelimited = 2,
    WT_Fixed32 = 5,
};

struct Field
{
    uint32_t number = 0;
    uint8_t wireType = 0;
    uint64_t varint = 0; // valid for WT_Varint/WT_Fixed32/WT_Fixed64
    QByteArray bytes; // valid for WT_LengthDelimited
};

// Parses a whole protobuf message. Returns false on malformed input.
bool parseMessage(const QByteArray &data, QVector<Field> &out);

// Field lookup helpers (index = n-th occurrence of the field number).
bool fieldVarint(const QVector<Field> &fields, uint32_t number, uint64_t &value, int index = 0);
bool fieldBytes(const QVector<Field> &fields, uint32_t number, QByteArray &value, int index = 0);
bool fieldString(const QVector<Field> &fields, uint32_t number, QString &value, int index = 0);
QVector<QByteArray> allBytesFields(const QVector<Field> &fields, uint32_t number);

// Builds a protobuf message field by field.
class Writer
{
public:
    Writer &varint(uint32_t number, uint64_t value);
    Writer &svarint64(uint32_t number, int64_t value); // two's complement varint
    Writer &bytes(uint32_t number, const QByteArray &value);
    Writer &string(uint32_t number, const QString &value);
    const QByteArray &data() const
    {
        return m_data;
    }

private:
    QByteArray m_data;
};

void appendVarint(QByteArray &out, uint64_t value);

} // namespace pb
