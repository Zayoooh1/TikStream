#pragma once
#include <QColor>
#include <QElapsedTimer>
#include <QIcon>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

QIcon themedIcon(const QString &name);

class PaddedSpinBox : public QSpinBox
{
public:
    explicit PaddedSpinBox(QWidget *parent = nullptr);
};

// Wskaźnik poziomu jak w OBS: zielony → żółty → czerwony, skala dB, wskazanie szczytu z opadaniem.
class MeterWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MeterWidget(QWidget *parent = nullptr, Qt::Orientation orientation = Qt::Horizontal);
    void setLevel(float linearPeak); // 0..1
    void setMuted(bool m) { m_muted = m; update(); }
    QSize sizeHint() const override { return m_orientation == Qt::Vertical ? QSize(14, 92) : QSize(150, 12); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void animateLevel();
    QElapsedTimer m_clock;
    QTimer m_animation;
    float m_targetDb = -60.f;
    float m_db = -60.f;
    float m_peakDb = -60.f;
    qint64 m_peakTime = 0;
    qint64 m_lastFrame = 0;
    bool m_muted = false;
    Qt::Orientation m_orientation;
};

class ColorButton : public QPushButton
{
    Q_OBJECT
public:
    explicit ColorButton(const QString &title, QWidget *parent = nullptr);
    void setColor(const QColor &c);
    QColor color() const { return m_color; }

signals:
    void colorChanged(const QColor &);

private:
    void refresh();
    QColor m_color = Qt::white;
    QString m_title;
};

// Nagłówek sekcji w panelach (mały, wersalikami, przygaszony).
class SectionLabel : public QWidget
{
    Q_OBJECT
public:
    explicit SectionLabel(const QString &text, QWidget *parent = nullptr);
};
