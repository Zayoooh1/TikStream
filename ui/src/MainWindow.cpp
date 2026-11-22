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
#include <QFileInfo>
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
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
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
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <memory>
#include <thread>
#include <utility>

namespace {
enum Page { PgScreen, PgWindow, PgCamera, PgImage, PgText, PgColor, PgMedia, PgBrowser, PgAlertBox, PgChatOverlay, PgApplicationAudio, PgAudioDevice, PgNone };

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
    f->setHorizontalSpacing(8);
    f->setVerticalSpacing(8);
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

bool newSourceOptions(QWidget *parent, const QString &type, const QJsonArray &captureList, const QJsonObject &audioDevices, QJsonObject &options)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(sourceTypeInfo(type).name);
    dialog.resize(type == "Browser" || type == "Text" ? 560 : 520, 0);
    auto *layout = new QVBoxLayout(&dialog);
    auto *f = new QFormLayout;
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setHorizontalSpacing(12);
    f->setVerticalSpacing(9);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(f);

    std::function<void()> collect;

    if (type == "Browser") {
        auto *urlRow = new QHBoxLayout;
        auto *url = new QLineEdit;
        url->setPlaceholderText("https://... or local HTML file (leave empty for preview card)");
        auto *browse = new QPushButton("File…");
        urlRow->addWidget(url, 1);
        urlRow->addWidget(browse);
        f->addRow("URL", urlRow);
        QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, url] {
            const QString p = QFileDialog::getOpenFileName(&dialog, "Select HTML File", {}, "HTML Files (*.html *.htm);;All Files (*.*)");
            if (!p.isEmpty()) url->setText(QUrl::fromLocalFile(p).toString());
        });

        auto *sizeRow = new QHBoxLayout;
        auto *w = new PaddedSpinBox;
        w->setRange(64, 3840); w->setSingleStep(10); w->setSuffix(" px"); w->setValue(800);
        auto *h = new PaddedSpinBox;
        h->setRange(64, 2160); h->setSingleStep(10); h->setSuffix(" px"); h->setValue(600);
        sizeRow->addWidget(w, 1);
        sizeRow->addWidget(new QLabel("×"));
        sizeRow->addWidget(h, 1);
        f->addRow("Size", sizeRow);

        auto *fps = new PaddedSpinBox;
        fps->setRange(1, 60); fps->setSuffix(" fps"); fps->setValue(30);
        f->addRow("FPS", fps);

        auto *css = new QPlainTextEdit("html, body { width: 100%; height: 100%; margin: 0; padding: 0; overflow: hidden; background-color: rgba(0, 0, 0, 0); } body { display: flex; align-items: center; justify-content: center; }");
        css->setFixedHeight(84);
        f->addRow("Custom CSS", css);

        auto *muted = new QCheckBox("Mute browser audio");
        f->addRow("", muted);
        layout->addWidget(hintLabel("Renders web pages, alerts, and local HTML overlays off-screen with a transparent background."));

        collect = [&options, url, w, h, fps, css, muted] {
            options["browserUrl"] = url->text().trimmed();
            options["browserWidth"] = w->value();
            options["browserHeight"] = h->value();
            options["browserFps"] = fps->value();
            options["browserCss"] = css->toPlainText();
            options["browserMuted"] = muted->isChecked();
            options["browserReloadNonce"] = 0;
        };
    } else if (type == "Screen") {
        auto *combo = new QComboBox;
        widenChoice(combo);
        int primary = 0;
        for (const auto &v : captureList) {
            const auto c = v.toObject();
            if (c["kind"].toString() != "Monitor") continue;
            if (c["isPrimary"].toBool()) primary = combo->count();
            combo->addItem(themedIcon("monitor"), c["label"].toString(), c["id"].toString());
        }
        if (combo->count() > 0) combo->setCurrentIndex(primary);
        f->addRow("Display", combo);
        auto *cursor = new QCheckBox("Show mouse cursor");
        cursor->setChecked(true);
        f->addRow("", cursor);
        layout->addWidget(hintLabel("Captures the selected monitor directly from GPU memory via DXGI Desktop Duplication."));

        collect = [&options, combo, cursor] {
            options["captureId"] = combo->currentData().toString();
            options["captureCursor"] = cursor->isChecked();
        };
    } else if (type == "Text") {
        auto *txt = new QPlainTextEdit("My Stream!");
        txt->setFixedHeight(80);
        f->addRow("Text", txt);
        auto *font = new QFontComboBox;
        font->setCurrentFont(QFont("Segoe UI"));
        f->addRow("Font", font);
        auto *szRow = new QHBoxLayout;
        auto *sz = new PaddedSpinBox;
        sz->setRange(8, 400); sz->setSuffix(" px"); sz->setValue(72);
        auto *bold = new QCheckBox("Bold");
        bold->setChecked(true);
        szRow->addWidget(sz, 1);
        szRow->addWidget(bold);
        f->addRow("Size", szRow);
        auto *color = new ColorButton("Text Color");
        color->setColor(Qt::white);
        f->addRow("Color", color);
        auto *outRow = new QHBoxLayout;
        auto *outColor = new ColorButton("Outline");
        outColor->setColor(Qt::black);
        auto *outW = new PaddedSpinBox;
        outW->setRange(0, 40); outW->setSuffix(" px"); outW->setValue(6);
        outRow->addWidget(outColor, 1);
        outRow->addWidget(outW);
        f->addRow("Outline", outRow);
        auto *bg = new ColorButton("Background");
        bg->setColor(Qt::transparent);
        f->addRow("Background", bg);

        collect = [&options, txt, font, sz, bold, color, outColor, outW, bg] {
            options["text"] = txt->toPlainText();
            options["fontName"] = font->currentFont().family();
            options["fontSize"] = sz->value();
            options["bold"] = bold->isChecked();
            options["colorArgb"] = double(qint32(color->color().rgba()));
            options["outlineArgb"] = double(qint32(outColor->color().rgba()));
            options["outlineWidth"] = outW->value();
            options["backgroundArgb"] = double(qint32(bg->color().rgba()));
        };
    } else if (type == "AudioInput" || type == "AudioOutput") {
        auto *combo = new QComboBox;
        widenChoice(combo);
        combo->addItem("System default device", "");
        const QJsonArray devs = audioDevices[type == "AudioInput" ? "inputs" : "outputs"].toArray();
        for (const auto &d : devs)
            combo->addItem(d.toObject()["name"].toString(), d.toObject()["id"].toString());
        f->addRow("Device", combo);
        layout->addWidget(hintLabel(type == "AudioInput"
            ? "Captures audio from the selected microphone or input device in this scene."
            : "Captures audio playing through the selected speakers or headphones in this scene."));

        collect = [&options, combo] {
            options["deviceId"] = combo->currentData().toString();
        };
    } else if (type == "AlertBox") {
        auto makeFileRow = [&](const QString &title, const QString &filter, const QString &placeholder) {
            auto *row = new QHBoxLayout;
            auto *edit = new QLineEdit;
            edit->setPlaceholderText(placeholder);
            auto *btn = new QPushButton("Browse…");
            row->addWidget(edit, 1);
            row->addWidget(btn);
            QObject::connect(btn, &QPushButton::clicked, &dialog, [&dialog, edit, title, filter] {
                const QString p = QFileDialog::getOpenFileName(&dialog, title, {}, filter);
                if (!p.isEmpty()) edit->setText(QDir::toNativeSeparators(p));
            });
            return QPair<QHBoxLayout *, QLineEdit *>{row, edit};
        };
        auto *onGift = new QCheckBox("Enable Gift alerts");
        onGift->setChecked(true);
        auto [gImgRow, gImg] = makeFileRow("Select Gift Alert Image/GIF", "Images & GIFs (*.gif *.png *.jpg *.jpeg *.webp)", "GIF / PNG / JPG for Gift alert");
        auto [gSndRow, gSnd] = makeFileRow("Select Gift Alert Sound", "Audio (*.mp3 *.wav)", "MP3 / WAV sound for Gift alert");
        auto *onFollow = new QCheckBox("Enable Follow alerts");
        onFollow->setChecked(true);
        auto [fImgRow, fImg] = makeFileRow("Select Follow Alert Image/GIF", "Images & GIFs (*.gif *.png *.jpg *.jpeg *.webp)", "GIF / PNG / JPG for Follow alert");
        auto [fSndRow, fSnd] = makeFileRow("Select Follow Alert Sound", "Audio (*.mp3 *.wav)", "MP3 / WAV sound for Follow alert");
        auto *dur = new PaddedSpinBox;
        dur->setRange(2, 15); dur->setSuffix(" s"); dur->setValue(5);
        auto *vol = new PaddedSpinBox;
        vol->setRange(0, 100); vol->setSuffix(" %"); vol->setValue(100);

        f->addRow("", onGift);
        f->addRow("Gift Image/GIF", gImgRow);
        f->addRow("Gift Sound", gSndRow);
        f->addRow("", onFollow);
        f->addRow("Follow Image/GIF", fImgRow);
        f->addRow("Follow Sound", fSndRow);
        f->addRow("Duration", dur);
        f->addRow("Sound Volume", vol);
        layout->addWidget(hintLabel("Plays animated GIFs / images and MP3/WAV sounds in real time when someone follows or sends a gift on TikTok LIVE."));

        collect = [&options, onGift, gImg, gSnd, onFollow, fImg, fSnd, dur, vol] {
            options["alertOnGift"] = onGift->isChecked();
            options["alertGiftImagePath"] = gImg->text().trimmed();
            options["alertGiftSoundPath"] = gSnd->text().trimmed();
            options["alertOnFollow"] = onFollow->isChecked();
            options["alertFollowImagePath"] = fImg->text().trimmed();
            options["alertFollowSoundPath"] = fSnd->text().trimmed();
            options["alertDurationSec"] = dur->value();
            options["alertVolume"] = vol->value() / 100.0;
            options["volume"] = vol->value() / 100.0;
            options["monitorMode"] = "MonitorAndOutput";
        };
    } else {
        return true;
    }

    auto *buttons = new QDialogButtonBox;
    auto *add = buttons->addButton("Add Source", QDialogButtonBox::AcceptRole);
    add->setObjectName("primary");
    buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return false;
    if (collect) collect();
    return true;
}
QRectF itemRect(const QJsonObject &i) { return QRectF(i["x"].toDouble(), i["y"].toDouble(), i["w"].toDouble(1), i["h"].toDouble(1)); }
QColor argbOf(const QJsonValue &v, QColor def = Qt::white) { return v.isDouble() ? QColor::fromRgba(QRgb(quint32(qint64(v.toDouble())))) : def; }
double argbVal(const QColor &c) { return double(qint32(c.rgba())); }

void applyDarkTitleBar(QWidget *w)
{
    if (!w) return;
    const HWND hwnd = HWND(w->winId());
    if (!hwnd) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    COLORREF caption = RGB(0x18, 0x1a, 0x24);
    COLORREF text = RGB(0xe8, 0xea, 0xf2);
    COLORREF border = RGB(0x2c, 0x30, 0x42);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
}
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
    setupTray();

    m_alertTimer = new QTimer(this);
    m_alertTimer->setSingleShot(true);
    connect(m_alertTimer, &QTimer::timeout, this, [this] {
        if (m_alertAnimTimer) m_alertAnimTimer->stop();
        if (m_alertMovie) { m_alertMovie->stop(); delete m_alertMovie; m_alertMovie = nullptr; }
        m_alertKind.clear();
        m_alertUser.clear();
        m_alertDetail.clear();
        ++m_alertRevision;
        updateAlertOverlays();
        if (!m_alertQueue.isEmpty()) QTimer::singleShot(250, this, &MainWindow::showNextAlert);
    });
    m_alertAnimTimer = new QTimer(this);
    m_alertAnimTimer->setTimerType(Qt::PreciseTimer);
    connect(m_alertAnimTimer, &QTimer::timeout, this, [this] {
        if (m_alertKind.isEmpty()) { m_alertAnimTimer->stop(); return; }
        ++m_alertRevision;
        updateAlertOverlays();
    });

    m_layoutSaveTimer = new QTimer(this);
    m_layoutSaveTimer->setSingleShot(true);
    m_layoutSaveTimer->setInterval(400);
    connect(m_layoutSaveTimer, &QTimer::timeout, this, &MainWindow::saveDockLayout);

    QSettings qs("TikStream", "UI2");
    const QByteArray savedGeo = qs.value("geometry").toByteArray();
    const QByteArray savedState = qs.value("state").toByteArray();
    if (!savedGeo.isEmpty()) restoreGeometry(savedGeo);
    const bool hadState = !savedState.isEmpty() && restoreState(savedState);
    if (!hadState && !qs.value("compactLayoutV1").toBool()) arrangeDocks(false);
    if (hadState) m_dualDockPositioned = true;
    for (auto *dock : findChildren<QDockWidget *>()) {
        if (dock->isFloating()) {
            const QByteArray dg = qs.value("dockGeo/" + dock->objectName()).toByteArray();
            if (!dg.isEmpty()) dock->restoreGeometry(dg);
        }
        auto schedSave = [this] { if (m_layoutSaveTimer && !m_loading) m_layoutSaveTimer->start(); };
        connect(dock, &QDockWidget::dockLocationChanged, this, [schedSave](Qt::DockWidgetArea) { schedSave(); });
        connect(dock, &QDockWidget::topLevelChanged, this, [schedSave](bool) { schedSave(); });
        connect(dock, &QDockWidget::visibilityChanged, this, [schedSave](bool) { schedSave(); });
    }
    QTimer::singleShot(0, this, [this] {
        for (auto *dock : findChildren<QDockWidget *>()) {
            if (dock->isFloating() && dock->isVisible()) {
                ensureDockOnScreen(dock);
                applyDarkTitleBar(dock);
            }
        }
    });

    for (const auto &v : sources()) uploadRaster(v.toObject());
    refreshCaptureList();
    refreshCameras();
    rebuildScenes();
    rebuildSourceList();
    rebuildMixer();
    applyHotkeys();
    if (m_sourceList->count() > 0) m_sourceList->setCurrentRow(m_sourceList->count() - 1);
    loadProperties();
    syncDualPreviewDock();

    m_statsTimer = new QTimer(this);
    connect(m_statsTimer, &QTimer::timeout, this, &MainWindow::pollStats);
    m_statsTimer->start(1000);
    m_levelsTimer = new QTimer(this);
    connect(m_levelsTimer, &QTimer::timeout, this, &MainWindow::pollLevels);
    auto *devicesTimer = new QTimer(this);
    connect(devicesTimer, &QTimer::timeout, this, [this] { refreshAudioDevices(); });
    devicesTimer->start(3000);
    m_levelsTimer->start(40);

    m_updateTimer = new QTimer(this);
    m_updateTimer->setInterval(6 * 3600 * 1000); // check every 6 hours when enabled
    connect(m_updateTimer, &QTimer::timeout, this, [this] {
        if (m_s["autoUpdateEnabled"].toBool(true) && !m_streaming && !m_recording)
            checkForUpdates(false);
    });
    if (!QCoreApplication::arguments().contains("--test")) {
        m_updateTimer->start();
        QTimer::singleShot(5000, this, [this] {
            if (m_s["autoUpdateEnabled"].toBool(true) && !m_streaming && !m_recording)
                checkForUpdates(false);
        });
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::showEvent(QShowEvent *e)
{
    QMainWindow::showEvent(e);
    applyDarkTitleBar(this);
}

// =============================== construction ===============================

void MainWindow::buildMenu()
{
    auto *file = menuBar()->addMenu("File");
    file->addAction(themedIcon("folder"), "Show Recordings", this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_s["recordFolder"].toString()));
    });
    file->addSeparator();
    file->addAction("Export Scene Collection…", this, &MainWindow::exportSceneCollection);
    file->addAction("Import Scene Collection…", this, &MainWindow::importSceneCollection);
    file->addSeparator();
    file->addAction(themedIcon("gear"), "Settings", QKeySequence("Ctrl+,"), this, &MainWindow::openSettings);
    file->addSeparator();
    file->addAction(themedIcon("exit"), "Exit", this, [this] { m_reallyQuit = true; close(); });

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
    view->addAction(themedIcon("sliders"), "Stats & Performance Graph…", this, &MainWindow::openStatsGraphDialog);
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
    help->addAction(themedIcon("refresh"), "Check for Updates…", this, [this] { checkForUpdates(true); });
    help->addSeparator();
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
    auto *info = new QLabel("Drag to move  •  corner = resize (Shift = free aspect)  •  Alt + drag edge = crop  •  wheel = scale  •  Delete = remove");
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
        connect(d, &QDockWidget::topLevelChanged, d, [d](bool floating) {
            if (floating) QTimer::singleShot(0, d, [d] { applyDarkTitleBar(d); });
        });
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
    m_liveChat->setKickAccount(m_s["kickUsername"].toString("zayosh"));
    connect(m_liveChat, &LiveChat::overlayChanged, this, &MainWindow::updateChatOverlays);
    connect(m_liveChat, &LiveChat::alertTriggered, this, &MainWindow::triggerAlert);
    connect(m_liveChat, &LiveChat::accountChanged, this, [this](const QString &nick) {
        m_s["tikTokUsername"] = nick;
        pushSettings();
    });
    connect(m_liveChat, &LiveChat::kickAccountChanged, this, [this](const QString &ch) {
        m_s["kickUsername"] = ch;
        pushSettings();
    });
    auto *chat = makeDock("LIVE Chat (TikTok + Kick)", "dockLiveChat", m_liveChat, Qt::RightDockWidgetArea);
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
    m_btnLock = toolBtn("sliders", "Lock / Unlock Source Position");
    auto *refresh = toolBtn("refresh", "Refresh Windows, Displays, and Cameras");
    connect(add, &QToolButton::clicked, this, &MainWindow::addSourceDialog);
    connect(m_btnRemove, &QToolButton::clicked, this, &MainWindow::removeSelectedItem);
    connect(m_btnUp, &QToolButton::clicked, this, [this] { moveSelectedItem(+1); });
    connect(m_btnDown, &QToolButton::clicked, this, [this] { moveSelectedItem(-1); });
    connect(m_btnLock, &QToolButton::clicked, this, &MainWindow::toggleSelectedItemLock);
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
            m.addAction(selectedItem()["locked"].toBool() ? "Unlock Position" : "Lock Position", this, &MainWindow::toggleSelectedItemLock);
            if (selectedSource()["type"].toString() == "Browser")
                m.addAction("Interact…", this, [this] { openBrowserInteractDialog(selectedSource()["id"].toString()); });
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
    auto *dSources = makeDock("Sources", "dockSources", withToolbar(m_sourceList, {add, m_btnRemove, m_btnUp, m_btnDown, m_btnLock}, {refresh}), Qt::LeftDockWidgetArea);
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
    m_btnReplay = new QPushButton(themedIcon("refresh"), "  Start Replay Buffer");
    m_btnReplay->setObjectName("replayButton");
    m_btnSaveReplay = new QPushButton(themedIcon("folder"), "  Save Replay");
    m_btnSaveReplay->setEnabled(false);
    auto *btnSettings = new QPushButton(themedIcon("gear"), "  Settings");
    for (auto *b : {m_btnStream, m_btnRecord, m_btnReplay, m_btnSaveReplay, btnSettings}) {
        b->setMinimumHeight(b == m_btnStream ? 38 : 30);
        b->setIconSize(QSize(18, 18));
        b->setCursor(Qt::PointingHandCursor);
        cv->addWidget(b);
    }
    cv->addStretch();
    connect(m_btnStream, &QPushButton::clicked, this, &MainWindow::toggleStream);
    connect(m_btnRecord, &QPushButton::clicked, this, &MainWindow::toggleRecord);
    connect(m_btnReplay, &QPushButton::clicked, this, &MainWindow::toggleReplay);
    connect(m_btnSaveReplay, &QPushButton::clicked, this, &MainWindow::saveReplay);
    connect(btnSettings, &QPushButton::clicked, this, &MainWindow::openSettings);
    auto *dCtl = makeDock("Controls", "dockControls", ctl, Qt::RightDockWidgetArea);

    splitDockWidget(dProps, dMix, Qt::Vertical);
    splitDockWidget(dMix, dCtl, Qt::Vertical);

    // ---------- Detached Horizontal Dual-Canvas Preview Dock ----------
    m_dualPreview = new PreviewWidget;
    m_dualPreview->setCanvasSize(m_s["dualCanvasWidth"].toInt(1920), m_s["dualCanvasHeight"].toInt(1080));
    m_dualPreview->setMinimumSize(360, 220);
    m_dualPreviewDock = new QDockWidget("Horizontal Preview (Dual-Canvas)", this);
    m_dualPreviewDock->setObjectName("dockDualPreview");
    m_dualPreviewDock->setWidget(m_dualPreview);
    m_dualPreviewDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_dualPreviewDock);
    m_dualPreviewDock->setFloating(true);
    connect(m_dualPreviewDock, &QDockWidget::topLevelChanged, m_dualPreviewDock, [this](bool floating) {
        if (floating) QTimer::singleShot(0, m_dualPreviewDock, [this] { applyDarkTitleBar(m_dualPreviewDock); });
        QTimer::singleShot(30, m_dualPreview, [this] {
            if (m_dualPreviewDock && m_dualPreviewDock->isVisible() && m_dualPreview) m_dualPreview->emitSize();
        });
    });
    connect(m_dualPreviewDock, &QDockWidget::dockLocationChanged, m_dualPreviewDock, [this](Qt::DockWidgetArea) {
        QTimer::singleShot(30, m_dualPreview, [this] {
            if (m_dualPreviewDock && m_dualPreviewDock->isVisible() && m_dualPreview) m_dualPreview->emitSize();
        });
    });
    m_dualPreviewDock->hide();
    if (auto *pm = findChild<QMenu *>("panelsMenu")) {
        auto *act = m_dualPreviewDock->toggleViewAction();
        act->setText("Horizontal Preview Dock (Dual-Canvas)");
        pm->addAction(act);
    }
    connect(m_dualPreview, &PreviewWidget::nativeResized, this, [this](qint64 hwnd, int w, int h) {
        if (m_dualPreviewDock && m_dualPreviewDock->isVisible())
            EngineHost::instance().call("setDualPreview", QJsonObject{{"hwnd", hwnd}, {"w", w}, {"h", h}});
    });
    connect(m_dualPreview, &PreviewWidget::canvasPressed, this, [this](QPointF p) { onCanvasPressedAt(p, true); });
    connect(m_dualPreview, &PreviewWidget::canvasDragged, this, &MainWindow::onCanvasDragged);
    connect(m_dualPreview, &PreviewWidget::canvasReleased, this, [this] {
        m_drag = Drag::None;
        m_dragTimer->stop();
        if (m_dragChanged) {
            m_dragChanged = false;
            pushSettings();
            updateSelection();
        }
    });
    connect(m_dualPreview, &PreviewWidget::canvasWheel, this, [this](int d) { onCanvasWheelAt(d, true); });
    connect(m_dualPreview, &PreviewWidget::deletePressed, this, &MainWindow::removeSelectedItem);
    connect(m_dualPreviewDock, &QDockWidget::visibilityChanged, this, [this](bool vis) {
        if (!vis) {
            EngineHost::instance().call("setDualPreview", QJsonObject{{"hwnd", 0}, {"w", 0}, {"h", 0}});
        } else if (m_dualPreview) {
            if (m_dualPreviewDock->isFloating()) {
                ensureDockOnScreen(m_dualPreviewDock);
                QTimer::singleShot(0, m_dualPreviewDock, [this] { applyDarkTitleBar(m_dualPreviewDock); });
            }
            const qreal dpr = m_dualPreview->devicePixelRatioF();
            EngineHost::instance().call("setDualPreview", QJsonObject{
                {"hwnd", qint64(m_dualPreview->winId())},
                {"w", std::max(2, int(std::lround(m_dualPreview->width() * dpr)))},
                {"h", std::max(2, int(std::lround(m_dualPreview->height() * dpr)))},
            });
            updateSelection();
        }
    });
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
    content->setMinimumWidth(0);
    auto *cv = new QVBoxLayout(content);
    cv->setContentsMargins(10, 10, 10, 14);
    cv->setSpacing(10);
    cv->addWidget(new SectionLabel("Source"));
    m_pages = new QStackedWidget;
    m_pages->setMinimumWidth(0);
    m_pages->addWidget(pageScreen());
    m_pages->addWidget(pageWindow());
    m_pages->addWidget(pageCamera());
    m_pages->addWidget(pageImage());
    m_pages->addWidget(pageText());
    m_pages->addWidget(pageColor());
    m_pages->addWidget(pageMedia());
    m_pages->addWidget(pageBrowser());
    m_pages->addWidget(pageAlertBox());
    m_pages->addWidget(pageChatOverlay());
    m_pages->addWidget(pageApplicationAudio());
    m_pages->addWidget(pageAudioDevice());
    m_pages->addWidget(new QWidget);
    cv->addWidget(m_pages);
    m_srcStatus = new QLabel;
    m_srcStatus->setObjectName("statusPill");
    cv->addWidget(m_srcStatus);
    m_transform = buildTransform();
    m_transform->setMinimumWidth(0);
    cv->addWidget(m_transform);
    cv->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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

QWidget *MainWindow::pageBrowser()
{
    auto *w = new QWidget;
    w->setMinimumWidth(0);
    auto *f = form(w);

    auto *urlRow = new QHBoxLayout();
    urlRow->setSpacing(5);
    m_browserUrl = new QLineEdit;
    m_browserUrl->setMinimumWidth(0);
    m_browserUrl->setPlaceholderText("https://... or local HTML");
    auto *browse = new QPushButton("File…");
    browse->setMinimumWidth(0);
    browse->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    browse->setToolTip("Select a local HTML file");
    urlRow->addWidget(m_browserUrl, 1);
    urlRow->addWidget(browse);
    f->addRow("URL", urlRow);

    auto *sizeRow = new QHBoxLayout();
    sizeRow->setSpacing(4);
    m_browserWidth = new PaddedSpinBox;
    m_browserWidth->setRange(64, 3840);
    m_browserWidth->setSingleStep(10);
    m_browserWidth->setSuffix(" px");
    m_browserWidth->setMinimumWidth(0);
    m_browserWidth->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_browserHeight = new PaddedSpinBox;
    m_browserHeight->setRange(64, 2160);
    m_browserHeight->setSingleStep(10);
    m_browserHeight->setSuffix(" px");
    m_browserHeight->setMinimumWidth(0);
    m_browserHeight->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sizeRow->addWidget(m_browserWidth, 1);
    sizeRow->addWidget(new QLabel("×"));
    sizeRow->addWidget(m_browserHeight, 1);
    f->addRow("Size", sizeRow);

    m_browserFps = new PaddedSpinBox;
    m_browserFps->setRange(1, 60);
    m_browserFps->setSuffix(" fps");
    m_browserFps->setMinimumWidth(0);
    f->addRow("FPS", m_browserFps);

    m_browserCss = new QPlainTextEdit;
    m_browserCss->setMinimumWidth(0);
    m_browserCss->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_browserCss->setFixedHeight(78);
    m_browserCss->setPlaceholderText("html, body { width: 100%; height: 100%; margin: 0; padding: 0; overflow: hidden; background-color: rgba(0, 0, 0, 0); } body { display: flex; align-items: center; justify-content: center; }");
    f->addRow("CSS", m_browserCss);

    m_browserMuted = new QCheckBox("Mute browser audio");
    f->addRow("", m_browserMuted);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);
    auto *interact = new QPushButton(themedIcon("globe"), " Interact");
    interact->setMinimumWidth(0);
    interact->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    interact->setCursor(Qt::PointingHandCursor);
    auto *refresh = new QPushButton(themedIcon("refresh"), " Refresh");
    refresh->setMinimumWidth(0);
    refresh->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    refresh->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(interact, 1);
    btnRow->addWidget(refresh, 1);
    f->addRow("", btnRow);
    auto *hint = hintLabel("Renders web overlays (Tipply, StreamElements, Streamlabs, TikFinity) with audio routed to the Audio Mixer.");
    hint->setMinimumWidth(0);
    f->addRow("", hint);

    connect(interact, &QPushButton::clicked, this, [this] {
        const QString id = selectedSource()["id"].toString();
        if (!id.isEmpty()) openBrowserInteractDialog(id);
    });

    connect(m_browserUrl, &QLineEdit::editingFinished, this, [this] {
        if (m_loading) return;
        const QString u = m_browserUrl->text().trimmed();
        editSource([u](QJsonObject &s) { s["browserUrl"] = u; });
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString p = QFileDialog::getOpenFileName(this, "Select HTML File", {}, "HTML Files (*.html *.htm);;All Files (*.*)");
        if (p.isEmpty()) return;
        const QString u = QUrl::fromLocalFile(p).toString();
        m_browserUrl->setText(u);
        editSource([u](QJsonObject &s) { s["browserUrl"] = u; });
    });
    connect(m_browserWidth, &QSpinBox::valueChanged, this, [this](int v) { if (!m_loading) editSource([v](QJsonObject &s) { s["browserWidth"] = v; }); });
    connect(m_browserHeight, &QSpinBox::valueChanged, this, [this](int v) { if (!m_loading) editSource([v](QJsonObject &s) { s["browserHeight"] = v; }); });
    connect(m_browserFps, &QSpinBox::valueChanged, this, [this](int v) { if (!m_loading) editSource([v](QJsonObject &s) { s["browserFps"] = v; }); });
    connect(m_browserCss, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_loading) editSource([this](QJsonObject &s) { s["browserCss"] = m_browserCss->toPlainText(); });
    });
    connect(m_browserMuted, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_loading) editSource([on](QJsonObject &s) { s["browserMuted"] = on; });
    });
    connect(refresh, &QPushButton::clicked, this, [this] {
        editSource([](QJsonObject &s) { s["browserReloadNonce"] = s["browserReloadNonce"].toInt() + 1; });
    });
    return w;
}

QWidget *MainWindow::pageAlertBox()
{
    auto *w = new QWidget;
    auto *f = form(w);

    auto addPicker = [&](const QString &rowLabel, QLineEdit *&edit, const QString &title, const QString &filter, const QString &placeholder, const char *jsonKey) {
        auto *row = new QHBoxLayout();
        edit = new QLineEdit;
        edit->setPlaceholderText(placeholder);
        auto *btn = new QPushButton("Browse…");
        row->addWidget(edit, 1);
        row->addWidget(btn);
        f->addRow(rowLabel, row);
        connect(btn, &QPushButton::clicked, this, [this, edit, title, filter, jsonKey] {
            const QString p = QFileDialog::getOpenFileName(this, title, {}, filter);
            if (p.isEmpty()) return;
            const QString nat = QDir::toNativeSeparators(p);
            edit->setText(nat);
            editSource([nat, jsonKey](QJsonObject &s) { s[jsonKey] = nat; });
        });
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, jsonKey] {
            if (!m_loading) editSource([edit, jsonKey](QJsonObject &s) { s[jsonKey] = edit->text().trimmed(); });
        });
    };

    m_alertOnGift = new QCheckBox("Enable Gift alerts");
    f->addRow("", m_alertOnGift);
    addPicker("Gift Image/GIF", m_alertGiftImage, "Select Gift Image / GIF", "Images & GIFs (*.gif *.png *.jpg *.jpeg *.webp)", "GIF / PNG / JPG", "alertGiftImagePath");
    addPicker("Gift Sound", m_alertGiftSound, "Select Gift Sound", "Audio (*.mp3 *.wav)", "MP3 / WAV", "alertGiftSoundPath");

    m_alertOnFollow = new QCheckBox("Enable Follow alerts");
    f->addRow("", m_alertOnFollow);
    addPicker("Follow Image/GIF", m_alertFollowImage, "Select Follow Image / GIF", "Images & GIFs (*.gif *.png *.jpg *.jpeg *.webp)", "GIF / PNG / JPG", "alertFollowImagePath");
    addPicker("Follow Sound", m_alertFollowSound, "Select Follow Sound", "Audio (*.mp3 *.wav)", "MP3 / WAV", "alertFollowSoundPath");

    m_alertDuration = new PaddedSpinBox;
    m_alertDuration->setRange(2, 15);
    m_alertDuration->setSuffix(" s");
    f->addRow("Duration", m_alertDuration);

    m_alertVolume = new PaddedSpinBox;
    m_alertVolume->setRange(0, 100);
    m_alertVolume->setSingleStep(5);
    m_alertVolume->setSuffix(" %");
    f->addRow("Sound Volume", m_alertVolume);

    auto *testRow = new QHBoxLayout();
    auto *testGift = new QPushButton("Test Gift Alert");
    auto *testFollow = new QPushButton("Test Follow Alert");
    testGift->setCursor(Qt::PointingHandCursor);
    testFollow->setCursor(Qt::PointingHandCursor);
    testRow->addWidget(testGift);
    testRow->addWidget(testFollow);
    f->addRow("", testRow);
    f->addRow("", hintLabel("Animated GIFs, PNG/JPG images, and MP3/WAV sounds play in real time on both your stream and headphones."));

    connect(m_alertDuration, &QSpinBox::valueChanged, this, [this](int v) {
        if (!m_loading) editSource([v](QJsonObject &s) { s["alertDurationSec"] = v; });
    });
    connect(m_alertVolume, &QSpinBox::valueChanged, this, [this](int v) {
        if (!m_loading) editSource([v](QJsonObject &s) { s["alertVolume"] = v / 100.0; });
    });
    connect(m_alertOnGift, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_loading) editSource([on](QJsonObject &s) { s["alertOnGift"] = on; });
    });
    connect(m_alertOnFollow, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_loading) editSource([on](QJsonObject &s) { s["alertOnFollow"] = on; });
    });
    connect(testGift, &QPushButton::clicked, this, [this] { triggerAlert("gift", "alex_gg", "🎁 Rose ×10"); });
    connect(testFollow, &QPushButton::clicked, this, [this] { triggerAlert("follow", "maya_live", "just followed!"); });
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
    auto *shadowItem = addEntry("Drop Shadow", true, item["shadowEnabled"].toBool());
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
    auto *cropL = hslider(0, 95); cropL->setValue(qRound(item["cropLeft"].toDouble() * 100));
    auto *cropR = hslider(0, 95); cropR->setValue(qRound(item["cropRight"].toDouble() * 100));
    auto *cropT = hslider(0, 95); cropT->setValue(qRound(item["cropTop"].toDouble() * 100));
    auto *cropB = hslider(0, 95); cropB->setValue(qRound(item["cropBottom"].toDouble() * 100));
    {
        auto *p = new QWidget;
        auto *f = form(p);
        auto addRowPct = [&](const QString &lbl, QSlider *s, const char *key) {
            auto *vLbl = hintLabel(QString("%1%").arg(s->value()));
            auto *row = new QHBoxLayout; row->addWidget(s, 1); row->addWidget(vLbl);
            f->addRow(lbl, row);
            connect(s, &QSlider::valueChanged, &dlg, [=](int val) {
                vLbl->setText(QString("%1%").arg(val));
                updateItem([this, key, val](QJsonObject &i) {
                    if (!i.value("hasDualLayout").toBool()) {
                        const QRectF dr = displayRectFor(i, true);
                        i["hasDualLayout"] = true;
                        i["dualFit"] = i.value("fit").toString("Free");
                        i["dualX"] = dr.x(); i["dualY"] = dr.y(); i["dualW"] = dr.width(); i["dualH"] = dr.height();
                        i["dualCropLeft"] = i.value("cropLeft").toDouble();
                        i["dualCropRight"] = i.value("cropRight").toDouble();
                        i["dualCropTop"] = i.value("cropTop").toDouble();
                        i["dualCropBottom"] = i.value("cropBottom").toDouble();
                    }
                    i[key] = val / 100.0;
                });
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

    // 3: GPU Drop Shadow (center X/Y, spread/blur, intensity)
    auto *shOffX = hslider(-100, 100); shOffX->setValue(qRound(item["shadowOffsetX"].toDouble(0.01) * 1000));
    auto *shOffY = hslider(-100, 100); shOffY->setValue(qRound(item["shadowOffsetY"].toDouble(0.015) * 1000));
    auto *shBlur = hslider(2, 120);    shBlur->setValue(qRound(item["shadowBlur"].toDouble(0.03) * 1000));
    auto *shInt  = hslider(0, 100);    shInt->setValue(qRound(item["shadowIntensity"].toDouble(0.65) * 100));
    {
        auto *p = new QWidget;
        auto *f = form(p);
        auto addShRow = [&](const QString &lbl, QSlider *sl, const QString &key, double div, const QString &suf) {
            auto *row = new QHBoxLayout;
            auto *vl = hintLabel(QString("%1%2").arg(sl->value() / (div == 1000.0 ? 10.0 : 1.0), 0, 'f', div == 1000.0 ? 1 : 0).arg(suf));
            row->addWidget(sl, 1);
            row->addWidget(vl);
            f->addRow(lbl, row);
            connect(sl, &QSlider::valueChanged, &dlg, [=](int val) {
                vl->setText(QString("%1%2").arg(val / (div == 1000.0 ? 10.0 : 1.0), 0, 'f', div == 1000.0 ? 1 : 0).arg(suf));
                updateItem([key, val, div](QJsonObject &i) { i[key] = val / div; });
            });
        };
        addShRow("Center X", shOffX, "shadowOffsetX", 1000.0, "%");
        addShRow("Center Y", shOffY, "shadowOffsetY", 1000.0, "%");
        addShRow("Spread / Blur", shBlur, "shadowBlur", 1000.0, "%");
        addShRow("Intensity", shInt, "shadowIntensity", 100.0, "%");
        f->addRow("", hintLabel("Ultra-fast GPU shader shadow (0% CPU). Works on webcams, games, PNG/GIFs, and TikTok Alerts. Toggle the checkbox on the left to enable."));
        pages->addWidget(p);
    }

    connect(list, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(list, &QListWidget::itemChanged, &dlg, [=](QListWidgetItem *it) {
        const bool on = it->checkState() == Qt::Checked;
        if (it == chromaItem) updateItem([on](QJsonObject &i) { i["chromaKeyEnabled"] = on; });
        else if (it == shadowItem) updateItem([on](QJsonObject &i) { i["shadowEnabled"] = on; });
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
        shadowItem->setCheckState(Qt::Unchecked);
        shOffX->setValue(10); shOffY->setValue(15); shBlur->setValue(30); shInt->setValue(65);
        updateItem([](QJsonObject &i) {
            i["cropLeft"] = 0; i["cropRight"] = 0; i["cropTop"] = 0; i["cropBottom"] = 0;
            i["brightness"] = 0; i["contrast"] = 1; i["saturation"] = 1;
            i["chromaKeyEnabled"] = false; i["chromaKeyArgb"] = int(QColor(0, 255, 0).rgba());
            i["chromaKeySimilarity"] = 0.25; i["chromaKeySmoothness"] = 0.08;
            i["shadowEnabled"] = false; i["shadowOffsetX"] = 0.01; i["shadowOffsetY"] = 0.015;
            i["shadowBlur"] = 0.03; i["shadowIntensity"] = 0.65;
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
    {
        auto *monIt = new QListWidgetItem("Audio Monitoring", list);
        monIt->setSizeHint(QSize(0, 34));
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

    // Audio Monitoring (listen to this source in your headphones)
    {
        auto *p = new QWidget;
        auto *f = form(p);
        auto *mon = new QComboBox;
        mon->addItem("Monitor Off", "Off");
        mon->addItem("Monitor Only (mute stream/recording output)", "MonitorOnly");
        mon->addItem("Monitor and Output", "MonitorAndOutput");
        const QString curMon = src["monitorMode"].toString("Off");
        mon->setCurrentIndex(std::max(0, mon->findData(curMon)));
        f->addRow("Monitoring", mon);
        f->addRow("", hintLabel("Routes this audio source to your default playback device (headphones) after all filters and volume."));
        connect(mon, &QComboBox::currentIndexChanged, &dlg, [=, &updateSrc] {
            const QString mode = mon->currentData().toString();
            updateSrc([mode](QJsonObject &s) { s["monitorMode"] = mode; });
        });
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
        const QString label = (item["locked"].toBool() ? QStringLiteral("🔒 ") : QString()) + src["name"].toString();
        auto *it = new QListWidgetItem(themedIcon(sourceTypeInfo(src["type"].toString()).icon), label, m_sourceList);
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
                                            {"Text", PgText}, {"Color", PgColor}, {"Media", PgMedia}, {"Browser", PgBrowser}, {"AlertBox", PgAlertBox}, {"ChatOverlay", PgChatOverlay}, {"Game", PgWindow},
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
    } else if (type == "Browser") {
        m_browserUrl->setText(src["browserUrl"].toString());
        m_browserWidth->setValue(src["browserWidth"].toInt(800));
        m_browserHeight->setValue(src["browserHeight"].toInt(600));
        m_browserFps->setValue(src["browserFps"].toInt(30));
        m_browserCss->setPlainText(src["browserCss"].toString("body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }"));
        m_browserMuted->setChecked(src["browserMuted"].toBool(false));
    } else if (type == "AlertBox") {
        const QString fallbackImg = src["alertImagePath"].toString();
        const QString fallbackSnd = src["alertSoundPath"].toString();
        m_alertGiftImage->setText(src["alertGiftImagePath"].toString().isEmpty() ? fallbackImg : src["alertGiftImagePath"].toString());
        m_alertGiftSound->setText(src["alertGiftSoundPath"].toString().isEmpty() ? fallbackSnd : src["alertGiftSoundPath"].toString());
        m_alertFollowImage->setText(src["alertFollowImagePath"].toString().isEmpty() ? fallbackImg : src["alertFollowImagePath"].toString());
        m_alertFollowSound->setText(src["alertFollowSoundPath"].toString().isEmpty() ? fallbackSnd : src["alertFollowSoundPath"].toString());
        m_alertDuration->setValue(src["alertDurationSec"].toInt(5));
        m_alertVolume->setValue(qRound(src["alertVolume"].toDouble(1.0) * 100.0));
        m_alertOnGift->setChecked(src["alertOnGift"].toBool(true));
        m_alertOnFollow->setChecked(src["alertOnFollow"].toBool(true));
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
        if (sourceTypeInfo(t).audio || t == "Media" || t == "AlertBox" || t == "Browser") {
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
        else if (type == "AlertBox")
            channelName = channelName.isEmpty() ? "Alert Sounds" : channelName + " · Alert Sounds";
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
    } else if (r.existingId.isEmpty() && (r.type == "Browser" || r.type == "Screen" || r.type == "Text" || r.type == "AudioInput" || r.type == "AudioOutput" || r.type == "AlertBox")) {
        if (r.type == "Screen") refreshCaptureList();
        if (r.type == "AudioInput" || r.type == "AudioOutput") m_audioDevices = EngineHost::instance().callObj("listAudioDevices");
        if (!newSourceOptions(this, r.type, m_captureList, m_audioDevices, options)) return;
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
        if (type == "Browser") {
            src["browserUrl"] = "";
            src["browserWidth"] = 800;
            src["browserHeight"] = 600;
            src["browserFps"] = 30;
            src["browserCss"] = "html, body { width: 100%; height: 100%; margin: 0; padding: 0; overflow: hidden; background-color: rgba(0, 0, 0, 0); } body { display: flex; align-items: center; justify-content: center; }";
            src["browserMuted"] = false;
            src["browserReloadNonce"] = 0;
            src["monitorMode"] = "MonitorAndOutput";
        }
        if (type == "AlertBox") {
            src["alertImagePath"] = "";
            src["alertSoundPath"] = "";
            src["alertDurationSec"] = 5;
            src["alertOnGift"] = true;
            src["alertOnFollow"] = true;
            src["monitorMode"] = "MonitorAndOutput";
        }
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
                     {"zoom", 1.0}, {"panX", 0.0}, {"panY", 0.0}, {"mirror", false}, {"corner", 0.0}, {"opacity", 1.0},
                     {"cropLeft", 0.0}, {"cropRight", 0.0}, {"cropTop", 0.0}, {"cropBottom", 0.0}, {"hasDualLayout", false}};
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
    } else if (type == "Browser") {
        const auto sObj = sourceById(sourceId);
        const double bw = std::max(64, sObj["browserWidth"].toInt(800));
        const double bh = std::max(64, sObj["browserHeight"].toInt(600));
        const double sc = std::min({1.0, 0.9 * cw / bw, 0.9 * ch / bh});
        const double w = bw * sc / cw, h = bh * sc / ch;
        item["x"] = (1.0 - w) / 2.0; item["y"] = (1.0 - h) / 2.0; item["w"] = w; item["h"] = h;
        const double dcw = std::max(128, m_s["dualCanvasWidth"].toInt(1920));
        const double dch = std::max(128, m_s["dualCanvasHeight"].toInt(1080));
        const double dsc = std::min({1.0, 0.9 * dcw / bw, 0.9 * dch / bh});
        const double dw = bw * dsc / dcw, dh = bh * dsc / dch;
        item["hasDualLayout"] = true;
        item["dualFit"] = "Free";
        item["dualX"] = (1.0 - dw) / 2.0; item["dualY"] = (1.0 - dh) / 2.0; item["dualW"] = dw; item["dualH"] = dh;
        item["dualCropLeft"] = 0.0; item["dualCropRight"] = 0.0; item["dualCropTop"] = 0.0; item["dualCropBottom"] = 0.0;
    } else if (type == "AlertBox") {
        // Dimensions are determined dynamically once an alert image/GIF + text is rendered.
        item["x"] = 0.5; item["y"] = 0.25; item["w"] = 0.0; item["h"] = 0.0;
    } else if (m_rasterSizes.contains(sourceId)) {
        const QSize sz = m_rasterSizes.value(sourceId);
        const double scale = std::min(1.0, 0.8 * cw / sz.width());
        const double w = sz.width() * scale / cw, h = sz.height() * scale / ch;
        item["x"] = (1 - w) / 2; item["y"] = 0.42; item["w"] = w; item["h"] = h;
    } else {
        // image/text: size determined after rasterization (w = 0 -> automatic)
        item["x"] = 0.1; item["y"] = 0.42; item["w"] = 0.0; item["h"] = 0.0;
    }
    if (!sourceTypeInfo(type).audio && !item.value("hasDualLayout").toBool() && item.value("w").toDouble() > 0) {
        const QRectF dr = displayRectFor(item, true);
        item["hasDualLayout"] = true;
        item["dualFit"] = item.value("fit").toString("Free");
        item["dualX"] = dr.x(); item["dualY"] = dr.y(); item["dualW"] = dr.width(); item["dualH"] = dr.height();
        item["dualCropLeft"] = 0.0; item["dualCropRight"] = 0.0; item["dualCropTop"] = 0.0; item["dualCropBottom"] = 0.0;
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

void MainWindow::triggerAlert(const QString &kind, const QString &user, const QString &detail)
{
    if (m_alertQueue.size() < 32) m_alertQueue.enqueue({kind, user, detail});
    if (m_alertKind.isEmpty()) showNextAlert();
}

void MainWindow::showNextAlert()
{
    while (!m_alertQueue.isEmpty()) {
        const auto ev = m_alertQueue.dequeue();
        int maxDur = 0;
        QString soundPath, imagePath, alertSourceId;
        double volume = 1.0;
        for (const auto &v : sources()) {
            const auto s = v.toObject();
            if (s["type"].toString() != "AlertBox") continue;
            if (ev.kind == "gift" && !s["alertOnGift"].toBool(true)) continue;
            if (ev.kind == "follow" && !s["alertOnFollow"].toBool(true)) continue;
            maxDur = std::max(maxDur, std::clamp(s["alertDurationSec"].toInt(5), 2, 15));
            volume = s["alertVolume"].toDouble(1.0);
            if (alertSourceId.isEmpty()) alertSourceId = s["id"].toString();
            if (soundPath.isEmpty()) {
                soundPath = s[ev.kind == "gift" ? "alertGiftSoundPath" : "alertFollowSoundPath"].toString();
                if (soundPath.isEmpty()) soundPath = s["alertSoundPath"].toString();
            }
            if (imagePath.isEmpty()) {
                imagePath = s[ev.kind == "gift" ? "alertGiftImagePath" : "alertFollowImagePath"].toString();
                if (imagePath.isEmpty()) imagePath = s["alertImagePath"].toString();
            }
        }
        if (maxDur <= 0) continue;

        if (m_alertMovie) { m_alertMovie->stop(); delete m_alertMovie; m_alertMovie = nullptr; }
        m_alertCachedImg = QImage();
        auto trimAlpha = [](const QImage &srcImg) -> QImage {
            if (srcImg.isNull()) return {};
            const QImage img = srcImg.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
            for (int y = 0; y < img.height(); ++y) {
                const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < img.width(); ++x) {
                    if (qAlpha(line[x]) > 10) {
                        if (x < minX) minX = x;
                        if (x > maxX) maxX = x;
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                    }
                }
            }
            if (maxX < minX || maxY < minY) return img;
            return img.copy(minX, minY, maxX - minX + 1, maxY - minY + 1);
        };
        auto scaleAlertImg = [this, trimAlpha](const QImage &raw) -> QImage {
            if (raw.isNull()) return {};
            const QImage trimmed = trimAlpha(raw);
            const int maxDim = std::min(520, std::max(340, std::min(m_canvasW, m_canvasH) / 2));
            if (trimmed.width() > maxDim || trimmed.height() > maxDim)
                return trimmed.scaled(maxDim, maxDim, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (trimmed.width() < 260 && trimmed.height() < 260)
                return trimmed.scaled(300, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            return trimmed;
        };

        if (!imagePath.isEmpty() && imagePath.endsWith(".gif", Qt::CaseInsensitive) && QFileInfo::exists(imagePath)) {
            m_alertMovie = new QMovie(imagePath, QByteArray(), this);
            if (m_alertMovie->isValid()) {
                connect(m_alertMovie, &QMovie::frameChanged, this, [this, scaleAlertImg] {
                    if (m_alertMovie) m_alertCachedImg = scaleAlertImg(m_alertMovie->currentImage());
                });
                m_alertMovie->start();
                m_alertCachedImg = scaleAlertImg(m_alertMovie->currentImage());
            } else {
                delete m_alertMovie;
                m_alertMovie = nullptr;
            }
        } else if (!imagePath.isEmpty()) {
            m_alertCachedImg = scaleAlertImg(QImage(imagePath));
        }

        // Pre-rasterize outlined bold text ONCE per alert (instead of every frame) so animations run at full 60+ FPS!
        const QColor accent = ev.kind == "gift" ? QColor(254, 44, 85) : QColor(37, 244, 238);
        const int refW = m_alertCachedImg.isNull() ? 500 : std::clamp(m_alertCachedImg.width(), 340, 560);
        m_alertScaleRef = refW / 500.0;
        m_alertPad = std::max(26, int(30 * m_alertScaleRef));
        m_alertGapAfterImg = m_alertCachedImg.isNull() ? 0 : std::max(10, int(12 * m_alertScaleRef));
        m_alertLineGap = 0;

        auto prerenderText = [](const QString &text, int pixelSize, const QColor &fillColor) -> QImage {
            if (text.isEmpty()) return {};
            QFont f("Segoe UI");
            f.setPixelSize(pixelSize);
            f.setWeight(QFont::Bold);
            const QFontMetrics fm(f);
            const qreal outlineW = std::max(4.5, pixelSize * 0.13);
            const int margin = int(std::ceil(outlineW)) + 4;
            const int tw = fm.horizontalAdvance(text);
            const int th = fm.height();
            QImage out(tw + margin * 2, th + margin * 2, QImage::Format_ARGB32_Premultiplied);
            out.fill(Qt::transparent);
            QPainter p(&out);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::TextAntialiasing, true);
            QPainterPath path;
            path.addText(margin, margin + fm.ascent(), f, text);
            p.save();
            p.translate(0, std::max(2.5, pixelSize * 0.06));
            p.strokePath(path, QPen(QColor(0, 0, 0, 155), outlineW * 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.fillPath(path, QColor(0, 0, 0, 155));
            p.restore();
            p.strokePath(path, QPen(QColor(10, 12, 18, 245), outlineW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.fillPath(path, fillColor);
            return out;
        };

        m_alertUserImg = prerenderText(ev.user, std::max(38, int(54 * m_alertScaleRef)), accent);
        m_alertDetailImg = prerenderText(ev.detail, std::max(26, int(36 * m_alertScaleRef)), Qt::white);

        const int contentW = std::max({m_alertCachedImg.width(), m_alertUserImg.width(), m_alertDetailImg.width(), 180});
        const int aw = contentW + m_alertPad * 2;
        const int ah = m_alertPad + m_alertCachedImg.height() + m_alertGapAfterImg
                     + m_alertUserImg.height() + (!m_alertUserImg.isNull() && !m_alertDetailImg.isNull() ? m_alertLineGap : 0)
                     + m_alertDetailImg.height() + m_alertPad;
        m_alertCanvas = QImage(aw, ah, QImage::Format_ARGB32_Premultiplied);

        m_alertKind = ev.kind;
        m_alertUser = ev.user;
        m_alertDetail = ev.detail;
        m_alertStartMs = QDateTime::currentMSecsSinceEpoch();
        m_alertDurMs = maxDur * 1000;
        ++m_alertRevision;
        updateAlertOverlays();
        if (!soundPath.isEmpty())
            EngineHost::instance().call("playAlertSound", {{"sourceId", alertSourceId}, {"path", soundPath}, {"volume", volume}});
        const int targetFps = std::clamp(m_s["fps"].toInt(60), 15, 120);
        m_alertAnimTimer->setInterval(std::max(8, int(std::lround(1000.0 / targetFps))));
        m_alertAnimTimer->start();
        m_alertTimer->start(m_alertDurMs);
        return;
    }
}

void MainWindow::updateAlertOverlays()
{
    for (const auto &v : sources()) {
        const auto s = v.toObject();
        if (s["type"].toString() == "AlertBox") uploadRaster(s);
    }
}

void MainWindow::toggleSelectedItemLock()
{
    if (m_selItem.isEmpty()) return;
    pushUndo();
    editItem([](QJsonObject &i) { i["locked"] = !i["locked"].toBool(); });
    rebuildSourceList();
}

void MainWindow::openBrowserInteractDialog(const QString &sourceId)
{
    const auto src = sourceById(sourceId);
    if (src.isEmpty()) return;
    const int bw = std::max(64, src["browserWidth"].toInt(800));
    const int bh = std::max(64, src["browserHeight"].toInt(600));

    class InteractView final : public QWidget {
    public:
        QString id;
        int bw, bh;
        QImage frame;
        InteractView(const QString &sourceId, int w, int h, QWidget *parent = nullptr)
            : QWidget(parent), id(sourceId), bw(w), bh(h)
        {
            setMouseTracking(true);
            setFocusPolicy(Qt::StrongFocus);
            setMinimumSize(std::min(w, 800), std::min(h, 600));
        }
        QPoint mapPt(const QPointF &p) const
        {
            const int x = std::clamp(int(p.x() * bw / std::max(1, width())), 0, bw - 1);
            const int y = std::clamp(int(p.y() * bh / std::max(1, height())), 0, bh - 1);
            return {x, y};
        }
        void paintEvent(QPaintEvent *) override
        {
            QPainter p(this);
            p.fillRect(rect(), QColor(20, 22, 30));
            if (!frame.isNull()) p.drawImage(rect(), frame);
        }
        void mouseMoveEvent(QMouseEvent *e) override
        {
            const QPoint pt = mapPt(e->position());
            EngineHost::instance().call("browserInteract", {{"id", id}, {"kind", "move"}, {"x", pt.x()}, {"y", pt.y()}});
        }
        void mousePressEvent(QMouseEvent *e) override
        {
            const QPoint pt = mapPt(e->position());
            const int b = e->button() == Qt::MiddleButton ? 1 : (e->button() == Qt::RightButton ? 2 : 0);
            EngineHost::instance().call("browserInteract", {{"id", id}, {"kind", "click"}, {"x", pt.x()}, {"y", pt.y()}, {"button", b}, {"up", false}, {"clicks", 1}});
        }
        void mouseReleaseEvent(QMouseEvent *e) override
        {
            const QPoint pt = mapPt(e->position());
            const int b = e->button() == Qt::MiddleButton ? 1 : (e->button() == Qt::RightButton ? 2 : 0);
            EngineHost::instance().call("browserInteract", {{"id", id}, {"kind", "click"}, {"x", pt.x()}, {"y", pt.y()}, {"button", b}, {"up", true}, {"clicks", 1}});
        }
        void wheelEvent(QWheelEvent *e) override
        {
            const QPoint pt = mapPt(e->position());
            EngineHost::instance().call("browserInteract", {{"id", id}, {"kind", "wheel"}, {"x", pt.x()}, {"y", pt.y()}, {"dx", e->angleDelta().x()}, {"dy", e->angleDelta().y()}});
        }
        void keyPressEvent(QKeyEvent *e) override
        {
            const int vk = int(e->nativeVirtualKey());
            if (vk) EngineHost::instance().call("browserInteract", {{"id", id}, {"kind", "key"}, {"ev", "down"}, {"vk", vk}, {"ch", 0}});
            for (const QChar ch : e->text())
                EngineHost::instance().call("browserInteract", {{"id", id}, {"kind", "key"}, {"ev", "char"}, {"vk", vk}, {"ch", int(ch.unicode())}});
        }
        void keyReleaseEvent(QKeyEvent *e) override
        {
            const int vk = int(e->nativeVirtualKey());
            if (vk) EngineHost::instance().call("browserInteract", {{"id", id}, {"kind", "key"}, {"ev", "up"}, {"vk", vk}, {"ch", 0}});
        }
    };

    QDialog dlg(this);
    dlg.setWindowTitle("Interact — " + src["name"].toString());
    dlg.resize(std::min(bw + 24, 1000), std::min(bh + 56, 760));
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(8, 8, 8, 8);
    auto *view = new InteractView(sourceId, bw, bh, &dlg);
    v->addWidget(view, 1);
    v->addWidget(hintLabel("Click, scroll, or type directly inside the page (e.g. accept cookies or sign in)."));

    auto *timer = new QTimer(&dlg);
    timer->setInterval(60);
    connect(timer, &QTimer::timeout, &dlg, [sourceId, view] {
        const auto r = EngineHost::instance().callObj("browserFrame", {{"id", sourceId}});
        if (!r["ok"].toBool()) return;
        const QByteArray raw = QByteArray::fromBase64(r["bmp"].toString().toLatin1());
        QImage img;
        if (img.loadFromData(raw, "BMP")) {
            view->frame = img;
            view->update();
        }
    });
    timer->start();
    dlg.exec();
}

void MainWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    m_tray = new QSystemTrayIcon(themedIcon("logo"), this);
    m_tray->setToolTip("TikStream");
    auto *menu = new QMenu(this);
    menu->addAction("Show / Hide TikStream", this, [this] {
        if (isVisible() && !isMinimized()) hide();
        else { showNormal(); raise(); activateWindow(); }
    });
    menu->addSeparator();
    menu->addAction("Start / Stop Streaming", this, &MainWindow::toggleStream);
    menu->addAction("Start / Stop Recording", this, &MainWindow::toggleRecord);
    menu->addAction("Start / Stop Replay Buffer", this, &MainWindow::toggleReplay);
    menu->addAction("Save Replay", this, &MainWindow::saveReplay);
    menu->addSeparator();
    menu->addAction("Exit", this, [this] { m_reallyQuit = true; close(); });
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
            showNormal(); raise(); activateWindow();
        }
    });
    m_tray->show();
}

void MainWindow::uploadRaster(const QJsonObject &src)
{
    const QString type = src["type"].toString();
    if (type != "Image" && type != "Text" && type != "ChatOverlay" && type != "AlertBox") return;
    const QString id = src["id"].toString();
    QString key;
    if (type == "ChatOverlay") {
        if (m_liveChat) m_liveChat->ensureStarted();
        key = QString("chat|%1|%2|%3|%4")
                  .arg(src["showChat"].toBool(true))
                  .arg(src["showGifts"].toBool(true))
                  .arg(src["showTaps"].toBool(true))
                  .arg(m_liveChat ? m_liveChat->overlayRevision() : 0);
    } else if (type == "AlertBox") {
        if (m_liveChat) m_liveChat->ensureStarted();
        key = QString("alert|%1|%2|%3").arg(m_alertRevision).arg(src["alertImagePath"].toString()).arg(m_alertKind);
    } else {
        key = rasterKey(src);
    }
    if (m_rasterKeys.value(id) == key && m_rasterSizes.contains(id)) { /* content unchanged */ }
    else {
        m_rasterKeys[id] = key;
        QImage img;
        if (type == "ChatOverlay" && m_liveChat) {
            img = m_liveChat->renderOverlay(540, 720, src["showChat"].toBool(true), src["showGifts"].toBool(true), src["showTaps"].toBool(true));
        } else if (type == "AlertBox") {
            if (!m_alertKind.isEmpty() && !m_alertCanvas.isNull()) {
                const int aw = m_alertCanvas.width();
                const qreal sc = m_alertScaleRef;

                // Smooth easing helpers for entrance, idle motion, and exit
                const double elapsedMs = std::max(0.0, double(QDateTime::currentMSecsSinceEpoch() - m_alertStartMs));
                const double durMs = std::max(1000.0, double(m_alertDurMs));
                const double remainMs = std::max(0.0, durMs - elapsedMs);
                auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
                auto easeOutCubic = [](double t) { return 1.0 - std::pow(1.0 - t, 3.0); };
                auto easeOutBack = [](double t) {
                    const double c1 = 1.70158, c3 = c1 + 1.0;
                    return 1.0 + c3 * std::pow(t - 1.0, 3.0) + c1 * std::pow(t - 1.0, 2.0);
                };

                const double fadeIn = easeOutCubic(clamp01(elapsedMs / 360.0));
                const double fadeOut = easeOutCubic(clamp01(remainMs / 420.0));
                const double masterAlpha = fadeIn * fadeOut;

                const double imgPop = easeOutBack(clamp01(elapsedMs / 480.0));
                const double imgScale = (0.55 + 0.45 * imgPop) * (0.88 + 0.12 * fadeOut);
                const double imgFloatY = std::sin(elapsedMs * 0.0042) * (7.0 * sc);
                const double imgTiltDeg = std::sin(elapsedMs * 0.0028) * 2.6;

                m_alertCanvas.fill(Qt::transparent);
                QPainter p(&m_alertCanvas);
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);

                int y = m_alertPad;
                if (!m_alertCachedImg.isNull()) {
                    p.save();
                    p.setOpacity(masterAlpha);
                    p.translate(aw / 2.0, y + m_alertCachedImg.height() / 2.0 + imgFloatY);
                    p.rotate(imgTiltDeg);
                    p.scale(imgScale, imgScale);
                    p.drawImage(-m_alertCachedImg.width() / 2.0, -m_alertCachedImg.height() / 2.0, m_alertCachedImg);
                    p.restore();
                    y += m_alertCachedImg.height() + m_alertGapAfterImg;
                }

                auto drawAnimText = [&](const QImage &txtImg, int topY, double delayMs, double phase) {
                    if (txtImg.isNull()) return;
                    const double tIn = clamp01((elapsedMs - delayMs) / 420.0);
                    const double eIn = easeOutBack(tIn);
                    const double alpha = easeOutCubic(tIn) * fadeOut;
                    if (alpha <= 0.005) return;
                    const double slideY = (1.0 - easeOutCubic(tIn)) * (26.0 * sc) + std::sin(elapsedMs * 0.0045 + phase) * (3.0 * sc) - (1.0 - fadeOut) * (14.0 * sc);
                    const double txtScale = (0.82 + 0.18 * eIn) * (0.94 + 0.06 * fadeOut);
                    p.save();
                    p.setOpacity(alpha);
                    p.translate(aw / 2.0, topY + txtImg.height() / 2.0 + slideY);
                    p.scale(txtScale, txtScale);
                    p.drawImage(-txtImg.width() / 2.0, -txtImg.height() / 2.0, txtImg);
                    p.restore();
                };

                if (!m_alertUserImg.isNull()) {
                    drawAnimText(m_alertUserImg, y, 70.0, 0.0);
                    y += m_alertUserImg.height() + m_alertLineGap;
                }
                if (!m_alertDetailImg.isNull()) {
                    drawAnimText(m_alertDetailImg, y, 150.0, 1.1);
                }
                img = m_alertCanvas;
            }
        } else {
            img = rasterizeSource(src);
        }
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
            if (it.value("w").toDouble() <= 0) {
                const double cx = 0.5;
                const double cy = type == "AlertBox" ? 0.28 : std::clamp(it.value("y").toDouble(0.42), 0.0, 1.0);
                scale = std::min({1.0, (type == "AlertBox" ? 0.65 : 0.8) * m_canvasW / sz.width(), 0.55 * m_canvasH / sz.height()});
                const double w = sz.width() * scale / m_canvasW, h = sz.height() * scale / m_canvasH;
                it["x"] = std::clamp(cx - w / 2.0, 0.0, std::max(0.0, 1.0 - w));
                it["y"] = std::clamp(cy - h / 2.0, 0.0, std::max(0.0, 1.0 - h));
                it["w"] = w;
                it["h"] = h;
                if (!it.value("hasDualLayout").toBool()) {
                    const QRectF dr = displayRectFor(it, true);
                    it["hasDualLayout"] = true;
                    it["dualFit"] = "Free";
                    it["dualX"] = dr.x(); it["dualY"] = dr.y(); it["dualW"] = dr.width(); it["dualH"] = dr.height();
                    it["dualCropLeft"] = 0.0; it["dualCropRight"] = 0.0; it["dualCropTop"] = 0.0; it["dualCropBottom"] = 0.0;
                }
                items[k] = it;
                changed = true;
                continue;
            } else if (type == "AlertBox") {
                // Keep the user's chosen width and center when switching between Gift and Follow alerts!
                const double cx = it["x"].toDouble() + it["w"].toDouble() / 2.0;
                const double cy = it["y"].toDouble() + it["h"].toDouble() / 2.0;
                const double w = it["w"].toDouble();
                const double h = w * m_canvasW * (double(sz.height()) / std::max(1, sz.width())) / m_canvasH;
                it["x"] = std::clamp(cx - w / 2.0, 0.0, std::max(0.0, 1.0 - w));
                it["y"] = std::clamp(cy - h / 2.0, 0.0, std::max(0.0, 1.0 - h));
                it["h"] = h;
                items[k] = it;
                changed = true;
                continue;
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
    else if (action == "toggleReplay") toggleReplay();
    else if (action == "saveReplay") saveReplay();
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
    return displayRectFor(item, false);
}

QRectF MainWindow::displayRectFor(const QJsonObject &item, bool dual) const
{
    const double cw = dual ? std::max(128, m_s["dualCanvasWidth"].toInt(1920)) : m_canvasW;
    const double ch = dual ? std::max(128, m_s["dualCanvasHeight"].toInt(1080)) : m_canvasH;
    const bool hasDual = dual && item["hasDualLayout"].toBool();
    const QString fit = hasDual ? item["dualFit"].toString("Free") : item["fit"].toString("Free");
    if (fit == "Fill") return QRectF(0, 0, 1, 1);
    if (fit == "FitBlur") {
        const QSizeF ns = naturalSize(item["sourceId"].toString());
        const double s = std::min(cw / ns.width(), ch / ns.height());
        const double w = ns.width() * s / cw, h = ns.height() * s / ch;
        return QRectF((1 - w) / 2, (1 - h) / 2, w, h);
    }
    if (hasDual)
        return QRectF(item["dualX"].toDouble(), item["dualY"].toDouble(), item["dualW"].toDouble(1), item["dualH"].toDouble(1));
    QRectF r = itemRect(item);
    if (dual && m_canvasW > 0 && m_canvasH > 0) {
        const double aspectRatio = (double(m_canvasW) / m_canvasH) / (cw / ch);
        const double cx = r.center().x(), cy = r.center().y();
        double nw = r.width() * aspectRatio, nh = r.height();
        if (nw > 0.96) { const double k = 0.96 / nw; nw *= k; nh *= k; }
        r = QRectF(cx - nw * 0.5, cy - nh * 0.5, nw, nh);
    }
    return r;
}

QJsonObject MainWindow::makeFree(QJsonObject item, bool dual) const
{
    if (item.isEmpty()) return item;
    if (!item.value("hasDualLayout").toBool()) {
        const QRectF dr = displayRectFor(item, true);
        item["hasDualLayout"] = true;
        item["dualFit"] = dual ? QStringLiteral("Free") : item.value("fit").toString("Free");
        item["dualX"] = dr.x(); item["dualY"] = dr.y(); item["dualW"] = dr.width(); item["dualH"] = dr.height();
        item["dualCropLeft"] = item.value("cropLeft").toDouble();
        item["dualCropRight"] = item.value("cropRight").toDouble();
        item["dualCropTop"] = item.value("cropTop").toDouble();
        item["dualCropBottom"] = item.value("cropBottom").toDouble();
    }
    if (dual) {
        if (item.value("dualFit").toString("Free") == "Free") return item;
        const QRectF r = displayRectFor(item, true);
        item["dualFit"] = "Free";
        item["dualX"] = r.x(); item["dualY"] = r.y(); item["dualW"] = r.width(); item["dualH"] = r.height();
        return item;
    }
    if (item.value("fit").toString("Free") == "Free") return item;
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
    const bool show = !item.isEmpty() && visual && item["visible"].toBool(true);
    EngineHost::instance().call("setSelection", show ? rectObj(displayRectFor(item, false)) : QJsonObject{});
    if (m_dualPreviewDock && m_dualPreviewDock->isVisible())
        EngineHost::instance().call("setDualSelection", show ? rectObj(displayRectFor(item, true)) : QJsonObject{});
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
    onCanvasPressedAt(p, false);
}

void MainWindow::onCanvasPressedAt(QPointF p, bool dual)
{
    m_drag = Drag::None;
    m_dragDual = dual;
    if (m_applyTimer->isActive()) pushSettings();
    PreviewWidget *pw = dual && m_dualPreview ? m_dualPreview : m_preview;
    const double cw = dual ? std::max(128, m_s["dualCanvasWidth"].toInt(1920)) : m_canvasW;
    const double ch = dual ? std::max(128, m_s["dualCanvasHeight"].toInt(1080)) : m_canvasH;
    const double s = std::min(pw->width() / cw, pw->height() / ch);
    const double tolX = 12.0 / (cw * s), tolY = 12.0 / (ch * s);
    const bool altHeld = (QApplication::keyboardModifiers() & Qt::AltModifier) != 0;

    // 1. selected item: Alt + edge = crop, otherwise corner handle = resize
    const QJsonObject sel = selectedItem();
    if (!sel.isEmpty() && sel["visible"].toBool(true) && !sel["locked"].toBool() && !sourceTypeInfo(selectedSource()["type"].toString()).audio) {
        const QRectF r = displayRectFor(sel, dual);
        if (altHeld) {
            const double cTolX = tolX * 1.4, cTolY = tolY * 1.4;
            const bool inY = p.y() >= r.top() - cTolY && p.y() <= r.bottom() + cTolY;
            const bool inX = p.x() >= r.left() - cTolX && p.x() <= r.right() + cTolX;
            Drag cropDrag = Drag::None;
            if (inY && std::abs(p.x() - r.left()) <= cTolX) cropDrag = Drag::CropLeft;
            else if (inY && std::abs(p.x() - r.right()) <= cTolX) cropDrag = Drag::CropRight;
            else if (inX && std::abs(p.y() - r.top()) <= cTolY) cropDrag = Drag::CropTop;
            else if (inX && std::abs(p.y() - r.bottom()) <= cTolY) cropDrag = Drag::CropBottom;
            if (cropDrag != Drag::None) {
                pushUndo();
                m_drag = cropDrag;
                m_dragStart = p;
                m_startRect = r;
                const bool useDual = dual && sel["hasDualLayout"].toBool();
                m_startCropL = sel[useDual ? "dualCropLeft" : "cropLeft"].toDouble();
                m_startCropR = sel[useDual ? "dualCropRight" : "cropRight"].toDouble();
                m_startCropT = sel[useDual ? "dualCropTop" : "cropTop"].toDouble();
                m_startCropB = sel[useDual ? "dualCropBottom" : "cropBottom"].toDouble();
                return;
            }
        }
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
        const QRectF r = displayRectFor(it, dual);
        if (!r.contains(p)) continue;
        if (it["id"].toString() != m_selItem) selectItem(it["id"].toString());
        if (it["locked"].toBool()) return;
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
    QJsonObject item = makeFree(selectedItem(), m_dragDual);
    if (item.isEmpty()) return;
    const bool snapScreen = m_s["snapping"].toBool(true);
    const bool snapSources = m_s["snapToSources"].toBool(true);
    const double tol = 0.012;
    QRectF r = m_startRect;

    QList<double> targetsX, targetsY;
    if (snapScreen) { targetsX << 0.0 << 0.5 << 1.0; targetsY << 0.0 << 0.5 << 1.0; }
    if (snapSources) {
        for (const auto &v : activeScene()["items"].toArray()) {
            const auto other = v.toObject();
            if (other["id"].toString() == m_selItem || !other["visible"].toBool(true)) continue;
            if (sourceTypeInfo(sourceById(other["sourceId"].toString())["type"].toString()).audio) continue;
            const QRectF orr = displayRectFor(other, m_dragDual);
            targetsX << orr.left() << orr.center().x() << orr.right();
            targetsY << orr.top() << orr.center().y() << orr.bottom();
        }
    }

    if (m_drag == Drag::CropLeft || m_drag == Drag::CropRight || m_drag == Drag::CropTop || m_drag == Drag::CropBottom) {
        double cl = m_startCropL, cr = m_startCropR, ct = m_startCropT, cb = m_startCropB;
        const double visU = std::max(0.02, 1.0 - m_startCropL - m_startCropR);
        const double visV = std::max(0.02, 1.0 - m_startCropT - m_startCropB);
        const double fullW = m_startRect.width() / visU;
        const double fullH = m_startRect.height() / visV;
        const double maxSum = 0.98;
        if (m_drag == Drag::CropLeft) {
            const double dx = p.x() - m_dragStart.x();
            cl = std::clamp(m_startCropL + dx / std::max(1e-4, fullW), 0.0, std::max(0.0, maxSum - cr));
            r.setLeft(m_startRect.left() + (cl - m_startCropL) * fullW);
        } else if (m_drag == Drag::CropRight) {
            const double dx = m_dragStart.x() - p.x();
            cr = std::clamp(m_startCropR + dx / std::max(1e-4, fullW), 0.0, std::max(0.0, maxSum - cl));
            r.setRight(m_startRect.right() - (cr - m_startCropR) * fullW);
        } else if (m_drag == Drag::CropTop) {
            const double dy = p.y() - m_dragStart.y();
            ct = std::clamp(m_startCropT + dy / std::max(1e-4, fullH), 0.0, std::max(0.0, maxSum - cb));
            r.setTop(m_startRect.top() + (ct - m_startCropT) * fullH);
        } else if (m_drag == Drag::CropBottom) {
            const double dy = m_dragStart.y() - p.y();
            cb = std::clamp(m_startCropB + dy / std::max(1e-4, fullH), 0.0, std::max(0.0, maxSum - ct));
            r.setBottom(m_startRect.bottom() - (cb - m_startCropB) * fullH);
        }
        if (m_dragDual) {
            item["hasDualLayout"] = true;
            item["dualFit"] = "Free";
            item["dualX"] = r.x(); item["dualY"] = r.y(); item["dualW"] = std::max(0.005, r.width()); item["dualH"] = std::max(0.005, r.height());
            item["dualCropLeft"] = cl; item["dualCropRight"] = cr; item["dualCropTop"] = ct; item["dualCropBottom"] = cb;
        } else {
            item["x"] = r.x(); item["y"] = r.y(); item["w"] = std::max(0.005, r.width()); item["h"] = std::max(0.005, r.height());
            item["cropLeft"] = cl; item["cropRight"] = cr; item["cropTop"] = ct; item["cropBottom"] = cb;
        }
        setItem(item);
        m_dragChanged = true;
        if (!m_dragTimer->isActive()) {
            sendDragTransform();
            m_dragTimer->start();
        }
        return;
    }

    if (m_drag == Drag::Move) {
        r.moveTopLeft(m_startRect.topLeft() + (p - m_dragStart));
        if (!targetsX.isEmpty()) {
            double d = 0;
            snap(r.left(), targetsX, tol, &d);
            if (d == 0) snap(r.right(), targetsX, tol, &d);
            if (d == 0) snap(r.center().x(), targetsX, tol, &d);
            r.translate(d, 0);
        }
        if (!targetsY.isEmpty()) {
            double d = 0;
            snap(r.top(), targetsY, tol, &d);
            if (d == 0) snap(r.bottom(), targetsY, tol, &d);
            if (d == 0) snap(r.center().y(), targetsY, tol, &d);
            r.translate(0, d);
        }
    } else {
        QPointF c = p;
        if (!targetsX.isEmpty()) c.setX(snap(c.x(), targetsX, tol));
        if (!targetsY.isEmpty()) c.setY(snap(c.y(), targetsY, tol));
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
    if (m_dragDual) {
        item["hasDualLayout"] = true;
        item["dualFit"] = "Free";
        item["dualX"] = r.x(); item["dualY"] = r.y(); item["dualW"] = r.width(); item["dualH"] = r.height();
    } else {
        item["x"] = r.x(); item["y"] = r.y(); item["w"] = r.width(); item["h"] = r.height();
    }
    setItem(item);
    m_dragChanged = true;
    if (!m_dragTimer->isActive()) {
        sendDragTransform();
        m_dragTimer->start();
    }
    if (!m_dragDual && m_fit->currentData().toString() != "Free") { m_loading = true; m_fit->setCurrentIndex(0); m_loading = false; }
}

void MainWindow::sendDragTransform()
{
    const auto item = selectedItem();
    if (m_drag == Drag::None || item.isEmpty()) return;
    const QString sceneId = activeScene()["id"].toString();
    const QString itemId = item["id"].toString();
    if (m_drag == Drag::CropLeft || m_drag == Drag::CropRight || m_drag == Drag::CropTop || m_drag == Drag::CropBottom) {
        const QRectF r = displayRectFor(item, m_dragDual);
        EngineHost::instance().call("setItemCrop", {
            {"sceneId", sceneId}, {"itemId", itemId}, {"dual", m_dragDual},
            {"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()},
            {"cl", item[m_dragDual ? "dualCropLeft" : "cropLeft"].toDouble()},
            {"cr", item[m_dragDual ? "dualCropRight" : "cropRight"].toDouble()},
            {"ct", item[m_dragDual ? "dualCropTop" : "cropTop"].toDouble()},
            {"cb", item[m_dragDual ? "dualCropBottom" : "cropBottom"].toDouble()},
        });
        return;
    }
    if (m_dragDual) {
        EngineHost::instance().call("setDualItemTransform", {
            {"sceneId", sceneId}, {"itemId", itemId},
            {"x", item["dualX"]}, {"y", item["dualY"]}, {"w", item["dualW"]}, {"h", item["dualH"]},
            {"fit", item["dualFit"].toString("Free")},
        });
    } else {
        auto args = rectObj(itemRect(item));
        args["sceneId"] = sceneId;
        args["itemId"] = itemId;
        args["fit"] = item["fit"];
        EngineHost::instance().call("setItemTransform", args);
    }
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
    if (m_selItem.isEmpty() || selectedItem()["locked"].toBool() || sourceTypeInfo(selectedSource()["type"].toString()).audio) return;
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
    onCanvasWheelAt(delta, false);
}

void MainWindow::onCanvasWheelAt(int delta, bool dual)
{
    if (m_selItem.isEmpty() || selectedItem()["locked"].toBool() || sourceTypeInfo(selectedSource()["type"].toString()).audio) return;
    const double f = delta > 0 ? 1.06 : 1 / 1.06;
    editItem([this, f, dual](QJsonObject &i) {
        i = makeFree(i, dual);
        QRectF r = displayRectFor(i, dual);
        const QPointF c = r.center();
        r.setSize(QSizeF(std::max(0.02, r.width() * f), std::max(0.02, r.height() * f)));
        r.moveCenter(c);
        if (dual) { i["dualX"] = r.x(); i["dualY"] = r.y(); i["dualW"] = r.width(); i["dualH"] = r.height(); }
        else { i["x"] = r.x(); i["y"] = r.y(); i["w"] = r.width(); i["h"] = r.height(); }
    });
    if (!dual && m_fit->currentData().toString() != "Free") { m_loading = true; m_fit->setCurrentIndex(0); m_loading = false; }
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

void MainWindow::toggleReplay()
{
    m_btnReplay->setEnabled(false);
    pushSettings();
    const auto r = EngineHost::instance().callObj(m_replayActive ? "stopReplay" : "startReplay");
    if (r.contains("error")) QMessageBox::warning(this, "Replay Buffer", r["error"].toString());
    m_btnReplay->setEnabled(true);
    pollStats();
}

void MainWindow::saveReplay()
{
    if (!m_replayActive) return;
    m_btnSaveReplay->setEnabled(false);
    const auto r = EngineHost::instance().callObj("saveReplay");
    m_btnSaveReplay->setEnabled(m_replayActive);
    if (r.contains("error")) {
        QMessageBox::warning(this, "Replay Buffer", r["error"].toString());
    } else {
        const QString saved = QFileInfo(r["path"].toString()).fileName();
        m_stMsg->setText("Replay saved: " + saved);
    }
}

void MainWindow::openSettings()
{
    m_audioDevices = EngineHost::instance().callObj("listAudioDevices");
    SettingsDialog dlg(m_s, m_audioDevices, m_streaming || m_recording || m_replayActive, this);
    connect(&dlg, &SettingsDialog::checkForUpdatesRequested, this, [this] { checkForUpdates(true); });
    connect(&dlg, &SettingsDialog::applied, this, [this](const QJsonObject &s) {
        m_s = s;
        pushSettings();
        applyHotkeys();
        m_previewStack->setCurrentIndex(m_s["previewEnabled"].toBool(true) ? 0 : 1);
        m_mixerSignature.clear();
        rebuildMixer();
        loadProperties();
        syncDualPreviewDock();
    });
    dlg.exec();
}

void MainWindow::checkForUpdates(bool manual)
{
    if (m_updateCheckPending) return;
    m_updateCheckPending = true;
    auto res = std::make_shared<QJsonObject>();
    auto *worker = QThread::create([res] {
        *res = EngineHost::instance().callObj("checkUpdate");
    });
    connect(worker, &QThread::finished, this, [this, res, manual] {
        m_updateCheckPending = false;
        const QJsonObject info = *res;
        const QString err = info["error"].toString();
        if (!err.isEmpty()) {
            if (manual) QMessageBox::warning(this, "Updates", "Could not check for updates:\n" + err);
            return;
        }
        const QString latest = info["latestVersion"].toString();
        const QString current = info["currentVersion"].toString();
        if (info["updateAvailable"].toBool()) {
            if (!manual && latest == m_skippedUpdateVersion) return;
            showUpdateDialog(info);
        } else if (manual) {
            QMessageBox::information(this, "Updates",
                QString("You are using the latest version of TikStream (v%1).").arg(current.isEmpty() ? latest : current));
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::showUpdateDialog(const QJsonObject &info)
{
    const QString current = info["currentVersion"].toString();
    const QString latest = info["latestVersion"].toString();
    const QString releaseUrl = info["releaseUrl"].toString();
    const QString downloadUrl = info["downloadUrl"].toString();
    const qint64 sizeBytes = qint64(info["downloadSize"].toDouble());

    QDialog dlg(this);
    dlg.setWindowTitle("TikStream Update Available");
    dlg.resize(560, 420);
    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(10);

    auto *title = new QLabel(QString("<b>TikStream v%1 is available</b> (installed: v%2)").arg(latest, current));
    v->addWidget(title);

    auto *notes = new QPlainTextEdit;
    notes->setReadOnly(true);
    notes->setPlainText(info["notes"].toString("New version available on GitHub Releases."));
    v->addWidget(notes, 1);

    auto *statusLbl = hintLabel(sizeBytes > 0
        ? QString("Installer size: %1 MB. Your scenes and settings will be preserved.").arg(sizeBytes / 1024.0 / 1024.0, 0, 'f', 1)
        : "Your scenes and settings will be preserved during upgrade.");
    v->addWidget(statusLbl);

    auto *progress = new QProgressBar;
    progress->setRange(0, 100);
    progress->setValue(0);
    progress->hide();
    v->addWidget(progress);

    auto *buttons = new QDialogButtonBox;
    auto *installBtn = buttons->addButton("Download & Install", QDialogButtonBox::AcceptRole);
    installBtn->setObjectName("primary");
    installBtn->setEnabled(!downloadUrl.isEmpty());
    auto *webBtn = buttons->addButton("Open Release Page", QDialogButtonBox::ActionRole);
    auto *laterBtn = buttons->addButton("Later", QDialogButtonBox::RejectRole);
    v->addWidget(buttons);

    auto *poll = new QTimer(&dlg);
    poll->setInterval(250);
    connect(poll, &QTimer::timeout, &dlg, [this, &dlg, poll, progress, statusLbl, installBtn, laterBtn] {
        const auto st = EngineHost::instance().callObj("updateStatus");
        progress->setValue(st["progress"].toInt());
        statusLbl->setText(st["status"].toString());
        const QString err = st["error"].toString();
        if (!err.isEmpty()) {
            poll->stop();
            statusLbl->setText("Download failed: " + err);
            installBtn->setEnabled(true);
            laterBtn->setEnabled(true);
            return;
        }
        if (st["ready"].toBool()) {
            poll->stop();
            if (m_streaming || m_recording) {
                QMessageBox::information(&dlg, "Update Ready",
                    "The update has been downloaded. Stop streaming or recording, then click Download & Install again to apply it.");
                installBtn->setText("Install Now");
                installBtn->setEnabled(true);
                laterBtn->setEnabled(true);
                return;
            }
            const auto r = EngineHost::instance().callObj("installUpdate");
            if (r.contains("error")) {
                QMessageBox::warning(&dlg, "Update", r["error"].toString());
                installBtn->setEnabled(true);
                laterBtn->setEnabled(true);
                return;
            }
            dlg.accept();
            QTimer::singleShot(300, this, &QWidget::close);
        }
    });

    connect(installBtn, &QPushButton::clicked, &dlg, [this, &dlg, downloadUrl, poll, progress, statusLbl, installBtn, laterBtn] {
        if (m_streaming || m_recording) {
            QMessageBox::warning(&dlg, "Update", "Please stop streaming or recording before installing an update.");
            return;
        }
        const auto st = EngineHost::instance().callObj("updateStatus");
        if (st["ready"].toBool()) {
            const auto r = EngineHost::instance().callObj("installUpdate");
            if (r.contains("error")) { QMessageBox::warning(&dlg, "Update", r["error"].toString()); return; }
            dlg.accept();
            QTimer::singleShot(300, this, &QWidget::close);
            return;
        }
        installBtn->setEnabled(false);
        laterBtn->setEnabled(false);
        progress->setValue(0);
        progress->show();
        statusLbl->setText("Starting download…");
        const auto r = EngineHost::instance().callObj("startUpdate", {{"url", downloadUrl}});
        if (r.contains("error")) {
            QMessageBox::warning(&dlg, "Update", r["error"].toString());
            installBtn->setEnabled(true);
            laterBtn->setEnabled(true);
            return;
        }
        poll->start();
    });
    connect(webBtn, &QPushButton::clicked, &dlg, [releaseUrl] {
        if (!releaseUrl.isEmpty()) QDesktopServices::openUrl(QUrl(releaseUrl));
    });
    connect(laterBtn, &QPushButton::clicked, &dlg, [this, &dlg, latest] {
        m_skippedUpdateVersion = latest;
        dlg.reject();
    });
    dlg.exec();
}

void MainWindow::updateLiveButtons()
{
    m_btnStream->setText(m_streaming ? "  Stop Streaming" : "  Start Streaming");
    m_btnStream->setProperty("live", m_streaming);
    m_btnRecord->setText(m_recording ? "  Stop Recording" : "  Start Recording");
    m_btnRecord->setProperty("live", m_recording);
    m_btnReplay->setText(m_replayActive ? "  Stop Replay Buffer" : "  Start Replay Buffer");
    m_btnReplay->setProperty("live", m_replayActive);
    m_btnSaveReplay->setEnabled(m_replayActive);
    for (auto *b : {m_btnStream, m_btnRecord, m_btnReplay}) { b->style()->unpolish(b); b->style()->polish(b); }
    m_stLive->setProperty("on", m_streaming);
    m_stRec->setProperty("on", m_recording);
    for (auto *l : {m_stLive, m_stRec}) { l->style()->unpolish(l); l->style()->polish(l); }
}

void MainWindow::pollStats()
{
    const auto st = EngineHost::instance().callObj("stats");
    if (st.isEmpty()) return;
    const bool s = st["streaming"].toBool(), r = st["recording"].toBool(), rep = st["replayActive"].toBool();
    if (s != m_streaming || r != m_recording || rep != m_replayActive) { m_streaming = s; m_recording = r; m_replayActive = rep; updateLiveButtons(); }

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

    auto pushHist = [](QList<double> &h, double val) { h.append(val); if (h.size() > 60) h.removeFirst(); };
    pushHist(m_histFps, st["renderFps"].toDouble());
    pushHist(m_histRenderMs, st["renderMs"].toDouble());
    pushHist(m_histKbps, kbps);
    if (m_statsGraphWidget) m_statsGraphWidget->update();

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

void MainWindow::exportSceneCollection()
{
    const QString path = QFileDialog::getSaveFileName(this, "Export Scene Collection", "TikStream-Scenes.json", "Scene Collections (*.json)");
    if (path.isEmpty()) return;
    QJsonObject doc{
        {"format", "tikstream-scene-collection"},
        {"version", 1},
        {"activeSceneId", m_s["activeSceneId"]},
        {"scenes", m_s["scenes"]},
        {"sources", m_s["sources"]},
    };
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { QMessageBox::warning(this, "Export", "Could not write file."); return; }
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    m_stMsg->setText("Scene collection exported: " + QFileInfo(path).fileName());
}

void MainWindow::importSceneCollection()
{
    const QString path = QFileDialog::getOpenFileName(this, "Import Scene Collection", {}, "Scene Collections (*.json)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, "Import", "Could not read file."); return; }
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    if (!doc["scenes"].isArray() || doc["scenes"].toArray().isEmpty()) {
        QMessageBox::warning(this, "Import", "Invalid scene collection file.");
        return;
    }
    if (QMessageBox::question(this, "Import Scene Collection", "Replace current scenes and sources with the imported collection?") != QMessageBox::Yes)
        return;
    pushUndo();
    m_s["scenes"] = doc["scenes"];
    if (doc["sources"].isArray()) m_s["sources"] = doc["sources"];
    if (!doc["activeSceneId"].toString().isEmpty()) m_s["activeSceneId"] = doc["activeSceneId"];
    rebuildScenes();
    rebuildSourceList();
    rebuildMixer();
    for (const auto &v : sources()) uploadRaster(v.toObject());
    loadProperties();
    pushSettings();
    m_stMsg->setText("Scene collection imported: " + QFileInfo(path).fileName());
}

void MainWindow::openStatsGraphDialog()
{
    if (m_statsGraphWidget && m_statsGraphWidget->window()) {
        m_statsGraphWidget->window()->showNormal();
        m_statsGraphWidget->window()->raise();
        m_statsGraphWidget->window()->activateWindow();
        return;
    }
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("TikStream — Stats & Performance Graph");
    dlg->resize(640, 420);
    auto *v = new QVBoxLayout(dlg);
    v->setContentsMargins(12, 12, 12, 12);

    class GraphWidget final : public QWidget {
    public:
        const QList<double> *fps, *ms, *kbps;
        GraphWidget(const QList<double> *f, const QList<double> *m, const QList<double> *k, QWidget *p)
            : QWidget(p), fps(f), ms(m), kbps(k) {}
        void paintEvent(QPaintEvent *) override
        {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.fillRect(rect(), QColor(18, 20, 28));
            auto drawChart = [&](const QRectF &r, const QString &title, const QList<double> &data, double maxVal, const QColor &col, const QString &unit) {
                p.setPen(QPen(QColor(50, 54, 72), 1));
                p.setBrush(QColor(24, 27, 38));
                p.drawRoundedRect(r, 8, 8);
                const double latest = data.isEmpty() ? 0.0 : data.last();
                p.setPen(col);
                p.setFont(QFont("Segoe UI", 10, QFont::Bold));
                p.drawText(r.adjusted(10, 6, -10, 0), Qt::AlignLeft | Qt::AlignTop,
                           QString("%1: %2 %3").arg(title).arg(latest, 0, 'f', unit == "ms" ? 2 : 1).arg(unit));
                if (data.size() < 2) return;
                const QRectF plot = r.adjusted(10, 28, -10, -8);
                double hi = maxVal;
                for (double v : data) hi = std::max(hi, v * 1.15);
                if (hi <= 0.001) hi = 1.0;
                p.setPen(QPen(col, 2.0));
                for (int i = 1; i < data.size(); ++i) {
                    const double x1 = plot.left() + plot.width() * (i - 1) / 59.0;
                    const double x2 = plot.left() + plot.width() * i / 59.0;
                    const double y1 = plot.bottom() - plot.height() * std::clamp(data[i - 1] / hi, 0.0, 1.0);
                    const double y2 = plot.bottom() - plot.height() * std::clamp(data[i] / hi, 0.0, 1.0);
                    p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
                }
            };
            const double h = (height() - 24) / 3.0;
            drawChart(QRectF(4, 4, width() - 8, h - 4), "Render FPS", *fps, 65.0, QColor(80, 230, 140), "FPS");
            drawChart(QRectF(4, 12 + h, width() - 8, h - 4), "Frame Render Time", *ms, 16.6, QColor(37, 244, 238), "ms");
            drawChart(QRectF(4, 20 + 2 * h, width() - 8, h - 4), "Stream Bitrate", *kbps, 8000.0, QColor(254, 44, 85), "kbps");
        }
    };

    auto *gw = new GraphWidget(&m_histFps, &m_histRenderMs, &m_histKbps, dlg);
    m_statsGraphWidget = gw;
    v->addWidget(gw, 1);
    dlg->show();
}

void MainWindow::ensureDockOnScreen(QDockWidget *dock)
{
    if (!dock || !dock->isFloating()) return;
    if (dock->width() < 200 || dock->height() < 120) {
        if (dock == m_dualPreviewDock) dock->resize(560, 360);
        else dock->resize(std::max(dock->width(), 280), std::max(dock->height(), 220));
    }
    const QRect g = dock->frameGeometry();
    bool onAnyScreen = false;
    for (QScreen *scr : QGuiApplication::screens()) {
        const QRect inter = g.intersected(scr->availableGeometry());
        if (inter.width() >= 80 && inter.height() >= 48) { onAnyScreen = true; break; }
    }
    if (!onAnyScreen) {
        QScreen *target = screen() ? screen() : QGuiApplication::primaryScreen();
        if (target) {
            const QRect av = target->availableGeometry();
            const int w = std::min(dock->width(), av.width() - 40);
            const int h = std::min(dock->height(), av.height() - 40);
            dock->resize(w, h);
            dock->move(av.center().x() - w / 2, av.center().y() - h / 2);
        }
    }
}

void MainWindow::saveDockLayout()
{
    if (QCoreApplication::arguments().contains("--test")) return;
    QSettings qs("TikStream", "UI2");
    qs.setValue("geometry", saveGeometry());
    qs.setValue("state", saveState());
    qs.setValue("compactLayoutV1", true);
    for (auto *dock : findChildren<QDockWidget *>()) {
        if (dock->objectName().isEmpty()) continue;
        if (dock->isFloating()) qs.setValue("dockGeo/" + dock->objectName(), dock->saveGeometry());
        else qs.remove("dockGeo/" + dock->objectName());
    }
    qs.sync();
}

void MainWindow::syncDualPreviewDock()
{
    if (!m_dualPreviewDock || !m_dualPreview) return;
    const bool dualActive = m_s["dualCanvasEnabled"].toBool(false) || m_s["secondStreamEnabled"].toBool(false);
    const bool showDock = dualActive && m_s["dualCanvasPreviewEnabled"].toBool(true) && !QCoreApplication::arguments().contains("--test");
    m_dualPreview->setCanvasSize(m_s["dualCanvasWidth"].toInt(1920), m_s["dualCanvasHeight"].toInt(1080));
    if (showDock) {
        if (!m_dualDockPositioned && m_dualPreviewDock->isFloating()) {
            m_dualDockPositioned = true;
            m_dualPreviewDock->resize(560, 360);
            QScreen *target = screen() ? screen() : QGuiApplication::primaryScreen();
            const QRect av = target ? target->availableGeometry() : QRect(80, 80, 1280, 720);
            m_dualPreviewDock->move(av.right() - 580, av.top() + 60);
        }
        m_dualPreviewDock->show();
        if (m_dualPreviewDock->isFloating()) {
            ensureDockOnScreen(m_dualPreviewDock);
            m_dualPreviewDock->raise();
        }
    } else {
        m_dualPreviewDock->hide();
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (!m_reallyQuit && m_s["minimizeToTray"].toBool(false) && m_tray && m_tray->isVisible()) {
        saveDockLayout();
        hide();
        e->ignore();
        return;
    }
    if ((m_streaming || m_recording) &&
        QMessageBox::question(this, "TikStream", "Streaming or recording is active. Stop and exit?") != QMessageBox::Yes) {
        m_reallyQuit = false;
        e->ignore();
        return;
    }
    if (m_tray) m_tray->hide();
    m_statsTimer->stop();
    m_levelsTimer->stop();
    pushSettings();
    saveDockLayout();
    e->accept();
}
