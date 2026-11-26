#pragma once

#include <QDateTime>
#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

struct StreamSessionSummary
{
    QDateTime startTimeUtc;
    QDateTime endTimeUtc;
    double durationSec = 0.0;

    int peakViewers = 0;
    int peakTikTokViewers = 0;
    int peakKickViewers = 0;
    double avgViewers = 0.0;

    qint64 totalGifts = 0;
    qint64 totalDiamonds = 0;
    qint64 totalTaps = 0;
    qint64 totalFollows = 0;
    qint64 totalChatMessages = 0;
    QList<QPair<QString, qint64>> topGifters;

    qint64 encodedFrames = 0;
    qint64 droppedPackets = 0;
    int reconnects = 0;
    double avgBitrateKbps = 0.0;
    double avgFps = 0.0;

    QStringList savedReplays;
};

class PostStreamSummaryDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit PostStreamSummaryDialog(const StreamSessionSummary &summary, const QString &autoSavedPath = {}, QWidget *parent = nullptr);

    static QJsonObject toJson(const StreamSessionSummary &s);
    static QString toMarkdown(const StreamSessionSummary &s);
    static QString autoSaveReport(const StreamSessionSummary &s);

private:
    StreamSessionSummary m_summary;
};
