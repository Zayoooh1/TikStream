#pragma once
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>
#include <QObject>

// Global hotkeys (work even when a game is in the foreground) — RegisterHotKey.
class Hotkeys : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit Hotkeys(QObject *parent = nullptr);
    ~Hotkeys() override;

    // action -> key sequence, e.g. "toggleStream" -> "Ctrl+F9". Returns list of actions that failed to register.
    QStringList apply(const QHash<QString, QString> &map);
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;

    static QList<QPair<QString, QString>> actions(); // (id, label)

signals:
    void triggered(const QString &action);

private:
    void clear();
    QHash<int, QString> m_ids;
};
