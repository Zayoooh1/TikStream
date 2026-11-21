#pragma once
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>
#include <QObject>

// Globalne skróty klawiszowe (działają też, gdy gra jest na pierwszym planie) — RegisterHotKey.
class Hotkeys : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit Hotkeys(QObject *parent = nullptr);
    ~Hotkeys() override;

    // action → sekwencja, np. "toggleStream" → "Ctrl+F9". Zwraca listę akcji, których nie udało się zarejestrować.
    QStringList apply(const QHash<QString, QString> &map);
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override;

    static QList<QPair<QString, QString>> actions(); // (id, opis)

signals:
    void triggered(const QString &action);

private:
    void clear();
    QHash<int, QString> m_ids;
};
