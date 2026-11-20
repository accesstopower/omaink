#pragma once

#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>

#include "overlay.h"

// Paints the ink and turns the mouse or a pen tablet into strokes. Finished
// strokes are drawn once into a cached image, so a screen full of ink stays
// cheap to repaint while a new stroke is drawn over it.
class InkCanvas : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(Overlay *controller READ overlay WRITE setOverlay NOTIFY overlayChanged)

public:
    explicit InkCanvas(QQuickItem *parent = nullptr);

    Overlay *overlay() const { return m_overlay; }
    void setOverlay(Overlay *overlay);

    void paint(QPainter *painter) override;

signals:
    void overlayChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    QPointF normalized(const QPointF &position) const;

    QPointer<Overlay> m_overlay;
    QImage m_cache;
    bool m_cacheDirty = true;
    QTimer m_animation;
};
