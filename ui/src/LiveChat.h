#pragma once

#include <QCache>
#include <QIcon>
#include <QImage>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QJsonArray;
class GiftNotifications;

class LiveChat final : public QWidget
{
    Q_OBJECT
public:
    explicit LiveChat(QWidget *parent = nullptr);
    ~LiveChat() override;

    void setAccount(const QString &username);
    QString account() const { return m_account; }
    void ensureStarted();
    void setDemoPreview(bool enabled);
    bool demoPreview() const { return m_demoPreview; }
    quint64 overlayRevision() const { return m_overlayRevision; }
    QImage renderOverlay(int width, int height, bool showChat, bool showGifts, bool showTaps) const;

signals:
    void overlayChanged();
    void accountChanged(const QString &username);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void start();
    void stopProcess();
    void applyAccountFromInput();
    void notifyOverlay();
    void readEvents();
    void appendMessages(const QJsonArray &messages);
    void requestAvatar(const QString &url);
    void startAvatar(const QString &url);

    QLineEdit *m_accountEdit = nullptr;
    QLabel *m_status;
    QLabel *m_taps;
    QListWidget *m_messages;
    GiftNotifications *m_gifts;
    QPushButton *m_connection;
    QString m_account;
    QCache<QString, QIcon> m_avatars{128};
    QSet<QString> m_requests;
    QQueue<QString> m_avatarQueue;
    QByteArray m_input;
    bool m_started = false;
    bool m_paused = false;
    bool m_demoPreview = false;
    quint64 m_overlayRevision = 0;
    QString m_tapsText;
    QNetworkAccessManager m_network;
    QProcess m_process;
};
