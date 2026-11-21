
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "ink.h"
#include "inkrenderer.h"
#include "padserver.h"
#include "theme.h"
#include "translator.h"
#include "websocket.h"

namespace {

StrokePtr stroke(const QString &id, const QString &tool, QVector<InkPoint> pts) {
    auto s = StrokePtr::create();
    s->id = id;
    s->tool = tool;
    s->pts = pts;
    return s;
}

void draw(InkModel &model, const QString &id, const QString &tool, QVector<InkPoint> pts) {
    QVERIFY(model.begin(stroke(id, tool, {pts.first()})));
    model.extend(id, pts.mid(1));
    model.end(id, 1000);
}

// A client frame: always masked, as browsers send them.
QByteArray clientFrame(quint8 opcode, const QByteArray &payload) {
    QByteArray frame;
    frame.append(char(0x80 | opcode));
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    if (payload.size() < 126) {
        frame.append(char(0x80 | payload.size()));
    } else {
        frame.append(char(0x80 | 126));
        frame.append(char(payload.size() >> 8));
        frame.append(char(payload.size() & 0xFF));
    }
    frame.append(mask, 4);
    for (int i = 0; i < payload.size(); ++i)
        frame.append(char(payload[i] ^ mask[i % 4]));
    return frame;
}

int coverage(const QPainterPath &path, const QSize &size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawPath(path);
    painter.end();
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            count += qAlpha(image.pixel(x, y)) > 128;
    return count;
}

}

class OmaInkTests : public QObject {
    Q_OBJECT

    QTemporaryDir m_tlsDir;
    PadServer::Tls m_tls;

    void trustTestCertificate(QSslSocket &socket)
    {
        auto config = socket.sslConfiguration();
        config.setCaCertificates(m_tls.configuration.localCertificateChain());
        socket.setSslConfiguration(config);
    }

    void connectTls(QSslSocket &socket, const PadServer &server)
    {
        trustTestCertificate(socket);
        socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
        socket.connectToHostEncrypted(m_tls.host, server.port());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tlsDir.isValid());
        QFile host(m_tlsDir.filePath("host"));
        QVERIFY(host.open(QIODevice::WriteOnly));
        QCOMPARE(host.write("127.0.0.1\n"), 10);
        host.close();
        QProcess openssl;
        openssl.setWorkingDirectory(m_tlsDir.path());
        openssl.start("openssl", {"req", "-x509", "-newkey", "rsa:2048", "-nodes",
                                   "-keyout", "key.pem", "-out", "cert.pem", "-days", "1",
                                   "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1"});
        QVERIFY(openssl.waitForFinished(10000));
        QVERIFY2(openssl.exitStatus() == QProcess::NormalExit && openssl.exitCode() == 0,
                 openssl.readAllStandardError().constData());
        QString error;
        QVERIFY2(PadServer::loadTls(m_tlsDir.path(), m_tls, error), qPrintable(error));
    }

    void acceptKeyMatchesTheRfc()
    {
        QCOMPARE(WebSocket::acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), QByteArray("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    }

    void framesRoundTrip()
    {
        const QByteArray big(70000, 'x');
        for (const QByteArray &payload : {QByteArray("hi"), QByteArray(300, 'y')}) {
            QByteArray buffer = clientFrame(WebSocket::Text, payload);
            WebSocket::Frame frame;
            QCOMPARE(WebSocket::take(buffer, frame, 1 << 20), WebSocket::Parse::Ok);
            QCOMPARE(frame.payload, payload);
            QVERIFY(buffer.isEmpty());
        }
        const QByteArray encoded = WebSocket::encode(WebSocket::Binary, big);
        QCOMPARE(encoded.size(), big.size() + 10);
    }

    void framesWaitAndRefuse()
    {
        QByteArray partial = clientFrame(WebSocket::Text, "hello").left(4);
        WebSocket::Frame frame;
        QCOMPARE(WebSocket::take(partial, frame, 1024), WebSocket::Parse::Incomplete);

        QByteArray unmasked = WebSocket::encode(WebSocket::Text, "hello");
        QCOMPARE(WebSocket::take(unmasked, frame, 1024), WebSocket::Parse::Error);

        QByteArray huge = clientFrame(WebSocket::Text, QByteArray(300, 'z'));
        QCOMPARE(WebSocket::take(huge, frame, 100), WebSocket::Parse::Error);
    }

    void strokeJsonRoundTrips()
    {
        auto s = stroke("a1", "calligraphy", {{0.1, 0.2, 0.3}, {0.4, 0.5, 0.6}});
        s->color = QColor("#0a84ff");
        s->angle = 45;
        QString error;
        const auto back = Stroke::fromJson(s->toJson(), &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->tool, QString("calligraphy"));
        QCOMPARE(back->color.name(), QString("#0a84ff"));
        QCOMPARE(back->pts.size(), 2);
        QCOMPARE(back->pts[1].p, 0.6);
        QCOMPARE(back->angle, 45.0);
    }

    void strokeJsonRejectsNonsense()
    {
        QVERIFY(!Stroke::fromJson({{"id", "x"}, {"tool", "flamethrower"}}));
        QVERIFY(!Stroke::fromJson({{"tool", "pen"}}));
        QVERIFY(!Stroke::fromJson({{"id", "x"}, {"color", "not a color"}}));
        const auto clamped = Stroke::fromJson({{"id", "x"}, {"size", 9999}});
        QCOMPARE(clamped->size, 40.0);
    }

    void undoRedoAndClear()
    {
        InkModel model;
        draw(model, "a", "pen", {{0.1, 0.1, 1}, {0.2, 0.2, 1}});
        draw(model, "b", "pen", {{0.5, 0.5, 1}, {0.6, 0.6, 1}});
        QCOMPARE(model.strokes().size(), 2);
        model.undo();
        QCOMPARE(model.strokes().size(), 1);
        model.redo();
        QCOMPARE(model.strokes().size(), 2);
        model.clear();
        QVERIFY(model.isEmpty());
        model.undo();
        QCOMPARE(model.strokes().size(), 2);
        // A new stroke after an undo drops what could be redone.
        model.undo();
        draw(model, "c", "pen", {{0.3, 0.3, 1}});
        QVERIFY(!model.canRedo());
    }

    void eraserRemovesWhatItTouchesAndUndoes()
    {
        InkModel model;
        model.setAspect(1);
        draw(model, "keep", "pen", {{0.1, 0.1, 1}, {0.2, 0.1, 1}});
        draw(model, "gone", "pen", {{0.5, 0.2, 1}, {0.5, 0.8, 1}});
        QSignalSpy removed(&model, &InkModel::removed);
        // A fast swipe across the vertical stroke, with no sample on it.
        auto eraser = stroke("e", "eraser", {{0.3, 0.5, 1}});
        eraser->size = 10;
        QVERIFY(model.begin(eraser));
        model.extend("e", {{0.7, 0.5, 1}});
        model.end("e", 1000);
        QCOMPARE(model.strokes().size(), 1);
        QCOMPARE(model.strokes().first()->id, QString("keep"));
        QCOMPARE(removed.size(), 1);
        model.undo();
        QCOMPARE(model.strokes().size(), 2);
    }

    void shapesKeepTwoCorners()
    {
        InkModel model;
        draw(model, "r", "rect", {{0.1, 0.1, 1}, {0.2, 0.3, 1}, {0.5, 0.6, 1}});
        QCOMPARE(model.strokes().first()->pts.size(), 2);
        QCOMPARE(model.strokes().first()->pts.last().x, 0.5);
    }

    void lasersFadeAndNeverCommit()
    {
        InkModel model;
        draw(model, "l", "laser", {{0.1, 0.1, 1}, {0.4, 0.4, 1}});
        QVERIFY(model.isEmpty());
        QVERIFY(model.hasFading());
        QCOMPARE(model.liveStrokes().size(), 1);
        model.prune(1000 + InkModel::laserFadeMs + 1);
        QVERIFY(!model.hasFading());
        QVERIFY(!model.canUndo());
    }

    void cancelLeavesNothingAndKeepsRedo()
    {
        InkModel model;
        draw(model, "a", "pen", {{0.1, 0.1, 1}});
        model.undo();
        QVERIFY(model.begin(stroke("palm", "pen", {{0.5, 0.5, 1}})));
        model.cancel("palm");
        QVERIFY(model.isEmpty());
        QVERIFY(model.liveStrokes().isEmpty());
        QVERIFY(model.canRedo());
    }

    void disconnectEndsAPadsStrokes()
    {
        InkModel model;
        QVERIFY(model.begin(stroke("c1:5", "pen", {{0.1, 0.1, 1}})));
        QVERIFY(model.begin(stroke("c2:5", "pen", {{0.2, 0.2, 1}})));
        model.cancelPrefix("c1:");
        QCOMPARE(model.strokes().size(), 1);
        QCOMPARE(model.liveStrokes().size(), 1);
    }

    void boardsCycle()
    {
        InkModel model;
        model.cycleBoard();
        QCOMPARE(model.board(), QString("white"));
        model.cycleBoard();
        QCOMPARE(model.board(), QString("dark"));
        model.cycleBoard();
        QCOMPARE(model.board(), QString());
        model.setBoard("plaid");
        QCOMPARE(model.board(), QString());
    }

    void penOutlineHasNoHoles()
    {
        // Back and forth over itself: pieces wound against each other would
        // cancel out where they overlap and leave gaps in the line.
        auto s = stroke("z", "pen", {{0.1, 0.5, 1}, {0.9, 0.5, 1}, {0.1, 0.52, 1}, {0.9, 0.54, 1}});
        s->size = 30;
        const QSizeF canvas(400, 400);
        const auto path = InkRenderer::filledOutline(*s, canvas);
        QVERIFY(path.contains(QPointF(200, 200)));
        QVERIFY(path.contains(QPointF(200, 204)));
        QVERIFY(path.contains(QPointF(200, 208)));
    }

    void calligraphyIsThinAlongTheNib()
    {
        // With a flat nib, a horizontal stroke is a hairline and a vertical
        // one is full width.
        const QSizeF canvas(1080, 1080);
        auto across = stroke("a", "calligraphy", {{0.2, 0.5, 1}, {0.8, 0.5, 1}});
        auto down = stroke("d", "calligraphy", {{0.5, 0.2, 1}, {0.5, 0.8, 1}});
        across->angle = down->angle = 0;
        across->size = down->size = 6;
        const int thin = coverage(InkRenderer::filledOutline(*across, canvas), canvas.toSize());
        const int thick = coverage(InkRenderer::filledOutline(*down, canvas), canvas.toSize());
        QVERIFY2(thick > thin * 6, qPrintable(QString("thin %1, thick %2").arg(thin).arg(thick)));
    }

    void inkScalesWithTheScreen()
    {
        const auto s = stroke("s", "pen", {{0.5, 0.2, 1}, {0.5, 0.8, 1}});
        const int small = coverage(InkRenderer::filledOutline(*s, {540, 540}), {540, 540});
        const int large = coverage(InkRenderer::filledOutline(*s, {1080, 1080}), {1080, 1080});
        QVERIFY(large > small * 3 && large < small * 5);
    }

    void tokenPersistsAndResets()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("state/omaink/pad-key");
        const QString first = PadServer::loadToken(path);
        QCOMPARE(first.size(), 16);
        QCOMPARE(PadServer::loadToken(path), first);
        QVERIFY(QFileInfo(path).permissions() & QFile::ReadOwner);
        QVERIFY(!(QFileInfo(path).permissions() & QFile::ReadOther));
        QVERIFY(PadServer::loadToken(path, true) != first);
    }

    void firewallFixNamesTheBlockedPort()
    {
        QTemporaryDir dir;
        const QString conf = dir.filePath("ufw.conf"), rules = dir.filePath("user.rules");
        write(conf, "ENABLED=yes\n");
        write(rules, "### tuple ### allow tcp 53317 0.0.0.0/0 any 0.0.0.0/0 in\n"
                     "### tuple ### limit tcp 22 0.0.0.0/0 any 0.0.0.0/0 in\n");
        QCOMPARE(PadServer::firewallFix(7777, conf, rules, "192.168.1.0/24"),
                 QString("sudo ufw allow from 192.168.1.0/24 to any port 7777 proto tcp comment omaink"));

        write(rules, "### tuple ### allow tcp 7000:8000 192.168.1.0/24 any 0.0.0.0/0 in\n");
        QVERIFY(PadServer::firewallFix(7777, conf, rules, "192.168.1.0/24").isEmpty());
        write(rules, "### tuple ### allow any 7777 0.0.0.0/0 any 0.0.0.0/0 in\n");
        QVERIFY(PadServer::firewallFix(7777, conf, rules, "").isEmpty());

        write(conf, "ENABLED=no\n");
        write(rules, "");
        QVERIFY(PadServer::firewallFix(7777, conf, rules, "").isEmpty());
    }

    void padRequiresTlsAndTrustedIdentity()
    {
        InkModel model;
        PadServer server(&model);
        QTemporaryDir missing;
        QVERIFY(missing.isValid());
        PadServer::Tls invalid;
        QString error;
        QVERIFY(!PadServer::loadTls(missing.path(), invalid, error));
        QVERIFY(!server.listen(0, invalid));
        QVERIFY(!server.isListening());
        QVERIFY(server.pairingUrl().isEmpty());

        server.setToken("testkey123456789");
        QVERIFY2(server.listen(0, m_tls), qPrintable(server.errorString()));
        const QUrl url(server.pairingUrl());
        QCOMPARE(url.scheme(), QString("https"));
        QCOMPARE(url.host(), m_tls.host);
        QCOMPARE(url.port(), int(server.port()));
        QVERIFY(!url.hasQuery());
        QCOMPARE(url.fragment(), QString("k=testkey123456789"));

        QTcpSocket plain;
        plain.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(plain.waitForConnected(2000));
        QSignalSpy read(&plain, &QTcpSocket::readyRead);
        plain.write("GET /pad HTTP/1.1\r\nHost: pad\r\n\r\n");
        QTRY_COMPARE_WITH_TIMEOUT(plain.state(), QAbstractSocket::UnconnectedState, 2000);
        QVERIFY(read.isEmpty());

        for (bool trustCertificate : {false, true}) {
            QSslSocket socket;
            socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
            if (trustCertificate)
                trustTestCertificate(socket);
            QSignalSpy encrypted(&socket, &QSslSocket::encrypted);
            QSignalSpy errors(&socket, &QSslSocket::peerVerifyError);
            // The trusted certificate covers 127.0.0.1 only.
            socket.connectToHostEncrypted(m_tls.host, server.port(),
                                          trustCertificate ? QString("other.invalid") : m_tls.host);
            QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty() &&
                                     socket.state() == QAbstractSocket::UnconnectedState, 2000);
            QVERIFY(encrypted.isEmpty());
        }
    }

    void serverServesThePadAndGuardsTheSocket()
    {
        InkModel model;
        PadServer server(&model);
        server.setToken("testkey123456789");
        server.setCaptureCommand({});
        QVERIFY2(server.listen(0, m_tls), qPrintable(server.errorString()));

        auto request = [&](const QByteArray &head) {
            QSslSocket socket;
            connectTls(socket, server);
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 2000 && !socket.isEncrypted())
                QTest::qWait(10);
            if (!socket.isEncrypted())
                return QByteArray();
            socket.write(head);
            QByteArray reply;
            clock.restart();
            while (clock.elapsed() < 2000 && socket.state() != QAbstractSocket::UnconnectedState) {
                QTest::qWait(10);
                reply += socket.readAll();
            }
            return reply + socket.readAll();
        };

        const QByteArray page = request("GET /pad?k=x HTTP/1.1\r\nHost: pad\r\n\r\n");
        QVERIFY(page.startsWith("HTTP/1.1 200"));
        QVERIFY(page.contains("pad.js"));
        QVERIFY(request("GET /icon.png HTTP/1.1\r\n\r\n").contains("\x89PNG"));
        QVERIFY(request("GET /nope HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 404"));
        const QByteArray upgrade = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        QVERIFY(request("GET /ws?k=wrongkey12345678 HTTP/1.1\r\n" + upgrade).startsWith("HTTP/1.1 403"));
        QVERIFY(request("GET /ws HTTP/1.1\r\n" + upgrade).startsWith("HTTP/1.1 403"));
    }

    void padStrokesReachTheModel()
    {
        InkModel model;
        PadServer server(&model);
        server.setToken("testkey123456789");
        server.setCaptureCommand({});
        QVERIFY2(server.listen(0, m_tls), qPrintable(server.errorString()));

        QSslSocket socket;
        connectTls(socket, server);
        QTRY_VERIFY_WITH_TIMEOUT(socket.isEncrypted(), 2000);
        socket.write("GET /ws?k=testkey123456789 HTTP/1.1\r\nHost: pad\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        QByteArray received;
        QTRY_VERIFY_WITH_TIMEOUT((received += socket.readAll()).contains("\"t\":\"state\""), 2000);
        QVERIFY(received.startsWith("HTTP/1.1 101"));
        QVERIFY(received.contains("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
        received.clear();
        QCOMPARE(server.clientCount(), 1);

        auto send = [&](const QJsonObject &message) {
            socket.write(clientFrame(WebSocket::Text, QJsonDocument(message).toJson(QJsonDocument::Compact)));
        };
        send({{"t", "begin"}, {"stroke", QJsonObject{{"id", "1"}, {"tool", "pen"}, {"color", "#30d158"},
                                                     {"pts", QJsonArray{QJsonArray{0.1, 0.1, 0.5}}}}}});
        send({{"t", "move"}, {"id", "1"}, {"pts", QJsonArray{QJsonArray{0.2, 0.2, 0.6}, QJsonArray{0.3, 0.3, 0.7}}}});
        send({{"t", "end"}, {"id", "1"}});
        QTRY_COMPARE_WITH_TIMEOUT(model.strokes().size(), 1, 2000);
        QCOMPARE(model.strokes().first()->id, QString("c1:1"));
        QCOMPARE(model.strokes().first()->pts.size(), 3);
        QTRY_VERIFY_WITH_TIMEOUT((received += socket.readAll()).contains("\"t\":\"add\""), 2000);
        // The add carries the ink version frames must reach to show it.
        QVERIFY(QRegularExpression("\"v\":[1-9]").match(QString::fromUtf8(received)).hasMatch());

        send({{"t", "undo"}});
        QTRY_VERIFY_WITH_TIMEOUT(model.isEmpty(), 2000);
        send({{"t", "board"}, {"board", "dark"}});
        QTRY_COMPARE_WITH_TIMEOUT(model.board(), QString("dark"), 2000);

        // Half a stroke, then the tablet drops off the network.
        send({{"t", "begin"}, {"stroke", QJsonObject{{"id", "2"}, {"tool", "pen"}, {"pts", QJsonArray{QJsonArray{0.5, 0.5}}}}}});
        QTRY_COMPARE_WITH_TIMEOUT(model.liveStrokes().size(), 1, 2000);
        socket.abort();
        QTRY_COMPARE_WITH_TIMEOUT(server.clientCount(), 0, 2000);
        QVERIFY(model.liveStrokes().isEmpty());
    }

    void mirrorSendsFrames()
    {
        QTemporaryDir dir;
        const QString fake = dir.filePath("grim");
        QFile script(fake);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf 'JPEGDATA'\n");
        script.close();
        QVERIFY(QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

        InkModel model;
        PadServer server(&model);
        server.setToken("testkey123456789");
        server.setCaptureCommand(fake);
        QVERIFY2(server.listen(0, m_tls), qPrintable(server.errorString()));
        QSslSocket socket;
        connectTls(socket, server);
        QTRY_VERIFY_WITH_TIMEOUT(socket.isEncrypted(), 2000);
        socket.write("GET /ws?k=testkey123456789 HTTP/1.1\r\nUpgrade: websocket\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
        socket.write(clientFrame(WebSocket::Text, R"({"t":"mirror","on":true})"));
        QByteArray received;
        QTRY_VERIFY_WITH_TIMEOUT((received += socket.readAll()).contains(QByteArray("\x82\x0c", 2)), 3000);
        QVERIFY(received.contains("JPEGDATA"));
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        auto write = [](const QString &path, const QByteArray &data) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(write(response, "{\"int\":0}"));

        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        QVERIFY(write(response, "{\"int\":6}"));
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"}})");
        write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})");

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        QCOMPARE(Translator::pluralCategory("ar", 11), QString("many"));
        QCOMPARE(Translator::pluralCategory("ja", 1), QString("other"));
    }

private:
    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(OmaInkTests)
#include "omaink_tests.moc"
