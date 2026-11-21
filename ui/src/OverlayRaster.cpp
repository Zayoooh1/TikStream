#include "OverlayRaster.h"

#include <QFileInfo>
#include <QFont>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

static QColor argb(const QJsonValue &v, QColor def)
{
    if (!v.isDouble()) return def;
    return QColor::fromRgba(QRgb(quint32(qint64(v.toDouble()))));
}

QString rasterKey(const QJsonObject &o)
{
    if (o["type"].toString() == "Image") {
        const QString p = o["filePath"].toString();
        return "I|" + p + "|" + QString::number(QFileInfo(p).lastModified().toMSecsSinceEpoch());
    }
    QStringList k{"T", o["text"].toString(), o["fontName"].toString(), QString::number(o["fontSize"].toDouble()),
                  QString::number(o["bold"].toBool()), QString::number(o["colorArgb"].toDouble()),
                  QString::number(o["outlineArgb"].toDouble()), QString::number(o["outlineWidth"].toDouble()),
                  QString::number(o["backgroundArgb"].toDouble())};
    return k.join('|');
}

QImage rasterizeSource(const QJsonObject &o)
{
    if (o["type"].toString() == "Image") {
        QImageReader reader(o["filePath"].toString());
        reader.setAutoTransform(true);
        QImage img = reader.read();
        if (img.isNull()) return {};
        if (img.width() > 4096 || img.height() > 4096) img = img.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QString text = o["text"].toString();
    if (text.isEmpty()) text = " ";
    QFont font(o["fontName"].toString("Segoe UI"));
    font.setPixelSize(std::max(8, int(o["fontSize"].toDouble(72))));
    font.setBold(o["bold"].toBool(true));
    font.setStyleStrategy(QFont::PreferAntialias);
    const QColor fill = argb(o["colorArgb"], Qt::white);
    const QColor outline = argb(o["outlineArgb"], Qt::black);
    const QColor bg = argb(o["backgroundArgb"], Qt::transparent);
    const double ow = o["outlineWidth"].toDouble(6);

    QPainterPath path;
    const QFontMetricsF fm(font);
    double y = fm.ascent();
    for (const QString &line : text.split('\n')) {
        path.addText(0, y, font, line);
        y += fm.lineSpacing();
    }
    const QRectF b = path.boundingRect().isEmpty() ? QRectF(0, 0, 10, fm.height()) : path.boundingRect();
    const double pad = ow + (bg.alpha() > 0 ? font.pixelSize() * 0.35 : 6.0);
    const int w = std::clamp(int(std::ceil(b.width() + pad * 2)), 4, 4096);
    const int h = std::clamp(int(std::ceil(b.height() + pad * 2)), 4, 4096);

    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    if (bg.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(0, 0, w, h), std::min(w, h) * 0.22, std::min(w, h) * 0.22);
    }
    p.translate(pad - b.x(), pad - b.y());
    if (ow > 0 && outline.alpha() > 0) p.strokePath(path, QPen(outline, ow * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, fill);
    p.end();
    return img;
}
