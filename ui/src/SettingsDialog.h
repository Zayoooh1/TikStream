#pragma once
#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

class QListWidget;
class QStackedWidget;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QPushButton;
class QKeySequenceEdit;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(const QJsonObject &settings, const QJsonObject &audioDevices, bool live, QWidget *parent = nullptr);
    QJsonObject result() const;

signals:
    void applied(const QJsonObject &settings);

private:
    QWidget *pageGeneral();
    QWidget *pageStream();
    QWidget *pageOutput();
    QWidget *pageVideo();
    QWidget *pageAudio();
    QWidget *pageHotkeys();
    QWidget *pageAdvanced();
    void addPage(const QString &title, const QString &icon, QWidget *page);
    void markDirty();
    template <typename W> W *track(W *w);

    QJsonObject m_s;
    QJsonObject m_devices;
    bool m_live;

    QListWidget *m_nav = nullptr;
    QStackedWidget *m_pages = nullptr;
    QPushButton *m_apply = nullptr;

    // ogólne
    QCheckBox *m_confirmStop = nullptr, *m_autoRecord = nullptr, *m_snapping = nullptr;
    // transmisja
    QLineEdit *m_server = nullptr, *m_key = nullptr;
    QCheckBox *m_secondEnabled = nullptr;
    QLineEdit *m_secondName = nullptr, *m_secondServer = nullptr, *m_secondKey = nullptr;
    // wyjście
    QSpinBox *m_bitrate = nullptr, *m_keyframe = nullptr, *m_bframes = nullptr;
    QComboBox *m_preset = nullptr, *m_profile = nullptr, *m_audioBitrate = nullptr, *m_recFormat = nullptr;
    QCheckBox *m_aq = nullptr, *m_lookahead = nullptr, *m_zeroFpsImpact = nullptr;
    QLineEdit *m_folder = nullptr;
    // obraz
    QSpinBox *m_videoWidth = nullptr, *m_videoHeight = nullptr, *m_fps = nullptr;
    // dźwięk
    QComboBox *m_desktopDev = nullptr, *m_micDev = nullptr;
    // skróty
    QHash<QString, QKeySequenceEdit *> m_hotkeys;
    // zaawansowane
    QComboBox *m_priority = nullptr;
    QCheckBox *m_preview = nullptr, *m_reconnect = nullptr;
    QSpinBox *m_retryDelay = nullptr, *m_retryMax = nullptr;
};
