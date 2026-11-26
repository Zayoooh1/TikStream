#include "PostStreamSummaryDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

namespace {
QString fmtDuration(double sec)
{
    const qint64 s = std::max<qint64>(0, qint64(std::llround(sec)));
    return QString("%1:%2:%3")
        .arg(s / 3600, 2, 10, QChar('0'))
        .arg((s / 60) % 60, 2, 10, QChar('0'))
        .arg(s % 60, 2, 10, QChar('0'));
}

QFrame *makeTile(const QString &title, const QString &value, const QString &sub = {}, const QString &accent = QStringLiteral("#25f4ee"))
{
    auto *f = new QFrame;
    f->setObjectName("card");
    auto *v = new QVBoxLayout(f);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(3);
    auto *t = new QLabel(title);
    t->setObjectName("hint");
    auto *val = new QLabel(value);
    val->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 700; color: %1;").arg(accent));
    v->addWidget(t);
    v->addWidget(val);
    if (!sub.isEmpty()) {
        auto *s = new QLabel(sub);
        s->setObjectName("hint");
        s->setWordWrap(true);
        v->addWidget(s);
    }
    return f;
}
} // namespace

QJsonObject PostStreamSummaryDialog::toJson(const StreamSessionSummary &s)
{
    QJsonArray gifters;
    for (const auto &[nick, diamonds] : s.topGifters) {
        gifters.append(QJsonObject{{"nick", nick}, {"diamonds", diamonds}});
    }
    QJsonArray replays;
    for (const auto &p : s.savedReplays) replays.append(p);

    return QJsonObject{
        {"startTimeUtc", s.startTimeUtc.toUTC().toString(Qt::ISODate)},
        {"endTimeUtc", s.endTimeUtc.toUTC().toString(Qt::ISODate)},
        {"durationSec", s.durationSec},
        {"peakViewers", s.peakViewers},
        {"peakTikTokViewers", s.peakTikTokViewers},
        {"peakKickViewers", s.peakKickViewers},
        {"avgViewers", s.avgViewers},
        {"totalGifts", s.totalGifts},
        {"totalDiamonds", s.totalDiamonds},
        {"estimatedUsd", s.totalDiamonds * 0.005},
        {"totalTaps", s.totalTaps},
        {"totalFollows", s.totalFollows},
        {"totalChatMessages", s.totalChatMessages},
        {"topGifters", gifters},
        {"encodedFrames", s.encodedFrames},
        {"droppedPackets", s.droppedPackets},
        {"reconnects", s.reconnects},
        {"avgBitrateKbps", s.avgBitrateKbps},
        {"avgFps", s.avgFps},
        {"savedReplays", replays},
    };
}

QString PostStreamSummaryDialog::toMarkdown(const StreamSessionSummary &s)
{
    const QLocale en(QLocale::English);
    QStringList lines;
    lines << QStringLiteral("# TikStream — Podsumowanie streama");
    lines << QStringLiteral("- **Czas trwania:** %1 (%2 → %3)")
                 .arg(fmtDuration(s.durationSec),
                      s.startTimeUtc.toLocalTime().toString("yyyy-MM-dd HH:mm"),
                      s.endTimeUtc.toLocalTime().toString("HH:mm"));
    lines << QStringLiteral("- **Widzowie:** Peak %1 (TikTok: %2, Kick: %3) | Średnio: %4")
                 .arg(en.toString(s.peakViewers), en.toString(s.peakTikTokViewers),
                      en.toString(s.peakKickViewers), QString::number(s.avgViewers, 'f', 1));
    lines << QStringLiteral("- **Diamenty / Gifty:** %1 💎 (~$%2) | Gifty: %3 | Followy: %4 | Lajki: %5 | Wiadomości czatu: %6")
                 .arg(en.toString(s.totalDiamonds), QString::number(s.totalDiamonds * 0.005, 'f', 2),
                      en.toString(s.totalGifts), en.toString(s.totalFollows),
                      en.toString(s.totalTaps), en.toString(s.totalChatMessages));
    lines << QStringLiteral("- **Zdrowie streama:** Śr. FPS: %1 | Śr. Bitrate: %2 kbps | Klatki: %3 | Dropped: %4 | Reconnecty: %5")
                 .arg(QString::number(s.avgFps, 'f', 1), QString::number(s.avgBitrateKbps, 'f', 0),
                      en.toString(s.encodedFrames), en.toString(s.droppedPackets), QString::number(s.reconnects));
    if (!s.topGifters.isEmpty()) {
        lines << QStringLiteral("\n## Top 5 Darczyńców");
        for (int i = 0; i < s.topGifters.size(); ++i) {
            lines << QStringLiteral("%1. **%2** — %3 💎")
                         .arg(i + 1).arg(s.topGifters[i].first, en.toString(s.topGifters[i].second));
        }
    }
    if (!s.savedReplays.isEmpty()) {
        lines << QStringLiteral("\n## Zapisane klipy Replay Buffer");
        for (const auto &r : s.savedReplays) lines << QStringLiteral("- %1").arg(QFileInfo(r).fileName());
    }
    return lines.join('\n');
}

QString PostStreamSummaryDialog::autoSaveReport(const StreamSessionSummary &s)
{
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString baseDir = appData.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QDir(appData).filePath("TikStream");
    const QString dirPath = QDir(baseDir).filePath("reports");
    QDir().mkpath(dirPath);
    const QString stamp = (s.endTimeUtc.isValid() ? s.endTimeUtc.toLocalTime() : QDateTime::currentDateTime())
        .toString("yyyy-MM-dd_HH-mm-ss");
    const QString fullPath = QDir(dirPath).filePath(QStringLiteral("stream_%1.json").arg(stamp));
    QFile f(fullPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(toJson(s)).toJson(QJsonDocument::Indented));
        return fullPath;
    }
    return {};
}

PostStreamSummaryDialog::PostStreamSummaryDialog(const StreamSessionSummary &summary, const QString &autoSavedPath, QWidget *parent)
    : QDialog(parent), m_summary(summary)
{
    setWindowTitle("Raport po streamie — TikStream");
    resize(700, 560);

    const QLocale en(QLocale::English);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 14);
    root->setSpacing(12);

    auto *header = new QLabel(QStringLiteral("Podsumowanie transmisji  •  %1")
        .arg(summary.startTimeUtc.toLocalTime().toString("yyyy-MM-dd HH:mm")));
    header->setObjectName("cardTitle");
    header->setStyleSheet("font-size: 18px; font-weight: 700;");
    root->addWidget(header);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    auto *bv = new QVBoxLayout(body);
    bv->setContentsMargins(0, 0, 6, 0);
    bv->setSpacing(12);

    // KPI tiles
    auto *grid = new QGridLayout;
    grid->setSpacing(10);
    grid->addWidget(makeTile("Czas trwania", fmtDuration(summary.durationSec),
        summary.startTimeUtc.toLocalTime().toString("HH:mm") + " – " + summary.endTimeUtc.toLocalTime().toString("HH:mm"), "#e2e8f0"), 0, 0);
    grid->addWidget(makeTile("Peak widzów", en.toString(summary.peakViewers),
        QStringLiteral("TikTok: %1  •  Kick: %2  •  Śr: %3")
            .arg(en.toString(summary.peakTikTokViewers), en.toString(summary.peakKickViewers), QString::number(summary.avgViewers, 'f', 1)),
        "#25f4ee"), 0, 1);
    grid->addWidget(makeTile("Diamenty", en.toString(summary.totalDiamonds) + " 💎",
        QStringLiteral("Szacowane: ~$%1  •  Gifty: %2")
            .arg(QString::number(summary.totalDiamonds * 0.005, 'f', 2), en.toString(summary.totalGifts)),
        "#ffd666"), 1, 0);
    grid->addWidget(makeTile("Aktywność", en.toString(summary.totalChatMessages) + " msg",
        QStringLiteral("Followy: %1  •  Lajki: %2")
            .arg(en.toString(summary.totalFollows), en.toString(summary.totalTaps)),
        "#4ade80"), 1, 1);
    bv->addLayout(grid);

    // Stream health card
    auto *healthCard = new QFrame(body);
    healthCard->setObjectName("card");
    auto *hv = new QVBoxLayout(healthCard);
    hv->setContentsMargins(14, 10, 14, 12);
    auto *ht = new QLabel("Zdrowie połączenia i enkodera");
    ht->setObjectName("cardTitle");
    hv->addWidget(ht);
    auto *hl = new QLabel(QStringLiteral("Śr. FPS: <b>%1</b>   •   Śr. Bitrate: <b>%2 kbps</b>   •   Zakodowane klatki: <b>%3</b>   •   Dropped: <b>%4</b>   •   Reconnecty: <b>%5</b>")
        .arg(QString::number(summary.avgFps, 'f', 1), QString::number(summary.avgBitrateKbps, 'f', 0),
             en.toString(summary.encodedFrames), en.toString(summary.droppedPackets), QString::number(summary.reconnects)));
    hl->setWordWrap(true);
    hv->addWidget(hl);
    bv->addWidget(healthCard);

    // Top 5 gifters
    auto *giftCard = new QFrame(body);
    giftCard->setObjectName("card");
    auto *gv = new QVBoxLayout(giftCard);
    gv->setContentsMargins(14, 10, 14, 12);
    auto *gt = new QLabel("Top 5 Darczyńców");
    gt->setObjectName("cardTitle");
    gv->addWidget(gt);
    if (summary.topGifters.isEmpty()) {
        auto *none = new QLabel("Brak zarejestrowanych giftów w tej sesji.");
        none->setObjectName("hint");
        gv->addWidget(none);
    } else {
        for (int i = 0; i < summary.topGifters.size(); ++i) {
            auto *row = new QHBoxLayout;
            auto *rank = new QLabel(QStringLiteral("#%1  %2").arg(i + 1).arg(summary.topGifters[i].first));
            rank->setStyleSheet("font-weight: 600;");
            auto *pts = new QLabel(QStringLiteral("%1 💎").arg(en.toString(summary.topGifters[i].second)));
            pts->setStyleSheet("color: #ffd666; font-weight: 600;");
            row->addWidget(rank);
            row->addStretch(1);
            row->addWidget(pts);
            gv->addLayout(row);
        }
    }
    bv->addWidget(giftCard);

    // Saved replays
    if (!summary.savedReplays.isEmpty()) {
        auto *repCard = new QFrame(body);
        repCard->setObjectName("card");
        auto *rv = new QVBoxLayout(repCard);
        rv->setContentsMargins(14, 10, 14, 12);
        auto *rh = new QHBoxLayout;
        auto *rt = new QLabel(QStringLiteral("Zapisane klipy Replay Buffer (%1)").arg(summary.savedReplays.size()));
        rt->setObjectName("cardTitle");
        auto *openFolder = new QPushButton("Otwórz folder");
        openFolder->setCursor(Qt::PointingHandCursor);
        const QString folder = QFileInfo(summary.savedReplays.first()).absolutePath();
        connect(openFolder, &QPushButton::clicked, this, [folder] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        });
        rh->addWidget(rt);
        rh->addStretch(1);
        rh->addWidget(openFolder);
        rv->addLayout(rh);
        for (const auto &p : summary.savedReplays) {
            auto *lbl = new QLabel("• " + QFileInfo(p).fileName());
            lbl->setToolTip(p);
            rv->addWidget(lbl);
        }
        bv->addWidget(repCard);
    }

    if (!autoSavedPath.isEmpty()) {
        auto *savedLbl = new QLabel(QStringLiteral("Raport zapisano automatycznie: %1").arg(QDir::toNativeSeparators(autoSavedPath)));
        savedLbl->setObjectName("hint");
        savedLbl->setWordWrap(true);
        bv->addWidget(savedLbl);
    }

    bv->addStretch(1);
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    // Action buttons
    auto *btnRow = new QHBoxLayout;
    auto *copyBtn = new QPushButton("Kopiuj podsumowanie");
    copyBtn->setCursor(Qt::PointingHandCursor);
    auto *exportBtn = new QPushButton("Eksportuj JSON");
    exportBtn->setCursor(Qt::PointingHandCursor);
    auto *closeBtn = new QPushButton("Zamknij");
    closeBtn->setObjectName("primary");
    closeBtn->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(copyBtn);
    btnRow->addWidget(exportBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(copyBtn, &QPushButton::clicked, this, [this, copyBtn] {
        QApplication::clipboard()->setText(toMarkdown(m_summary));
        copyBtn->setText("Skopiowano ✓");
    });
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString defName = QStringLiteral("stream_%1.json")
            .arg(m_summary.endTimeUtc.toLocalTime().toString("yyyy-MM-dd_HH-mm-ss"));
        const QString path = QFileDialog::getSaveFileName(this, "Eksportuj raport streama", defName, "JSON (*.json)");
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, "Eksport", "Nie udało się zapisać pliku.");
            return;
        }
        f.write(QJsonDocument(toJson(m_summary)).toJson(QJsonDocument::Indented));
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}
