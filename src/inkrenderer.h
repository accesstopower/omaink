#pragma once

#include "ink.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSizeF>

// Draws strokes. The tablet page (src/web/pad.js) carries a line-for-line
// JavaScript copy of this file, so ink looks the same on both screens; change
// them together.
namespace InkRenderer {

constexpr double referenceHeight = 1080;

// How wide a stroke's body is, in pixels on a 1080-high screen.
double bodyWidth(const Stroke &stroke);

// The filled shape of a pen or calligraphy stroke, in canvas pixels.
QPainterPath filledOutline(const Stroke &stroke, const QSizeF &canvas);

// The lines a stroke follows, in canvas pixels: the smoothed path, or a
// shape's edges. The eraser tests against these.
QList<QPolygonF> outlines(const Stroke &stroke, const QSizeF &canvas);

// How opaque a laser stroke still is, from 1 while drawn down to 0.
double laserOpacity(const Stroke &stroke, qint64 now);

void paint(QPainter &painter, const Stroke &stroke, const QSizeF &canvas, qint64 now);

QColor boardColor(const QString &board);

}
