#include "Widgets.h"
#include <QLineEdit>

PaddedSpinBox::PaddedSpinBox(QWidget *parent) : QSpinBox(parent)
{
    lineEdit()->setTextMargins(9, 0, 0, 0);
}

#include <QColorDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSvgRenderer>
#include <cmath>

QIcon themedIcon(const QString &name)
{
    // Icons rendered from SVG in two states: normal and disabled
    QIcon icon;
    QSvgRenderer r(QStringLiteral(":/icons/%1.svg").arg(name));
    for (int size : {16, 20, 24, 32}) {
        for (auto mode : {QIcon::Normal, QIcon::Disabled}) {
            QPixmap pm(size * 2, size * 2);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            r.render(&p);
            if (mode == QIcon::Disabled) {
                p.setCompositionMode(QPainter::CompositionMode_SourceIn);
                p.fillRect(pm.rect(), QColor(255, 255, 255, 70));
            }
            p.end();
            pm.setDevicePixelRatio(2.0);
            icon.addPixmap(pm, mode);
        }
    }
    return icon;
}

// ---------------- MeterWidget ----------------

MeterWidget::MeterWidget(QWidget *parent, Qt::Orientation orientation) : QWidget(parent), m_orientation(orientation)
{
    if (orientation == Qt::Vertical) {
        setFixedWidth(12);
        setMinimumHeight(64);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    } else {
        setMinimumHeight(10);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    m_clock.start();
    m_animation.setInterval(16);
    m_animation.setTimerType(Qt::PreciseTimer);
    connect(&m_animation, &QTimer::timeout, this, &MeterWidget::animateLevel);
}

void MeterWidget::setLevel(float linear)
{
    float db = linear > 0.000001f ? 20.f * std::log10(linear) : -60.f;
    m_targetDb = std::clamp(db, -60.f, 0.f);
    const qint64 now = m_clock.elapsed();
    if (m_targetDb >= m_peakDb && m_targetDb > -60.f) {
        m_peakDb = m_targetDb;
        m_peakTime = now;
    }
    if (!m_animation.isActive() && (m_targetDb != m_db || m_peakDb > m_db)) {
        m_lastFrame = now;
        m_animation.start();
    }
}

void MeterWidget::animateLevel()
{
    const qint64 now = m_clock.elapsed();
    const float seconds = float(now - m_lastFrame) / 1000.f;
    m_lastFrame = now;
    if (m_targetDb > m_db)
        m_db += (m_targetDb - m_db) * (1.f - std::exp(-seconds / 0.025f));
    else
        m_db = std::max(m_targetDb, m_db - 52.f * seconds);
    if (std::abs(m_targetDb - m_db) < 0.1f) m_db = m_targetDb;
    if (now - m_peakTime >= 1500)
        m_peakDb = std::max(m_db, m_peakDb - 35.f * seconds);
    if (m_db == m_targetDb && m_peakDb <= m_db) m_animation.stop();
    update();
}

void MeterWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    QRect track = rect();
    if (m_orientation == Qt::Vertical) {
        p.translate(0, height());
        p.rotate(-90);
        track = QRect(0, 0, height(), width());
    }
    const QRect r = track.adjusted(0, 0, -1, -1);
    p.fillRect(r, QColor(0x14, 0x15, 0x1b));

    auto xFor = [&](float db) { return int(r.left() + (db + 60.f) / 60.f * r.width()); };
    const int xYellow = xFor(-20.f), xRed = xFor(-9.f);

    // Zone backgrounds (dimmed) — OBS style
    p.fillRect(QRect(QPoint(r.left(), r.top()), QPoint(xYellow, r.bottom())), QColor(0x1f, 0x3d, 0x2a));
    p.fillRect(QRect(QPoint(xYellow, r.top()), QPoint(xRed, r.bottom())), QColor(0x45, 0x3f, 0x1e));
    p.fillRect(QRect(QPoint(xRed, r.top()), QPoint(r.right(), r.bottom())), QColor(0x4a, 0x1f, 0x24));

    if (!m_muted) {
        const int x = xFor(m_db);
        auto seg = [&](int a, int b, QColor c) {
            if (x <= a) return;
            p.fillRect(QRect(QPoint(a, r.top()), QPoint(std::min(x, b), r.bottom())), c);
        };
        seg(r.left(), xYellow, QColor(0x3d, 0xd6, 0x6f));
        seg(xYellow, xRed, QColor(0xf5, 0xd0, 0x3b));
        seg(xRed, r.right(), QColor(0xfe, 0x2c, 0x55));
        if (m_peakDb > -59.f) {
            const int px = xFor(m_peakDb);
            p.fillRect(QRect(px - 1, r.top(), 2, r.height() + 1), QColor(235, 235, 240));
        }
    }
    // Tick marks every 6 dB
    p.setPen(QColor(0, 0, 0, 90));
    for (int db = -54; db < 0; db += 6) {
        int x = xFor(float(db));
        p.drawLine(x, r.top(), x, r.bottom());
    }
}

// ---------------- ColorButton ----------------

ColorButton::ColorButton(const QString &title, QWidget *parent) : QPushButton(parent), m_title(title)
{
    setCursor(Qt::PointingHandCursor);
    connect(this, &QPushButton::clicked, this, [this] {
        QColor c = QColorDialog::getColor(m_color, window(), m_title, QColorDialog::ShowAlphaChannel);
        if (c.isValid()) { setColor(c); emit colorChanged(c); }
    });
    refresh();
}

void ColorButton::setColor(const QColor &c)
{
    m_color = c;
    refresh();
}

void ColorButton::refresh()
{
    QPixmap pm(28, 28);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Transparency backdrop
    p.setBrush(QColor(90, 90, 100));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(2, 2, 24, 24), 6, 6);
    p.setBrush(m_color);
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    p.drawRoundedRect(QRectF(2.5, 2.5, 23, 23), 6, 6);
    p.end();
    pm.setDevicePixelRatio(2.0);
    setIcon(QIcon(pm));
    setText(m_title);
}

// ---------------- SectionLabel ----------------

SectionLabel::SectionLabel(const QString &text, QWidget *parent) : QWidget(parent)
{
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(0, 8, 0, 2);
    auto *lbl = new QLabel(text.toUpper(), this);
    lbl->setObjectName("sectionLabel");
    l->addWidget(lbl);
    auto *line = new QWidget(this);
    line->setObjectName("sectionLine");
    line->setFixedHeight(1);
    l->addWidget(line, 1);
}
