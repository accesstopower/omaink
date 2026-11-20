#include "ink.h"

#include "inkrenderer.h"

#include <QDateTime>
#include <QJsonArray>
#include <QLineF>
#include <algorithm>
#include <cmath>

const QStringList InkModel::tools{"pen", "calligraphy", "marker", "highlighter", "laser",
                                  "arrow", "rect", "ellipse", "eraser"};
const QStringList InkModel::boards{"", "white", "dark"};

QJsonObject Stroke::toJson() const {
    QJsonArray points;
    for (const auto &point : pts)
        points.append(QJsonArray{std::round(point.x * 1e5) / 1e5, std::round(point.y * 1e5) / 1e5,
                                 std::round(point.p * 1e3) / 1e3});
    return {{"id", id}, {"tool", tool}, {"color", color.name()}, {"size", size}, {"angle", angle}, {"pts", points}};
}

StrokePtr Stroke::fromJson(const QJsonObject &object, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return StrokePtr();
    };
    auto stroke = StrokePtr::create();
    stroke->id = object.value("id").toString();
    stroke->tool = object.value("tool").toString("pen");
    if (stroke->id.isEmpty() || stroke->id.size() > 64)
        return fail("stroke needs an id");
    if (!InkModel::tools.contains(stroke->tool))
        return fail("unknown tool " + stroke->tool);
    const QColor color(object.value("color").toString("#ff3b30"));
    if (!color.isValid())
        return fail("invalid color");
    stroke->color = color;
    stroke->size = std::clamp(object.value("size").toDouble(3.5), 0.5, 40.0);
    stroke->angle = std::fmod(object.value("angle").toDouble(30), 180.0);
    for (const auto &value : object.value("pts").toArray()) {
        const auto point = value.toArray();
        if (point.size() < 2)
            continue;
        stroke->pts.append({point.at(0).toDouble(), point.at(1).toDouble(),
                            std::clamp(point.size() > 2 ? point.at(2).toDouble(0.5) : 0.5, 0.0, 1.0)});
    }
    return stroke;
}

InkModel::InkModel(QObject *parent) : QObject(parent) {}

QList<StrokePtr> InkModel::liveStrokes() const {
    QList<StrokePtr> live;
    for (const auto &id : m_liveOrder)
        if (const auto stroke = m_live.value(id); stroke && stroke->tool != "eraser")
            live.append(stroke);
    return live + m_fading;
}

void InkModel::setBoard(const QString &board) {
    const QString next = boards.contains(board) ? board : QString();
    if (next == m_board)
        return;
    m_board = next;
    emit boardChanged();
    emit changed();
}

void InkModel::cycleBoard() {
    setBoard(boards.at((boards.indexOf(m_board) + 1) % boards.size()));
}

void InkModel::setAspect(double aspect) {
    if (aspect > 0.1 && aspect < 10)
        m_aspect = aspect;
}

bool InkModel::begin(const StrokePtr &stroke) {
    if (!stroke || stroke->id.isEmpty() || m_live.contains(stroke->id))
        return false;
    m_live.insert(stroke->id, stroke);
    m_liveOrder.append(stroke->id);
    if (stroke->tool == "eraser") {
        m_eraseBefore.insert(stroke->id, m_strokes);
        for (const auto &point : stroke->pts)
            eraseAt(point, stroke->size);
    }
    emit changed();
    return true;
}

void InkModel::extend(const QString &id, const QVector<InkPoint> &pts) {
    const auto stroke = m_live.value(id);
    if (!stroke || pts.isEmpty())
        return;
    if (stroke->isShape()) {
        // A shape is only its two corners; the latest point drags the second.
        if (stroke->pts.isEmpty())
            stroke->pts.append(pts.first());
        stroke->pts.resize(1);
        stroke->pts.append(pts.last());
    } else if (stroke->tool == "eraser") {
        // Fill in between samples so a quick swipe can't skip over a stroke.
        InkPoint last = stroke->pts.isEmpty() ? pts.first() : stroke->pts.last();
        for (const auto &point : pts) {
            const double dx = (point.x - last.x) * m_aspect * 1080, dy = (point.y - last.y) * 1080;
            const int steps = std::clamp(int(std::hypot(dx, dy) / std::max(2.0, stroke->size * 0.5)), 1, 64);
            for (int i = 1; i <= steps; ++i) {
                const double t = double(i) / steps;
                eraseAt({last.x + (point.x - last.x) * t, last.y + (point.y - last.y) * t, 1}, stroke->size);
            }
            stroke->pts.append(point);
            last = point;
        }
    } else {
        stroke->pts += pts;
    }
    emit changed();
}

void InkModel::end(const QString &id, qint64 now) {
    const auto stroke = m_live.take(id);
    if (!stroke)
        return;
    m_liveOrder.removeAll(id);
    if (stroke->tool == "eraser") {
        const auto before = m_eraseBefore.take(id);
        if (before.size() != m_strokes.size())
            pushUndo(before);
    } else if (stroke->tool == "laser") {
        stroke->endedAt = now;
        m_fading.append(stroke);
    } else if (!stroke->pts.isEmpty()) {
        pushUndo(m_strokes);
        m_strokes.append(stroke);
        emit committed(stroke);
    }
    emit changed();
}

void InkModel::cancel(const QString &id) {
    const auto stroke = m_live.value(id);
    if (!stroke)
        return;
    // An eraser has already removed what it touched, so it finishes normally
    // and stays undoable. Anything else is dropped, so a palm or a stray tap
    // leaves nothing behind.
    if (stroke->tool == "eraser") {
        end(id, 0);
        return;
    }
    m_live.remove(id);
    m_liveOrder.removeAll(id);
    emit changed();
}

void InkModel::cancelPrefix(const QString &prefix) {
    const auto ids = m_liveOrder;
    for (const auto &id : ids)
        if (id.startsWith(prefix))
            end(id, QDateTime::currentMSecsSinceEpoch());
}

void InkModel::undo() {
    if (m_undo.isEmpty())
        return;
    m_redo.append(m_strokes);
    m_strokes = m_undo.takeLast();
    emit reset();
    emit historyChanged();
    emit changed();
}

void InkModel::redo() {
    if (m_redo.isEmpty())
        return;
    m_undo.append(m_strokes);
    m_strokes = m_redo.takeLast();
    emit reset();
    emit historyChanged();
    emit changed();
}

void InkModel::clear() {
    m_fading.clear();
    if (m_strokes.isEmpty()) {
        emit changed();
        return;
    }
    pushUndo(m_strokes);
    m_strokes.clear();
    emit reset();
    emit changed();
}

void InkModel::prune(qint64 now) {
    const auto before = m_fading.size();
    m_fading.erase(std::remove_if(m_fading.begin(), m_fading.end(),
                                  [now](const StrokePtr &s) { return now - s->endedAt > laserFadeMs; }),
                   m_fading.end());
    if (m_fading.size() != before)
        emit changed();
}

void InkModel::pushUndo(const QList<StrokePtr> &snapshot) {
    m_undo.append(snapshot);
    if (m_undo.size() > historyLimit)
        m_undo.removeFirst();
    m_redo.clear();
    emit historyChanged();
}

double InkModel::distanceTo(const Stroke &stroke, double x, double y) const {
    const QSizeF reference(m_aspect * InkRenderer::referenceHeight, InkRenderer::referenceHeight);
    const QPointF point(x * reference.width(), y * reference.height());
    const auto lines = InkRenderer::outlines(stroke, reference);
    double best = 1e12;
    for (const auto &line : lines) {
        if (line.size() == 1)
            best = std::min(best, QLineF(point, line.first()).length());
        for (int i = 1; i < line.size(); ++i) {
            const QPointF a = line.at(i - 1), b = line.at(i), ab = b - a;
            const double length2 = QPointF::dotProduct(ab, ab);
            const double t = length2 > 0 ? std::clamp(QPointF::dotProduct(point - a, ab) / length2, 0.0, 1.0) : 0;
            best = std::min(best, QLineF(point, a + ab * t).length());
        }
    }
    return best - InkRenderer::bodyWidth(stroke) / 2;
}

void InkModel::eraseAt(const InkPoint &point, double radius) {
    QStringList gone;
    m_strokes.erase(std::remove_if(m_strokes.begin(), m_strokes.end(),
                                   [&](const StrokePtr &stroke) {
                                       if (distanceTo(*stroke, point.x, point.y) > radius)
                                           return false;
                                       gone.append(stroke->id);
                                       return true;
                                   }),
                    m_strokes.end());
    if (!gone.isEmpty())
        emit removed(gone);
}
