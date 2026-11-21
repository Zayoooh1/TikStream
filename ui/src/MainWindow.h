#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>

class PreviewWidget;
class MeterWidget;
class ColorButton;
class Hotkeys;
class LiveChat;
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

private:
    // ---------- construction ----------
    void buildMenu();
    void buildCentral();
    void buildDocks();
    void buildStatusBar();
    void buildProperties(QWidget *host);
    QWidget *pageScreen();
    QWidget *pageWindow();
    QWidget *pageApplicationAudio();
    QWidget *pageAudioDevice();
    void refreshAudioDevices(bool force = false);
    void populateAudioDeviceChoice();
    void arrangeDocks(bool horizontal);
    QWidget *pageCamera();
    QWidget *pageImage();
    QWidget *pageText();
    QWidget *pageColor();
    QWidget *pageMedia();
    QWidget *pageChatOverlay();
    QFrame *buildTransform();
    void openVideoFiltersDialog(const QString &itemId);
    void openAudioFiltersDialog(const QString &sourceId);

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
    bool croppable(const QString &type) const;
    void updateSelection();
    void onCanvasPressed(QPointF p);
    void onCanvasDragged(QPointF p);
    void onCanvasWheel(int delta);
    QJsonObject makeFree(QJsonObject item) const;
    double snap(double v, const QList<double> &targets, double tol, double *delta = nullptr) const;

    // ---------- live ----------
    void toggleStream();
    bool prepareStream();
    void toggleRecord();
    void openSettings();
    void pollStats();
    void pollLevels();
    void updateLiveButtons();

    QJsonObject m_s;
    int m_canvasW, m_canvasH;
    QString m_gpu;
    bool m_loading = false;
    bool m_streaming = false, m_recording = false;
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

    enum class Drag { None, Move, Resize };
    Drag m_drag = Drag::None;
    QTimer *m_dragTimer = nullptr;
    bool m_dragChanged = false;
    QPointF m_dragStart, m_anchor;
    QRectF m_startRect;

    QTimer *m_applyTimer = nullptr, *m_statsTimer = nullptr, *m_levelsTimer = nullptr;
    Hotkeys *m_hotkeys = nullptr;

    PreviewWidget *m_preview = nullptr;
    QStackedWidget *m_previewStack = nullptr;
    QListWidget *m_sceneList = nullptr, *m_sourceList = nullptr;
    QToolButton *m_btnRemove = nullptr, *m_btnUp = nullptr, *m_btnDown = nullptr;

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
    QLineEdit *m_imagePath = nullptr, *m_mediaPath = nullptr;
    QCheckBox *m_mediaLoop = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QFontComboBox *m_font = nullptr;
    QSpinBox *m_fontSize = nullptr, *m_outlineW = nullptr;
    QCheckBox *m_bold = nullptr;
    ColorButton *m_textColor = nullptr, *m_outlineColor = nullptr, *m_bgColor = nullptr, *m_fillColor = nullptr;
    LiveChat *m_liveChat = nullptr;
    QCheckBox *m_chatShowMessages = nullptr, *m_chatShowGifts = nullptr, *m_chatShowTaps = nullptr;
    QPushButton *m_chatDemoBtn = nullptr;

    // mixer
    QBoxLayout *m_mixerLayout = nullptr;
    bool m_mixerVertical = false;
    struct Strip { MeterWidget *meter; QLabel *db; QToolButton *mute; QSlider *slider; };
    QHash<QString, Strip> m_strips;

    // controls panel
    QPushButton *m_btnStream = nullptr, *m_btnRecord = nullptr;

    // status bar
    QLabel *m_stLive = nullptr, *m_stRec = nullptr, *m_stCpu = nullptr, *m_stFps = nullptr,
           *m_stBitrate = nullptr, *m_stRender = nullptr, *m_stDropped = nullptr, *m_stMsg = nullptr;
};
