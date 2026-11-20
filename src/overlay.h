#pragma once

#include "ink.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QVariantList>

class PadServer;
class QQuickWindow;

// The overlay's state and actions: what tool is in hand, whether clicks
// draw or pass through to the apps underneath, which screen it covers, and
// the commands `omaink <command>` sends to the running copy.
class Overlay : public QObject {
    Q_OBJECT
    Q_PROPERTY(InkModel *model READ model CONSTANT)
    Q_PROPERTY(bool drawing READ drawing NOTIFY drawingChanged)
    Q_PROPERTY(bool inkVisible READ inkVisible NOTIFY inkVisibleChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QString color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(int sizeIndex READ sizeIndex WRITE setSizeIndex NOTIFY sizeChanged)
    Q_PROPERTY(int nibAngle READ nibAngle NOTIFY nibAngleChanged)
    Q_PROPERTY(QString board READ board NOTIFY boardChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool empty READ empty NOTIFY historyChanged)
    Q_PROPERTY(QStringList padUrls READ padUrls NOTIFY padChanged)
    Q_PROPERTY(QString padError READ padError NOTIFY padChanged)
    Q_PROPERTY(int padClients READ padClients NOTIFY padChanged)
    Q_PROPERTY(QString firewallFix READ firewallFix NOTIFY padChanged)
    Q_PROPERTY(QString qrSource READ qrSource NOTIFY padChanged)
    Q_PROPERTY(QString output READ output NOTIFY outputChanged)
    Q_PROPERTY(QStringList palette READ palette CONSTANT)
    Q_PROPERTY(QVariantList sizes READ sizes CONSTANT)

public:
    static const QStringList paletteColors;
    static const QList<double> sizePresets;
    static const QList<int> nibAngles;

    explicit Overlay(InkModel *model, QObject *parent = nullptr);

    // Puts the window on the overlay layer, covering the screen, with clicks
    // passing through until drawing starts. Call before the window is shown.
    void attach(QQuickWindow *window, const QString &preferredOutput);
    void setPadServer(PadServer *server, const QString &error);

    InkModel *model() const { return m_model; }
    bool drawing() const { return m_drawing; }
    bool inkVisible() const { return m_visible; }
    QString tool() const { return m_tool; }
    void setTool(const QString &tool);
    QString color() const { return m_color; }
    void setColor(const QString &color);
    int sizeIndex() const { return m_sizeIndex; }
    void setSizeIndex(int index);
    int nibAngle() const { return m_nibAngle; }
    QString board() const { return m_model->board(); }
    bool canUndo() const { return m_model->canUndo(); }
    bool canRedo() const { return m_model->canRedo(); }
    bool empty() const { return m_model->isEmpty(); }
    QStringList padUrls() const;
    QString padError() const { return m_padError; }
    int padClients() const;
    QString firewallFix() const;
    QString qrSource() const { return m_qrSource; }
    QString output() const { return m_output; }
    QStringList palette() const { return paletteColors; }
    QVariantList sizes() const;

    Q_INVOKABLE void setDrawing(bool drawing);
    Q_INVOKABLE void toggleDrawing() { setDrawing(!m_drawing); }
    Q_INVOKABLE void setInkVisible(bool visible);
    Q_INVOKABLE void undo() { m_model->undo(); }
    Q_INVOKABLE void redo() { m_model->redo(); }
    Q_INVOKABLE void clear() { m_model->clear(); }
    Q_INVOKABLE void cycleBoard() { m_model->cycleBoard(); }
    Q_INVOKABLE void cycleNib();
    Q_INVOKABLE void stepSize(int delta) { setSizeIndex(m_sizeIndex + delta); }

    // Mouse and tablet input from the canvas, in fractions of the screen.
    Q_INVOKABLE void beginLocal(QPointF point, bool erase);
    Q_INVOKABLE void extendLocal(QPointF point);
    Q_INVOKABLE void endLocal();
    bool localStrokeActive() const { return !m_localId.isEmpty(); }

    // Runs a command from `omaink <command>` and answers in JSON.
    QJsonObject run(const QString &command, const QStringList &arguments);

    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void drawingChanged();
    void inkVisibleChanged();
    void toolChanged();
    void colorChanged();
    void sizeChanged();
    void nibAngleChanged();
    void boardChanged();
    void historyChanged();
    void padChanged();
    void outputChanged();
    void notify(const QString &message);
    void quitRequested();

private:
    struct Monitor {
        QString name;
        int width = 0;
        bool focused = false;
    };
    // Hyprland's monitors, with their sizes in hardware pixels.
    static QList<Monitor> monitors();
    static QString focusedOutput();
    void moveTo(const QString &output);
    void applyInputMode();
    void updateGeometry();
    void makeQrCode();
    double currentPressure() const;

    InkModel *m_model;
    QPointer<QQuickWindow> m_window;
    QPointer<PadServer> m_pad;
    QString m_padError;
    int m_knownClients = 0;
    QString m_qrSource;
    QString m_output;
    int m_physicalWidth = 1920;

    bool m_drawing = false;
    bool m_visible = true;
    QString m_tool = "pen";
    QString m_color = "#ff3b30";
    int m_sizeIndex = 1;
    int m_nibAngle = 15;

    QString m_localId;
    int m_localCount = 0;
    double m_tabletPressure = 0.75;
    bool m_tabletEraser = false;
    QElapsedTimer m_tabletClock;
};
