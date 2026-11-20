#include "padserver.h"

#include "websocket.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTcpSocket>
#include <QtEndian>
#include <QUrlQuery>
#include <algorithm>

namespace {

constexpr qint64 maxHeader = 16 * 1024;
constexpr qint64 maxMessage = 2 * 1024 * 1024;
// A frame is skipped for a tablet that hasn't taken the last ones yet, so a
// slow Wi-Fi link shows a lower frame rate instead of a growing delay.
constexpr qint64 maxBacklog = 1536 * 1024;
constexpr int frameIntervalMs = 110;

bool sameKey(const QString &a, const QString &b) {
    // Compares every character either way, so timing can't reveal the key.
    if (a.size() != b.size() || a.isEmpty())
        return false;
    int difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        difference |= a.at(i).unicode() ^ b.at(i).unicode();
    return difference == 0;
}

QVector<InkPoint> pointsFrom(const QJsonValue &value) {
    QVector<InkPoint> points;
    for (const auto &item : value.toArray()) {
        const auto point = item.toArray();
        if (point.size() >= 2)
            points.append({point.at(0).toDouble(), point.at(1).toDouble(),
                           std::clamp(point.size() > 2 ? point.at(2).toDouble(0.5) : 0.5, 0.0, 1.0)});
    }
    return points;
}

bool isPrivate(const QHostAddress &address) {
    const quint32 ip = address.toIPv4Address();
    return (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168);
}

} // namespace

PadServer::PadServer(InkModel *model, QObject *parent)
    : QObject(parent), m_model(model), m_grimPath(QStandardPaths::findExecutable("grim")) {
    connect(&m_server, &QTcpServer::newConnection, this, &PadServer::onConnection);

    connect(m_model, &InkModel::changed, this, [this] { ++m_inkVersion; });
    connect(m_model, &InkModel::committed, this, [this](const StrokePtr &stroke) {
        // Frames captured from here on show this stroke.
        ++m_inkVersion;
        broadcast({{"t", "add"}, {"stroke", stroke->toJson()}, {"v", qint64(m_inkVersion)}});
    });
    connect(m_model, &InkModel::removed, this, [this](const QStringList &ids) {
        broadcast({{"t", "remove"}, {"ids", QJsonArray::fromStringList(ids)}});
    });
    connect(m_model, &InkModel::reset, this, [this] {
        for (auto *client : std::as_const(m_clients))
            if (client->upgraded)
                sendText(*client, stateMessage(*client));
    });
    connect(m_model, &InkModel::boardChanged, this, [this] {
        broadcast({{"t", "board"}, {"board", m_model->board()}});
    });

    m_mirrorTimer.setSingleShot(true);
    connect(&m_mirrorTimer, &QTimer::timeout, this, &PadServer::capture);
    connect(&m_grim, &QProcess::finished, this, &PadServer::captureFinished);
    connect(&m_grim, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            captureFinished(-1, QProcess::CrashExit);
    });
}

PadServer::~PadServer() {
    if (m_grim.state() != QProcess::NotRunning) {
        m_grim.kill();
        m_grim.waitForFinished(500);
    }
    // Sockets close as the server is torn down; their handlers must not run
    // against clients that are already gone.
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it)
        QObject::disconnect(it.key(), nullptr, this, nullptr);
    qDeleteAll(m_clients);
    m_clients.clear();
}

bool PadServer::listen(quint16 port) {
    if (m_server.listen(QHostAddress::Any, port))
        return true;
    m_error = m_server.serverError() == QAbstractSocket::AddressInUseError
                  ? tr("Port %1 is already in use. Start with --port to pick another.").arg(port)
                  : m_server.errorString();
    return false;
}

QString PadServer::loadToken(const QString &path, bool reset, QString *error) {
    QFile file(path);
    if (!reset && file.open(QIODevice::ReadOnly)) {
        const QString token = QString::fromLatin1(file.readAll()).trimmed();
        static const QRegularExpression valid("^[a-z0-9]{12,64}$");
        if (valid.match(token).hasMatch())
            return token;
    }

    const QString alphabet = "abcdefghijkmnpqrstuvwxyz23456789";
    QString token;
    for (int i = 0; i < 16; ++i)
        token += alphabet.at(QRandomGenerator::system()->bounded(int(alphabet.size())));

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
        out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        out.write(token.toLatin1() + "\n");
        if (!out.commit() && error)
            *error = out.errorString();
    } else if (error) {
        *error = out.errorString();
    }
    return token;
}

QStringList PadServer::addresses() {
    QStringList preferred, others;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            (isPrivate(address) ? preferred : others).append(address.toString());
        }
    }
    return preferred + others;
}

QString PadServer::localSubnet() {
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || !isPrivate(entry.ip()))
                continue;
            const int prefix = entry.prefixLength() > 0 ? entry.prefixLength() : 24;
            const quint32 mask = prefix >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> prefix);
            return QString("%1/%2").arg(QHostAddress(entry.ip().toIPv4Address() & mask).toString()).arg(prefix);
        }
    }
    return {};
}

QString PadServer::firewallFix(quint16 port, const QString &confPath, const QString &rulesPath,
                               const QString &subnet) {
    // Omarchy turns ufw on, and it drops a tablet's connection without a
    // reply, which Safari shows as a blank page. Both files are readable
    // without root, so the problem can be named before anyone is stuck.
    QFile conf(confPath);
    if (!conf.open(QIODevice::ReadOnly) ||
        !QString::fromUtf8(conf.readAll()).contains(QRegularExpression("^\\s*ENABLED\\s*=\\s*yes", QRegularExpression::MultilineOption)))
        return {};
    QFile rules(rulesPath);
    if (!rules.open(QIODevice::ReadOnly))
        return {};
    static const QRegularExpression tuple("^### tuple ### (allow|limit) (tcp|any) (\\S+) ", QRegularExpression::MultilineOption);
    for (auto it = tuple.globalMatch(QString::fromUtf8(rules.readAll())); it.hasNext();) {
        const QString ports = it.next().captured(3);
        for (const auto &part : ports.split(',')) {
            const auto range = part.split(':');
            const uint low = range.first().toUInt(), high = range.last().toUInt();
            if (part == "any" || (low <= port && port <= high))
                return {};
        }
    }
    return subnet.isEmpty() ? QString("sudo ufw allow %1/tcp comment omaink").arg(port)
                            : QString("sudo ufw allow from %1 to any port %2 proto tcp comment omaink").arg(subnet).arg(port);
}

QString PadServer::url(const QString &address, quint16 port, const QString &token) {
    return QString("http://%1:%2/pad?k=%3").arg(address).arg(port).arg(token);
}

int PadServer::clientCount() const {
    return int(std::count_if(m_clients.begin(), m_clients.end(), [](const Client *c) { return c->upgraded; }));
}

void PadServer::setOutput(const QString &name, int logicalWidth, int physicalWidth, double aspect) {
    const bool changed = name != m_output || std::abs(aspect - m_aspect) > 1e-3;
    m_output = name;
    m_logicalWidth = std::max(320, logicalWidth);
    m_physicalWidth = std::max(m_logicalWidth, physicalWidth);
    m_aspect = aspect;
    if (changed)
        broadcast({{"t", "aspect"}, {"aspect", m_aspect}, {"output", m_output}});
}

void PadServer::onConnection() {
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        auto *client = new Client;
        client->socket = socket;
        m_clients.insert(socket, client);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { onDisconnected(socket); });
    }
}

void PadServer::onDisconnected(QTcpSocket *socket) {
    Client *client = m_clients.take(socket);
    if (!client)
        return;
    const bool wasPad = client->upgraded;
    if (wasPad)
        m_model->cancelPrefix(QString("c%1:").arg(client->id));
    // A read handler may still be holding this client when an abort lands
    // here, so free it once control is back in the event loop.
    QTimer::singleShot(0, this, [client] { delete client; });
    socket->deleteLater();
    if (wasPad)
        emit clientsChanged();
}

void PadServer::onReadyRead(QTcpSocket *socket) {
    Client *client = m_clients.value(socket);
    if (!client)
        return;
    client->buffer += socket->readAll();
    if (client->upgraded) {
        handleFrames(*client);
        return;
    }
    const qsizetype end = client->buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (client->buffer.size() > maxHeader)
            socket->abort();
        return;
    }
    const QByteArray head = client->buffer.left(end);
    client->buffer.remove(0, end + 4);
    handleRequest(*client, head);
}

void PadServer::respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
                        const QByteArray &extraHeaders) {
    static const QHash<int, QByteArray> reasons{
        {200, "OK"}, {302, "Found"}, {403, "Forbidden"}, {404, "Not Found"}, {405, "Method Not Allowed"}};
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + reasons.value(status, "Error") + "\r\n";
    response += "Content-Type: " + type + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-cache\r\nConnection: close\r\n" + extraHeaders + "\r\n";
    socket->write(response + body);
    socket->disconnectFromHost();
}

void PadServer::handleRequest(Client &client, const QByteArray &head) {
    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const qsizetype colon = lines[i].indexOf(':');
        if (colon > 0)
            headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
    }
    if (request.size() < 3 || request[0] != "GET") {
        respond(client.socket, 405, "text/plain", "GET only\n");
        return;
    }
    const QUrl target(QString::fromUtf8("http://pad" + request[1]));
    const QString path = target.path();
    const QString key = QUrlQuery(target).queryItemValue("k");

    if (path == "/ws") {
        if (!sameKey(key, m_token) || headers.value("upgrade").toLower() != "websocket" ||
            !headers.contains("sec-websocket-key")) {
            respond(client.socket, 403, "text/plain", "This pad link is missing its key. Run `omaink pad` on the computer.\n");
            return;
        }
        client.socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " +
                             WebSocket::acceptKey(headers.value("sec-websocket-key")) + "\r\n\r\n");
        client.upgraded = true;
        client.id = m_nextId++;
        sendText(client, stateMessage(client));
        emit clientsChanged();
        handleFrames(client);
        return;
    }

    static const QHash<QString, QPair<QString, QByteArray>> files{
        {"/pad", {":/web/pad.html", "text/html; charset=utf-8"}},
        {"/pad.js", {":/web/pad.js", "text/javascript; charset=utf-8"}},
        {"/pad.css", {":/web/pad.css", "text/css; charset=utf-8"}},
    };
    if (path == "/") {
        respond(client.socket, 302, "text/plain", "", "Location: /pad" +
                (target.hasQuery() ? "?" + target.query().toUtf8() : QByteArray()) + "\r\n");
    } else if (path == "/icon.png") {
        respond(client.socket, 200, "image/png", icon());
    } else if (files.contains(path)) {
        QFile file(files[path].first);
        if (file.open(QIODevice::ReadOnly))
            respond(client.socket, 200, files[path].second, file.readAll());
        else
            respond(client.socket, 404, "text/plain", "Not found\n");
    } else {
        respond(client.socket, 404, "text/plain", "Not found\n");
    }
}

QByteArray PadServer::icon() {
    if (!m_icon.isEmpty())
        return m_icon;
    // iPadOS wants a PNG for the home screen, so render the app's SVG icon.
    QImage image(180, 180, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QSvgRenderer renderer(QString(":/web/icon.svg"));
    QPainter painter(&image);
    renderer.render(&painter);
    painter.end();
    QBuffer buffer(&m_icon);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return m_icon;
}

void PadServer::handleFrames(Client &client) {
    WebSocket::Frame frame;
    while (client.socket && client.socket->state() == QAbstractSocket::ConnectedState) {
        const auto result = WebSocket::take(client.buffer, frame, maxMessage);
        if (result == WebSocket::Parse::Incomplete)
            return;
        if (result == WebSocket::Parse::Error) {
            client.socket->abort();
            return;
        }
        switch (frame.opcode) {
        case WebSocket::Ping:
            client.socket->write(WebSocket::encode(WebSocket::Pong, frame.payload));
            break;
        case WebSocket::Close:
            client.socket->write(WebSocket::encode(WebSocket::Close, frame.payload.left(2)));
            client.socket->disconnectFromHost();
            return;
        case WebSocket::Text:
        case WebSocket::Binary:
            client.message = frame.payload;
            client.messageOpcode = frame.opcode;
            break;
        case WebSocket::Continuation:
            if (client.message.size() + frame.payload.size() > maxMessage) {
                client.socket->abort();
                return;
            }
            client.message += frame.payload;
            break;
        default:
            break;
        }
        if (frame.fin && (frame.opcode == WebSocket::Text || frame.opcode == WebSocket::Continuation) &&
            client.messageOpcode == WebSocket::Text) {
            const QByteArray text = client.message;
            client.message.clear();
            handleMessage(client, text);
        }
    }
}

void PadServer::handleMessage(Client &client, const QByteArray &text) {
    const QJsonObject message = QJsonDocument::fromJson(text).object();
    const QString type = message.value("t").toString();
    const QString prefix = QString("c%1:").arg(client.id);
    const QString id = prefix + message.value("id").toString();

    if (type == "begin") {
        QString error;
        auto stroke = Stroke::fromJson(message.value("stroke").toObject(), &error);
        if (!stroke) {
            sendText(client, {{"t", "error"}, {"message", error}});
            return;
        }
        stroke->id = prefix + stroke->id;
        m_model->begin(stroke);
    } else if (type == "move") {
        m_model->extend(id, pointsFrom(message.value("pts")));
    } else if (type == "end") {
        m_model->end(id, QDateTime::currentMSecsSinceEpoch());
    } else if (type == "cancel") {
        m_model->cancel(id);
    } else if (type == "undo") {
        m_model->undo();
    } else if (type == "redo") {
        m_model->redo();
    } else if (type == "clear") {
        m_model->clear();
    } else if (type == "board") {
        if (message.contains("board"))
            m_model->setBoard(message.value("board").toString());
        else
            m_model->cycleBoard();
    } else if (type == "mirror") {
        client.mirror = message.value("on").toBool();
        scheduleMirror();
    }
}

void PadServer::sendText(Client &client, const QJsonObject &message) {
    if (client.upgraded && client.socket->state() == QAbstractSocket::ConnectedState)
        client.socket->write(WebSocket::encode(WebSocket::Text, QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void PadServer::broadcast(const QJsonObject &message) {
    const QByteArray frame = WebSocket::encode(WebSocket::Text, QJsonDocument(message).toJson(QJsonDocument::Compact));
    for (auto *client : std::as_const(m_clients))
        if (client->upgraded && client->socket->state() == QAbstractSocket::ConnectedState)
            client->socket->write(frame);
}

QJsonObject PadServer::stateMessage(const Client &client) const {
    QJsonArray strokes;
    for (const auto &stroke : m_model->strokes())
        strokes.append(stroke->toJson());
    return {{"t", "state"},
            {"you", QString("c%1:").arg(client.id)},
            {"strokes", strokes},
            {"board", m_model->board()},
            {"aspect", m_aspect},
            {"output", m_output},
            {"mirror", !m_grimPath.isEmpty()},
            {"mirrorError", m_grimPath.isEmpty() ? tr("Install grim to see the screen on the tablet.") : m_mirrorError}};
}

bool PadServer::anyMirror() const {
    return std::any_of(m_clients.begin(), m_clients.end(), [](const Client *c) { return c->upgraded && c->mirror; });
}

void PadServer::scheduleMirror() {
    if (!anyMirror() || m_grimPath.isEmpty() || m_grim.state() != QProcess::NotRunning || m_mirrorTimer.isActive())
        return;
    const qint64 elapsed = m_captureClock.isValid() ? m_captureClock.elapsed() : frameIntervalMs;
    m_mirrorTimer.start(int(std::max<qint64>(0, frameIntervalMs - elapsed)));
}

void PadServer::capture() {
    if (!anyMirror())
        return;
    // Tablets show the screen at most about 1600 pixels wide, so capture at
    // that size: smaller frames reach the tablet sooner over Wi-Fi. grim
    // scales from the screen's logical size, so a scaled screen can still
    // give up to its full hardware resolution.
    const double scale = std::min(double(m_physicalWidth) / m_logicalWidth, 1600.0 / m_logicalWidth);
    QStringList arguments{"-t", "jpeg", "-q", "62", "-s", QString::number(scale, 'f', 3)};
    if (!m_output.isEmpty())
        arguments << "-o" << m_output;
    arguments << "-";
    m_captureClock.start();
    m_captureVersion = m_inkVersion;
    m_grim.start(m_grimPath, arguments);
}

void PadServer::captureFinished(int code, QProcess::ExitStatus status) {
    const QByteArray jpeg = m_grim.readAllStandardOutput();
    const QString stderrText = QString::fromUtf8(m_grim.readAllStandardError()).trimmed();
    if (code == 0 && status == QProcess::NormalExit && !jpeg.isEmpty()) {
        if (!m_mirrorError.isEmpty()) {
            m_mirrorError.clear();
            broadcast({{"t", "mirror-error"}, {"message", ""}});
        }
        // Four bytes of ink version, big-endian, then the JPEG.
        QByteArray payload(4, Qt::Uninitialized);
        qToBigEndian(m_captureVersion, payload.data());
        const QByteArray frame = WebSocket::encode(WebSocket::Binary, payload + jpeg);
        for (auto *client : std::as_const(m_clients))
            if (client->upgraded && client->mirror && client->socket->bytesToWrite() < maxBacklog)
                client->socket->write(frame);
        scheduleMirror();
        return;
    }
    const QString message = tr("The screen mirror failed: %1")
                                .arg(stderrText.isEmpty() ? m_grim.errorString() : stderrText);
    if (message != m_mirrorError) {
        m_mirrorError = message;
        broadcast({{"t", "mirror-error"}, {"message", message}});
    }
    // Try again later; a screen that was switched off may come back.
    m_captureClock.start();
    if (anyMirror())
        m_mirrorTimer.start(3000);
}
