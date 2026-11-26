#pragma once
#include "PostStreamSummaryDialog.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QQueue>

class PreviewWidget;
class MeterWidget;
class ColorButton;
class Hotkeys;
class LiveChat;
class SoundboardWidget;
class QListWidget;
class QStackedWidget;
class QComboBox;
class QSlider;
class QCheckBox;
class QLabel;
class QPushButton;
class QToolButton;
class QPlainTextEdit;
class QFontComboBox;
class QSpinBox;
class QLineEdit;
class QTimer;
class QFrame;
class QVBoxLayout;
class QBoxLayout;
class QSystemTrayIcon;
class QMovie;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(int canvasW, int canvasH, const QString &gpuName);
    ~MainWindow() override;

    // used by test mode in main.cpp
    void triggerAction(const QString &action);

protected:
    void closeEvent(QCloseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // ---------- construction ----------
    void buildMenu();
    void buildCentral();
    void buildDocks();
    void buildStatusBar();
    void setupTray();
    void buildProperties(QWidget *host);
    QWidget *pageScreen();
    QWidget *pageWindow();
    QWidget *pageApplicationAudio();
    QWidget *pageAudioDevice();
    void refreshAudioDevices(bool force = false);
    void populateAudioDeviceChoice();
    void arrangeDocks(bool horizontal);
    void arrangeStudioGrid();
    void setUiZoom(double scale);
    QWidget *pageCamera();
    QWidget *pageImage();
    QWidget *pageText();
    QWidget *pageColor();
    QWidget *pageMedia();
    QWidget *pageBrowser();
    QWidget *pageAlertBox();
    QWidget *pageChatOverlay();
    QWidget *pageGoalBar();
    QWidget *pageNowPlaying();
    QFrame *buildTransform();
    void openVideoFiltersDialog(const QString &itemId);
    void openAudioFiltersDialog(const QString &sourceId);
    void openBrowserInteractDialog(const QString &sourceId);
    void toggleSelectedItemLock();
    void triggerAlert(const QString &kind, const QString &user, const QString &detail);
    void showNextAlert(int slotIdx);
    void updateAlertOverlays();

    // ---------- model ----------
    QJsonArray sources() const { return m_s["sources"].toArray(); }
    QJsonArray scenes() const { return m_s["scenes"].toArray(); }
    int activeSceneIndex() const;
    QJsonObject activeScene() const;
    void setActiveScene(const QJsonObject &scene);
    QJsonObject sourceById(const QString &id) const;
    void setSource(const QJsonObject &src);
    QJsonObject itemById(const QString &id) const;
    void setItem(const QJsonObject &item);
    QJsonObject selectedItem() const { return itemById(m_selItem); }
    QJsonObject selectedSource() const { return sourceById(selectedItem()["sourceId"].toString()); }
    void editSource(const std::function<void(QJsonObject &)> &fn);
    void editItem(const std::function<void(QJsonObject &)> &fn);
    void applySoon();
    void sendDragTransform();
    void updateSourceStatus();
    void pushSettings();

    // ---------- views ----------
    void rebuildScenes();
    void rebuildSourceList();
    void rebuildMixer();
    void loadProperties();
    void refreshCaptureList();
    void refreshCameras();
    void fillCameraModes(const QString &cameraName);
    void selectItem(const QString &itemId);

    // ---------- actions ----------
    void addSourceDialog();
    QString addSource(const QString &type, const QString &name, const QString &existingId, bool visible, const QJsonObject &options = {});
    void removeSelectedItem();
    void moveSelectedItem(int dir);
    void addScene();
    void removeScene();
    void moveScene(int dir);
    void switchScene(int delta);
    void uploadRaster(const QJsonObject &src);
    void updateChatOverlays();
    void updateGoalBarOverlays();
    void updateNowPlayingOverlays();
    void pollNowPlaying();
    void handleGiftTtsAndSoundboard(const QString &user, const QString &giftName, int count, int diamonds, const QString &detail);
    void handleFollowTts(const QString &user);
    void handleChatTts(const QString &platform, const QString &user, const QString &text);
    void startStreamTelemetry();
    void finalizeStreamTelemetry();
    void applyHotkeys();
    void pushUndo();
    void undo();
    void redo();
    void copySelectedItem();
    void pasteItem();
    void duplicateSelectedItem();
    void nudgeSelectedItem(int dxPx, int dyPx);

    // ---------- preview ----------
    QSizeF naturalSize(const QString &sourceId) const;
    QRectF displayRect(const QJsonObject &item) const;
    QRectF displayRectFor(const QJsonObject &item, bool dual) const;
    bool croppable(const QString &type) const;
    void updateSelection();
    void onCanvasPressed(QPointF p);
    void onCanvasPressedAt(QPointF p, bool dual);
    void onCanvasDragged(QPointF p);
    void onCanvasWheel(int delta);
    void onCanvasWheelAt(int delta, bool dual);
    QJsonObject makeFree(QJsonObject item, bool dual = false) const;
    double snap(double v, const QList<double> &targets, double tol, double *delta = nullptr) const;

    // ---------- live ----------
    void toggleStream();
    bool prepareStream(QJsonObject *startArgs = nullptr);
    void toggleRecord();
    void toggleReplay();
    void saveReplay();
    void openSettings();
    void checkForUpdates(bool manual);
    void showUpdateDialog(const QJsonObject &info);
    void exportSceneCollection();
    void importSceneCollection();
    void openStatsGraphDialog();
    void syncDualPreviewDock();
    QJsonObject alertBoxSource(const QString &role = {}) const;
    void editAlertBoxSource(const QString &role, const std::function<void(QJsonObject &)> &fn);
    void ensureSeparateAlertBoxes();
    void loadAlertBoxDock();
    void saveDockLayout();
    void ensureDockOnScreen(QDockWidget *dock);
    void pollStats();
    void pollLevels();
    void updateLiveButtons();

    QJsonObject m_s;
    int m_canvasW, m_canvasH;
    QString m_gpu;
    bool m_loading = false;
    bool m_streaming = false, m_recording = false, m_replayActive = false;
    QString m_selItem;
    QList<QJsonObject> m_undoStack, m_redoStack;
    QJsonObject m_clipboardItem, m_clipboardSource;
    qint64 m_lastUndoMs = 0;

    QHash<QString, QString> m_rasterKeys;
    QHash<QString, QSize> m_rasterSizes;
    QHash<QString, QJsonObject> m_srcStats;
    QJsonArray m_cameras, m_captureList;
    QJsonObject m_audioDevices;
    bool m_audioDeviceScanPending = false;
    bool m_forceAudioDeviceRefresh = false;
    QString m_mixerSignature;

    enum class Drag { None, Move, Resize, CropLeft, CropRight, CropTop, CropBottom };
    Drag m_drag = Drag::None;
    bool m_dragDual = false;
    QTimer *m_dragTimer = nullptr;
    bool m_dragChanged = false;
    QPointF m_dragStart, m_anchor;
    QRectF m_startRect;
    double m_startCropL = 0, m_startCropR = 0, m_startCropT = 0, m_startCropB = 0;

    QTimer *m_applyTimer = nullptr, *m_statsTimer = nullptr, *m_levelsTimer = nullptr, *m_updateTimer = nullptr, *m_alertAnimTimer = nullptr, *m_layoutSaveTimer = nullptr;
    bool m_updateCheckPending = false;
    bool m_reallyQuit = false;
    QString m_skippedUpdateVersion;
    struct AlertEvent { QString kind, user, detail; };
    struct AlertSlot {
        QQueue<AlertEvent> queue;
        QString kind, user, detail;
        qint64 startMs = 0;
        int durMs = 5000;
        quint64 revision = 0;
        QMovie *movie = nullptr;
        QImage cachedImg, userImg, detailImg, canvas;
        int gapAfterImg = 0, lineGap = 0, pad = 48;
        qreal scaleRef = 1.0;
        QTimer *timer = nullptr;
    };
    AlertSlot m_alertSlots[2]; // 0 = gift, 1 = follow (fully independent & concurrent)
    QSystemTrayIcon *m_tray = nullptr;
    QList<double> m_histFps, m_histRenderMs, m_histKbps;
    QPointer<QWidget> m_statsGraphWidget;
    Hotkeys *m_hotkeys = nullptr;

    PreviewWidget *m_preview = nullptr, *m_dualPreview = nullptr;
    QDockWidget *m_dualPreviewDock = nullptr;
    QDockWidget *m_alertBoxDock = nullptr;
    QWidget *m_alertBoxForm = nullptr;
    QLabel *m_alertBoxEmpty = nullptr;
    bool m_dualDockPositioned = false;
    bool m_layoutReady = false;
    QStackedWidget *m_previewStack = nullptr;
    QListWidget *m_sceneList = nullptr, *m_sourceList = nullptr;
    QToolButton *m_btnRemove = nullptr, *m_btnUp = nullptr, *m_btnDown = nullptr, *m_btnLock = nullptr;

    // properties
    QStackedWidget *m_propsStack = nullptr;
    QLabel *m_propsIcon = nullptr, *m_srcStatus = nullptr;
    QLineEdit *m_srcName = nullptr;
    QWidget *m_propsBody = nullptr, *m_propsEmpty = nullptr;
    QStackedWidget *m_pages = nullptr;
    QFrame *m_transform = nullptr;
    QComboBox *m_fit = nullptr;
    QSlider *m_zoom = nullptr, *m_panX = nullptr, *m_panY = nullptr, *m_corner = nullptr, *m_opacity = nullptr;
    QCheckBox *m_mirror = nullptr;
    QWidget *m_cropRows = nullptr;

    QComboBox *m_screenCombo = nullptr, *m_windowCombo = nullptr;
    QComboBox *m_gameMode = nullptr, *m_windowPriority = nullptr, *m_applicationCombo = nullptr;
    QComboBox *m_audioDeviceCombo = nullptr;
    QComboBox *m_captureMethod = nullptr;
    QWidget *m_windowTarget = nullptr;
    QCheckBox *m_screenCursor = nullptr, *m_windowCursor = nullptr;
    QComboBox *m_camDevice = nullptr, *m_camMode = nullptr;
    QLineEdit *m_imagePath = nullptr, *m_mediaPath = nullptr, *m_browserUrl = nullptr;
    QLineEdit *m_alertGiftImage = nullptr, *m_alertGiftSound = nullptr, *m_alertFollowImage = nullptr, *m_alertFollowSound = nullptr;
    QCheckBox *m_mediaLoop = nullptr, *m_browserMuted = nullptr, *m_alertOnGift = nullptr, *m_alertOnFollow = nullptr;
    QSpinBox *m_browserWidth = nullptr, *m_browserHeight = nullptr, *m_browserFps = nullptr, *m_alertDuration = nullptr,
             *m_alertGiftVolume = nullptr, *m_alertFollowVolume = nullptr,
             *m_alertGiftMonitorVolume = nullptr, *m_alertFollowMonitorVolume = nullptr;
    QPlainTextEdit *m_text = nullptr, *m_browserCss = nullptr;
    QFontComboBox *m_font = nullptr;
    QSpinBox *m_fontSize = nullptr, *m_outlineW = nullptr;
    QCheckBox *m_bold = nullptr;
    ColorButton *m_textColor = nullptr, *m_outlineColor = nullptr, *m_bgColor = nullptr, *m_fillColor = nullptr;
    LiveChat *m_liveChat = nullptr;
    QCheckBox *m_chatShowMessages = nullptr, *m_chatShowGifts = nullptr, *m_chatShowTaps = nullptr;
    QPushButton *m_chatDemoBtn = nullptr;

    // GoalBar properties
    QLineEdit *m_goalTitle = nullptr;
    QComboBox *m_goalMetric = nullptr;
    QSpinBox *m_goalTarget = nullptr;
    QCheckBox *m_goalAutoReset = nullptr, *m_goalShowPercent = nullptr;
    ColorButton *m_goalFillColor = nullptr, *m_goalBgColor = nullptr, *m_goalTextColor = nullptr;

    // NowPlaying properties & SMTC cache
    QCheckBox *m_npHidePaused = nullptr, *m_npShowArt = nullptr;
    QLineEdit *m_npAppFilter = nullptr;
    ColorButton *m_npBgColor = nullptr, *m_npAccentColor = nullptr, *m_npTextColor = nullptr;
    QTimer *m_nowPlayingTimer = nullptr;
    quint64 m_npRevision = 0;
    QJsonObject m_npState;
    QImage m_npArt;

    // Soundboard & TTS runtime
    QDockWidget *m_soundboardDock = nullptr;
    SoundboardWidget *m_soundboard = nullptr;
    qint64 m_lastChatTtsMs = 0;
    QString m_lastChatTtsText;

    // Post-Stream Summary telemetry
    bool m_sessionActive = false;
    StreamSessionSummary m_session;
    double m_fpsSum = 0.0;
    int m_fpsSamples = 0;
    double m_kbpsSum = 0.0;
    int m_kbpsSamples = 0;
    double m_viewerSum = 0.0;
    int m_viewerSamples = 0;
    int m_viewerSampleTick = 0;

    // mixer
    QBoxLayout *m_mixerLayout = nullptr;
    bool m_mixerVertical = false;
    struct Strip { MeterWidget *meter; QLabel *db; QToolButton *mute; QSlider *slider; };
    QHash<QString, Strip> m_strips;

    // controls panel
    QPushButton *m_btnStream = nullptr, *m_btnRecord = nullptr, *m_btnReplay = nullptr, *m_btnSaveReplay = nullptr, *m_uiZoomReset = nullptr;
    double m_uiZoom = 1.0;
    QString m_baseQss;

    // status bar
    QLabel *m_stLive = nullptr, *m_stRec = nullptr, *m_stCpu = nullptr, *m_stFps = nullptr,
           *m_stBitrate = nullptr, *m_stRender = nullptr, *m_stDropped = nullptr, *m_stMsg = nullptr;
};
