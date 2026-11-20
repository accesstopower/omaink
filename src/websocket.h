#pragma once

#include <QByteArray>

// The small part of RFC 6455 the tablet connection needs, so the app doesn't
// pull in another Qt module for one socket.
namespace WebSocket {

enum Opcode : quint8 { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

struct Frame {
    quint8 opcode = 0;
    bool fin = true;
    QByteArray payload;
};

enum class Parse { Ok, Incomplete, Error };

// The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key.
QByteArray acceptKey(const QByteArray &key);

// An unmasked frame, as a server sends it.
QByteArray encode(quint8 opcode, const QByteArray &payload);

// Takes one frame off the front of the buffer. Client frames must be masked,
// and anything larger than maxPayload is refused rather than buffered.
Parse take(QByteArray &buffer, Frame &frame, qint64 maxPayload);

}
