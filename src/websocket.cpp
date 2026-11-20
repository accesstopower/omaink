#include "websocket.h"

#include <QCryptographicHash>
#include <QtEndian>

namespace WebSocket {

QByteArray acceptKey(const QByteArray &key) {
    return QCryptographicHash::hash(key.trimmed() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                    QCryptographicHash::Sha1)
        .toBase64();
}

QByteArray encode(quint8 opcode, const QByteArray &payload) {
    QByteArray frame;
    frame.append(char(0x80 | (opcode & 0x0F)));
    const quint64 length = quint64(payload.size());
    if (length < 126) {
        frame.append(char(length));
    } else if (length <= 0xFFFF) {
        frame.append(char(126));
        quint16 big = qToBigEndian(quint16(length));
        frame.append(reinterpret_cast<const char *>(&big), 2);
    } else {
        frame.append(char(127));
        quint64 big = qToBigEndian(length);
        frame.append(reinterpret_cast<const char *>(&big), 8);
    }
    return frame + payload;
}

Parse take(QByteArray &buffer, Frame &frame, qint64 maxPayload) {
    if (buffer.size() < 2)
        return Parse::Incomplete;
    const auto *data = reinterpret_cast<const uchar *>(buffer.constData());
    const bool fin = data[0] & 0x80;
    const quint8 opcode = data[0] & 0x0F;
    const bool masked = data[1] & 0x80;
    if ((data[0] & 0x70) || !masked)
        return Parse::Error;

    quint64 length = data[1] & 0x7F;
    qsizetype offset = 2;
    if (length == 126) {
        if (buffer.size() < 4)
            return Parse::Incomplete;
        length = qFromBigEndian<quint16>(data + 2);
        offset = 4;
    } else if (length == 127) {
        if (buffer.size() < 10)
            return Parse::Incomplete;
        length = qFromBigEndian<quint64>(data + 2);
        offset = 10;
    }
    if (length > quint64(maxPayload))
        return Parse::Error;
    // Control frames are short and never fragmented.
    if ((opcode & 0x8) && (length > 125 || !fin))
        return Parse::Error;
    if (buffer.size() < offset + 4 + qsizetype(length))
        return Parse::Incomplete;

    const uchar *mask = data + offset;
    offset += 4;
    QByteArray payload(qsizetype(length), Qt::Uninitialized);
    for (qsizetype i = 0; i < qsizetype(length); ++i)
        payload[i] = char(data[offset + i] ^ mask[i % 4]);
    buffer.remove(0, offset + qsizetype(length));

    frame.opcode = opcode;
    frame.fin = fin;
    frame.payload = payload;
    return Parse::Ok;
}

}
