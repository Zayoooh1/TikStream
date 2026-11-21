#pragma once
#include <QWidget>

// Natywne okno, w które silnik C# rysuje podgląd przez swapchain D3D11 (Qt nic tu nie maluje).
class PreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget *parent = nullptr);
    void setCanvasSize(int w, int h) { m_canvasW = w; m_canvasH = h; }
    QPaintEngine *paintEngine() const override { return nullptr; }

    // znormalizowane współrzędne płótna (0..1) dla punktu w widżecie
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
