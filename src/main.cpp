// omaink — draw over your screen with a mouse, a pen tablet, or an iPad, so
// annotations show up live and in any screen recording.


#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <unistd.h>

#include "ink.h"
#include "inkcanvas.h"
#include "overlay.h"
#include "padserver.h"
#include "theme.h"
#include "translator.h"

namespace {

const char *version = "0.1.0";
constexpr quint16 defaultPort = 7777;

const char *usage = R"(Usage: omaink [command] [options]

Draw over your screen with a mouse, a pen tablet, or an iPad.

Commands:
  open            Start OmaInk (the default)
  toggle          Start or stop drawing; bind this to a key
  draw | done     Start or stop drawing
  visible         Show or hide the ink (show and hide also work)
  undo | redo     Undo or redo the last change
  clear           Clear the ink (undo brings it back)
  board [white|dark|off]
                  Draw on a blank board instead of the screen
  tool <name>     pen, calligraphy, marker, highlighter, laser,
                  arrow, rect, ellipse or eraser
  color <#hex>    Ink color, like #ff3b30
  pad [reset]     Show the tablet's address; reset makes a new key
  status          Show what OmaInk is doing
  quit            Stop OmaInk

Options:
  --output <name> Screen to start on, like HDMI-A-1 (default: focused)
  --port <port>   Port for the tablet pad (default: 7777)
  --no-pad        Don't serve the tablet pad
  --json          Answer in JSON
  -h, --help      Show this help
  -v, --version   Show the version
)";

struct Options {
    QString command = "open";
    QStringList arguments;
    QString output;
    quint16 port = defaultPort;
    bool pad = true;
    bool json = false;
};

QString socketPath() {
    QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (dir.isEmpty())
        dir = QDir::tempPath() + QString("/omaink-%1").arg(getuid());
    QDir().mkpath(dir);
    return dir + "/omaink.sock";
}

QString keyPath() {
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    return state + "/omaink/pad-key";
}

void print(const QString &text, FILE *stream = stdout) {
    std::fputs(text.toUtf8().constData(), stream);
    std::fputc('\n', stream);
}

// Prints a reply from the running copy for a person, or as JSON for scripts.
int report(const Options &options, const QJsonObject &reply) {
    if (options.json) {
        print(QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
        return reply.value("ok").toBool() ? 0 : 1;
    }
    if (!reply.value("ok").toBool()) {
        print("omaink: " + reply.value("error").toString(), stderr);
        return 1;
    }
    if (options.command == "pad") {
        const auto pad = reply.value("pad").toObject();
        if (!pad.value("error").toString().isEmpty())
            print("omaink: " + pad.value("error").toString(), stderr);
        const auto urls = pad.value("urls").toArray();
        // In a terminal, the QR code comes first, so the tablet can scan
        // it straight off the screen.
        const QString qrencode = QStandardPaths::findExecutable("qrencode");
        if (!urls.isEmpty() && !qrencode.isEmpty() && isatty(STDOUT_FILENO)) {
            QProcess qr;
            qr.start(qrencode, {"-t", "ANSIUTF8", "-m", "2", urls.first().toString()});
            if (qr.waitForFinished(3000) && qr.exitCode() == 0)
                std::fputs(qr.readAllStandardOutput().constData(), stdout);
        }
        for (const auto &url : urls)
            print(url.toString());
        return urls.isEmpty() ? 1 : 0;
    }
    if (options.command == "status") {
        if (!reply.value("running").toBool(true)) {
            print("OmaInk isn't running.");
            return 1;
        }
        print(QString("%1, %2 strokes, %3 on %4, board %5, %6 tablet(s)")
                  .arg(reply.value("drawing").toBool() ? "Drawing" : "Not drawing")
                  .arg(reply.value("strokes").toInt())
                  .arg(reply.value("tool").toString(), reply.value("output").toString(),
                       reply.value("board").toString())
                  .arg(reply.value("pad").toObject().value("clients").toInt()));
    }
    return 0;
}

// Sends the command to the copy that's already running, if there is one.
bool sendToRunning(const Options &options, QJsonObject *reply) {
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(300))
        return false;
    const QJsonObject request{{"command", options.command},
                              {"arguments", QJsonArray::fromStringList(options.arguments)}};
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    socket.flush();
    QByteArray line;
    while (!line.contains('\n') && socket.waitForReadyRead(3000))
        line += socket.readAll();
    *reply = QJsonDocument::fromJson(line).object();
    if (reply->isEmpty())
        *reply = {{"ok", false}, {"error", "OmaInk didn't answer."}};
    return true;
}

int parse(int argc, char *argv[], Options &options) {
    bool commandSeen = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "-h" || arg == "--help" || arg == "help") {
            std::fputs(usage, stdout);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            print(QString("omaink %1").arg(version));
            return 0;
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--no-pad") {
            options.pad = false;
        } else if (arg == "--output" || arg == "--port") {
            if (i + 1 >= argc) {
                print("omaink: " + arg + " needs a value", stderr);
                return 1;
            }
            const QString value = QString::fromLocal8Bit(argv[++i]);
            if (arg == "--output") {
                options.output = value;
            } else {
                bool ok = false;
                const uint port = value.toUInt(&ok);
                if (!ok || port == 0 || port > 65535) {
                    print("omaink: --port needs a number from 1 to 65535", stderr);
                    return 1;
                }
                options.port = quint16(port);
            }
        } else if (arg.startsWith("-")) {
            print("omaink: unknown option " + arg + "\n\n" + usage, stderr);
            return 1;
        } else if (!commandSeen) {
            options.command = arg;
            commandSeen = true;
        } else {
            options.arguments.append(arg);
        }
    }
    return -1;
}

} // namespace

int main(int argc, char *argv[]) {
    Options options;
    if (const int code = parse(argc, argv, options); code >= 0)
        return code;

    // Commands for a copy that's already running don't need a display, so
    // they're sent before any window system is touched.
    {
        QCoreApplication client(argc, argv);
        QJsonObject reply;
        if (sendToRunning(options, &reply))
            return report(options, reply);

        const QString command = options.command;
        if (command == "status") {
            return report(options, {{"ok", true}, {"running", false}});
        }
        if (command == "quit" || command == "done" || command == "hide") {
            if (!options.json)
                print("OmaInk isn't running.");
            return 0;
        }
        if (command == "pad") {
            // The address works before OmaInk starts, so it can be set up on
            // the tablet ahead of time.
            const QString key = PadServer::loadToken(keyPath(), options.arguments.value(0) == "reset");
            QJsonArray urls;
            for (const auto &address : PadServer::addresses())
                urls.append(PadServer::url(address, options.port, key));
            return report(options, {{"ok", true}, {"running", false}, {"pad", QJsonObject{{"urls", urls}}}});
        }
    }

    QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication app(argc, argv);
    app.setApplicationName("omaink");
    app.setApplicationVersion(version);
    // Matches the window to omaink.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("omaink");
    app.setWindowIcon(QIcon::fromTheme("omaink"));
    app.setQuitOnLastWindowClosed(false);

    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        print("omaink: needs a Wayland desktop such as Hyprland.", stderr);
        return 1;
    }

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);
    Theme theme;

    InkModel model;
    Overlay overlay(&model);

    PadServer *pad = nullptr;
    if (options.pad) {
        pad = new PadServer(&model, &app);
        QString error;
        pad->setToken(PadServer::loadToken(keyPath(), false, &error));
        if (!pad->listen(options.port))
            error = pad->errorString();
        if (!error.isEmpty())
            print("omaink: " + error, stderr);
        overlay.setPadServer(pad->isListening() ? pad : nullptr, error);
    }

    // One copy runs at a time; later `omaink <command>` calls talk to it here.
    QLocalServer::removeServer(socketPath());
    QLocalServer commands;
    commands.setSocketOptions(QLocalServer::UserAccessOption);
    if (!commands.listen(socketPath()))
        print("omaink: can't listen for commands: " + commands.errorString(), stderr);
    QObject::connect(&commands, &QLocalServer::newConnection, &app, [&] {
        while (QLocalSocket *socket = commands.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                if (!socket->canReadLine())
                    return;
                const auto request = QJsonDocument::fromJson(socket->readLine()).object();
                QStringList arguments;
                for (const auto &value : request.value("arguments").toArray())
                    arguments.append(value.toString());
                const QString command = request.value("command").toString();
                if (command == "pad" && arguments.value(0) == "reset" && pad) {
                    pad->setToken(PadServer::loadToken(keyPath(), true));
                    overlay.setPadServer(pad, overlay.padError());
                }
                const QJsonObject reply = overlay.run(command, arguments);
                socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + "\n");
                socket->flush();
            });
        }
    });
    QObject::connect(&overlay, &Overlay::quitRequested, &app, [&app] {
        // Lets the reply to `omaink quit` go out first.
        QTimer::singleShot(50, &app, &QCoreApplication::quit);
    });

    qmlRegisterType<InkCanvas>("OmaInk", 1, 0, "InkCanvas");
    qmlRegisterUncreatableType<Overlay>("OmaInk", 1, 0, "Overlay", "Provided by the app");
    qmlRegisterUncreatableType<InkModel>("OmaInk", 1, 0, "InkModel", "Provided by the app");

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("overlay", &overlay);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!window)
        return 1;
    overlay.attach(window, options.output);

    if (options.command != "open") {
        const QJsonObject reply = overlay.run(options.command, options.arguments);
        if (!reply.value("ok").toBool())
            print("omaink: " + reply.value("error").toString(), stderr);
    }
    if (pad && pad->isListening() && !overlay.padUrls().isEmpty())
        print("Draw from a tablet: " + overlay.padUrls().first(), stderr);

    const int code = app.exec();
    commands.close();
    return code;
}
