#include "MainWindow.h"

#include "AddSourceDialog.h"
#include "EngineHost.h"
#include "Hotkeys.h"
#include "LiveChat.h"
#include "OverlayRaster.h"
#include "PreviewWidget.h"
#include "SettingsDialog.h"
#include "SourceTypes.h"
#include "Widgets.h"

#include <windows.h>
#include <dwmapi.h>

#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QSlider>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

namespace {
enum Page { PgScreen, PgWindow, PgCamera, PgImage, PgText, PgColor, PgMedia, PgChatOverlay, PgApplicationAudio, PgAudioDevice, PgNone };

QString newId() { return QUuid::createUuid().toString(QUuid::Id128); }

QSlider *hslider(int min, int max)
{
    auto *s = new QSlider(Qt::Horizontal);
    s->setRange(min, max);
    return s;
}

QFormLayout *form(QWidget *w)
{
    auto *f = new QFormLayout(w);
    f->setContentsMargins(0, 0, 0, 0);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setHorizontalSpacing(12);
    f->setVerticalSpacing(9);
    f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    return f;
}

QLabel *hintLabel(const QString &t)
{
    auto *l = new QLabel(t);
    l->setObjectName("hint");
    l->setWordWrap(true);
    return l;
}

QToolButton *toolBtn(const QString &icon, const QString &tip)
{
    auto *b = new QToolButton;
    b->setIcon(themedIcon(icon));
    b->setIconSize(QSize(16, 16));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

QString fmtTime(double sec)
{
    const int s = int(sec);
    return QString("%1:%2:%3").arg(s / 3600, 2, 10, QChar('0')).arg(s / 60 % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

QJsonObject rectObj(const QRectF &r) { return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}}; }

void widenChoice(QComboBox *combo)
{
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(12);
    QObject::connect(combo, &QComboBox::currentTextChanged, combo, &QWidget::setToolTip);
}

QComboBox *captureMethodChoice(QWidget *parent = nullptr)
{
    auto *combo = new QComboBox(parent);
    widenChoice(combo);
    combo->addItems({"Windows Graphics Capture", "DXGI — no yellow border"});
    combo->setItemData(0, "Captures the window directly. Windows may display a capture border.", Qt::ToolTipRole);
    combo->setItemData(1, "Crops from the display image. The window must stay on top, and overlays may be visible.", Qt::ToolTipRole);
    return combo;
}

void populateApps(QComboBox *combo, const QString &id = {})
{
    const QSignalBlocker blocked(combo);
    combo->clear();
    combo->addItem("— select application —", "");
    for (const auto &app : EngineHost::instance().callArr("listAudioApplications"))
        combo->addItem(app.toString(), app.toString());
    if (!id.isEmpty() && combo->findData(id) < 0)
        combo->addItem(id + " • waiting to launch", id);
    combo->setCurrentIndex(std::max(0, combo->findData(id)));
    combo->setToolTip(combo->currentText());
}

bool newCaptureOptions(QWidget *parent, const QString &type, const QJsonArray &targets, QJsonObject &options)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(sourceTypeInfo(type).name);
    dialog.resize(580, 0);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);
    const bool game = type == "Game";
    const bool audio = type == "AudioApplication";
    auto *mode = new QComboBox(&dialog);
    mode->addItems({"Any fullscreen application", "Specific window"});
    if (game) form->addRow("Mode", mode);
    else mode->hide();
    auto *target = new QComboBox;
    widenChoice(target);
    if (audio) populateApps(target);
    else {
        target->addItem("— select window —", "");
        for (const auto &entry : targets) {
            const auto window = entry.toObject();
            if (window["kind"].toString() == "Window")
                target->addItem(window["label"].toString(), window["id"].toString());
        }
    }
    form->addRow(audio ? "Application" : "Window", target);
    auto *priority = new QComboBox(&dialog);
    priority->addItems({"Match by application name", "Require exact window title"});
    auto *cursor = new QCheckBox("Capture cursor", &dialog);
    auto *method = captureMethodChoice(&dialog);
    auto *gameAudio = new QCheckBox("Add game audio to mixer", &dialog);
    cursor->setChecked(true);
    if (!audio) {
        form->addRow("Method", method);
        form->addRow("Window Match", priority);
        form->addRow(cursor);
        if (game) form->addRow(gameAudio);
        layout->addWidget(hintLabel("Minimized windows stay in the list. If an app pauses rendering, the preview keeps the last frame."));
    } else {
        method->hide();
        priority->hide();
        cursor->hide();
        layout->addWidget(hintLabel("Captures audio from the selected application and its child processes. The channel will appear in the mixer."));
    }
    if (!game) gameAudio->hide();
    auto *buttons = new QDialogButtonBox;
    auto *add = buttons->addButton("Add Source", QDialogButtonBox::AcceptRole);
    buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    auto update = [=] {
        const bool autoGame = game && mode->currentIndex() == 0;
        form->setRowVisible(target, !autoGame);
        if (!audio) form->setRowVisible(priority, !autoGame);
        if (game) form->setRowVisible(gameAudio, !autoGame);
        add->setEnabled(autoGame || !target->currentData().toString().isEmpty());
    };
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(target, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update();
    if (dialog.exec() != QDialog::Accepted) return false;
    if (audio) options["deviceId"] = target->currentData().toString();
    else {
        options["captureId"] = target->currentData().toString();
        options["captureCursor"] = cursor->isChecked();
        options["captureFromDesktop"] = method->currentIndex() == 1;
        options["matchWindowTitle"] = priority->currentIndex() == 1;
        if (game) options["gameMode"] = mode->currentIndex() == 0 ? "Fullscreen" : "Window";
        if (game && mode->currentIndex() == 1 && gameAudio->isChecked())
            options["gameAudioProcess"] = target->currentData().toString().section('|', 0, 0);
    }
    return true;
}
QRectF itemRect(const QJsonObject &i) { return QRectF(i["x"].toDouble(), i["y"].toDouble(), i["w"].toDouble(1), i["h"].toDouble(1)); }
QColor argbOf(const QJsonValue &v, QColor def = Qt::white) { return v.isDouble() ? QColor::fromRgba(QRgb(quint32(qint64(v.toDouble())))) : def; }
double argbVal(const QColor &c) { return double(qint32(c.rgba())); }
} // namespace

// =====================================================================================

MainWindow::MainWindow(int canvasW, int canvasH, const QString &gpuName)
    : m_canvasW(canvasW), m_canvasH(canvasH), m_gpu(gpuName)
{
    setWindowTitle("TikStream");
    setWindowIcon(themedIcon("logo"));
    resize(1640, 980);
    setDockNestingEnabled(true);

    m_s = EngineHost::instance().callObj("getSettings");
    m_audioDevices = EngineHost::instance().callObj("listAudioDevices");
    const QSettings uiSettings("TikStream", "UI2");
    m_mixerVertical = uiSettings.value("mixerVertical", false).toBool();

    m_applyTimer = new QTimer(this);
    m_applyTimer->setSingleShot(true);
    m_applyTimer->setInterval(15);
    connect(m_applyTimer, &QTimer::timeout, this, &MainWindow::pushSettings);
    m_dragTimer = new QTimer(this);
    m_dragTimer->setSingleShot(true);
    m_dragTimer->setTimerType(Qt::PreciseTimer);
    m_dragTimer->setInterval(16);
    connect(m_dragTimer, &QTimer::timeout, this, &MainWindow::sendDragTransform);

    m_hotkeys = new Hotkeys(this);
    connect(m_hotkeys, &Hotkeys::triggered, this, &MainWindow::triggerAction);

    buildMenu();
    buildCentral();
    buildDocks();
    buildStatusBar();

    QSettings qs("TikStream", "UI2");
    restoreGeometry(qs.value("geometry").toByteArray());
    restoreState(qs.value("state").toByteArray());
    if (!qs.value("compactLayoutV1").toBool()) arrangeDocks(false);

    for (const auto &v : sources()) uploadRaster(v.toObject());
    refreshCaptureList();
    refreshCameras();
    rebuildScenes();
    rebuildSourceList();
    rebuildMixer();
    applyHotkeys();
    if (m_sourceList->count() > 0) m_sourceList->setCurrentRow(m_sourceList->count() - 1);
    loadProperties();

    m_statsTimer = new QTimer(this);
    connect(m_statsTimer, &QTimer::timeout, this, &MainWindow::pollStats);
    m_statsTimer->start(1000);
    m_levelsTimer = new QTimer(this);
    connect(m_levelsTimer, &QTimer::timeout, this, &MainWindow::pollLevels);
    auto *devicesTimer = new QTimer(this);
    connect(devicesTimer, &QTimer::timeout, this, [this] { refreshAudioDevices(); });
    devicesTimer->start(3000);
    m_levelsTimer->start(40);
}

MainWindow::~MainWindow() = default;

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(HWND(winId()), 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
}

// =============================== construction ===============================

void MainWindow::buildMenu()
{
    auto *file = menuBar()->addMenu("File");
    file->addAction(themedIcon("folder"), "Show Recordings", this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_s["recordFolder"].toString()));
    });
    file->addAction(themedIcon("gear"), "Settings", QKeySequence("Ctrl+,"), this, &MainWindow::openSettings);
    file->addSeparator();
    file->addAction(themedIcon("exit"), "Exit", this, &QWidget::close);

    auto *edit = menuBar()->addMenu("Edit");
    edit->addAction("Undo", QKeySequence::Undo, this, &MainWindow::undo);
    edit->addAction("Redo", QKeySequence::Redo, this, &MainWindow::redo);
    edit->addSeparator();
    edit->addAction("Copy Source", QKeySequence::Copy, this, &MainWindow::copySelectedItem);
    edit->addAction("Paste Source", QKeySequence::Paste, this, &MainWindow::pasteItem);
    edit->addAction("Duplicate Source", QKeySequence("Ctrl+D"), this, &MainWindow::duplicateSelectedItem);
    edit->addSeparator();
    edit->addAction(themedIcon("plus"), "Add Source…", QKeySequence("Ctrl+N"), this, &MainWindow::addSourceDialog);
    edit->addAction(themedIcon("trash"), "Remove Selected Source", QKeySequence::Delete, this, &MainWindow::removeSelectedItem);
    edit->addSeparator();
    edit->addAction("Center Selected", QKeySequence("Ctrl+R"), this, [this] {
        pushUndo();
        editItem([this](QJsonObject &i) { i = makeFree(i); QRectF r = itemRect(i); r.moveCenter({0.5, 0.5}); i["x"] = r.x(); i["y"] = r.y(); });
    });
    edit->addAction("Fill Screen", QKeySequence("Ctrl+F"), this, [this] { pushUndo(); editItem([](QJsonObject &i) { i["fit"] = "Fill"; }); loadProperties(); });

    auto *view = menuBar()->addMenu("View");
    view->addAction(themedIcon("refresh"), "Refresh Windows, Displays, and Cameras", this, [this] { refreshCaptureList(); refreshCameras(); });
    view->addSeparator();
    auto *panels = view->addMenu("Dock Layout");
    panels->setObjectName("panelsMenu");
    panels->addAction("Side Docks", this, [this] { arrangeDocks(false); });
    panels->addAction("Horizontal Docks Below Preview", this, [this] { arrangeDocks(true); });
    auto *verticalMixer = view->addAction("Vertical Mixer Bars");
    verticalMixer->setCheckable(true);
    verticalMixer->setChecked(m_mixerVertical);
    connect(verticalMixer, &QAction::toggled, this, [this](bool vertical) {
        m_mixerVertical = vertical;
        QSettings("TikStream", "UI2").setValue("mixerVertical", vertical);
        m_mixerSignature.clear();
        rebuildMixer();
    });
    auto *scale = view->addMenu("UI Scale");
    auto *scaleGroup = new QActionGroup(scale);
    for (int percent : {70, 85, 100}) {
        auto *action = scale->addAction(QString("%1%").arg(percent));
        action->setCheckable(true);
        action->setChecked(QSettings("TikStream", "UI2").value("interfaceScale", 85).toInt() == percent);
        scaleGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, percent] {
            QSettings("TikStream", "UI2").setValue("interfaceScale", percent);
            QMessageBox::information(this, "UI Scale", "The new UI scale will take effect after restarting TikStream.");
        });
    }

    auto *help = menuBar()->addMenu("Help");
    help->addAction("Open Log File", this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(EngineHost::instance().callObj("logPath")["path"].toString()));
    });
    help->addAction("About", this, [this] {
        QMessageBox::about(this, "TikStream",
                           "<b>TikStream</b><br>Vertical and horizontal RTMP streaming and recording.<br><br>"
                           "Desktop Duplication → D3D11 → NVENC (zero-copy)<br>GPU: " + m_gpu);
    });
}

void MainWindow::buildCentral()
{
    auto *area = new QWidget(this);
    area->setObjectName("previewArea");
    auto *v = new QVBoxLayout(area);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_previewStack = new QStackedWidget(area);
    m_preview = new PreviewWidget;
    m_preview->setCanvasSize(m_canvasW, m_canvasH);
    m_previewStack->addWidget(m_preview);
    // When preview is disabled (to save GPU) — show a clear message instead of a blank box
    auto *off = new QWidget;
    auto *ov = new QVBoxLayout(off);
    ov->addStretch();
    auto *offIcon = new QLabel;
    offIcon->setPixmap(themedIcon("eye-off").pixmap(40, 40));
    offIcon->setAlignment(Qt::AlignCenter);
    auto *offText = new QLabel("Preview is disabled\n(streaming and recording continue normally)");
    offText->setAlignment(Qt::AlignCenter);
    offText->setObjectName("hint");
    auto *offBtn = new QPushButton("Enable Preview");
    offBtn->setObjectName("primary");
    offBtn->setCursor(Qt::PointingHandCursor);
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(offBtn);
    btnRow->addStretch();
    ov->addWidget(offIcon);
    ov->addWidget(offText);
    ov->addSpacing(8);
    ov->addLayout(btnRow);
    ov->addStretch();
    m_previewStack->addWidget(off);
    connect(offBtn, &QPushButton::clicked, this, [this] {
        m_s["previewEnabled"] = true;
        pushSettings();
        m_previewStack->setCurrentIndex(0);
    });
    m_previewStack->setCurrentIndex(m_s["previewEnabled"].toBool(true) ? 0 : 1);
    v->addWidget(m_previewStack, 1);
    auto *bar = new QWidget(area);
    bar->setObjectName("previewBar");
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(12, 4, 12, 4);
    auto *info = new QLabel("Drag item to move  •  corner = resize (Shift = free aspect)  •  mouse wheel = scale  •  Delete = remove");
    info->setObjectName("previewInfo");
    bl->addWidget(info);
    bl->addStretch();
    v->addWidget(bar);
    setCentralWidget(area);

    connect(m_preview, &PreviewWidget::nativeResized, this, [](qint64 hwnd, int w, int h) {
        EngineHost::instance().call("setPreview", QJsonObject{{"hwnd", hwnd}, {"w", w}, {"h", h}});
    });
    connect(m_preview, &PreviewWidget::canvasPressed, this, &MainWindow::onCanvasPressed);
    connect(m_preview, &PreviewWidget::canvasDragged, this, &MainWindow::onCanvasDragged);
    connect(m_preview, &PreviewWidget::canvasReleased, this, [this] {
        m_drag = Drag::None;
        m_dragTimer->stop();
        if (m_dragChanged) {
            m_dragChanged = false;
            pushSettings();
            updateSelection();
        }
    });
    connect(m_preview, &PreviewWidget::canvasWheel, this, &MainWindow::onCanvasWheel);
    connect(m_preview, &PreviewWidget::deletePressed, this, &MainWindow::removeSelectedItem);
}

void MainWindow::buildDocks()
{
    auto makeDock = [this](const QString &title, const QString &objName, QWidget *content, Qt::DockWidgetArea area) {
        auto *d = new QDockWidget(title, this);
        d->setObjectName(objName);
        d->setWidget(content);
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
        d->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(d, &QWidget::customContextMenuRequested, this, [this, d](const QPoint &pos) {
            QMenu menu(d);
            auto move = [this, d](Qt::DockWidgetArea side) { d->setFloating(false); addDockWidget(side, d); };
            menu.addAction("Top · horizontal", d, [move] { move(Qt::TopDockWidgetArea); });
            menu.addAction("Bottom · horizontal", d, [move] { move(Qt::BottomDockWidgetArea); });
            menu.addAction("Left · vertical", d, [move] { move(Qt::LeftDockWidgetArea); });
            menu.addAction("Right · vertical", d, [move] { move(Qt::RightDockWidgetArea); });
            menu.exec(d->mapToGlobal(pos));
        });
        addDockWidget(area, d);
        return d;
    };
    auto withToolbar = [](QWidget *main, const QList<QWidget *> &left, const QList<QWidget *> &right) {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        v->addWidget(main, 1);
        auto *tb = new QWidget;
        tb->setObjectName("dockToolbar");
        auto *tl = new QHBoxLayout(tb);
        tl->setContentsMargins(6, 3, 6, 3);
        tl->setSpacing(2);
        for (auto *b : left) tl->addWidget(b);
        tl->addStretch();
        for (auto *b : right) tl->addWidget(b);
        v->addWidget(tb);
        return w;
    };

    m_liveChat = new LiveChat;
    m_liveChat->setAccount(m_s["tikTokUsername"].toString());
    connect(m_liveChat, &LiveChat::overlayChanged, this, &MainWindow::updateChatOverlays);
    connect(m_liveChat, &LiveChat::accountChanged, this, [this](const QString &nick) {
        m_s["tikTokUsername"] = nick;
        pushSettings();
    });
    auto *chat = makeDock("TikTok LIVE Chat", "dockLiveChat", m_liveChat, Qt::RightDockWidgetArea);
    chat->setFeatures(chat->features() | QDockWidget::DockWidgetClosable);
    findChild<QMenu *>("panelsMenu")->addAction(chat->toggleViewAction());
    chat->hide();

    // ---------- Scenes ----------
    m_sceneList = new QListWidget;
    m_sceneList->setObjectName("sceneList");
    m_sceneList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    auto *sAdd = toolBtn("plus", "Add Scene");
    auto *sDel = toolBtn("trash", "Remove Scene");
    auto *sUp = toolBtn("up", "Move Up");
    auto *sDown = toolBtn("down", "Move Down");
    connect(sAdd, &QToolButton::clicked, this, &MainWindow::addScene);
    connect(sDel, &QToolButton::clicked, this, &MainWindow::removeScene);
    connect(sUp, &QToolButton::clicked, this, [this] { moveScene(-1); });
    connect(sDown, &QToolButton::clicked, this, [this] { moveScene(+1); });
    connect(m_sceneList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_loading || row < 0 || row >= scenes().size()) return;
        m_s["activeSceneId"] = scenes()[row].toObject()["id"];
        m_selItem.clear();
        rebuildSourceList();
        rebuildMixer();
        loadProperties();
        applySoon();
    });
    connect(m_sceneList, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_loading) return;
        const int row = m_sceneList->row(it);
        QJsonArray arr = scenes();
        if (row < 0 || row >= arr.size()) return;
        QJsonObject sc = arr[row].toObject();
        if (!it->text().trimmed().isEmpty()) sc["name"] = it->text().trimmed();
        arr[row] = sc;
        m_s["scenes"] = arr;
        applySoon();
    });
    auto *dScenes = makeDock("Scenes", "dockScenes", withToolbar(m_sceneList, {sAdd, sDel, sUp, sDown}, {}), Qt::LeftDockWidgetArea);

    // ---------- Sources ----------
    m_sourceList = new QListWidget;
    m_sourceList->setObjectName("sourceList");
    m_sourceList->setIconSize(QSize(18, 18));
    m_sourceList->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *add = toolBtn("plus", "Add Source (Ctrl+N)");
    m_btnRemove = toolBtn("trash", "Remove Source from Scene");
    m_btnUp = toolBtn("up", "Move Up (to front)");
    m_btnDown = toolBtn("down", "Move Down");
    auto *refresh = toolBtn("refresh", "Refresh Windows, Displays, and Cameras");
    connect(add, &QToolButton::clicked, this, &MainWindow::addSourceDialog);
    connect(m_btnRemove, &QToolButton::clicked, this, &MainWindow::removeSelectedItem);
    connect(m_btnUp, &QToolButton::clicked, this, [this] { moveSelectedItem(+1); });
    connect(m_btnDown, &QToolButton::clicked, this, [this] { moveSelectedItem(-1); });
    connect(refresh, &QToolButton::clicked, this, [this] { refreshCaptureList(); refreshCameras(); });
    connect(m_sourceList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it) {
        if (m_loading) return;
        m_selItem = it ? it->data(Qt::UserRole).toString() : QString();
        loadProperties();
        updateSelection();
    });
    connect(m_sourceList, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_loading) return;
        QJsonObject item = itemById(it->data(Qt::UserRole).toString());
        if (item.isEmpty()) return;
        item["visible"] = it->checkState() == Qt::Checked;
        setItem(item);
        rebuildMixer();
        updateSelection();
        applySoon();
    });
    connect(m_sourceList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu m(this);
        m.addAction(themedIcon("plus"), "Add Source…", this, &MainWindow::addSourceDialog);
        if (!m_selItem.isEmpty()) {
            m.addSeparator();
            m.addAction("Filters…", this, [this] {
                const auto src = selectedSource();
                if (sourceTypeInfo(src["type"].toString()).audio) openAudioFiltersDialog(src["id"].toString());
                else openVideoFiltersDialog(m_selItem);
            });
            m.addAction("Rename", this, [this] { m_srcName->setFocus(); m_srcName->selectAll(); });
            if (!sourceTypeInfo(selectedSource()["type"].toString()).audio) {
                m.addAction("Center", this, [this] {
                    editItem([this](QJsonObject &i) { i = makeFree(i); QRectF r = itemRect(i); r.moveCenter({0.5, 0.5}); i["x"] = r.x(); i["y"] = r.y(); });
                });
                m.addAction("Fill Screen", this, [this] { editItem([](QJsonObject &i) { i["fit"] = "Fill"; }); loadProperties(); });
            }
            m.addSeparator();
            m.addAction(themedIcon("trash"), "Remove", this, &MainWindow::removeSelectedItem);
        }
        m.exec(m_sourceList->viewport()->mapToGlobal(pos));
    });
    auto *dSources = makeDock("Sources", "dockSources", withToolbar(m_sourceList, {add, m_btnRemove, m_btnUp, m_btnDown}, {refresh}), Qt::LeftDockWidgetArea);
    splitDockWidget(dScenes, dSources, Qt::Vertical);

    // ---------- Properties ----------
    auto *props = new QWidget;
    buildProperties(props);
    props->setMinimumWidth(300);
    auto *dProps = makeDock("Properties", "dockProps", props, Qt::RightDockWidgetArea);

    // ---------- Mixer ----------
    auto *mix = new QWidget;
    m_mixerLayout = new QBoxLayout(QBoxLayout::TopToBottom, mix);
    m_mixerLayout->setContentsMargins(6, 6, 6, 6);
    m_mixerLayout->setSpacing(6);
    m_mixerLayout->addStretch();
    auto *mixScroll = new QScrollArea;
    mixScroll->setWidgetResizable(true);
    mixScroll->setFrameShape(QFrame::NoFrame);
    mixScroll->setWidget(mix);
    auto *dMix = makeDock("Audio Mixer", "dockMixer", mixScroll, Qt::RightDockWidgetArea);

    // ---------- Controls ----------
    auto *ctl = new QWidget;
    auto *cv = new QVBoxLayout(ctl);
    cv->setContentsMargins(8, 8, 8, 8);
    cv->setSpacing(6);
    m_btnStream = new QPushButton(themedIcon("broadcast"), "  Start Streaming");
    m_btnStream->setObjectName("streamButton");
    m_btnRecord = new QPushButton(themedIcon("record"), "  Start Recording");
    m_btnRecord->setObjectName("recordButton");
    auto *btnSettings = new QPushButton(themedIcon("gear"), "  Settings");
    for (auto *b : {m_btnStream, m_btnRecord, btnSettings}) {
        b->setMinimumHeight(b == m_btnStream ? 38 : 30);
        b->setIconSize(QSize(18, 18));
        b->setCursor(Qt::PointingHandCursor);
        cv->addWidget(b);
    }
    cv->addStretch();
    connect(m_btnStream, &QPushButton::clicked, this, &MainWindow::toggleStream);
    connect(m_btnRecord, &QPushButton::clicked, this, &MainWindow::toggleRecord);
    connect(btnSettings, &QPushButton::clicked, this, &MainWindow::openSettings);
    auto *dCtl = makeDock("Controls", "dockControls", ctl, Qt::RightDockWidgetArea);

    splitDockWidget(dProps, dMix, Qt::Vertical);
    splitDockWidget(dMix, dCtl, Qt::Vertical);
}

void MainWindow::arrangeDocks(bool horizontal)
{
    auto *scenesDock = findChild<QDockWidget *>("dockScenes");
    auto *sourcesDock = findChild<QDockWidget *>("dockSources");
    auto *propertiesDock = findChild<QDockWidget *>("dockProps");
    auto *mixerDock = findChild<QDockWidget *>("dockMixer");
    auto *controlsDock = findChild<QDockWidget *>("dockControls");
    for (auto *dock : {scenesDock, sourcesDock, propertiesDock, mixerDock, controlsDock}) {
        dock->setFloating(false);
        removeDockWidget(dock);
    }
    if (horizontal) {
        for (auto *dock : {scenesDock, sourcesDock, propertiesDock, mixerDock, controlsDock})
            addDockWidget(Qt::BottomDockWidgetArea, dock, Qt::Horizontal);
        resizeDocks({scenesDock, sourcesDock, propertiesDock, mixerDock, controlsDock},
                    {150, 185, 300, 350, 205}, Qt::Horizontal);
        resizeDocks({mixerDock}, {220}, Qt::Vertical);
    } else {
        for (auto *dock : {scenesDock, sourcesDock})
            addDockWidget(Qt::LeftDockWidgetArea, dock, Qt::Vertical);
        for (auto *dock : {propertiesDock, mixerDock, controlsDock})
            addDockWidget(Qt::RightDockWidgetArea, dock, Qt::Vertical);
        resizeDocks({scenesDock, propertiesDock}, {215, 310}, Qt::Horizontal);
        resizeDocks({scenesDock, sourcesDock}, {145, 370}, Qt::Vertical);
        resizeDocks({propertiesDock, mixerDock, controlsDock}, {410, 180, 126}, Qt::Vertical);
    }
    for (auto *dock : {scenesDock, sourcesDock, propertiesDock, mixerDock, controlsDock}) dock->show();
}

void MainWindow::buildStatusBar()
{
    auto *sb = statusBar();
    sb->setSizeGripEnabled(false);
    m_stMsg = new QLabel;
    m_stMsg->setObjectName("statusMsg");
    sb->addWidget(m_stMsg, 1);
    auto add = [sb](QLabel *&l, const QString &name) {
        l = new QLabel;
        l->setObjectName(name);
        sb->addPermanentWidget(l);
    };
    add(m_stDropped, "stItem");
    add(m_stBitrate, "stItem");
    add(m_stLive, "stLive");
    add(m_stRec, "stRec");
    add(m_stCpu, "stItem");
    add(m_stRender, "stItem");
    add(m_stFps, "stItem");
    m_stLive->setText("●  LIVE  00:00:00");
    m_stRec->setText("●  REC  00:00:00");
}

// ---------- properties ----------

void MainWindow::buildProperties(QWidget *host)
{
    auto *v = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    m_propsStack = new QStackedWidget;
    v->addWidget(m_propsStack);

    m_propsEmpty = new QWidget;
    auto *ev = new QVBoxLayout(m_propsEmpty);
    auto *el = new QLabel("Select a source in the list or preview\nto view its settings.\n\nClick + to add a new source");
    el->setAlignment(Qt::AlignCenter);
    el->setObjectName("hint");
    ev->addWidget(el);
    m_propsStack->addWidget(m_propsEmpty);

    m_propsBody = new QWidget;
    auto *bv = new QVBoxLayout(m_propsBody);
    bv->setContentsMargins(0, 0, 0, 0);
    bv->setSpacing(0);
    auto *head = new QWidget;
    head->setObjectName("propsHeader");
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(14, 10, 14, 10);
    m_propsIcon = new QLabel;
    m_srcName = new QLineEdit;
    m_srcName->setObjectName("nameEdit");
    hl->addWidget(m_propsIcon);
    hl->addWidget(m_srcName, 1);
    bv->addWidget(head);
    connect(m_srcName, &QLineEdit::returnPressed, m_srcName, &QWidget::clearFocus);
    connect(m_srcName, &QLineEdit::editingFinished, this, [this] {
        const QString n = m_srcName->text().trimmed();
        if (n.isEmpty() || n == selectedSource()["name"].toString()) return;
        editSource([n](QJsonObject &s) { s["name"] = n; });
        rebuildSourceList();
        rebuildMixer();
    });

    auto *content = new QWidget;
    auto *cv = new QVBoxLayout(content);
    cv->setContentsMargins(14, 10, 14, 14);
    cv->setSpacing(10);
    cv->addWidget(new SectionLabel("Source"));
    m_pages = new QStackedWidget;
    m_pages->addWidget(pageScreen());
    m_pages->addWidget(pageWindow());
    m_pages->addWidget(pageCamera());
    m_pages->addWidget(pageImage());
    m_pages->addWidget(pageText());
    m_pages->addWidget(pageColor());
    m_pages->addWidget(pageMedia());
    m_pages->addWidget(pageChatOverlay());
    m_pages->addWidget(pageApplicationAudio());
    m_pages->addWidget(pageAudioDevice());
    m_pages->addWidget(new QWidget);
    cv->addWidget(m_pages);
    m_srcStatus = new QLabel;
    m_srcStatus->setObjectName("statusPill");
    cv->addWidget(m_srcStatus);
    m_transform = buildTransform();
    cv->addWidget(m_transform);
    cv->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    bv->addWidget(scroll, 1);
    m_propsStack->addWidget(m_propsBody);
}

QWidget *MainWindow::pageScreen()
{
    auto *w = new QWidget;
    auto *f = form(w);
    m_screenCombo = new QComboBox;
    m_screenCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_screenCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    f->addRow(new QLabel("Display"));
    f->addRow(m_screenCombo);
    m_screenCursor = new QCheckBox("Show mouse cursor");
    f->addRow(m_screenCursor);
    connect(m_screenCombo, &QComboBox::activated, this, [this] {
        if (m_loading || m_screenCombo->currentIndex() < 0) return;
        m_screenCombo->setToolTip(m_screenCombo->currentText());
        const QString monitorId = m_screenCombo->currentData().toString();
        auto source = selectedSource();
        if (source.isEmpty() || source["captureId"].toString() == monitorId) return;

        auto item = selectedItem();
        bool shared = false;
        for (const auto &sceneVal : scenes()) {
            for (const auto &value : sceneVal.toObject()["items"].toArray()) {
                const auto other = value.toObject();
                if (other["id"] != item["id"] && other["sourceId"] == item["sourceId"]) { shared = true; break; }
            }
            if (shared) break;
        }
        source["captureId"] = monitorId;
        if (shared) {
            source["id"] = newId();
            auto definitions = sources();
            definitions.append(source);
            m_s["sources"] = definitions;
            item["sourceId"] = source["id"];
            setItem(item);
        } else {
            setSource(source);
        }
        updateSourceStatus();
        applySoon();
    });
    connect(m_screenCursor, &QCheckBox::toggled, this, [this](bool on) { if (!m_loading) editSource([on](QJsonObject &s) { s["captureCursor"] = on; }); });
    return w;
}

QWidget *MainWindow::pageWindow()
{
    auto *w = new QWidget;
    auto *f = form(w);
    m_gameMode = new QComboBox;
    widenChoice(m_gameMode);
    m_gameMode->addItem("Any fullscreen application", "Fullscreen");
    m_gameMode->addItem("Specific window", "Window");
    f->addRow(m_gameMode);
    m_captureMethod = captureMethodChoice();
    f->addRow(m_captureMethod);
    auto *desktopNote = hintLabel("DXGI crops a region of the display. Keep the game on top. Overlays may appear in the capture.");
    f->addRow(desktopNote);
    connect(m_captureMethod, &QComboBox::currentIndexChanged, desktopNote, [desktopNote](int index) {
        desktopNote->setVisible(index == 1);
    });
    desktopNote->hide();
    connect(m_captureMethod, &QComboBox::activated, this, [this](int index) {
        if (!m_loading) editSource([index](QJsonObject &s) { s["captureFromDesktop"] = index == 1; });
    });

    m_windowTarget = new QWidget;
    auto *row = new QHBoxLayout(m_windowTarget);
    row->setContentsMargins(0, 0, 0, 0);
    m_windowCombo = new QComboBox;
    widenChoice(m_windowCombo);
    auto *r = toolBtn("refresh", "Refresh window list");
    row->addWidget(m_windowCombo, 1);
    row->addWidget(r);
    f->addRow(m_windowTarget);
    m_windowPriority = new QComboBox;
    widenChoice(m_windowPriority);
    m_windowPriority->addItems({"Match by application name", "Require exact window title"});
    f->addRow(m_windowPriority);
    m_windowCursor = new QCheckBox("Capture cursor");
    f->addRow(m_windowCursor);

    connect(m_gameMode, &QComboBox::activated, this, [this](int index) {
        if (m_loading) return;
        editSource([this](QJsonObject &s) { s["gameMode"] = m_gameMode->currentData().toString(); });
        m_windowTarget->setVisible(index == 1);
        m_windowPriority->setVisible(index == 1);
    });
    connect(m_windowPriority, &QComboBox::activated, this, [this](int index) {
        if (!m_loading) editSource([index](QJsonObject &s) { s["matchWindowTitle"] = index == 1; });
    });
    connect(r, &QToolButton::clicked, this, &MainWindow::refreshCaptureList);
    connect(m_windowCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        editSource([this](QJsonObject &s) { s["captureId"] = m_windowCombo->currentData().toString(); });
    });
    connect(m_windowCursor, &QCheckBox::toggled, this, [this](bool on) { if (!m_loading) editSource([on](QJsonObject &s) { s["captureCursor"] = on; }); });
    return w;
}

QWidget *MainWindow::pageApplicationAudio()
{
    auto *w = new QWidget;
    auto *f = form(w);
    auto *row = new QHBoxLayout;
    m_applicationCombo = new QComboBox;
    widenChoice(m_applicationCombo);
    row->addWidget(m_applicationCombo, 1);
    auto *refresh = toolBtn("refresh", "Refresh applications");
    row->addWidget(refresh);
    f->addRow(new QLabel("Application"));
    f->addRow(row);
    connect(refresh, &QToolButton::clicked, this, [this] {
        populateApps(m_applicationCombo, selectedSource()["deviceId"].toString());
    });
    connect(m_applicationCombo, &QComboBox::activated, this, [this] {
        editSource([this](QJsonObject &s) { s["deviceId"] = m_applicationCombo->currentData().toString(); });
        rebuildMixer();
    });
    return w;
}

QWidget *MainWindow::pageAudioDevice()
{
    auto *w = new QWidget;
    auto *f = form(w);
    auto *row = new QHBoxLayout;
    m_audioDeviceCombo = new QComboBox;
    widenChoice(m_audioDeviceCombo);
    row->addWidget(m_audioDeviceCombo, 1);
    auto *refresh = toolBtn("refresh", "Refresh audio devices");
    row->addWidget(refresh);
    f->addRow(new QLabel("Device"));
    f->addRow(row);
    connect(refresh, &QToolButton::clicked, this, [this] { refreshAudioDevices(true); });
    connect(m_audioDeviceCombo, &QComboBox::activated, this, [this] {
        const auto id = m_audioDeviceCombo->currentData().toString();
        editSource([&id](QJsonObject &s) { s["deviceId"] = id; });
        rebuildMixer();
    });
    return w;
}

void MainWindow::populateAudioDeviceChoice()
{
    const auto source = selectedSource();
    const auto type = source["type"].toString();
    if (type != "AudioInput" && type != "AudioOutput") return;
    const bool input = type == "AudioInput";
    const auto devices = m_audioDevices[input ? "inputs" : "outputs"].toArray();
    const auto current = source["deviceId"].toString();
    const auto defaultId = m_audioDevices[input ? "defaultInput" : "defaultOutput"].toString();
    QSignalBlocker blocked(m_audioDeviceCombo);
    m_audioDeviceCombo->clear();
    m_audioDeviceCombo->addItem("System default device", "");
    for (const auto &entry : devices) {
        const auto device = entry.toObject();
        const auto id = device["id"].toString();
        const auto name = device["name"].toString();
        if (id == defaultId) m_audioDeviceCombo->setItemText(0, "Default — " + name);
        m_audioDeviceCombo->addItem(name, id);
    }
    if (!current.isEmpty() && m_audioDeviceCombo->findData(current) < 0)
        m_audioDeviceCombo->addItem("Selected device is disconnected", current);
    m_audioDeviceCombo->setCurrentIndex(std::max(0, m_audioDeviceCombo->findData(current)));
    m_audioDeviceCombo->setToolTip(m_audioDeviceCombo->currentText());
}

void MainWindow::refreshAudioDevices(bool force)
{
    m_forceAudioDeviceRefresh |= force;
    if (m_audioDeviceScanPending) return;
    m_audioDeviceScanPending = true;

    // Reading WASAPI device properties and audio resync can block — run on a background thread.
    const QJsonObject previous = m_audioDevices;
    const bool forced = std::exchange(m_forceAudioDeviceRefresh, false);
    auto result = std::make_shared<QJsonObject>();
    auto changed = std::make_shared<bool>(false);
    auto *scan = QThread::create([result, changed, previous, forced] {
        QJsonObject obj = EngineHost::instance().callObj("listAudioDevices");
        const bool needsSync = obj.take("needsSync").toBool(false);
        *result = obj;
        if (!obj.contains("inputs")) return;
        *changed = (obj != previous);
        if (forced || *changed || needsSync)
            EngineHost::instance().callObj("refreshAudioDevices");
    });
    connect(scan, &QThread::finished, this, [this, result, changed, forced] {
        m_audioDeviceScanPending = false;
        if (!result->contains("inputs") || (!forced && !*changed)) return;
        m_audioDevices = *result;
        populateAudioDeviceChoice();
        m_mixerSignature.clear();
        rebuildMixer();
    });
    connect(scan, &QThread::finished, scan, &QObject::deleteLater);
    scan->start();
}

QWidget *MainWindow::pageCamera()
{
    auto *w = new QWidget;
    auto *f = form(w);
    m_camDevice = new QComboBox;
    widenChoice(m_camDevice);
    f->addRow(new QLabel("Device"));
    f->addRow(m_camDevice);
    m_camMode = new QComboBox;
    widenChoice(m_camMode);
    m_camMode->setMaxVisibleItems(18);
    f->addRow(new QLabel("Mode"));
    f->addRow(m_camMode);
    f->addRow(hintLabel("For 60 FPS streaming, select 60 FPS in NV12 if your camera supports it. NV12 bypasses MJPEG decoding."));
    connect(m_camDevice, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        const QString name = m_camDevice->currentData().toString();
        fillCameraModes(name);
        int best = 0;
        for (int i = 0; i < m_camMode->count(); ++i) {
            const auto m = QJsonObject::fromVariantMap(m_camMode->itemData(i).toMap());
            if (m["width"].toInt() == 1920 && m["fps"].toInt() == 60) { best = i; break; }
        }
        const QJsonObject mode = m_camMode->count() ? QJsonObject::fromVariantMap(m_camMode->itemData(best).toMap()) : QJsonObject{};
        m_loading = true;
        m_camMode->setCurrentIndex(best);
        m_loading = false;
        editSource([&](QJsonObject &s) {
            s["cameraName"] = name;
            if (!mode.isEmpty()) {
                s["cameraWidth"] = mode["width"]; s["cameraHeight"] = mode["height"];
                s["cameraFps"] = mode["fps"]; s["cameraFormat"] = mode["format"];
            }
        });
    });
    connect(m_camMode, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_loading || i < 0) return;
        const auto m = QJsonObject::fromVariantMap(m_camMode->itemData(i).toMap());
        if (m.isEmpty()) return;
        editSource([&](QJsonObject &s) {
            s["cameraWidth"] = m["width"]; s["cameraHeight"] = m["height"]; s["cameraFps"] = m["fps"]; s["cameraFormat"] = m["format"];
        });
    });
    return w;
}

QWidget *MainWindow::pageImage()
{
    auto *w = new QWidget;
    auto *f = form(w);
    auto *row = new QHBoxLayout();
    m_imagePath = new QLineEdit;
    m_imagePath->setReadOnly(true);
    auto *browse = new QPushButton("Browse…");
    row->addWidget(m_imagePath, 1);
    row->addWidget(browse);
    f->addRow("File", row);
    f->addRow("", hintLabel("PNG with transparency works best."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(this, "Select Image", {}, "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)");
        if (p.isEmpty()) return;
        m_imagePath->setText(QDir::toNativeSeparators(p));
        editSource([p](QJsonObject &s) { s["filePath"] = QDir::toNativeSeparators(p); });
    });
    return w;
}

QWidget *MainWindow::pageText()
{
    auto *w = new QWidget;
    auto *f = form(w);
    m_text = new QPlainTextEdit;
    m_text->setFixedHeight(72);
    f->addRow("Text", m_text);
    m_font = new QFontComboBox;
    f->addRow("Font", m_font);
    auto *row = new QHBoxLayout();
    m_fontSize = new PaddedSpinBox;
    m_fontSize->setRange(8, 400);
    m_fontSize->setSuffix(" px");
    m_bold = new QCheckBox("Bold");
    row->addWidget(m_fontSize, 1);
    row->addWidget(m_bold);
    f->addRow("Size", row);
    m_textColor = new ColorButton("Text Color");
    f->addRow("Color", m_textColor);
    auto *orow = new QHBoxLayout();
    m_outlineColor = new ColorButton("Outline");
    m_outlineW = new PaddedSpinBox;
    m_outlineW->setRange(0, 40);
    m_outlineW->setSuffix(" px");
    orow->addWidget(m_outlineColor, 1);
    orow->addWidget(m_outlineW);
    f->addRow("Outline", orow);
    m_bgColor = new ColorButton("Background");
    f->addRow("Background", m_bgColor);

    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { if (!m_loading) editSource([this](QJsonObject &s) { s["text"] = m_text->toPlainText(); }); });
    connect(m_font, &QFontComboBox::currentFontChanged, this, [this](const QFont &fn) { if (!m_loading) editSource([fn](QJsonObject &s) { s["fontName"] = fn.family(); }); });
    connect(m_fontSize, &QSpinBox::valueChanged, this, [this](int v) { if (!m_loading) editSource([v](QJsonObject &s) { s["fontSize"] = v; }); });
    connect(m_bold, &QCheckBox::toggled, this, [this](bool b) { if (!m_loading) editSource([b](QJsonObject &s) { s["bold"] = b; }); });
    connect(m_outlineW, &QSpinBox::valueChanged, this, [this](int v) { if (!m_loading) editSource([v](QJsonObject &s) { s["outlineWidth"] = v; }); });
    connect(m_textColor, &ColorButton::colorChanged, this, [this](const QColor &c) { editSource([c](QJsonObject &s) { s["colorArgb"] = argbVal(c); }); });
    connect(m_outlineColor, &ColorButton::colorChanged, this, [this](const QColor &c) { editSource([c](QJsonObject &s) { s["outlineArgb"] = argbVal(c); }); });
    connect(m_bgColor, &ColorButton::colorChanged, this, [this](const QColor &c) { editSource([c](QJsonObject &s) { s["backgroundArgb"] = argbVal(c); }); });
    return w;
}

QWidget *MainWindow::pageColor()
{
    auto *w = new QWidget;
    auto *f = form(w);
    m_fillColor = new ColorButton("Fill Color");
    f->addRow("Color", m_fillColor);
    f->addRow("", hintLabel("Adjust size and position in the preview. The color's alpha channel controls transparency."));
    connect(m_fillColor, &ColorButton::colorChanged, this, [this](const QColor &c) { editSource([c](QJsonObject &s) { s["colorArgb"] = argbVal(c); }); });
    return w;
}

QWidget *MainWindow::pageMedia()
{
    auto *w = new QWidget;
    auto *f = form(w);
    auto *row = new QHBoxLayout();
    m_mediaPath = new QLineEdit;
    m_mediaPath->setReadOnly(true);
    auto *browse = new QPushButton("Browse…");
    row->addWidget(m_mediaPath, 1);
    row->addWidget(browse);
    f->addRow("File", row);
    m_mediaLoop = new QCheckBox("Loop playback");
    f->addRow("", m_mediaLoop);
    f->addRow("", hintLabel("Media audio will appear in the mixer. Supports MP4, MKV, MOV, WEBM, GIF, and more."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(this, "Select Video File", {}, "Video (*.mp4 *.mkv *.mov *.webm *.avi *.gif *.flv);;All Files (*.*)");
        if (p.isEmpty()) return;
        m_mediaPath->setText(QDir::toNativeSeparators(p));
        editSource([p](QJsonObject &s) { s["filePath"] = QDir::toNativeSeparators(p); });
    });
    connect(m_mediaLoop, &QCheckBox::toggled, this, [this](bool on) { if (!m_loading) editSource([on](QJsonObject &s) { s["loop"] = on; }); });
    return w;
}

QWidget *MainWindow::pageChatOverlay()
{
    auto *w = new QWidget;
    auto *f = form(w);
    m_chatShowMessages = new QCheckBox("Show chat messages");
    m_chatShowGifts = new QCheckBox("Show gifts");
    m_chatShowTaps = new QCheckBox("Show likes counter");
    m_chatDemoBtn = new QPushButton("Test Preview");
    m_chatDemoBtn->setCheckable(true);
    f->addRow("", m_chatShowMessages);
    f->addRow("", m_chatShowGifts);
    f->addRow("", m_chatShowTaps);
    f->addRow("", m_chatDemoBtn);
    f->addRow("", hintLabel("The overlay shares the TikTok LIVE Chat panel connection without capturing any browser window."));
    connect(m_chatShowMessages, &QCheckBox::toggled, this, [this](bool on) { if (!m_loading) editSource([on](QJsonObject &s) { s["showChat"] = on; }); });
    connect(m_chatShowGifts, &QCheckBox::toggled, this, [this](bool on) { if (!m_loading) editSource([on](QJsonObject &s) { s["showGifts"] = on; }); });
    connect(m_chatShowTaps, &QCheckBox::toggled, this, [this](bool on) { if (!m_loading) editSource([on](QJsonObject &s) { s["showTaps"] = on; }); });
    connect(m_chatDemoBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_liveChat) m_liveChat->setDemoPreview(on);
    });
    return w;
}

QFrame *MainWindow::buildTransform()
{
    auto *box = new QFrame;
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 4, 0, 0);
    v->setSpacing(9);
    v->addWidget(new SectionLabel("Transform & Appearance"));
    auto *w = new QWidget;
    auto *f = form(w);
    m_fit = new QComboBox;
    m_fit->addItem("Free (drag in preview)", "Free");
    m_fit->addItem("Fill screen (crop)", "Fill");
    m_fit->addItem("Fit + blurred background", "FitBlur");
    f->addRow("Fit Mode", m_fit);

    auto *btns = new QHBoxLayout();
    auto *center = new QPushButton("Center");
    auto *full = new QPushButton("Full Screen");
    auto *aspect = new QPushButton("1:1 Aspect");
    aspect->setToolTip("Restore original source aspect ratio");
    for (auto *b : {center, full, aspect}) btns->addWidget(b);
    f->addRow("", btns);

    m_cropRows = new QWidget;
    auto *cf = form(m_cropRows);
    m_zoom = hslider(100, 400);
    m_panX = hslider(-100, 100);
    m_panY = hslider(-100, 100);
    cf->addRow("Zoom", m_zoom);
    cf->addRow("Pan X", m_panX);
    cf->addRow("Pan Y", m_panY);
    f->addRow(m_cropRows);

    m_mirror = new QCheckBox("Mirror horizontally");
    f->addRow("", m_mirror);
    m_corner = hslider(0, 50);
    f->addRow("Corner Radius", m_corner);
    m_opacity = hslider(0, 100);
    f->addRow("Opacity", m_opacity);
    v->addWidget(w);

    connect(m_fit, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        const QString fit = m_fit->currentData().toString();
        editItem([this, fit](QJsonObject &i) {
            if (fit == "Free") i = makeFree(i);
            i["fit"] = fit;
        });
    });
    connect(center, &QPushButton::clicked, this, [this] {
        editItem([this](QJsonObject &i) { i = makeFree(i); QRectF r = itemRect(i); r.moveCenter({0.5, 0.5}); i["x"] = r.x(); i["y"] = r.y(); });
        loadProperties();
    });
    connect(full, &QPushButton::clicked, this, [this] { editItem([](QJsonObject &i) { i["fit"] = "Fill"; }); loadProperties(); });
    connect(aspect, &QPushButton::clicked, this, [this] {
        editItem([this](QJsonObject &i) {
            i = makeFree(i);
            const QSizeF ns = naturalSize(i["sourceId"].toString());
            QRectF r = itemRect(i);
            const QPointF c = r.center();
            r.setHeight(r.width() * m_canvasW * ns.height() / ns.width() / m_canvasH);
            r.moveCenter(c);
            i["x"] = r.x(); i["y"] = r.y(); i["w"] = r.width(); i["h"] = r.height();
        });
        loadProperties();
    });
    auto slider = [this](QSlider *s, const char *key, double div) {
        connect(s, &QSlider::valueChanged, this, [this, key, div](int val) { if (!m_loading) editItem([=](QJsonObject &i) { i[key] = val / div; }); });
    };
    slider(m_zoom, "zoom", 100.0);
    slider(m_panX, "panX", 100.0);
    slider(m_panY, "panY", 100.0);
    slider(m_corner, "corner", 100.0);
    slider(m_opacity, "opacity", 100.0);
    connect(m_mirror, &QCheckBox::toggled, this, [this](bool on) { if (!m_loading) editItem([on](QJsonObject &i) { i["mirror"] = on; }); });
    return box;
}

void MainWindow::openVideoFiltersDialog(const QString &itemId)
{
    QJsonObject item = itemById(itemId);
    if (item.isEmpty()) return;
    const QJsonObject src = sourceById(item["sourceId"].toString());

    QDialog dlg(this);
    dlg.setWindowTitle("Video Filters — " + src["name"].toString());
    dlg.resize(660, 360);

    auto *root = new QVBoxLayout(&dlg);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto *body = new QHBoxLayout;
    body->setSpacing(14);

    auto *leftCol = new QVBoxLayout;
    leftCol->setSpacing(6);
    auto *title = new QLabel("Video Filters (GPU)");
    title->setObjectName("cardTitle");
    leftCol->addWidget(title);

    auto *list = new QListWidget;
    list->setObjectName("filterList");
    list->setFixedWidth(210);
    auto addEntry = [&](const QString &label, bool checkable, bool checked = false) {
        auto *it = new QListWidgetItem(label, list);
        if (checkable) {
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        }
        it->setSizeHint(QSize(0, 34));
        return it;
    };
    addEntry("Edge Cropping", false);
    addEntry("Color Correction", false);
    auto *chromaItem = addEntry("Green Screen (Chroma Key)", true, item["chromaKeyEnabled"].toBool());
    leftCol->addWidget(list, 1);
    body->addLayout(leftCol);

    auto *pages = new QStackedWidget;
    auto updateItem = [this, itemId](const std::function<void(QJsonObject &)> &fn) {
        QJsonObject cur = itemById(itemId);
        if (cur.isEmpty()) return;
        fn(cur);
        setItem(cur);
        applySoon();
    };

    // 0: Edge Cropping
    auto *cropL = hslider(0, 45); cropL->setValue(qRound(item["cropLeft"].toDouble() * 100));
    auto *cropR = hslider(0, 45); cropR->setValue(qRound(item["cropRight"].toDouble() * 100));
    auto *cropT = hslider(0, 45); cropT->setValue(qRound(item["cropTop"].toDouble() * 100));
    auto *cropB = hslider(0, 45); cropB->setValue(qRound(item["cropBottom"].toDouble() * 100));
    {
        auto *p = new QWidget;
        auto *f = form(p);
        auto addRowPct = [&](const QString &lbl, QSlider *s, const char *key) {
            auto *vLbl = hintLabel(QString("%1%").arg(s->value()));
            auto *row = new QHBoxLayout; row->addWidget(s, 1); row->addWidget(vLbl);
            f->addRow(lbl, row);
            connect(s, &QSlider::valueChanged, &dlg, [=](int val) {
                vLbl->setText(QString("%1%").arg(val));
                updateItem([key, val](QJsonObject &i) { i[key] = val / 100.0; });
            });
        };
        addRowPct("Crop Left", cropL, "cropLeft");
        addRowPct("Crop Right", cropR, "cropRight");
        addRowPct("Crop Top", cropT, "cropTop");
        addRowPct("Crop Bottom", cropB, "cropBottom");
        pages->addWidget(p);
    }

    // 1: Color Correction
    auto *bright = hslider(-50, 50); bright->setValue(qRound(item["brightness"].toDouble() * 100));
    auto *contrast = hslider(50, 200); contrast->setValue(qRound(item["contrast"].toDouble(1) * 100));
    auto *sat = hslider(0, 200); sat->setValue(qRound(item["saturation"].toDouble(1) * 100));
    {
        auto *p = new QWidget;
        auto *f = form(p);
        auto addRowColor = [&](const QString &lbl, QSlider *s, const char *key) {
            auto *vLbl = hintLabel(QString("%1%").arg(s->value()));
            auto *row = new QHBoxLayout; row->addWidget(s, 1); row->addWidget(vLbl);
            f->addRow(lbl, row);
            connect(s, &QSlider::valueChanged, &dlg, [=](int val) {
                vLbl->setText(QString("%1%").arg(val));
                updateItem([key, val](QJsonObject &i) { i[key] = val / 100.0; });
            });
        };
        addRowColor("Brightness", bright, "brightness");
        addRowColor("Contrast", contrast, "contrast");
        addRowColor("Saturation", sat, "saturation");
        pages->addWidget(p);
    }

    // 2: Green Screen (Chroma Key)
    auto *chromaColor = new ColorButton("Key Color");
    chromaColor->setColor(argbOf(item["chromaKeyArgb"], QColor(0, 255, 0)));
    auto *chromaSim = hslider(1, 60); chromaSim->setValue(qRound(item["chromaKeySimilarity"].toDouble(0.25) * 100));
    auto *chromaSmooth = hslider(1, 30); chromaSmooth->setValue(qRound(item["chromaKeySmoothness"].toDouble(0.08) * 100));
    {
        auto *p = new QWidget;
        auto *f = form(p);
        f->addRow("Key Color", chromaColor);
        auto *simLbl = hintLabel(QString("%1%").arg(chromaSim->value()));
        auto *smLbl = hintLabel(QString("%1%").arg(chromaSmooth->value()));
        auto *r1 = new QHBoxLayout; r1->addWidget(chromaSim, 1); r1->addWidget(simLbl);
        auto *r2 = new QHBoxLayout; r2->addWidget(chromaSmooth, 1); r2->addWidget(smLbl);
        f->addRow("Similarity", r1);
        f->addRow("Smoothness", r2);
        f->addRow("", hintLabel("Processed entirely on the GPU (D3D11, 0% CPU). Toggle the eye icon on the left to enable."));
        connect(chromaColor, &ColorButton::colorChanged, &dlg, [=](const QColor &c) {
            updateItem([c](QJsonObject &i) { i["chromaKeyArgb"] = int(c.rgba()); });
        });
        connect(chromaSim, &QSlider::valueChanged, &dlg, [=](int val) {
            simLbl->setText(QString("%1%").arg(val));
            updateItem([val](QJsonObject &i) { i["chromaKeySimilarity"] = val / 100.0; });
        });
        connect(chromaSmooth, &QSlider::valueChanged, &dlg, [=](int val) {
            smLbl->setText(QString("%1%").arg(val));
            updateItem([val](QJsonObject &i) { i["chromaKeySmoothness"] = val / 100.0; });
        });
        pages->addWidget(p);
    }

    connect(list, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(list, &QListWidget::itemChanged, &dlg, [=](QListWidgetItem *it) {
        if (it != chromaItem) return;
        const bool on = it->checkState() == Qt::Checked;
        updateItem([on](QJsonObject &i) { i["chromaKeyEnabled"] = on; });
    });
    list->setCurrentRow(0);

    body->addWidget(pages, 1);
    root->addLayout(body, 1);

    auto *btns = new QHBoxLayout;
    auto *resetBtn = new QPushButton("Reset Filters");
    auto *closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    connect(resetBtn, &QPushButton::clicked, &dlg, [=] {
        cropL->setValue(0); cropR->setValue(0); cropT->setValue(0); cropB->setValue(0);
        bright->setValue(0); contrast->setValue(100); sat->setValue(100);
        chromaItem->setCheckState(Qt::Unchecked);
        chromaColor->setColor(QColor(0, 255, 0));
        chromaSim->setValue(25); chromaSmooth->setValue(8);
        updateItem([](QJsonObject &i) {
            i["cropLeft"] = 0; i["cropRight"] = 0; i["cropTop"] = 0; i["cropBottom"] = 0;
            i["brightness"] = 0; i["contrast"] = 1; i["saturation"] = 1;
            i["chromaKeyEnabled"] = false; i["chromaKeyArgb"] = int(QColor(0, 255, 0).rgba());
            i["chromaKeySimilarity"] = 0.25; i["chromaKeySmoothness"] = 0.08;
        });
    });
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btns->addWidget(resetBtn);
    btns->addStretch();
    btns->addWidget(closeBtn);
    root->addLayout(btns);

    dlg.exec();
}

void MainWindow::openAudioFiltersDialog(const QString &sourceId)
{
    QJsonObject src = sourceById(sourceId);
    if (src.isEmpty()) return;
    const bool isInput = src["type"].toString() == "AudioInput";

    QDialog dlg(this);
    dlg.setWindowTitle("Audio Filters — " + src["name"].toString());
    dlg.resize(680, 350);

    auto *root = new QVBoxLayout(&dlg);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto *body = new QHBoxLayout;
    body->setSpacing(14);

    // Left side: filter list with eye icons (for inputs: 4 filters; for outputs/apps/media: Compressor + Limiter)
    auto *leftCol = new QVBoxLayout;
    leftCol->setSpacing(6);
    auto *title = new QLabel("Audio Filters");
    title->setObjectName("cardTitle");
    leftCol->addWidget(title);

    auto *list = new QListWidget;
    list->setObjectName("filterList");
    list->setFixedWidth(210);
    QStringList filterKeys;
    auto addFilterItem = [&](const QString &label, const QString &key, bool enabled) {
        auto *it = new QListWidgetItem(label, list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
        it->setSizeHint(QSize(0, 34));
        filterKeys.append(key);
        return it;
    };
    if (isInput) {
        addFilterItem("Noise Suppression", "noiseSuppressEnabled", src["noiseSuppressEnabled"].toBool(false));
        addFilterItem("Noise Gate", "noiseGateEnabled", src["noiseGateEnabled"].toBool(false));
    }
    addFilterItem("Compressor", "compressorEnabled", src["compressorEnabled"].toBool(false));
    addFilterItem("Limiter", "limiterEnabled", src["limiterEnabled"].toBool(false));
    {
        auto *syncIt = new QListWidgetItem("Sync Offset (Delay)", list);
        syncIt->setSizeHint(QSize(0, 34));
        filterKeys.append("");
    }
    leftCol->addWidget(list, 1);
    body->addLayout(leftCol);

    auto *pages = new QStackedWidget;
    auto updateSrc = [this, sourceId](const std::function<void(QJsonObject &)> &fn) {
        QJsonObject cur = sourceById(sourceId);
        if (cur.isEmpty()) return;
        fn(cur);
        setSource(cur);
        applySoon();
    };

    // Helper: slider + editable dB QSpinBox (Enter or clicking outside saves without closing dialog)
    auto makeDbRow = [&](int minDb, int maxDb, int initialDb, const QString &key, bool asInt) -> QWidget * {
        auto *wrap = new QWidget;
        auto *row = new QHBoxLayout(wrap);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *sl = hslider(minDb, maxDb);
        sl->setValue(std::clamp(initialDb, minDb, maxDb));
        auto *sp = new QSpinBox;
        sp->setRange(minDb, maxDb);
        sp->setSuffix(" dB");
        sp->setFixedWidth(84);
        sp->setValue(sl->value());
        sp->setKeyboardTracking(false);
        row->addWidget(sl, 1);
        row->addWidget(sp);
        connect(sl, &QSlider::valueChanged, &dlg, [=](int val) {
            if (sp->value() != val) { QSignalBlocker b(sp); sp->setValue(val); }
            updateSrc([key, val, asInt](QJsonObject &s) { s[key] = asInt ? QJsonValue(val) : QJsonValue(double(val)); });
        });
        connect(sp, &QSpinBox::valueChanged, &dlg, [=](int val) {
            if (sl->value() != val) { QSignalBlocker b(sl); sl->setValue(val); }
            updateSrc([key, val, asInt](QJsonObject &s) { s[key] = asInt ? QJsonValue(val) : QJsonValue(double(val)); });
        });
        connect(sp, &QSpinBox::editingFinished, sp, &QWidget::clearFocus);
        return wrap;
    };

    if (isInput) {
        // Noise Suppression (Speex / RNNoise)
        auto *p = new QWidget;
        auto *f = form(p);
        auto *method = new QComboBox;
        method->addItem("Speex (low CPU usage, basic quality)", "speex");
        method->addItem("RNNoise (higher CPU usage, good quality)", "rnnoise");
        const QString curMethod = src["noiseSuppressMethod"].toString("rnnoise");
        method->setCurrentIndex(curMethod == "speex" ? 0 : 1);
        f->addRow("Method", method);

        auto *lvlWrap = makeDbRow(-60, 0, src["noiseSuppressLevelDb"].toInt(-30), "noiseSuppressLevelDb", true);
        auto *lvlTitle = new QLabel("Suppression Level");
        f->addRow(lvlTitle, lvlWrap);

        auto *info = hintLabel("");
        f->addRow("", info);

        auto syncMethodUi = [=] {
            const bool isSpeex = method->currentData().toString() == "speex";
            lvlTitle->setVisible(isSpeex);
            lvlWrap->setVisible(isSpeex);
            info->setText(isSpeex
                ? "SpeexDSP suppresses steady background noise with very low CPU usage."
                : "RNNoise (48 kHz neural network) automatically removes background noise, keyboard clicks, and fan hum without manual calibration.");
        };
        syncMethodUi();
        connect(method, &QComboBox::currentIndexChanged, &dlg, [=] {
            syncMethodUi();
            const QString m = method->currentData().toString();
            updateSrc([m](QJsonObject &s) { s["noiseSuppressMethod"] = m; });
        });
        pages->addWidget(p);

        // Noise Gate
        auto *pGate = new QWidget;
        auto *fGate = form(pGate);
        fGate->addRow("Open Threshold", makeDbRow(-100, 0, qRound(src["noiseGateThresholdDb"].toDouble(-40)), "noiseGateThresholdDb", false));
        fGate->addRow("", hintLabel("Audio quieter than the threshold is smoothly muted (4 dB hysteresis, 80 ms hold)."));
        pages->addWidget(pGate);
    }

    // Compressor
    {
        auto *p = new QWidget;
        auto *f = form(p);
        f->addRow("Threshold", makeDbRow(-100, 0, qRound(src["compressorThresholdDb"].toDouble(-18)), "compressorThresholdDb", false));

        auto *ratioWrap = new QWidget;
        auto *r2 = new QHBoxLayout(ratioWrap);
        r2->setContentsMargins(0, 0, 0, 0);
        r2->setSpacing(8);
        auto *compRatio = hslider(15, 200);
        compRatio->setValue(std::clamp(qRound(src["compressorRatio"].toDouble(4.0) * 10), 15, 200));
        auto *ratioSpin = new QDoubleSpinBox;
        ratioSpin->setRange(1.5, 20.0);
        ratioSpin->setSingleStep(0.5);
        ratioSpin->setDecimals(1);
        ratioSpin->setSuffix(":1");
        ratioSpin->setFixedWidth(84);
        ratioSpin->setValue(compRatio->value() / 10.0);
        ratioSpin->setKeyboardTracking(false);
        r2->addWidget(compRatio, 1);
        r2->addWidget(ratioSpin);
        f->addRow("Ratio", ratioWrap);
        f->addRow("", hintLabel("Evens out voice dynamics and tames loud volume spikes."));
        connect(compRatio, &QSlider::valueChanged, &dlg, [=](int val) {
            const double r = val / 10.0;
            if (std::abs(ratioSpin->value() - r) > 0.05) { QSignalBlocker b(ratioSpin); ratioSpin->setValue(r); }
            updateSrc([r](QJsonObject &s) { s["compressorRatio"] = r; });
        });
        connect(ratioSpin, &QDoubleSpinBox::valueChanged, &dlg, [=](double val) {
            const int sv = qRound(val * 10.0);
            if (compRatio->value() != sv) { QSignalBlocker b(compRatio); compRatio->setValue(sv); }
            updateSrc([val](QJsonObject &s) { s["compressorRatio"] = val; });
        });
        connect(ratioSpin, &QDoubleSpinBox::editingFinished, ratioSpin, &QWidget::clearFocus);
        pages->addWidget(p);
    }

    // Limiter
    {
        auto *p = new QWidget;
        auto *f = form(p);
        f->addRow("Ceiling", makeDbRow(-100, 0, qRound(src["limiterCeilingDb"].toDouble(-1)), "limiterCeilingDb", false));
        f->addRow("", hintLabel("Caps maximum signal level in dB (0 dB = full scale, negative values attenuate peaks)."));
        pages->addWidget(p);
    }

    // A/V Sync Offset (delay in ms — e.g. aligning microphone with a 1080p60 USB webcam)
    {
        auto *p = new QWidget;
        auto *f = form(p);
        auto *wrap = new QWidget;
        auto *row = new QHBoxLayout(wrap);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(8);
        auto *sl = hslider(0, 500);
        sl->setValue(std::clamp(src["syncOffsetMs"].toInt(0), 0, 500));
        auto *sp = new QSpinBox;
        sp->setRange(0, 500);
        sp->setSingleStep(10);
        sp->setSuffix(" ms");
        sp->setFixedWidth(90);
        sp->setValue(sl->value());
        sp->setKeyboardTracking(false);
        row->addWidget(sl, 1);
        row->addWidget(sp);
        f->addRow("Sync Offset", wrap);
        f->addRow("", hintLabel("Delays audio relative to video. For 1080p60 USB webcams (e.g. Facecam MK.2), 40–80 ms typically aligns voice and lips."));
        connect(sl, &QSlider::valueChanged, &dlg, [=](int val) {
            if (sp->value() != val) { QSignalBlocker b(sp); sp->setValue(val); }
            updateSrc([val](QJsonObject &s) { s["syncOffsetMs"] = val; });
        });
        connect(sp, &QSpinBox::valueChanged, &dlg, [=](int val) {
            if (sl->value() != val) { QSignalBlocker b(sl); sl->setValue(val); }
            updateSrc([val](QJsonObject &s) { s["syncOffsetMs"] = val; });
        });
        connect(sp, &QSpinBox::editingFinished, sp, &QWidget::clearFocus);
        pages->addWidget(p);
    }

    connect(list, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(list, &QListWidget::itemChanged, &dlg, [=, &updateSrc](QListWidgetItem *it) {
        const int row = list->row(it);
        const bool on = it->checkState() == Qt::Checked;
        if (row >= 0 && row < filterKeys.size() && !filterKeys[row].isEmpty()) {
            const QString k = filterKeys[row];
            updateSrc([k, on](QJsonObject &s) { s[k] = on; });
        }
    });
    list->setCurrentRow(0);

    body->addWidget(pages, 1);
    root->addLayout(body, 1);

    auto *btns = new QHBoxLayout;
    btns->addStretch();
    auto *closeBtn = new QPushButton("Close");
    closeBtn->setDefault(false);
    closeBtn->setAutoDefault(false);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btns->addWidget(closeBtn);
    root->addLayout(btns);

    dlg.exec();
}

// =============================== model ===============================

int MainWindow::activeSceneIndex() const
{
    const QJsonArray arr = scenes();
    const QString id = m_s["activeSceneId"].toString();
    for (int i = 0; i < arr.size(); ++i)
        if (arr[i].toObject()["id"].toString() == id) return i;
    return arr.isEmpty() ? -1 : 0;
}

QJsonObject MainWindow::activeScene() const
{
    const int i = activeSceneIndex();
    return i >= 0 ? scenes()[i].toObject() : QJsonObject{};
}

void MainWindow::setActiveScene(const QJsonObject &scene)
{
    QJsonArray arr = scenes();
    const int i = activeSceneIndex();
    if (i < 0) return;
    arr[i] = scene;
    m_s["scenes"] = arr;
}

QJsonObject MainWindow::sourceById(const QString &id) const
{
    for (const auto &v : sources())
        if (v.toObject()["id"].toString() == id) return v.toObject();
    return {};
}

void MainWindow::setSource(const QJsonObject &src)
{
    QJsonArray arr = sources();
    for (int i = 0; i < arr.size(); ++i)
        if (arr[i].toObject()["id"] == src["id"]) { arr[i] = src; break; }
    m_s["sources"] = arr;
}

QJsonObject MainWindow::itemById(const QString &id) const
{
    if (id.isEmpty()) return {};
    for (const auto &v : activeScene()["items"].toArray())
        if (v.toObject()["id"].toString() == id) return v.toObject();
    return {};
}

void MainWindow::setItem(const QJsonObject &item)
{
    QJsonObject sc = activeScene();
    QJsonArray items = sc["items"].toArray();
    for (int i = 0; i < items.size(); ++i)
        if (items[i].toObject()["id"] == item["id"]) { items[i] = item; break; }
    sc["items"] = items;
    setActiveScene(sc);
}

void MainWindow::editSource(const std::function<void(QJsonObject &)> &fn)
{
    QJsonObject s = selectedSource();
    if (s.isEmpty()) return;
    fn(s);
    setSource(s);
    uploadRaster(s);
    applySoon();
}

void MainWindow::editItem(const std::function<void(QJsonObject &)> &fn)
{
    QJsonObject i = selectedItem();
    if (i.isEmpty()) return;
    fn(i);
    setItem(i);
    applySoon();
    updateSelection();
}

void MainWindow::applySoon() { m_applyTimer->start(); }

void MainWindow::pushSettings()
{
    m_applyTimer->stop();
    const auto r = EngineHost::instance().callObj("setSettings", m_s);
    if (r.contains("error")) m_stMsg->setText("Settings error: " + r["error"].toString());
}

// =============================== views ===============================

void MainWindow::rebuildScenes()
{
    m_loading = true;
    m_sceneList->clear();
    for (const auto &v : scenes()) {
        auto *it = new QListWidgetItem(v.toObject()["name"].toString(), m_sceneList);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        it->setSizeHint(QSize(0, 32));
    }
    m_sceneList->setCurrentRow(activeSceneIndex());
    m_loading = false;
}

void MainWindow::rebuildSourceList()
{
    m_loading = true;
    m_sourceList->clear();
    const QJsonArray items = activeScene()["items"].toArray();
    QListWidgetItem *sel = nullptr;
    // list order: top layer first (like OBS)
    for (int i = items.size() - 1; i >= 0; --i) {
        const auto item = items[i].toObject();
        const auto src = sourceById(item["sourceId"].toString());
        auto *it = new QListWidgetItem(themedIcon(sourceTypeInfo(src["type"].toString()).icon), src["name"].toString(), m_sourceList);
        it->setData(Qt::UserRole, item["id"].toString());
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(item["visible"].toBool(true) ? Qt::Checked : Qt::Unchecked);
        if (sourceTypeInfo(src["type"].toString()).audio) it->setToolTip("Eye icon enables or mutes this audio source in the scene.");
        it->setSizeHint(QSize(0, 34));
        if (item["id"].toString() == m_selItem) sel = it;
    }
    if (sel) m_sourceList->setCurrentItem(sel);
    else m_selItem.clear();
    m_loading = false;
    const bool has = !m_selItem.isEmpty();
    m_btnRemove->setEnabled(has);
    m_btnUp->setEnabled(has);
    m_btnDown->setEnabled(has);
    updateSelection();
}

void MainWindow::selectItem(const QString &itemId)
{
    for (int r = 0; r < m_sourceList->count(); ++r)
        if (!itemId.isEmpty() && m_sourceList->item(r)->data(Qt::UserRole).toString() == itemId) { m_sourceList->setCurrentRow(r); return; }
    m_loading = true;
    m_sourceList->setCurrentRow(-1);
    m_loading = false;
    m_selItem.clear();
    loadProperties();
    updateSelection();
}

void MainWindow::loadProperties()
{
    const QJsonObject item = selectedItem();
    const QJsonObject src = sourceById(item["sourceId"].toString());
    const bool has = !src.isEmpty();
    m_btnRemove->setEnabled(has);
    m_btnUp->setEnabled(has);
    m_btnDown->setEnabled(has);
    if (!has) { m_propsStack->setCurrentWidget(m_propsEmpty); return; }

    m_loading = true;
    m_propsStack->setCurrentWidget(m_propsBody);
    const QString type = src["type"].toString();
    const auto info = sourceTypeInfo(type);
    m_propsIcon->setPixmap(themedIcon(info.icon).pixmap(20, 20));
    m_srcName->setText(src["name"].toString());
    m_srcName->setToolTip(info.name + " — click to rename");

    static const QHash<QString, int> pageOf{{"Screen", PgScreen}, {"Window", PgWindow}, {"Camera", PgCamera}, {"Image", PgImage},
                                            {"Text", PgText}, {"Color", PgColor}, {"Media", PgMedia}, {"ChatOverlay", PgChatOverlay}, {"Game", PgWindow},
                                            {"AudioApplication", PgApplicationAudio}, {"AudioInput", PgAudioDevice}, {"AudioOutput", PgAudioDevice}};
    m_pages->setCurrentIndex(pageOf.value(type, PgNone));
    // Match stacked widget height to the active page instead of the tallest one (avoids blank space)
    for (int i = 0; i < m_pages->count(); ++i)
        m_pages->widget(i)->setSizePolicy(QSizePolicy::Preferred, i == m_pages->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    m_pages->adjustSize();

    if (type == "Screen") {
        m_screenCombo->clear();
        const QString captureId = src["captureId"].toString();
        int primary = 0;
        for (const auto &v : m_captureList) {
            const auto c = v.toObject();
            if (c["kind"].toString() != "Monitor") continue;
            const auto label = c["label"].toString();
            if (c["isPrimary"].toBool()) primary = m_screenCombo->count();
            m_screenCombo->addItem(themedIcon("monitor"), label, c["id"].toString());
        }
        int index = captureId.isEmpty() ? primary : m_screenCombo->findData(captureId);
        if (!captureId.isEmpty() && index < 0) {
            index = m_screenCombo->count();
            m_screenCombo->addItem("Display unavailable (" + captureId + ")", captureId);
        }
        m_screenCombo->setPlaceholderText("No displays available");
        m_screenCombo->setCurrentIndex(m_screenCombo->count() > 0 ? index : -1);
        m_screenCombo->setToolTip(m_screenCombo->currentText());
        m_screenCursor->setChecked(src["captureCursor"].toBool(true));
    } else if (type == "Window" || type == "Game") {
        m_windowCombo->clear();
        m_windowCombo->addItem(themedIcon("window"), "— select window —", "");
        int sel = 0;
        const QString cur = src["captureId"].toString();
        for (const auto &v : m_captureList) {
            const auto c = v.toObject();
            if (c["kind"].toString() != "Window") continue;
            m_windowCombo->addItem(themedIcon("window"), c["label"].toString(), c["id"].toString());
            if (c["id"].toString() == cur) sel = m_windowCombo->count() - 1;
        }
        if (sel == 0 && !cur.isEmpty()) {
            m_windowCombo->addItem(themedIcon("window"), cur.section('|', 1) + " (closed)", cur);
            sel = m_windowCombo->count() - 1;
        }
        m_windowCombo->setCurrentIndex(sel);
        const bool game = type == "Game";
        m_captureMethod->setCurrentIndex(src["captureFromDesktop"].toBool() ? 1 : 0);
        m_gameMode->setVisible(game);
        m_gameMode->setCurrentIndex(game && src["gameMode"].toString("Fullscreen") == "Fullscreen" ? 0 : 1);
        m_windowTarget->setVisible(!game || m_gameMode->currentIndex() == 1);
        m_windowPriority->setVisible(!game || m_gameMode->currentIndex() == 1);
        m_windowPriority->setCurrentIndex(src["matchWindowTitle"].toBool() ? 1 : 0);
        m_windowCursor->setChecked(src["captureCursor"].toBool(true));
    } else if (type == "AudioApplication") {
        populateApps(m_applicationCombo, src["deviceId"].toString());
    } else if (type == "AudioInput" || type == "AudioOutput") {
        populateAudioDeviceChoice();
    } else if (type == "Camera") {
        m_camDevice->clear();
        m_camDevice->addItem("— select camera —", "");
        int sel = 0;
        for (const auto &v : m_cameras) {
            const QString n = v.toObject()["name"].toString();
            m_camDevice->addItem(themedIcon("camera"), n, n);
            if (n == src["cameraName"].toString()) sel = m_camDevice->count() - 1;
        }
        m_camDevice->setCurrentIndex(sel);
        fillCameraModes(src["cameraName"].toString());
    } else if (type == "Image") {
        m_imagePath->setText(src["filePath"].toString());
    } else if (type == "ChatOverlay") {
        m_chatShowMessages->setChecked(src["showChat"].toBool(true));
        m_chatShowGifts->setChecked(src["showGifts"].toBool(true));
        m_chatShowTaps->setChecked(src["showTaps"].toBool(true));
        m_chatDemoBtn->setChecked(m_liveChat && m_liveChat->demoPreview());
    } else if (type == "Text") {
        m_text->setPlainText(src["text"].toString());
        m_font->setCurrentFont(QFont(src["fontName"].toString("Segoe UI")));
        m_fontSize->setValue(int(src["fontSize"].toDouble(72)));
        m_bold->setChecked(src["bold"].toBool(true));
        m_textColor->setColor(argbOf(src["colorArgb"]));
        m_outlineColor->setColor(argbOf(src["outlineArgb"], Qt::black));
        m_outlineW->setValue(int(src["outlineWidth"].toDouble(6)));
        m_bgColor->setColor(argbOf(src["backgroundArgb"], Qt::transparent));
    } else if (type == "Color") {
        m_fillColor->setColor(argbOf(src["colorArgb"]));
    } else if (type == "Media") {
        m_mediaPath->setText(src["filePath"].toString());
        m_mediaLoop->setChecked(src["loop"].toBool(true));
    }

    m_transform->setVisible(!info.audio);
    m_fit->setCurrentIndex(std::max(0, m_fit->findData(item["fit"].toString("Free"))));
    m_cropRows->setVisible(croppable(type));
    m_zoom->setValue(qRound(item["zoom"].toDouble(1) * 100));
    m_panX->setValue(qRound(item["panX"].toDouble() * 100));
    m_panY->setValue(qRound(item["panY"].toDouble() * 100));
    m_mirror->setChecked(item["mirror"].toBool());
    m_corner->setValue(qRound(item["corner"].toDouble() * 100));
    m_opacity->setValue(qRound(item["opacity"].toDouble(1) * 100));

    updateSourceStatus();
    m_loading = false;
}

void MainWindow::rebuildMixer()
{
    QStringList sig;
    QHash<QString, bool> sceneAudio;
    for (const auto &v : activeScene()["items"].toArray()) {
        const auto item = v.toObject();
        const auto id = item["sourceId"].toString();
        sceneAudio[id] = sceneAudio.value(id) || item["visible"].toBool(true);
    }
    QList<QJsonObject> audio;
    for (const auto &v : sources()) {
        const auto s = v.toObject();
        const QString t = s["type"].toString();
        const auto id = s["id"].toString();
        const bool sceneDevice = sourceTypeInfo(t).audio && !s["globalAudio"].toBool();
        if (sceneDevice && !sceneAudio.contains(id)) continue;
        if (sourceTypeInfo(t).audio || t == "Media") {
            audio << s;
            sig << s["id"].toString() + s["name"].toString() + s["deviceId"].toString() + (s["muted"].toBool() ? "m" : "")
                + (sceneDevice && !sceneAudio.value(id) ? "h" : "");
        }
    }
    const QString signature = sig.join(';');
    if (signature == m_mixerSignature && !m_strips.isEmpty()) return;
    m_mixerSignature = signature;
    m_mixerLayout->setDirection(m_mixerVertical ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);

    while (m_mixerLayout->count() > 1) {
        auto *it = m_mixerLayout->takeAt(0);
        if (auto *w = it->widget()) w->deleteLater();
        delete it;
    }
    m_strips.clear();

    for (const auto &s : audio) {
        const QString id = s["id"].toString();
        const QString type = s["type"].toString();
        auto *w = new QFrame;
        w->setObjectName("channel");
        auto *v = new QVBoxLayout(w);
        v->setContentsMargins(7, 4, 7, 4);
        v->setSpacing(3);

        auto *head = new QHBoxLayout();
        auto *ic = new QLabel;
        ic->setPixmap(themedIcon(sourceTypeInfo(type).icon).pixmap(16, 16));
        QString channelName = s["name"].toString();
        const auto application = s["deviceId"].toString();
        if (type == "AudioApplication" && !application.isEmpty() && !channelName.contains(application, Qt::CaseInsensitive))
            channelName += " · " + application;
        auto *name = new QLabel(channelName);
        name->setWordWrap(true);
        name->setObjectName("channelName");
        if (m_mixerVertical) {
            w->setFixedWidth(118);
            name->setText(name->fontMetrics().elidedText(channelName, Qt::ElideRight, 100));
            name->setToolTip(channelName);
            name->setAlignment(Qt::AlignCenter);
        }
        auto *db = new QLabel("-inf dB");
        db->setObjectName("channelDb");
        auto *gear = toolBtn("dots", "Options");
        gear->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(gear);
        menu->addAction("Filters…", this, [this, id] { openAudioFiltersDialog(id); });
        menu->addSeparator();
        w->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(w, &QWidget::customContextMenuRequested, this, [w, menu](const QPoint &pos) {
            menu->popup(w->mapToGlobal(pos));
        });
        if (type == "AudioInput" || type == "AudioOutput") {
            auto *devMenu = menu->addMenu(themedIcon(type == "AudioInput" ? "mic" : "speaker"), "Device");
            auto *group = new QActionGroup(devMenu);
            const QJsonArray devs = m_audioDevices[type == "AudioInput" ? "inputs" : "outputs"].toArray();
            auto addDev = [&](const QString &label, const QString &devId) {
                auto *a = devMenu->addAction(label);
                a->setCheckable(true);
                a->setChecked(s["deviceId"].toString() == devId);
                group->addAction(a);
                connect(a, &QAction::triggered, this, [this, id, devId] {
                    QJsonObject src = sourceById(id);
                    src["deviceId"] = devId;
                    setSource(src);
                    applySoon();
                    QTimer::singleShot(0, this, &MainWindow::rebuildMixer);
                });
            };
            addDev("System default device", "");
            for (const auto &d : devs) addDev(d.toObject()["name"].toString(), d.toObject()["id"].toString());
        }
        menu->addAction("Rename…", this, [this, id] {
            QJsonObject src = sourceById(id);
            bool ok = false;
            const QString n = QInputDialog::getText(this, "Rename", "Name:", QLineEdit::Normal, src["name"].toString(), &ok).trimmed();
            if (!ok || n.isEmpty()) return;
            src["name"] = n;
            setSource(src);
            applySoon();
            rebuildSourceList();
            QTimer::singleShot(0, this, &MainWindow::rebuildMixer);
        });
        if (s["globalAudio"].toBool()) {
            menu->addSeparator();
            menu->addAction(themedIcon("trash"), "Remove Audio Source", this, [this, id] {
                QJsonArray arr = sources();
                for (int i = 0; i < arr.size(); ++i)
                    if (arr[i].toObject()["id"].toString() == id) { arr.removeAt(i); break; }
                m_s["sources"] = arr;
                applySoon();
                QTimer::singleShot(0, this, &MainWindow::rebuildMixer);
            });
        }
        gear->setMenu(menu);
        head->addWidget(ic);
        if (m_mixerVertical) v->addWidget(name);
        else head->addWidget(name, 1);
        head->addWidget(db);
        head->addWidget(gear);
        v->addLayout(head);

        auto *meter = new MeterWidget(nullptr, m_mixerVertical ? Qt::Vertical : Qt::Horizontal);
        if (!m_mixerVertical) {
            meter->setFixedHeight(10);
            v->addWidget(meter);
        }

        auto *row = new QHBoxLayout();
        auto *mute = toolBtn(s["muted"].toBool() ? "speaker-off" : "speaker", "Mute");
        mute->setCheckable(true);
        mute->setChecked(s["muted"].toBool());
        const bool hiddenInScene = sourceTypeInfo(type).audio && !s["globalAudio"].toBool() && !sceneAudio.value(id);
        meter->setMuted(mute->isChecked() || hiddenInScene);
        auto *slider = hslider(0, 200);
        if (m_mixerVertical) {
            slider->setOrientation(Qt::Vertical);
            auto *levels = new QHBoxLayout;
            levels->addStretch();
            levels->addWidget(meter);
            levels->addWidget(slider);
            levels->addStretch();
            v->addLayout(levels, 1);
        }
        slider->setObjectName("volume");
        slider->setValue(qRound(s["volume"].toDouble(1) * 100));
        auto *pct = new QLabel(QString("%1%").arg(slider->value()));
        pct->setObjectName("channelPct");
        pct->setFixedWidth(40);
        pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(mute);
        if (!m_mixerVertical) row->addWidget(slider, 1);
        else row->addStretch(1);
        row->addWidget(pct);
        v->addLayout(row);

        connect(slider, &QSlider::valueChanged, this, [this, id, pct](int val) {
            pct->setText(QString("%1%").arg(val));
            QJsonObject src = sourceById(id);
            src["volume"] = val / 100.0;
            setSource(src);
            applySoon();
        });
        connect(mute, &QToolButton::toggled, this, [this, id, mute, meter, hiddenInScene](bool m) {
            mute->setIcon(themedIcon(m ? "speaker-off" : "speaker"));
            meter->setMuted(m || hiddenInScene);
            QJsonObject src = sourceById(id);
            src["muted"] = m;
            setSource(src);
            applySoon();
        });
        m_mixerLayout->insertWidget(m_mixerLayout->count() - 1, w);
        m_strips[id] = Strip{meter, db, mute, slider};
    }
    if (audio.isEmpty()) {
        auto *l = hintLabel("No audio sources. Add a microphone or desktop audio via \"Add Source\" or in Settings → Audio.");
        l->setAlignment(Qt::AlignCenter);
        m_mixerLayout->insertWidget(0, l);
    }
}

void MainWindow::refreshCaptureList()
{
    m_captureList = EngineHost::instance().callArr("listSources");
    loadProperties();
}

void MainWindow::refreshCameras()
{
    std::thread([this] {
        const QJsonArray cams = EngineHost::instance().callArr("listCameras");
        QMetaObject::invokeMethod(this, [this, cams] {
            m_cameras = cams;
            if (selectedSource()["type"].toString() == "Camera") loadProperties();
        });
    }).detach();
}

void MainWindow::fillCameraModes(const QString &cameraName)
{
    const bool was = m_loading;
    m_loading = true;
    m_camMode->clear();
    const QJsonObject src = selectedSource();
    for (const auto &v : m_cameras) {
        const auto c = v.toObject();
        if (c["name"].toString() != cameraName) continue;
        for (const auto &mv : c["modes"].toArray()) {
            const auto m = mv.toObject();
            m_camMode->addItem(m["label"].toString(), m.toVariantMap());
            if (m["width"] == src["cameraWidth"] && m["height"] == src["cameraHeight"] && m["fps"] == src["cameraFps"] && m["format"] == src["cameraFormat"])
                m_camMode->setCurrentIndex(m_camMode->count() - 1);
        }
    }
    if (m_camMode->count() == 0) m_camMode->addItem(m_cameras.isEmpty() ? "Scanning cameras…" : "—");
    m_camMode->setEnabled(!m_camMode->itemData(0).isNull());
    m_loading = was;
}

// =============================== actions ===============================

void MainWindow::addSourceDialog()
{
    AddSourceDialog dlg(sources(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const auto r = dlg.result();
    if (r.type.isEmpty()) return;
    QJsonObject options;
    if (r.existingId.isEmpty() && (r.type == "Game" || r.type == "Window" || r.type == "AudioApplication")) {
        refreshCaptureList();
        if (!newCaptureOptions(this, r.type, m_captureList, options)) return;
    }
    const auto gameAudioProcess = options.take("gameAudioProcess").toString();
    const QString itemId = addSource(r.type, r.name, r.existingId, r.visible, options);
    if (!gameAudioProcess.isEmpty())
        addSource("AudioApplication", r.name + " — audio", {}, r.visible, {{"deviceId", gameAudioProcess}});
    rebuildSourceList();
    rebuildMixer();
    if (!itemId.isEmpty()) selectItem(itemId);
}

QString MainWindow::addSource(const QString &type, const QString &name, const QString &existingId, bool visible, const QJsonObject &options)
{
    pushUndo();
    QString sourceId = existingId;
    if (sourceId.isEmpty()) {
        QJsonObject src{{"id", newId()}, {"type", type}, {"name", name}, {"volume", 1.0}, {"muted", false}, {"loop", true}, {"captureCursor", true}};
        if (type == "Camera" && !m_cameras.isEmpty()) {
            const auto cam = m_cameras.first().toObject();
            src["cameraName"] = cam["name"];
            QJsonObject best;
            for (const auto &mv : cam["modes"].toArray()) {
                const auto m = mv.toObject();
                if (best.isEmpty()) best = m;
                if (m["width"].toInt() == 1920 && m["fps"].toInt() == 60) { best = m; break; }
            }
            if (!best.isEmpty()) {
                src["cameraWidth"] = best["width"]; src["cameraHeight"] = best["height"];
                src["cameraFps"] = best["fps"]; src["cameraFormat"] = best["format"];
            }
        }
        if (type == "Text") {
            src["text"] = name.startsWith("Text") ? "My Stream!" : name;
            src["fontName"] = "Segoe UI"; src["fontSize"] = 72; src["bold"] = true;
            src["colorArgb"] = argbVal(Qt::white); src["outlineArgb"] = argbVal(Qt::black); src["outlineWidth"] = 6; src["backgroundArgb"] = 0;
        }
        if (type == "Color") src["colorArgb"] = argbVal(QColor(0, 0, 0, 160));
        if (type == "Image" || type == "Media") {
            const QString filter = type == "Image" ? "Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp)"
                                                   : "Video (*.mp4 *.mkv *.mov *.webm *.avi *.gif *.flv);;All Files (*.*)";
            const QString p = QFileDialog::getOpenFileName(this, type == "Image" ? "Select Image" : "Select Video File", {}, filter);
            if (!p.isEmpty()) src["filePath"] = QDir::toNativeSeparators(p);
        }
        for (auto it = options.constBegin(); it != options.constEnd(); ++it) src[it.key()] = it.value();
        if (sourceTypeInfo(type).audio) src["globalAudio"] = false;
        QJsonArray arr = sources();
        arr.append(src);
        m_s["sources"] = arr;
        sourceId = src["id"].toString();
    }
    QJsonObject item{{"id", newId()}, {"sourceId", sourceId}, {"visible", visible}, {"fit", "Free"},
                     {"zoom", 1.0}, {"panX", 0.0}, {"panY", 0.0}, {"mirror", false}, {"corner", 0.0}, {"opacity", 1.0}};
    const double cw = m_canvasW, ch = m_canvasH;
    if (sourceTypeInfo(type).audio) {
        item["w"] = 0.0; item["h"] = 0.0;
    } else if (type == "Screen" || type == "Window" || type == "Game") {
        item["fit"] = "FitBlur";
        item["x"] = 0.0; item["y"] = 0.0; item["w"] = 1.0; item["h"] = 1.0;
    } else if (type == "Camera") {
        const double w = 0.42, h = w * cw / ch * 9.0 / 16.0;
        item["x"] = 1 - w - 0.03; item["y"] = 0.03; item["w"] = w; item["h"] = h;
        item["corner"] = 0.12; item["mirror"] = true;
    } else if (type == "Media") {
        const double h = cw * 9.0 / 16.0 / ch;
        item["x"] = 0.0; item["y"] = (1 - h) / 2; item["w"] = 1.0; item["h"] = h;
    } else if (type == "Color") {
        item["x"] = 0.0; item["y"] = 0.0; item["w"] = 1.0; item["h"] = 1.0;
    } else if (m_rasterSizes.contains(sourceId)) {
        const QSize sz = m_rasterSizes.value(sourceId);
        const double scale = std::min(1.0, 0.8 * cw / sz.width());
        const double w = sz.width() * scale / cw, h = sz.height() * scale / ch;
        item["x"] = (1 - w) / 2; item["y"] = 0.42; item["w"] = w; item["h"] = h;
    } else {
        // image/text: size determined after rasterization (w = 0 -> automatic)
        item["x"] = 0.1; item["y"] = 0.42; item["w"] = 0.0; item["h"] = 0.0;
    }
    QJsonObject sc = activeScene();
    QJsonArray items = sc["items"].toArray();
    items.append(item);
    sc["items"] = items;
    setActiveScene(sc);
    uploadRaster(sourceById(sourceId));
    applySoon();
    return item["id"].toString();
}

void MainWindow::removeSelectedItem()
{
    const QJsonObject item = selectedItem();
    if (item.isEmpty()) return;
    pushUndo();
    const QString sourceId = item["sourceId"].toString();
    QJsonObject sc = activeScene();
    QJsonArray items = sc["items"].toArray();
    for (int i = 0; i < items.size(); ++i)
        if (items[i].toObject()["id"] == item["id"]) { items.removeAt(i); break; }
    sc["items"] = items;
    setActiveScene(sc);

    // source unused in any scene -> remove completely
    bool used = false;
    for (const auto &s : scenes())
        for (const auto &it : s.toObject()["items"].toArray())
            if (it.toObject()["sourceId"].toString() == sourceId) used = true;
    if (!used) {
        QJsonArray arr = sources();
        for (int i = 0; i < arr.size(); ++i)
            if (arr[i].toObject()["id"].toString() == sourceId) { arr.removeAt(i); break; }
        m_s["sources"] = arr;
        m_rasterKeys.remove(sourceId);
        m_rasterSizes.remove(sourceId);
    }
    m_selItem.clear();
    rebuildSourceList();
    rebuildMixer();
    loadProperties();
    applySoon();
}

void MainWindow::moveSelectedItem(int dir)
{
    QJsonObject sc = activeScene();
    QJsonArray items = sc["items"].toArray();
    int i = -1;
    for (int k = 0; k < items.size(); ++k)
        if (items[k].toObject()["id"].toString() == m_selItem) i = k;
    const int j = i + dir;
    if (i < 0 || j < 0 || j >= items.size()) return;
    pushUndo();
    const QJsonValue item = items.at(i);
    items[i] = items.at(j);
    items[j] = item;
    sc["items"] = items;
    setActiveScene(sc);
    rebuildSourceList();
    applySoon();
}

void MainWindow::addScene()
{
    QJsonArray arr = scenes();
    QJsonObject sc{{"id", newId()}, {"name", QString("Scene %1").arg(arr.size() + 1)}, {"items", QJsonArray{}}};
    arr.append(sc);
    m_s["scenes"] = arr;
    m_s["activeSceneId"] = sc["id"];
    m_selItem.clear();
    rebuildScenes();
    rebuildSourceList();
    rebuildMixer();
    loadProperties();
    applySoon();
}

void MainWindow::removeScene()
{
    QJsonArray arr = scenes();
    if (arr.size() <= 1) { QMessageBox::information(this, "Scenes", "At least one scene must remain."); return; }
    const int i = activeSceneIndex();
    if (QMessageBox::question(this, "Remove Scene", QString("Remove scene \"%1\"?").arg(arr[i].toObject()["name"].toString())) != QMessageBox::Yes) return;
    arr.removeAt(i);
    m_s["scenes"] = arr;
    QSet<QString> used;
    for (const auto &s : arr)
        for (const auto &it : s.toObject()["items"].toArray()) used << it.toObject()["sourceId"].toString();
    QJsonArray src;
    for (const auto &v : sources()) {
        const auto s = v.toObject();
        if (s["globalAudio"].toBool() || used.contains(s["id"].toString())) src.append(s);
    }
    m_s["sources"] = src;
    m_s["activeSceneId"] = arr[std::min(i, int(arr.size()) - 1)].toObject()["id"];
    m_selItem.clear();
    rebuildScenes();
    rebuildSourceList();
    rebuildMixer();
    loadProperties();
    applySoon();
}

void MainWindow::moveScene(int dir)
{
    QJsonArray arr = scenes();
    const int i = activeSceneIndex(), j = i + dir;
    if (i < 0 || j < 0 || j >= arr.size()) return;
    const QJsonValue scene = arr.at(i);
    arr[i] = arr.at(j);
    arr[j] = scene;
    m_s["scenes"] = arr;
    rebuildScenes();
    applySoon();
}

void MainWindow::switchScene(int delta)
{
    const int n = scenes().size();
    if (n < 2) return;
    m_sceneList->setCurrentRow((activeSceneIndex() + delta + n) % n);
}

void MainWindow::updateChatOverlays()
{
    for (const auto &v : sources()) {
        const auto s = v.toObject();
        if (s["type"].toString() == "ChatOverlay") uploadRaster(s);
    }
}

void MainWindow::uploadRaster(const QJsonObject &src)
{
    const QString type = src["type"].toString();
    if (type != "Image" && type != "Text" && type != "ChatOverlay") return;
    const QString id = src["id"].toString();
    QString key;
    if (type == "ChatOverlay") {
        if (m_liveChat) m_liveChat->ensureStarted();
        key = QString("chat|%1|%2|%3|%4")
                  .arg(src["showChat"].toBool(true))
                  .arg(src["showGifts"].toBool(true))
                  .arg(src["showTaps"].toBool(true))
                  .arg(m_liveChat ? m_liveChat->overlayRevision() : 0);
    } else {
        key = rasterKey(src);
    }
    if (m_rasterKeys.value(id) == key && m_rasterSizes.contains(id)) { /* content unchanged */ }
    else {
        m_rasterKeys[id] = key;
        const QImage img = (type == "ChatOverlay" && m_liveChat)
            ? m_liveChat->renderOverlay(540, 720, src["showChat"].toBool(true), src["showGifts"].toBool(true), src["showTaps"].toBool(true))
            : rasterizeSource(src);
        const QSize oldSize = m_rasterSizes.value(id);
        if (img.isNull()) {
            m_rasterSizes.remove(id);
            EngineHost::instance().setSourcePixels(id, 0, 0, 0, nullptr);
            return;
        }
        m_rasterSizes[id] = img.size();
        EngineHost::instance().setSourcePixels(id, img.width(), img.height(), int(img.bytesPerLine()), img.constBits());
        if (oldSize == img.size()) return;
        m_rasterSizes[id + "#old"] = oldSize;
    }
    const QSize sz = m_rasterSizes.value(id);
    const QSize oldSize = m_rasterSizes.take(id + "#old");

    // adjust size of this source's items across all scenes (preserve pixel scale)
    QJsonArray arr = scenes();
    bool changed = false;
    for (int s = 0; s < arr.size(); ++s) {
        QJsonObject sc = arr[s].toObject();
        QJsonArray items = sc["items"].toArray();
        for (int k = 0; k < items.size(); ++k) {
            QJsonObject it = items[k].toObject();
            if (it["sourceId"].toString() != id || it["fit"].toString("Free") != "Free") continue;
            double scale;
            if (it["w"].toDouble() <= 0) {
                scale = std::min(1.0, 0.8 * m_canvasW / sz.width());
                const double w = sz.width() * scale / m_canvasW, h = sz.height() * scale / m_canvasH;
                it["x"] = (1 - w) / 2;
                it["y"] = std::clamp(it["y"].toDouble(0.42), 0.0, std::max(0.0, 1 - h));
            } else {
                if (oldSize.isEmpty()) continue;
                scale = it["w"].toDouble() * m_canvasW / oldSize.width();
            }
            it["w"] = sz.width() * scale / m_canvasW;
            it["h"] = sz.height() * scale / m_canvasH;
            items[k] = it;
            changed = true;
        }
        sc["items"] = items;
        arr[s] = sc;
    }
    if (changed) {
        m_s["scenes"] = arr;
        applySoon();
        updateSelection();
    }
}

void MainWindow::applyHotkeys()
{
    QHash<QString, QString> map;
    const QJsonObject hk = m_s["hotkeys"].toObject();
    for (auto it = hk.begin(); it != hk.end(); ++it) map[it.key()] = it.value().toString();
    const QStringList failed = m_hotkeys->apply(map);
    if (!failed.isEmpty()) m_stMsg->setText("Failed to register hotkeys: " + failed.join(", ") + " (in use by another app?)");
}

void MainWindow::triggerAction(const QString &action)
{
    if (action == "toggleStream") toggleStream();
    else if (action == "toggleRecord") toggleRecord();
    else if (action == "nextScene") switchScene(+1);
    else if (action == "prevScene") switchScene(-1);
    else if (action == "toggleMic") {
        QJsonArray arr = sources();
        bool anyUnmuted = false;
        for (const auto &v : arr)
            if (v.toObject()["type"].toString() == "AudioInput" && !v.toObject()["muted"].toBool()) anyUnmuted = true;
        for (int i = 0; i < arr.size(); ++i) {
            QJsonObject s = arr[i].toObject();
            if (s["type"].toString() == "AudioInput") { s["muted"] = anyUnmuted; arr[i] = s; }
        }
        m_s["sources"] = arr;
        m_mixerSignature.clear();
        rebuildMixer();
        applySoon();
        m_stMsg->setText(anyUnmuted ? "Microphone muted" : "Microphone unmuted");
    }
}

// =============================== preview ===============================

bool MainWindow::croppable(const QString &type) const
{
    return type == "Screen" || type == "Window" || type == "Game" || type == "Camera" || type == "Media";
}

QSizeF MainWindow::naturalSize(const QString &sourceId) const
{
    if (m_rasterSizes.contains(sourceId)) return m_rasterSizes.value(sourceId);
    const auto st = m_srcStats.value(sourceId);
    if (st["width"].toInt() > 1 && st["height"].toInt() > 1) return QSizeF(st["width"].toInt(), st["height"].toInt());
    return QSizeF(1920, 1080);
}

QRectF MainWindow::displayRect(const QJsonObject &item) const
{
    const QString fit = item["fit"].toString("Free");
    if (fit == "Fill") return QRectF(0, 0, 1, 1);
    if (fit == "FitBlur") {
        const QSizeF ns = naturalSize(item["sourceId"].toString());
        const double s = std::min(m_canvasW / ns.width(), m_canvasH / ns.height());
        const double w = ns.width() * s / m_canvasW, h = ns.height() * s / m_canvasH;
        return QRectF((1 - w) / 2, (1 - h) / 2, w, h);
    }
    return itemRect(item);
}

QJsonObject MainWindow::makeFree(QJsonObject item) const
{
    if (item.isEmpty() || item["fit"].toString("Free") == "Free") return item;
    const QRectF r = displayRect(item);
    item["fit"] = "Free";
    item["x"] = r.x(); item["y"] = r.y(); item["w"] = r.width(); item["h"] = r.height();
    return item;
}

void MainWindow::updateSourceStatus()
{
    const auto src = selectedSource();
    const auto st = m_srcStats.value(src["id"].toString());
    QString status = st.isEmpty() ? QString{} : "● " + st["status"].toString();
    const double captureFps = st["captureFps"].toDouble();
    if (src["type"].toString() == "Camera" && captureFps > 0 && captureFps < 0.8 * src["cameraFps"].toInt(60))
        status += "\nCamera is delivering fewer frames than configured. Check lighting and exposure time.";
    m_srcStatus->setWordWrap(true);
    m_srcStatus->setText(status);
}

void MainWindow::updateSelection()
{
    const QJsonObject item = selectedItem();
    const bool visual = !sourceTypeInfo(selectedSource()["type"].toString()).audio;
    EngineHost::instance().call("setSelection",
        !item.isEmpty() && visual && item["visible"].toBool(true) ? rectObj(displayRect(item)) : QJsonObject{});
}

double MainWindow::snap(double v, const QList<double> &targets, double tol, double *delta) const
{
    for (double t : targets)
        if (std::abs(v - t) < tol) { if (delta) *delta = t - v; return t; }
    if (delta) *delta = 0;
    return v;
}

void MainWindow::onCanvasPressed(QPointF p)
{
    m_drag = Drag::None;
    if (m_applyTimer->isActive()) pushSettings();
    // 10 px tolerance in preview pixels -> canvas units
    const double s = std::min(m_preview->width() / double(m_canvasW), m_preview->height() / double(m_canvasH));
    const double tolX = 10.0 / (m_canvasW * s), tolY = 10.0 / (m_canvasH * s);

    // 1. corner handle of the selected item
    const QJsonObject sel = selectedItem();
    if (!sel.isEmpty() && sel["visible"].toBool(true) && !sourceTypeInfo(selectedSource()["type"].toString()).audio) {
        const QRectF r = displayRect(sel);
        const QPointF corners[4] = {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()};
        const QPointF opposite[4] = {r.bottomRight(), r.bottomLeft(), r.topRight(), r.topLeft()};
        for (int k = 0; k < 4; ++k) {
            if (std::abs(p.x() - corners[k].x()) <= tolX && std::abs(p.y() - corners[k].y()) <= tolY) {
                pushUndo();
                m_drag = Drag::Resize;
                m_anchor = opposite[k];
                m_startRect = r;
                return;
            }
        }
    }
    // 2. item under cursor (top to bottom)
    const QJsonArray items = activeScene()["items"].toArray();
    for (int i = items.size() - 1; i >= 0; --i) {
        const auto it = items[i].toObject();
        if (!it["visible"].toBool(true) || sourceTypeInfo(sourceById(it["sourceId"].toString())["type"].toString()).audio) continue;
        const QRectF r = displayRect(it);
        if (!r.contains(p)) continue;
        if (it["id"].toString() != m_selItem) selectItem(it["id"].toString());
        pushUndo();
        m_drag = Drag::Move;
        m_dragStart = p;
        m_startRect = r;
        return;
    }
    selectItem({});
}

void MainWindow::onCanvasDragged(QPointF p)
{
    if (m_drag == Drag::None) return;
    QJsonObject item = makeFree(selectedItem());
    if (item.isEmpty()) return;
    const bool snapOn = m_s["snapping"].toBool(true);
    const double tol = 0.012;
    QRectF r = m_startRect;

    if (m_drag == Drag::Move) {
        r.moveTopLeft(m_startRect.topLeft() + (p - m_dragStart));
        if (snapOn) {
            double d;
            snap(r.left(), {0, 0.5}, tol, &d);
            if (d == 0) snap(r.right(), {1, 0.5}, tol, &d);
            if (d == 0) snap(r.center().x(), {0.5}, tol, &d);
            r.translate(d, 0);
            snap(r.top(), {0, 0.5}, tol, &d);
            if (d == 0) snap(r.bottom(), {1, 0.5}, tol, &d);
            if (d == 0) snap(r.center().y(), {0.5}, tol, &d);
            r.translate(0, d);
        }
    } else {
        QPointF c = p;
        if (snapOn) c = QPointF(snap(c.x(), {0, 0.5, 1}, tol), snap(c.y(), {0, 0.5, 1}, tol));
        double w = std::abs(c.x() - m_anchor.x()), h = std::abs(c.y() - m_anchor.y());
        if (!(QApplication::keyboardModifiers() & Qt::ShiftModifier) && m_startRect.height() > 0) {
            const double aspect = m_startRect.width() / m_startRect.height();
            if (w / std::max(h, 1e-6) > aspect) h = w / aspect; else w = h * aspect;
        }
        w = std::max(w, 0.02);
        h = std::max(h, 0.02);
        const double x = c.x() < m_anchor.x() ? m_anchor.x() - w : m_anchor.x();
        const double y = c.y() < m_anchor.y() ? m_anchor.y() - h : m_anchor.y();
        r = QRectF(x, y, w, h);
    }
    item["x"] = r.x(); item["y"] = r.y(); item["w"] = r.width(); item["h"] = r.height();
    setItem(item);
    m_dragChanged = true;
    if (!m_dragTimer->isActive()) {
        sendDragTransform();
        m_dragTimer->start();
    }
    if (m_fit->currentData().toString() != "Free") { m_loading = true; m_fit->setCurrentIndex(0); m_loading = false; }
}

void MainWindow::sendDragTransform()
{
    const auto item = selectedItem();
    if (m_drag == Drag::None || item.isEmpty()) return;
    auto args = rectObj(itemRect(item));
    args["sceneId"] = activeScene()["id"];
    args["itemId"] = item["id"];
    args["fit"] = item["fit"];
    EngineHost::instance().call("setItemTransform", args);
}

void MainWindow::pushUndo()
{
    if (m_s.isEmpty()) return;
    if (!m_undoStack.isEmpty() && m_undoStack.last() == m_s) return;
    m_undoStack.append(m_s);
    if (m_undoStack.size() > 50) m_undoStack.removeFirst();
    m_redoStack.clear();
}

void MainWindow::undo()
{
    if (m_undoStack.isEmpty()) return;
    m_redoStack.append(m_s);
    m_s = m_undoStack.takeLast();
    rebuildScenes();
    rebuildSourceList();
    rebuildMixer();
    loadProperties();
    updateSelection();
    for (const auto &v : sources()) uploadRaster(v.toObject());
    applySoon();
}

void MainWindow::redo()
{
    if (m_redoStack.isEmpty()) return;
    m_undoStack.append(m_s);
    m_s = m_redoStack.takeLast();
    rebuildScenes();
    rebuildSourceList();
    rebuildMixer();
    loadProperties();
    updateSelection();
    for (const auto &v : sources()) uploadRaster(v.toObject());
    applySoon();
}

void MainWindow::copySelectedItem()
{
    const QJsonObject item = selectedItem();
    if (item.isEmpty()) return;
    m_clipboardItem = item;
    m_clipboardSource = sourceById(item["sourceId"].toString());
}

void MainWindow::pasteItem()
{
    if (m_clipboardItem.isEmpty() || m_clipboardSource.isEmpty()) return;
    pushUndo();
    QJsonObject src = m_clipboardSource;
    const QString origId = src["id"].toString();
    // If the source no longer exists in definitions (e.g. deleted), recreate it with a new ID
    QString sourceId = origId;
    if (sourceById(origId).isEmpty()) {
        src["id"] = newId();
        sourceId = src["id"].toString();
        QJsonArray arr = sources();
        arr.append(src);
        m_s["sources"] = arr;
    }
    QJsonObject it = m_clipboardItem;
    it["id"] = newId();
    it["sourceId"] = sourceId;
    if (it["fit"].toString("Free") == "Free") {
        it["x"] = std::min(0.95, it["x"].toDouble() + 0.02);
        it["y"] = std::min(0.95, it["y"].toDouble() + 0.02);
    }
    QJsonObject sc = activeScene();
    QJsonArray items = sc["items"].toArray();
    items.append(it);
    sc["items"] = items;
    setActiveScene(sc);
    m_selItem = it["id"].toString();
    rebuildSourceList();
    rebuildMixer();
    loadProperties();
    uploadRaster(sourceById(sourceId));
    applySoon();
}

void MainWindow::duplicateSelectedItem()
{
    copySelectedItem();
    pasteItem();
}

void MainWindow::nudgeSelectedItem(int dxPx, int dyPx)
{
    if (m_selItem.isEmpty() || sourceTypeInfo(selectedSource()["type"].toString()).audio) return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastUndoMs > 800) { pushUndo(); m_lastUndoMs = nowMs; }
    editItem([this, dxPx, dyPx](QJsonObject &i) {
        i = makeFree(i);
        i["x"] = i["x"].toDouble() + double(dxPx) / std::max(1, m_canvasW);
        i["y"] = i["y"].toDouble() + double(dyPx) / std::max(1, m_canvasH);
    });
    updateSelection();
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    const auto *fw = QApplication::focusWidget();
    const bool typing = qobject_cast<const QLineEdit *>(fw) || qobject_cast<const QPlainTextEdit *>(fw)
        || qobject_cast<const QKeySequenceEdit *>(fw);
    if (!typing) {
        const auto mods = e->modifiers();
        if (e->matches(QKeySequence::Undo)) { undo(); return; }
        if (e->matches(QKeySequence::Redo) || (mods == (Qt::ControlModifier | Qt::ShiftModifier) && e->key() == Qt::Key_Z)) { redo(); return; }
        if (e->matches(QKeySequence::Copy)) { copySelectedItem(); return; }
        if (e->matches(QKeySequence::Paste)) { pasteItem(); return; }
        if (mods == Qt::ControlModifier && e->key() == Qt::Key_D) { duplicateSelectedItem(); return; }
        if (e->key() == Qt::Key_Delete && !m_selItem.isEmpty()) { removeSelectedItem(); return; }
        if (e->key() == Qt::Key_F2 && !m_selItem.isEmpty()) { m_srcName->setFocus(); m_srcName->selectAll(); return; }
        if (mods == Qt::ControlModifier && e->key() == Qt::Key_Up) { moveSelectedItem(+1); return; }
        if (mods == Qt::ControlModifier && e->key() == Qt::Key_Down) { moveSelectedItem(-1); return; }
        const int step = (mods & Qt::ShiftModifier) ? 10 : 2;
        if (!(mods & Qt::ControlModifier)) {
            if (e->key() == Qt::Key_Left) { nudgeSelectedItem(-step, 0); return; }
            if (e->key() == Qt::Key_Right) { nudgeSelectedItem(+step, 0); return; }
            if (e->key() == Qt::Key_Up) { nudgeSelectedItem(0, -step); return; }
            if (e->key() == Qt::Key_Down) { nudgeSelectedItem(0, +step); return; }
        }
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::onCanvasWheel(int delta)
{
    if (m_selItem.isEmpty() || sourceTypeInfo(selectedSource()["type"].toString()).audio) return;
    const double f = delta > 0 ? 1.06 : 1 / 1.06;
    editItem([this, f](QJsonObject &i) {
        i = makeFree(i);
        QRectF r = itemRect(i);
        const QPointF c = r.center();
        r.setSize(QSizeF(std::max(0.02, r.width() * f), std::max(0.02, r.height() * f)));
        r.moveCenter(c);
        i["x"] = r.x(); i["y"] = r.y(); i["w"] = r.width(); i["h"] = r.height();
    });
    if (m_fit->currentData().toString() != "Free") { m_loading = true; m_fit->setCurrentIndex(0); m_loading = false; }
}

// =============================== live ===============================

void MainWindow::toggleStream()
{
    if (!m_streaming) {
        if (!prepareStream()) return;
    } else if (m_s["confirmStreamStop"].toBool(true) &&
               QMessageBox::question(this, "Streaming", "Stop streaming?") != QMessageBox::Yes) {
        return;
    }
    pushSettings();
    m_btnStream->setEnabled(false);
    const auto r = EngineHost::instance().callObj(m_streaming ? "stopStream" : "startStream");
    if (r.contains("error")) QMessageBox::warning(this, "Streaming", r["error"].toString());
    m_btnStream->setEnabled(true);
    pollStats();
}

bool MainWindow::prepareStream()
{
    QSettings draft("TikStream", "UI2");
    QDialog dialog(this);
    dialog.setWindowTitle("Prepare Stream");
    dialog.resize(540, 0);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);

    auto *title = new QLineEdit(draft.value("streamDraft/title").toString());
    auto *category = new QLineEdit(draft.value("streamDraft/category").toString());
    auto *description = new QPlainTextEdit(draft.value("streamDraft/description").toString());
    description->setMinimumHeight(100);
    auto addField = [&dialog, form](const QString &label, QWidget *field, auto readText) {
        auto *row = new QHBoxLayout;
        row->addWidget(field, 1);
        auto *copy = new QPushButton("Copy");
        copy->setAccessibleName("Copy \"" + label + "\"");
        copy->setAutoDefault(false);
        connect(copy, &QPushButton::clicked, &dialog, [readText] {
            QApplication::clipboard()->setText(readText());
        });
        row->addWidget(copy, 0, Qt::AlignTop);
        form->addRow(label, row);
    };
    addField("Title", title, [title] { return title->text(); });
    addField("Category or Game", category, [category] { return category->text(); });
    addField("Description", description, [description] { return description->toPlainText(); });
    layout->addWidget(hintLabel("This is a local draft. Set these details in your platform's dashboard before going live."));

    const bool hasDestination = !m_s["serverUrl"].toString().trimmed().isEmpty()
        && !m_s["streamKey"].toString().trimmed().isEmpty();
    if (!hasDestination)
        layout->addWidget(hintLabel("To start RTMP streaming, configure the server URL and stream key."));

    auto *buttons = new QDialogButtonBox;
    auto *start = buttons->addButton("Start Streaming", QDialogButtonBox::AcceptRole);
    auto *save = buttons->addButton("Save Draft", QDialogButtonBox::ActionRole);
    auto *settings = buttons->addButton("Stream Settings", QDialogButtonBox::ActionRole);
    auto *cancel = buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
    start->setEnabled(hasDestination);
    settings->setVisible(!hasDestination);
    cancel->setDefault(true);
    layout->addWidget(buttons);

    auto saveDraft = [&] {
        draft.setValue("streamDraft/title", title->text());
        draft.setValue("streamDraft/category", category->text());
        draft.setValue("streamDraft/description", description->toPlainText());
    };
    connect(start, &QPushButton::clicked, &dialog, [&] { saveDraft(); dialog.accept(); });
    connect(save, &QPushButton::clicked, &dialog, [&] { saveDraft(); dialog.reject(); });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    bool openDestinationSettings = false;
    connect(settings, &QPushButton::clicked, &dialog, [&] {
        saveDraft();
        openDestinationSettings = true;
        dialog.reject();
    });
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (openDestinationSettings) openSettings();
    return accepted;
}

void MainWindow::toggleRecord()
{
    m_btnRecord->setEnabled(false);
    pushSettings();
    const auto r = EngineHost::instance().callObj(m_recording ? "stopRecord" : "startRecord");
    if (r.contains("error")) QMessageBox::warning(this, "Recording", r["error"].toString());
    m_btnRecord->setEnabled(true);
    pollStats();
}

void MainWindow::openSettings()
{
    m_audioDevices = EngineHost::instance().callObj("listAudioDevices");
    SettingsDialog dlg(m_s, m_audioDevices, m_streaming || m_recording, this);
    connect(&dlg, &SettingsDialog::applied, this, [this](const QJsonObject &s) {
        m_s = s;
        pushSettings();
        applyHotkeys();
        m_previewStack->setCurrentIndex(m_s["previewEnabled"].toBool(true) ? 0 : 1);
        m_mixerSignature.clear();
        rebuildMixer();
        loadProperties();
    });
    dlg.exec();
}

void MainWindow::updateLiveButtons()
{
    m_btnStream->setText(m_streaming ? "  Stop Streaming" : "  Start Streaming");
    m_btnStream->setProperty("live", m_streaming);
    m_btnRecord->setText(m_recording ? "  Stop Recording" : "  Start Recording");
    m_btnRecord->setProperty("live", m_recording);
    for (auto *b : {m_btnStream, m_btnRecord}) { b->style()->unpolish(b); b->style()->polish(b); }
    m_stLive->setProperty("on", m_streaming);
    m_stRec->setProperty("on", m_recording);
    for (auto *l : {m_stLive, m_stRec}) { l->style()->unpolish(l); l->style()->polish(l); }
}

void MainWindow::pollStats()
{
    const auto st = EngineHost::instance().callObj("stats");
    if (st.isEmpty()) return;
    const bool s = st["streaming"].toBool(), r = st["recording"].toBool();
    if (s != m_streaming || r != m_recording) { m_streaming = s; m_recording = r; updateLiveButtons(); }

    const int cw = st["canvasWidth"].toInt(m_canvasW), ch = st["canvasHeight"].toInt(m_canvasH);
    if (cw > 0 && ch > 0 && (cw != m_canvasW || ch != m_canvasH)) {
        m_canvasW = cw;
        m_canvasH = ch;
        m_preview->setCanvasSize(cw, ch);
    }
    m_srcStats.clear();
    for (const auto &v : st["sources"].toArray()) m_srcStats[v.toObject()["id"].toString()] = v.toObject();

    m_stLive->setText("●  LIVE  " + fmtTime(st["streamSeconds"].toDouble()));
    m_stRec->setText("●  REC  " + fmtTime(st["recordSeconds"].toDouble()));
    m_stCpu->setText(QString("CPU: %1%").arg(st["cpuPercent"].toDouble(), 0, 'f', 1));
    m_stRender->setText(QString("Render: %1 ms").arg(st["renderMs"].toDouble(), 0, 'f', 2));
    m_stFps->setText(QString("%1 / %2 FPS").arg(st["renderFps"].toDouble(), 0, 'f', 2).arg(m_s["fps"].toInt(60)));
    double kbps = 0;
    qint64 dropped = 0;
    for (const auto &ov : st["outputs"].toArray()) {
        const auto o = ov.toObject();
        if (o["name"].toString() == "Stream") { kbps = o["kbps"].toDouble(); dropped = qint64(o["dropped"].toDouble()); }
    }
    m_stBitrate->setText(s ? QString("%1 kbps").arg(kbps, 0, 'f', 0) : "— kbps");
    m_stDropped->setText(QString("Dropped: %1").arg(dropped));

    updateSourceStatus();
    if (m_drag == Drag::None) updateSelection(); // FitBlur depends on source dimensions

    const QString err = st["lastError"].toString();
    if (!err.isEmpty()) {
        EngineHost::instance().call("clearError");
        m_stMsg->setText("⚠ " + err);
        QMessageBox::warning(this, "TikStream", err);
    } else {
        m_stMsg->setText(QString("Scene: %1  •  %2").arg(activeScene()["name"].toString(), m_gpu));
    }
}

void MainWindow::pollLevels()
{
    const auto l = EngineHost::instance().callObj("levels");
    for (auto it = m_strips.begin(); it != m_strips.end(); ++it) {
        const double v = it.value().mute->isChecked() ? 0 : l[it.key()].toDouble();
        it.value().meter->setLevel(float(v));
        const double db = v > 0.000001 ? 20 * std::log10(v) : -1000;
        it.value().db->setText(db < -60 ? "-inf dB" : QString("%1 dB").arg(db, 0, 'f', 1));
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if ((m_streaming || m_recording) &&
        QMessageBox::question(this, "TikStream", "Streaming or recording is active. Stop and exit?") != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    m_statsTimer->stop();
    m_levelsTimer->stop();
    pushSettings();
    QSettings qs("TikStream", "UI2");
    qs.setValue("geometry", saveGeometry());
    qs.setValue("state", saveState());
    qs.setValue("compactLayoutV1", true);
    e->accept();
}
