#include "inkrenderer.h"

#include <QLineF>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {

struct Sample {
    QPointF pos;
    double p;
};

QVector<Sample> toCanvas(const Stroke &stroke, const QSizeF &canvas) {
    QVector<Sample> samples;
    samples.reserve(stroke.pts.size());
    for (const auto &point : stroke.pts) {
        const QPointF pos(point.x * canvas.width(), point.y * canvas.height());
        // Repeated positions add nothing but zero-length segments.
        if (!samples.isEmpty() && QLineF(samples.last().pos, pos).length() < 0.05) {
            samples.last().p = std::max(samples.last().p, point.p);
            continue;
        }
        samples.append({pos, point.p});
    }
    return samples;
}

// A Catmull-Rom curve through the samples, so loops stay round between input
// events instead of turning into polygons when the pen moves fast.
QVector<Sample> smooth(const QVector<Sample> &in, double step) {
    if (in.size() < 3)
        return in;
    QVector<Sample> out;
    out.reserve(in.size() * 3);
    for (int i = 0; i < in.size() - 1; ++i) {
        const QPointF p0 = in[std::max(0, i - 1)].pos, p1 = in[i].pos, p2 = in[i + 1].pos;
        const QPointF p3 = in[std::min<int>(in.size() - 1, i + 2)].pos;
        const double a = in[i].p, b = in[i + 1].p;
        const int steps = std::clamp(int(std::ceil(QLineF(p1, p2).length() / step)), 1, 12);
        for (int s = 0; s < steps; ++s) {
            const double t = double(s) / steps, t2 = t * t, t3 = t2 * t;
            const QPointF q = 0.5 * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
                                     (3 * p1 - p0 - 3 * p2 + p3) * t3);
            out.append({q, a + (b - a) * t});
        }
    }
    out.append(in.last());
    return out;
}

double signedArea(const QPolygonF &polygon) {
    double area = 0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon[i], &b = polygon[(i + 1) % polygon.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    return area / 2;
}

// Every piece of a stroke is wound the same way and the whole is filled with
// the nonzero rule, so the overlapping pieces merge into one shape: no seams
// between them, and translucent ink doesn't darken where they overlap.
void addWound(QPainterPath &path, QPolygonF polygon) {
    if (signedArea(polygon) < 0)
        std::reverse(polygon.begin(), polygon.end());
    path.addPolygon(polygon);
    path.closeSubpath();
}

QPolygonF disc(const QPointF &center, double radius) {
    const int sides = radius < 2 ? 8 : radius < 8 ? 16 : 28;
    QPolygonF polygon;
    for (int i = 0; i < sides; ++i) {
        const double a = 2 * M_PI * i / sides;
        polygon << center + QPointF(std::cos(a), std::sin(a)) * radius;
    }
    return polygon;
}

// A disc at every sample and a quad between neighbours: a round-tipped line
// whose width can change along it.
template <typename Radius>
void addRoundBody(QPainterPath &path, const QVector<Sample> &samples, Radius radius) {
    for (int i = 0; i < samples.size(); ++i) {
        const double r = radius(i);
        addWound(path, disc(samples[i].pos, r));
        if (i == 0)
            continue;
        const QPointF a = samples[i - 1].pos, b = samples[i].pos, d = b - a;
        const double length = std::hypot(d.x(), d.y());
        if (length < 1e-6)
            continue;
        const QPointF n(-d.y() / length, d.x() / length);
        const double ra = radius(i - 1), rb = r;
        addWound(path, QPolygonF{a + n * ra, b + n * rb, b - n * rb, a - n * ra});
    }
}

double scale(const QSizeF &canvas) { return canvas.height() / InkRenderer::referenceHeight; }

QPainterPath penOutline(const Stroke &stroke, const QSizeF &canvas) {
    const double width = InkRenderer::bodyWidth(stroke) * scale(canvas);
    const auto samples = smooth(toCanvas(stroke, canvas), std::max(1.5, width * 0.6));
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    addRoundBody(path, samples, [&](int i) { return std::max(0.4, width * (0.3 + 0.7 * samples[i].p) / 2); });
    return path;
}

// A broad nib held at a fixed angle: strokes along the nib come out as
// hairlines and strokes across it come out full width, which is what makes
// calligraphy look like calligraphy. The ends taper so strokes don't start
// and stop as blunt slabs.
QPainterPath calligraphyOutline(const Stroke &stroke, const QSizeF &canvas) {
    const double nib = InkRenderer::bodyWidth(stroke) * scale(canvas);
    const auto samples = smooth(toCanvas(stroke, canvas), std::max(1.0, nib * 0.25));
    const double angle = qDegreesToRadians(stroke.angle);
    const QPointF along(std::cos(angle), -std::sin(angle));
    const double hairline = std::max(0.45, nib * 0.03);

    QVector<double> fromStart(samples.size(), 0), toEnd(samples.size(), 0);
    for (int i = 1; i < samples.size(); ++i)
        fromStart[i] = fromStart[i - 1] + QLineF(samples[i - 1].pos, samples[i].pos).length();
    for (int i = 0; i < samples.size(); ++i)
        toEnd[i] = fromStart.last() - fromStart[i];
    const double taper = nib * 1.2;
    auto half = [&](int i) {
        const double ends = std::min(1.0, 0.3 + 0.7 * std::min(fromStart[i], toEnd[i]) / taper);
        return nib / 2 * (0.45 + 0.55 * samples[i].p) * ends;
    };

    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (int i = 1; i < samples.size(); ++i) {
        const QPointF a = samples[i - 1].pos, b = samples[i].pos;
        const QPointF ea = along * half(i - 1), eb = along * half(i);
        addWound(path, QPolygonF{a + ea, b + eb, b - eb, a - ea});
    }
    if (samples.size() == 1) {
        // A tap leaves the mark of the nib itself.
        const QPointF c = samples.first().pos, e = along * half(0);
        const QPointF n(-along.y() * hairline, along.x() * hairline);
        addWound(path, QPolygonF{c + e + n, c - e + n, c - e - n, c + e - n});
    }
    addRoundBody(path, samples, [&](int) { return hairline; });
    return path;
}

QPainterPath linePath(const QVector<Sample> &samples) {
    QPainterPath path;
    if (samples.isEmpty())
        return path;
    path.moveTo(samples.first().pos);
    for (int i = 1; i < samples.size(); ++i)
        path.lineTo(samples[i].pos);
    if (samples.size() == 1)
        path.lineTo(samples.first().pos + QPointF(0.01, 0));
    return path;
}

QList<QPolygonF> shapeOutlines(const Stroke &stroke, const QSizeF &canvas) {
    const auto samples = toCanvas(stroke, canvas);
    if (samples.isEmpty())
        return {};
    const QPointF a = samples.first().pos, b = samples.last().pos;
    if (stroke.tool == "rect") {
        const QRectF r = QRectF(a, b).normalized();
        return {QPolygonF{r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft(), r.topLeft()}};
    }
    if (stroke.tool == "ellipse") {
        const QRectF r = QRectF(a, b).normalized();
        QPolygonF polygon;
        for (int i = 0; i <= 64; ++i) {
            const double t = 2 * M_PI * i / 64;
            polygon << r.center() + QPointF(std::cos(t) * r.width() / 2, std::sin(t) * r.height() / 2);
        }
        return {polygon};
    }
    // An arrow: the shaft, then the two sides of the head at the end point.
    const double width = InkRenderer::bodyWidth(stroke) * scale(canvas);
    const double length = QLineF(a, b).length();
    const double head = std::min(length * 0.6, std::max(width * 4.5, 14 * scale(canvas)));
    const double angle = std::atan2(b.y() - a.y(), b.x() - a.x());
    auto wing = [&](double side) {
        return b - QPointF(std::cos(angle + side), std::sin(angle + side)) * head;
    };
    return {QPolygonF{a, b}, QPolygonF{wing(0.5), b, wing(-0.5)}};
}

} // namespace

namespace InkRenderer {

double bodyWidth(const Stroke &stroke) {
    if (stroke.tool == "calligraphy")
        return stroke.size * 2.6;
    if (stroke.tool == "marker")
        return stroke.size * 1.8;
    if (stroke.tool == "highlighter")
        return stroke.size * 5;
    if (stroke.tool == "laser")
        return stroke.size * 1.3;
    if (stroke.tool == "eraser")
        return 0;
    return stroke.size;
}

QPainterPath filledOutline(const Stroke &stroke, const QSizeF &canvas) {
    return stroke.tool == "calligraphy" ? calligraphyOutline(stroke, canvas) : penOutline(stroke, canvas);
}

QList<QPolygonF> outlines(const Stroke &stroke, const QSizeF &canvas) {
    if (stroke.isShape())
        return shapeOutlines(stroke, canvas);
    const auto samples = smooth(toCanvas(stroke, canvas), 4);
    QPolygonF line;
    for (const auto &sample : samples)
        line << sample.pos;
    return {line};
}

double laserOpacity(const Stroke &stroke, qint64 now) {
    if (stroke.endedAt <= 0)
        return 1;
    return std::clamp(1.0 - double(now - stroke.endedAt) / InkModel::laserFadeMs, 0.0, 1.0);
}

QColor boardColor(const QString &board) {
    if (board == "white")
        return QColor("#f8f8f4");
    if (board == "dark")
        return QColor("#1b1d1e");
    return Qt::transparent;
}

void paint(QPainter &painter, const Stroke &stroke, const QSizeF &canvas, qint64 now) {
    if (stroke.pts.isEmpty() || stroke.tool == "eraser")
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const double width = bodyWidth(stroke) * scale(canvas);

    if (stroke.tool == "pen" || stroke.tool == "calligraphy") {
        painter.setPen(Qt::NoPen);
        painter.setBrush(stroke.color);
        painter.drawPath(filledOutline(stroke, canvas));
    } else if (stroke.tool == "marker" || stroke.tool == "highlighter") {
        QColor color = stroke.color;
        if (stroke.tool == "highlighter")
            color.setAlphaF(0.38);
        // One path with one pen, so a highlighter crossing itself stays even.
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(linePath(smooth(toCanvas(stroke, canvas), std::max(2.0, width * 0.5))));
    } else if (stroke.tool == "laser") {
        const double opacity = laserOpacity(stroke, now);
        const auto path = linePath(smooth(toCanvas(stroke, canvas), 3));
        QColor glow = stroke.color;
        glow.setAlphaF(0.28 * opacity);
        painter.setPen(QPen(glow, width * 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        QColor core = stroke.color.lighter(170);
        core.setAlphaF(opacity);
        painter.setPen(QPen(core, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
    } else {
        painter.setPen(QPen(stroke.color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        for (const auto &line : shapeOutlines(stroke, canvas))
            painter.drawPolyline(line);
    }
    painter.restore();
}

} // namespace InkRenderer
