#include "OverlayRaster.h"

#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QImageReader>
#include <QLinearGradient>
#include <QLocale>
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
    const double rawW = std::max(4.0, std::ceil(b.width() + pad * 2));
    const double rawH = std::max(4.0, std::ceil(b.height() + pad * 2));
    const double fitScale = std::min({1.0, 4096.0 / rawW, 4096.0 / rawH});
    const int w = std::clamp(int(std::ceil(rawW * fitScale)), 4, 4096);
    const int h = std::clamp(int(std::ceil(rawH * fitScale)), 4, 4096);

    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    p.scale(fitScale, fitScale);
    if (bg.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(0, 0, rawW, rawH), std::min(rawW, rawH) * 0.22, std::min(rawW, rawH) * 0.22);
    }
    p.translate(pad - b.x(), pad - b.y());
    if (ow > 0 && outline.alpha() > 0) p.strokePath(path, QPen(outline, ow * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, fill);
    p.end();
    return img;
}

QImage rasterizeGoalBar(const QJsonObject &o, qint64 currentValue)
{
    constexpr int w = 640;
    constexpr int h = 128;
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const QString title = o["goalTitle"].toString("Cel: Diamenty").trimmed();
    const qint64 target = std::max<qint64>(1, o["goalTarget"].toInteger(1000));
    const qint64 value = std::max<qint64>(0, currentValue);
    const bool showPercent = o["goalShowPercent"].toBool(true);
    const QColor fillCol = argb(o["goalFillColorArgb"], QColor(254, 44, 85, 255));
    const QColor bgCol = argb(o["goalBgColorArgb"], QColor(20, 22, 31, 216));
    const QColor textCol = argb(o["goalTextColorArgb"], Qt::white);

    const double ratio = std::clamp(double(value) / double(target), 0.0, 1.0);
    const int pct = int(std::lround(ratio * 100.0));

    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    // Card background
    if (bgCol.alpha() > 0) {
        const QRectF card(2, 2, w - 4, h - 4);
        p.setPen(QPen(QColor(255, 255, 255, 38), 1.5));
        p.setBrush(bgCol);
        p.drawRoundedRect(card, 18, 18);
    }

    const int padX = 24;
    const int topY = 18;
    const int headerH = 38;

    // Right value label ("current / target (XX%)")
    const QLocale en(QLocale::English);
    QString valText = QStringLiteral("%1 / %2").arg(en.toString(value), en.toString(target));
    if (showPercent) valText += QStringLiteral("  (%1%)").arg(pct);

    QFont valFont(QStringLiteral("Segoe UI"));
    valFont.setPixelSize(22);
    valFont.setWeight(QFont::DemiBold);
    const QFontMetrics vfm(valFont);
    const int valW = vfm.horizontalAdvance(valText) + 8;

    QFont titleFont(QStringLiteral("Segoe UI"));
    titleFont.setPixelSize(24);
    titleFont.setWeight(QFont::Bold);
    const QFontMetrics tfm(titleFont);
    const int titleW = std::max(80, w - padX * 2 - valW - 14);

    p.setFont(titleFont);
    p.setPen(textCol);
    p.drawText(QRectF(padX, topY, titleW, headerH), Qt::AlignVCenter | Qt::AlignLeft,
               tfm.elidedText(title.isEmpty() ? QStringLiteral("Goal") : title, Qt::ElideRight, titleW));

    p.setFont(valFont);
    p.setPen(QColor(textCol.red(), textCol.green(), textCol.blue(), 230));
    p.drawText(QRectF(w - padX - valW, topY, valW, headerH), Qt::AlignVCenter | Qt::AlignRight, valText);

    // Progress track
    const QRectF track(padX, 68, w - padX * 2, 36);
    p.setPen(QPen(QColor(255, 255, 255, 30), 1.0));
    p.setBrush(QColor(255, 255, 255, 28));
    p.drawRoundedRect(track, 18, 18);

    // Progress fill
    if (ratio > 0.001) {
        const double fillW = std::max(36.0, track.width() * ratio);
        const QRectF fillRect(track.left(), track.top(), std::min(track.width(), fillW), track.height());
        QPainterPath clipPath;
        clipPath.addRoundedRect(track, 18, 18);
        p.save();
        p.setClipPath(clipPath);
        QLinearGradient grad(fillRect.topLeft(), fillRect.topRight());
        grad.setColorAt(0.0, fillCol.darker(115));
        grad.setColorAt(1.0, fillCol.lighter(115));
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawRoundedRect(fillRect, 18, 18);

        // Subtle top highlight
        QLinearGradient gloss(fillRect.topLeft(), QPointF(fillRect.left(), fillRect.center().y()));
        gloss.setColorAt(0.0, QColor(255, 255, 255, 65));
        gloss.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setBrush(gloss);
        p.drawRoundedRect(QRectF(fillRect.left(), fillRect.top(), fillRect.width(), fillRect.height() * 0.5), 14, 14);
        p.restore();
    }

    p.end();
    return img;
}

QImage rasterizeNowPlaying(const QJsonObject &o, const QJsonObject &mediaState, const QImage &albumArt)
{
    const bool hideWhenPaused = o["nowPlayingHideWhenPaused"].toBool(true);
    const bool showArt = o["nowPlayingShowArt"].toBool(true);
    const QString appFilter = o["nowPlayingAppFilter"].toString().trimmed();

    const bool isPlaying = mediaState["isPlaying"].toBool(false);
    const QString title = mediaState["title"].toString().trimmed();
    const QString artist = mediaState["artist"].toString().trimmed();
    const QString appId = mediaState["appId"].toString().trimmed();

    const bool filterMismatch = !appFilter.isEmpty() && !appId.contains(appFilter, Qt::CaseInsensitive);
    if (filterMismatch || (title.isEmpty() && artist.isEmpty()) || (hideWhenPaused && !isPlaying)) {
        QImage empty(1, 1, QImage::Format_ARGB32_Premultiplied);
        empty.fill(Qt::transparent);
        return empty;
    }

    constexpr int w = 620;
    constexpr int h = 136;
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const QColor bgCol = argb(o["nowPlayingBgColorArgb"], QColor(20, 22, 31, 216));
    const QColor accentCol = argb(o["nowPlayingAccentColorArgb"], QColor(37, 244, 238, 255));
    const QColor textCol = argb(o["nowPlayingTextColorArgb"], Qt::white);

    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    // Glass card background
    if (bgCol.alpha() > 0) {
        const QRectF card(2, 2, w - 4, h - 4);
        p.setPen(QPen(QColor(255, 255, 255, 36), 1.5));
        p.setBrush(bgCol);
        p.drawRoundedRect(card, 18, 18);
    }

    // Left accent strip
    p.setPen(Qt::NoPen);
    p.setBrush(accentCol);
    p.drawRoundedRect(QRectF(12, 22, 5, h - 44), 2.5, 2.5);

    int textX = 28;
    if (showArt) {
        const QRectF artRect(26, 18, 100, 100);
        QPainterPath clip;
        clip.addRoundedRect(artRect, 14, 14);
        p.save();
        p.setClipPath(clip);
        if (!albumArt.isNull()) {
            const QImage scaled = albumArt.scaled(100, 100, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            const int sx = std::max(0, (scaled.width() - 100) / 2);
            const int sy = std::max(0, (scaled.height() - 100) / 2);
            p.drawImage(artRect.topLeft(), scaled, QRect(sx, sy, 100, 100));
        } else {
            p.fillRect(artRect, QColor(36, 40, 56, 235));
            QFont noteFont(QStringLiteral("Segoe UI Symbol"));
            noteFont.setPixelSize(42);
            p.setFont(noteFont);
            p.setPen(accentCol);
            p.drawText(artRect, Qt::AlignCenter, QStringLiteral("♫"));
        }
        p.restore();
        p.setPen(QPen(QColor(255, 255, 255, 42), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(artRect, 14, 14);
        textX = 144;
    }

    const int textW = std::max(100, w - textX - 24);

    // Status line ("NOW PLAYING" / "PAUSED")
    QFont badgeFont(QStringLiteral("Segoe UI"));
    badgeFont.setPixelSize(13);
    badgeFont.setWeight(QFont::Bold);
    p.setFont(badgeFont);
    p.setPen(accentCol);
    p.drawText(QRectF(textX, 18, textW, 20), Qt::AlignVCenter | Qt::AlignLeft,
               isPlaying ? QStringLiteral("● NOW PLAYING") : QStringLiteral("❚❚ PAUSED"));

    // Track title (bold, elided)
    QFont titleFont(QStringLiteral("Segoe UI"));
    titleFont.setPixelSize(28);
    titleFont.setWeight(QFont::Bold);
    const QFontMetrics tfm(titleFont);
    p.setFont(titleFont);
    p.setPen(textCol);
    p.drawText(QRectF(textX, 42, textW, 40), Qt::AlignVCenter | Qt::AlignLeft,
               tfm.elidedText(title.isEmpty() ? artist : title, Qt::ElideRight, textW));

    // Artist (subtitle, elided)
    if (!artist.isEmpty() && !title.isEmpty()) {
        QFont artistFont(QStringLiteral("Segoe UI"));
        artistFont.setPixelSize(21);
        artistFont.setWeight(QFont::Normal);
        const QFontMetrics afm(artistFont);
        p.setFont(artistFont);
        p.setPen(QColor(textCol.red(), textCol.green(), textCol.blue(), 190));
        p.drawText(QRectF(textX, 86, textW, 32), Qt::AlignVCenter | Qt::AlignLeft,
                   afm.elidedText(artist, Qt::ElideRight, textW));
    }

    p.end();
    return img;
}
