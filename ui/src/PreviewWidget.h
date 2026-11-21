#pragma once
#include <QWidget>

// Native window where the C# engine renders the preview via D3D11 swapchain (Qt does not paint here).
class PreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget *parent = nullptr);
    void setCanvasSize(int w, int h) { m_canvasW = w; m_canvasH = h; }
    QPaintEngine *paintEngine() const override { return nullptr; }

    // normalized canvas coordinates (0..1) for a point in widget space
    QPointF toCanvas(const QPointF &widgetPos) const;

signals:
    void nativeResized(qint64 hwnd, int physW, int physH);
    void canvasPressed(QPointF p);
    void canvasDragged(QPointF p);
    void canvasReleased();
    void canvasWheel(int delta);
    void deletePressed();

protected:
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void paintEvent(QPaintEvent *) override {}
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void enterEvent(QEnterEvent *) override { setFocus(); }

private:
    void emitSize();
    int m_canvasW = 1080, m_canvasH = 1920;
    bool m_dragging = false;
};
