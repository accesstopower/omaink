#include "inkcanvas.h"

#include "inkrenderer.h"
#include "overlay.h"

#include <QDateTime>
#include <QPainter>
#include <QQuickWindow>

InkCanvas::InkCanvas(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAntialiasing(true);
    // Lasers fade over time, so they need frames even when nothing moves.
    m_animation.setInterval(16);
    connect(&m_animation, &QTimer::timeout, this, [this] {
        if (!m_overlay)
            return;
        m_overlay->model()->prune(QDateTime::currentMSecsSinceEpoch());
        update();
        if (!m_overlay->model()->hasFading())
            m_animation.stop();
    });
}

void InkCanvas::setOverlay(Overlay *overlay) {
    if (overlay == m_overlay)
        return;
    if (m_overlay)
        disconnect(m_overlay->model(), nullptr, this, nullptr);
    m_overlay = overlay;
    if (m_overlay) {
        InkModel *model = m_overlay->model();
        auto invalidate = [this] {
            m_cacheDirty = true;
            update();
        };
        connect(model, &InkModel::committed, this, invalidate);
        connect(model, &InkModel::removed, this, invalidate);
        connect(model, &InkModel::reset, this, invalidate);
        connect(model, &InkModel::changed, this, [this] {
            update();
            if (m_overlay->model()->hasFading() && !m_animation.isActive())
                m_animation.start();
        });
    }
    m_cacheDirty = true;
    update();
    emit overlayChanged();
}

void InkCanvas::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        m_cacheDirty = true;
}

void InkCanvas::paint(QPainter *painter) {
    if (!m_overlay)
        return;
    const QSizeF canvas = size();
    InkModel *model = m_overlay->model();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const QColor board = InkRenderer::boardColor(model->board());
    if (board.alpha() > 0)
        painter->fillRect(QRectF(QPointF(), canvas), board);

    const qreal ratio = window() ? window()->effectiveDevicePixelRatio() : 1.0;
    const QSize pixels = (canvas * ratio).toSize();
    if (m_cacheDirty || m_cache.size() != pixels) {
        m_cache = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
        m_cache.setDevicePixelRatio(ratio);
        m_cache.fill(Qt::transparent);
        QPainter cachePainter(&m_cache);
        for (const auto &stroke : model->strokes())
            InkRenderer::paint(cachePainter, *stroke, canvas, now);
        m_cacheDirty = false;
    }
    painter->drawImage(QPointF(), m_cache);

    for (const auto &stroke : model->liveStrokes())
        InkRenderer::paint(*painter, *stroke, canvas, now);
}

QPointF InkCanvas::normalized(const QPointF &position) const {
    return {position.x() / std::max(1.0, width()), position.y() / std::max(1.0, height())};
}

void InkCanvas::mousePressEvent(QMouseEvent *event) {
    if (!m_overlay || !m_overlay->drawing()) {
        event->ignore();
        return;
    }
    // The right button erases, whatever tool is in hand.
    m_overlay->beginLocal(normalized(event->position()), event->button() == Qt::RightButton);
    event->accept();
}

void InkCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (m_overlay)
        m_overlay->extendLocal(normalized(event->position()));
}

void InkCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (m_overlay) {
        m_overlay->extendLocal(normalized(event->position()));
        m_overlay->endLocal();
    }
}
