#pragma once

#include "ink.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTcpServer>
#include <QTimer>

class QTcpSocket;

// Serves the drawing pad to a tablet's browser and carries its strokes into
// the ink model, and the computer's screen back to the tablet so you can see
// what you're drawing on. Everything after the page itself needs the pairing
// key from the pad's address, so nobody else on the network can watch the
// screen or draw on it.
class PadServer : public QObject {
    Q_OBJECT

public:
    explicit PadServer(InkModel *model, QObject *parent = nullptr);
    ~PadServer() override;

    bool listen(quint16 port);
    QString errorString() const { return m_error; }
    bool isListening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }

    QString token() const { return m_token; }
    void setToken(const QString &token) { m_token = token; }
    // Reads the pairing key, making a new one the first time or on reset, so
    // a pad saved to a tablet's home screen keeps working across restarts.
    static QString loadToken(const QString &path, bool reset = false, QString *error = nullptr);
    // This machine's IPv4 addresses, home and office networks first.
    static QStringList addresses();
    static QString url(const QString &address, quint16 port, const QString &token);
    // The home or office network this machine is on, like 192.168.20.0/24.
    static QString localSubnet();
    // When ufw is on and nothing lets the port in, the command that does,
    // limited to the local network. Empty when the port is reachable.
    static QString firewallFix(quint16 port, const QString &confPath = "/etc/ufw/ufw.conf",
                               const QString &rulesPath = "/etc/ufw/user.rules",
                               const QString &subnet = localSubnet());

    int clientCount() const;

    // The screen the overlay is on, for the mirror and for the pad's shape.
    void setOutput(const QString &name, int logicalWidth, int physicalWidth, double aspect);
    void setCaptureCommand(const QString &program) { m_grimPath = program; }

signals:
    void clientsChanged();

private:
    struct Client {
        QTcpSocket *socket = nullptr;
        QByteArray buffer;
        bool upgraded = false;
        bool mirror = false;
        int id = 0;
        QByteArray message;
        quint8 messageOpcode = 0;
    };

    void onConnection();
    void onReadyRead(QTcpSocket *socket);
    void onDisconnected(QTcpSocket *socket);
    void handleRequest(Client &client, const QByteArray &head);
    void handleFrames(Client &client);
    void handleMessage(Client &client, const QByteArray &text);
    void respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
                 const QByteArray &extraHeaders = {});
    void sendText(Client &client, const QJsonObject &message);
    void broadcast(const QJsonObject &message);
    QJsonObject stateMessage(const Client &client) const;
    QByteArray icon();

    void scheduleMirror();
    void capture();
    void captureFinished(int code, QProcess::ExitStatus status);
    bool anyMirror() const;

    InkModel *m_model;
    QTcpServer m_server;
    QHash<QTcpSocket *, Client *> m_clients;
    QString m_error;
    QString m_token;
    int m_nextId = 1;

    QString m_output;
    int m_logicalWidth = 1920;
    int m_physicalWidth = 1920;
    double m_aspect = 16.0 / 9.0;

    QString m_grimPath;
    QProcess m_grim;
    QTimer m_mirrorTimer;
    QElapsedTimer m_captureClock;
    QString m_mirrorError;
    // Counts every change to the ink. Each mirror frame carries the count
    // from when its capture started, so a tablet can tell a frame that
    // predates a stroke from one that shows it.
    quint32 m_inkVersion = 0;
    quint32 m_captureVersion = 0;
    QByteArray m_icon;
};
