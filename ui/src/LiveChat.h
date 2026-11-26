#pragma once

#include <QCache>
#include <QIcon>
#include <QImage>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QJsonArray;
class QJsonObject;
class GiftNotifications;
class ChatMessageDelegate;

class LiveChat final : public QWidget
{
    Q_OBJECT
public:
    explicit LiveChat(QWidget *parent = nullptr);
    ~LiveChat() override;

    void setAccount(const QString &username);
    QString account() const { return m_account; }
    void setKickAccount(const QString &channel);
    QString kickAccount() const { return m_kickAccount; }
    void ensureStarted();
    void setDemoPreview(bool enabled);
    bool demoPreview() const { return m_demoPreview; }
    int viewerCount() const;
    int tiktokViewerCount() const { return m_ttViewers >= 0 ? int(m_ttViewers) : -1; }
    int kickViewerCount() const { return m_kickViewers >= 0 ? int(m_kickViewers) : -1; }
    qint64 totalGifts() const { return m_totalGifts; }
    qint64 totalDiamonds() const { return m_totalDiamonds; }
    qint64 totalTaps() const { return m_totalTaps; }
    qint64 totalFollows() const { return m_totalFollows; }
    qint64 totalChatMessages() const { return m_totalChatMessages; }
    QList<QPair<QString, qint64>> topGifters(int limit = 5) const;
    void resetSessionStats();
    quint64 overlayRevision() const { return m_overlayRevision; }
    QImage renderOverlay(int width, int height, bool showChat, bool showGifts, bool showTaps) const;

signals:
    void overlayChanged();
    void accountChanged(const QString &username);
    void kickAccountChanged(const QString &channel);
    void viewerCountChanged(int count);
    void alertTriggered(const QString &kind, const QString &user, const QString &detail);
    void statsChanged();
    void giftReceived(const QString &user, const QString &giftName, int count, int diamonds, const QString &detail);
    void followReceived(const QString &user);
    void chatMessageReceived(const QString &platform, const QString &user, const QString &text);
    void ttsSkipRequested();
    void ttsMuteToggled(bool muted);

protected:
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void start();
    void startKick();
    void stopProcess();
    void stopKickProcess();
    void applyAccountFromInput();
    void applyKickAccountFromInput();
    void setChatScale(qreal scale);
    void setAutoScroll(bool enabled);
    void updateGiftStats();
    void updateCombinedStatus();
    void handleViewerEvent(const QJsonObject &event, const QString &defaultPlatform);
    void setPlatformViewers(const QString &platform, qint64 count);
    void updateViewerCount();
    void notifyOverlay();
    void readEvents();
    void readKickEvents();
    void appendMessages(const QJsonArray &messages);
    void requestAvatar(const QString &url);
    void startAvatar(const QString &url);
    void requestEmote(const QString &url);
    void startEmote(const QString &url);

    QLineEdit *m_accountEdit = nullptr;
    QLineEdit *m_kickAccountEdit = nullptr;
    QLabel *m_status;
    QLabel *m_viewers = nullptr;
    QLabel *m_taps;
    QLabel *m_giftTotal = nullptr;
    QLabel *m_giftEarned = nullptr;
    qint64 m_totalGifts = 0;
    qint64 m_totalDiamonds = 0;
    qint64 m_totalTaps = 0;
    qint64 m_totalFollows = 0;
    qint64 m_totalChatMessages = 0;
    QHash<QString, qint64> m_gifterDiamonds;
    qint64 m_ttViewers = -1;
    qint64 m_kickViewers = -1;
    int m_lastEmittedViewers = -1;
    QPushButton *m_ttsSkipBtn = nullptr;
    QPushButton *m_ttsMuteBtn = nullptr;
    QListWidget *m_messages;
    ChatMessageDelegate *m_delegate = nullptr;
    QPushButton *m_zoomReset = nullptr;
    QPushButton *m_autoScrollBtn = nullptr;
    qreal m_chatScale = 1.0;
    bool m_autoScroll = true;
    GiftNotifications *m_gifts;
    QPushButton *m_connection;
    QString m_account;
    QString m_kickAccount;
    QString m_ttStatus;
    QString m_kickStatus;
    QCache<QString, QIcon> m_avatars{128};
    QSet<QString> m_requests;
    QQueue<QString> m_avatarQueue;
    QCache<QString, QPixmap> m_emotes{256};
    QSet<QString> m_emoteRequests;
    QQueue<QString> m_emoteQueue;
    QByteArray m_input;
    QByteArray m_kickInput;
    bool m_started = false;
    bool m_paused = false;
    bool m_demoPreview = false;
    quint64 m_overlayRevision = 0;
    QString m_tapsText;
    QNetworkAccessManager m_network;
    QProcess m_process;
    QProcess m_kickProcess;
};
