#pragma once

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSharedPointer>
#include <QStringList>
#include <QVector>

// One sample of a stroke. Positions are fractions of the screen (0 to 1), so
// the same stroke lands in the same place on the overlay and on a tablet of
// any size. Pressure runs from 0 to 1.
struct InkPoint {
    double x = 0;
    double y = 0;
    double p = 0.5;
};

struct Stroke {
    QString id;
    QString tool = "pen";
    QColor color = QColor("#ff3b30");
    // The base width in pixels on a screen 1080 pixels high; it scales with
    // the screen so ink keeps its proportions everywhere it is shown.
    double size = 3.5;
    // The calligraphy nib's angle in degrees, counterclockwise from flat.
    double angle = 30;
    QVector<InkPoint> pts;
    // When a laser stroke was lifted, in milliseconds since the epoch, so it
    // can fade out.
    qint64 endedAt = 0;

    bool isShape() const { return tool == "arrow" || tool == "rect" || tool == "ellipse"; }
    QJsonObject toJson() const;
    static QSharedPointer<Stroke> fromJson(const QJsonObject &object, QString *error = nullptr);
};
using StrokePtr = QSharedPointer<Stroke>;

// Every stroke on screen, whichever device drew it. Strokes arrive as begin,
// extend and end, so the overlay's mouse and any number of tablets can draw
// at once. Undo and redo work on snapshots of the finished strokes, which
// keeps clearing and erasing undoable without separate bookkeeping.
class InkModel : public QObject {
    Q_OBJECT

public:
    static const QStringList tools;
    static const QStringList boards;
    static constexpr qint64 laserFadeMs = 1400;
    static constexpr int historyLimit = 200;

    explicit InkModel(QObject *parent = nullptr);

    const QList<StrokePtr> &strokes() const { return m_strokes; }
    // Strokes still being drawn, followed by lasers that are fading.
    QList<StrokePtr> liveStrokes() const;

    QString board() const { return m_board; }
    void setBoard(const QString &board);
    void cycleBoard();

    // The overlay's width divided by its height. The eraser measures
    // distances in screen proportions, so a circle stays a circle.
    double aspect() const { return m_aspect; }
    void setAspect(double aspect);

    bool begin(const StrokePtr &stroke);
    void extend(const QString &id, const QVector<InkPoint> &pts);
    void end(const QString &id, qint64 now);
    void cancel(const QString &id);
    // Drops every unfinished stroke whose id starts with the prefix, as when a
    // tablet disconnects in the middle of a stroke.
    void cancelPrefix(const QString &prefix);

    void undo();
    void redo();
    void clear();
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool isEmpty() const { return m_strokes.isEmpty(); }

    bool hasFading() const { return !m_fading.isEmpty(); }
    void prune(qint64 now);

    // Distance from a point to a stroke's body, in pixels on a 1080-high
    // screen, for the eraser.
    double distanceTo(const Stroke &stroke, double x, double y) const;

signals:
    // Anything on screen changed, including strokes in progress.
    void changed();
    // A finished stroke joined the list.
    void committed(const StrokePtr &stroke);
    // Strokes left the list without a full reset (the eraser).
    void removed(const QStringList &ids);
    // The list was replaced (undo, redo, clear).
    void reset();
    void boardChanged();
    void historyChanged();

private:
    void pushUndo(const QList<StrokePtr> &snapshot);
    void eraseAt(const InkPoint &point, double radius);

    QList<StrokePtr> m_strokes;
    QHash<QString, StrokePtr> m_live;
    QStringList m_liveOrder;
    QList<StrokePtr> m_fading;
    QList<QList<StrokePtr>> m_undo;
    QList<QList<StrokePtr>> m_redo;
    QHash<QString, QList<StrokePtr>> m_eraseBefore;
    QString m_board;
    double m_aspect = 16.0 / 9.0;
};
