#include "PreviewWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setMinimumSize(200, 200);
}

void PreviewWidget::emitSize()
{
    const qreal dpr = devicePixelRatioF();
    emit nativeResized(qint64(winId()), qRound(width() * dpr), qRound(height() * dpr));
}

void PreviewWidget::resizeEvent(QResizeEvent *) { emitSize(); }
void PreviewWidget::showEvent(QShowEvent *) { emitSize(); }

QPointF PreviewWidget::toCanvas(const QPointF &pos) const
{
    // Same math as in the engine (Compositor.PreviewViewport): aspect-preserving fit
    const qreal s = std::min(width() / qreal(m_canvasW), height() / qreal(m_canvasH));
    const qreal w = m_canvasW * s, h = m_canvasH * s;
    const qreal x0 = (width() - w) / 2, y0 = (height() - h) / 2;
    return {(pos.x() - x0) / w, (pos.y() - y0) / h};
}

void PreviewWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    emit canvasPressed(toCanvas(e->position()));
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton)) emit canvasDragged(toCanvas(e->position()));
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_dragging) { m_dragging = false; emit canvasReleased(); }
}

void PreviewWidget::wheelEvent(QWheelEvent *e)
{
    emit canvasWheel(e->angleDelta().y());
    e->accept();
}

void PreviewWidget::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete) emit deletePressed();
    else QWidget::keyPressEvent(e);
}
