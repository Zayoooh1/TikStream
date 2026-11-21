#include "SettingsDialog.h"

#include "EngineHost.h"
#include "Hotkeys.h"
#include "Widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <numeric>
#include <memory>

namespace {
QString loginBrowser()
{
    const QStringList candidates{
        QStringLiteral("chrome.exe"), QStringLiteral("msedge.exe"), QStringLiteral("brave.exe")
    };
    for (const auto &name : candidates) {
        const QString key = "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + name;
        for (const auto &root : {QStringLiteral("HKEY_CURRENT_USER"), QStringLiteral("HKEY_LOCAL_MACHINE")}) {
            QSettings registry(root + '\\' + key, QSettings::NativeFormat);
            const QString path = registry.value(".").toString();
            if (QFileInfo(path).isFile()) return path;
        }
    }
    const QStringList installs{
        QStringLiteral("Google/Chrome/Application/chrome.exe"),
        QStringLiteral("Microsoft/Edge/Application/msedge.exe"),
        QStringLiteral("BraveSoftware/Brave-Browser/Application/brave.exe")
    };
    for (const auto &root : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"),
                            qEnvironmentVariable("LOCALAPPDATA")}) {
        if (root.isEmpty()) continue;
        for (const auto &install : installs) {
            const QString path = QDir(root).filePath(install);
            if (QFileInfo(path).isFile()) return path;
        }
    }
    return {};
}

QFrame *card(QWidget *parent, QFormLayout **form, const QString &title, const QString &subtitle = {})
{
    auto *f = new QFrame(parent);
    f->setObjectName("card");
    auto *v = new QVBoxLayout(f);
    v->setContentsMargins(18, 14, 18, 16);
    v->setSpacing(10);
    auto *t = new QLabel(title, f);
    t->setObjectName("cardTitle");
    v->addWidget(t);
    if (!subtitle.isEmpty()) {
        auto *s = new QLabel(subtitle, f);
        s->setObjectName("hint");
        s->setWordWrap(true);
        v->addWidget(s);
    }
    *form = new QFormLayout();
    (*form)->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    (*form)->setHorizontalSpacing(16);
    (*form)->setVerticalSpacing(10);
    (*form)->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    v->addLayout(*form);
    return f;
}

QWidget *page(QVBoxLayout **v)
{
    auto *w = new QWidget;
    *v = new QVBoxLayout(w);
    (*v)->setContentsMargins(0, 0, 10, 0);
    (*v)->setSpacing(12);
    return w;
}

QWidget *scrollPage(QWidget *content, QVBoxLayout *v)
{
    v->addStretch();
    auto *sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidget(content);
    return sa;
}

QLabel *hint(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName("hint");
    l->setWordWrap(true);
    return l;
}

QLabel *value(const QString &text)
{
    auto *l = new QLabel(text);
    l->setObjectName("valueLabel");
    return l;
}

QJsonObject firstSourceOfType(const QJsonArray &sources, const QString &type)
{
    for (const auto &v : sources)
        if (v.toObject()["type"].toString() == type && v.toObject()["globalAudio"].toBool()) return v.toObject();
    return {};
}
} // namespace

template <typename W>
W *SettingsDialog::track(W *w)
{
    if constexpr (std::is_base_of_v<QCheckBox, W>) connect(w, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    else if constexpr (std::is_base_of_v<QComboBox, W>) connect(w, &QComboBox::currentIndexChanged, this, &SettingsDialog::markDirty);
    else if constexpr (std::is_base_of_v<QSpinBox, W>) connect(w, &QSpinBox::valueChanged, this, &SettingsDialog::markDirty);
    else if constexpr (std::is_base_of_v<QLineEdit, W>) connect(w, &QLineEdit::textChanged, this, &SettingsDialog::markDirty);
    return w;
}

SettingsDialog::SettingsDialog(const QJsonObject &settings, const QJsonObject &audioDevices, bool live, QWidget *parent)
    : QDialog(parent), m_s(settings), m_devices(audioDevices), m_live(live)
{
    setWindowTitle("Ustawienia");
    resize(980, 700);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto *body = new QHBoxLayout();
    body->setSpacing(12);
    m_nav = new QListWidget(this);
    m_nav->setObjectName("settingsNav");
    m_nav->setFixedWidth(210);
    m_nav->setIconSize(QSize(20, 20));
    m_pages = new QStackedWidget(this);
    body->addWidget(m_nav);
    body->addWidget(m_pages, 1);
    root->addLayout(body, 1);

    addPage("Ogólne", "gear", pageGeneral());
    addPage("Transmisja", "broadcast", pageStream());
    addPage("Wyjście", "output", pageOutput());
    addPage("Obraz", "monitor", pageVideo());
    addPage("Dźwięk", "speaker", pageAudio());
    addPage("Skróty klawiszowe", "keyboard", pageHotkeys());
    addPage("Zaawansowane", "sliders", pageAdvanced());
    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    if (m_live) root->addWidget(hint("Trwa transmisja lub nagrywanie — ustawienia enkodera i obrazu można zmienić po zatrzymaniu."));

    auto *buttons = new QDialogButtonBox(this);
    auto *ok = buttons->addButton("OK", QDialogButtonBox::AcceptRole);
    auto *cancel = buttons->addButton("Anuluj", QDialogButtonBox::RejectRole);
    m_apply = buttons->addButton("Zastosuj", QDialogButtonBox::ApplyRole);
    ok->setObjectName("primary");
    m_apply->setEnabled(false);
    connect(ok, &QPushButton::clicked, this, [this] { emit applied(result()); accept(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, [this] { emit applied(result()); m_apply->setEnabled(false); });
    root->addWidget(buttons);
}

void SettingsDialog::addPage(const QString &title, const QString &icon, QWidget *p)
{
    auto *item = new QListWidgetItem(themedIcon(icon), title, m_nav);
    item->setSizeHint(QSize(0, 40));
    m_pages->addWidget(p);
}

void SettingsDialog::markDirty() { if (m_apply) m_apply->setEnabled(true); }

// =============================== strony ===============================

QWidget *SettingsDialog::pageGeneral()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Transmisja"));
    m_confirmStop = track(new QCheckBox("Pytaj o potwierdzenie przed zatrzymaniem transmisji"));
    m_confirmStop->setChecked(m_s["confirmStreamStop"].toBool(true));
    m_autoRecord = track(new QCheckBox("Automatycznie nagrywaj podczas transmisji"));
    m_autoRecord->setChecked(m_s["recordToFile"].toBool());
    f->addRow(m_confirmStop);
    f->addRow(m_autoRecord);

    QFormLayout *f2;
    v->addWidget(card(w, &f2, "Podgląd", "Jak zachowują się elementy, gdy przesuwasz je myszką."));
    m_snapping = track(new QCheckBox("Przyciągaj do krawędzi i środka ekranu"));
    m_snapping->setChecked(m_s["snapping"].toBool(true));
    f2->addRow(m_snapping);
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageStream()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Cel transmisji", "Wklej adres serwera i klucz z panelu transmisji serwisu, np. Twitcha, Kicka lub TikTok LIVE."));
    m_server = track(new QLineEdit(m_s["serverUrl"].toString()));
    m_server->setPlaceholderText("rtmps://adres-serwera/app");
    f->addRow("Serwer", m_server);
    auto *keyRow = new QHBoxLayout();
    m_key = track(new QLineEdit(m_s["streamKey"].toString()));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText("klucz streamu");
    auto *show = new QPushButton("Pokaż");
    show->setCheckable(true);
    connect(show, &QPushButton::toggled, this, [this, show](bool on) {
        m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        show->setText(on ? "Ukryj" : "Pokaż");
    });
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(show);
    f->addRow("Klucz strumienia", keyRow);
    f->addRow("", hint("Klucz jest zapisywany zaszyfrowany (Windows DPAPI) — nikt inny na tym komputerze go nie odczyta."));
    m_server->setEnabled(!m_live);
    m_key->setEnabled(!m_live);

    QFormLayout *f2;
    v->addWidget(card(w, &f2, "Multistream (drugie wyjście RTMP)", "Wysyła ten sam strumień z jednego enkodera NVENC na drugą platformę jednocześnie (np. Kick, YouTube Shorts LIVE, Twitch)."));
    m_secondEnabled = track(new QCheckBox("Włącz drugie wyjście równoległe"));
    m_secondEnabled->setChecked(m_s["secondStreamEnabled"].toBool());
    m_secondName = track(new QLineEdit(m_s["secondStreamName"].toString("Drugi kanał")));
    m_secondName->setPlaceholderText("np. Kick / YouTube");
    m_secondServer = track(new QLineEdit(m_s["secondServerUrl"].toString()));
    m_secondServer->setPlaceholderText("rtmps://adres-drugiego-serwera/app");
    auto *keyRow2 = new QHBoxLayout();
    m_secondKey = track(new QLineEdit(m_s["secondStreamKey"].toString()));
    m_secondKey->setEchoMode(QLineEdit::Password);
    m_secondKey->setPlaceholderText("klucz drugiego streamu");
    auto *show2 = new QPushButton("Pokaż");
    show2->setCheckable(true);
    connect(show2, &QPushButton::toggled, this, [this, show2](bool on) {
        m_secondKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        show2->setText(on ? "Ukryj" : "Pokaż");
    });
    keyRow2->addWidget(m_secondKey, 1);
    keyRow2->addWidget(show2);
    f2->addRow(m_secondEnabled);
    f2->addRow("Nazwa wyjścia", m_secondName);
    f2->addRow("Serwer RTMP", m_secondServer);
    f2->addRow("Klucz strumienia", keyRow2);
    auto updateSecondEnabled = [this](bool on) {
        const bool e = on && !m_live;
        m_secondName->setEnabled(e);
        m_secondServer->setEnabled(e);
        m_secondKey->setEnabled(e);
    };
    connect(m_secondEnabled, &QCheckBox::toggled, this, updateSecondEnabled);
    m_secondEnabled->setEnabled(!m_live);
    updateSecondEnabled(m_secondEnabled->isChecked());

    QFormLayout *loginForm;
    v->addWidget(card(w, &loginForm, "TikTok · Sandbox", "Zaloguj się w przeglądarce, aby sprawdzić połączenie z kontem testowym."));
    auto *login = new QPushButton("Zaloguj przez TikTok");
    auto *configure = new QPushButton("Dane Sandbox");
    auto *clear = new QPushButton;
    auto *status = hint("");
    status->setTextFormat(Qt::PlainText);
    auto *row = new QHBoxLayout;
    row->addWidget(login);
    row->addWidget(clear);
    row->addWidget(configure);
    row->addStretch();
    loginForm->addRow(row);
    loginForm->addRow(status);

    auto *loginTimer = new QTimer(w);
    auto *browser = new QProcess(w);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("TIKSTREAM_TIKTOK_SANDBOX_KEY");
    environment.remove("TIKSTREAM_TIKTOK_SANDBOX_SECRET");
    browser->setProcessEnvironment(environment);
    // Oddzielony profil pozwala zamknąć wyłącznie okno otwarte przez TikStream.
    auto profile = std::make_shared<std::unique_ptr<QTemporaryDir>>();
    auto closeBrowser = [browser] {
        if (browser->state() == QProcess::NotRunning) return;
        const auto pid = browser->processId();
        browser->terminate();
        QTimer::singleShot(1200, browser, [browser, pid] {
            if (pid != 0 && browser->processId() == pid) browser->kill();
        });
    };
    auto refresh = [login, clear, configure, status, loginTimer, closeBrowser] {
        const auto info = EngineHost::instance().callObj("tiktokLoginStatus");
        const auto name = info["name"].toString();
        const bool pending = info["pending"].toBool();
        if (pending && !loginTimer->isActive()) loginTimer->start(500);
        if (!pending) {
            loginTimer->stop();
            closeBrowser();
        }
        login->setEnabled(!pending && name.isEmpty());
        configure->setEnabled(!pending);
        clear->setVisible(pending || !name.isEmpty());
        clear->setText(pending ? "Anuluj" : "Wyczyść profil");
        status->setText(pending ? "Dokończ logowanie w przeglądarce." :
            name.isEmpty() ? info["error"].toString() : "Profil TikTok: " + name);
    };
    connect(loginTimer, &QTimer::timeout, w, refresh);
    auto configureLogin = [this, refresh, status] {
        QDialog dialog(this);
        dialog.setWindowTitle("Dane TikTok Sandbox");
        auto *form = new QFormLayout(&dialog);
        auto *key = new QLineEdit;
        auto *secret = new QLineEdit;
        secret->setEchoMode(QLineEdit::Password);
        form->addRow("Client key", key);
        form->addRow("Client secret", secret);
        form->addRow(hint("Użyj danych z zakładki Sandbox. Są przechowywane w pamięci do zamknięcia TikStream."));
        auto *buttons = new QDialogButtonBox(&dialog);
        auto *ok = buttons->addButton("Zapisz", QDialogButtonBox::AcceptRole);
        buttons->addButton("Anuluj", QDialogButtonBox::RejectRole);
        ok->setEnabled(false);
        auto validate = [key, secret, ok] { ok->setEnabled(!key->text().trimmed().isEmpty() && !secret->text().trimmed().isEmpty()); };
        connect(key, &QLineEdit::textChanged, &dialog, validate);
        connect(secret, &QLineEdit::textChanged, &dialog, validate);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted) return false;
        const auto result = EngineHost::instance().callObj("configureTikTokSandbox",
            {{"key", key->text()}, {"secret", secret->text()}});
        secret->clear();
        refresh();
        if (result.contains("error")) status->setText(result["error"].toString());
        return result["ok"].toBool();
    };
    connect(configure, &QPushButton::clicked, w, configureLogin);
    connect(login, &QPushButton::clicked, w, [refresh, status, configureLogin, browser, profile] {
        if (browser->state() != QProcess::NotRunning) return;
        const auto program = loginBrowser();
        if (program.isEmpty()) {
            status->setText("Do osobnego okna logowania potrzebny jest Chrome, Edge lub Brave.");
            return;
        }
        auto &host = EngineHost::instance();
        auto result = host.callObj("beginTikTokLogin");
        if (result["needsCredentials"].toBool()) {
            if (!configureLogin()) return;
            result = host.callObj("beginTikTokLogin");
        }
        if (result.contains("error")) {
            status->setText(result["error"].toString());
            return;
        }
        *profile = std::make_unique<QTemporaryDir>();
        if (!(*profile)->isValid()) {
            host.callObj("clearTikTokLogin");
            refresh();
            status->setText("Nie udało się utworzyć profilu przeglądarki.");
            return;
        }
        browser->start(program, {"--user-data-dir=" + (*profile)->path(), "--incognito",
                                 "--no-first-run", "--no-default-browser-check",
                                 "--app=" + result["url"].toString()});
        refresh();
    });
    connect(browser, &QProcess::errorOccurred, w, [profile, status, refresh](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        EngineHost::instance().callObj("clearTikTokLogin");
        profile->reset();
        refresh();
        status->setText("Nie udało się otworzyć okna logowania.");
    });
    connect(browser, &QProcess::finished, w, [this, profile, refresh, status] {
        profile->reset();
        auto &host = EngineHost::instance();
        const bool wasPending = host.callObj("tiktokLoginStatus")["pending"].toBool();
        if (wasPending) host.callObj("clearTikTokLogin");
        refresh();
        if (wasPending) status->setText("Okno logowania zostało zamknięte.");
        raise();
        activateWindow();
    });
    connect(this, &QDialog::finished, w, [browser] {
        if (browser->state() == QProcess::NotRunning) return;
        EngineHost::instance().callObj("clearTikTokLogin");
        browser->terminate();
        if (!browser->waitForFinished(500)) {
            browser->kill();
            browser->waitForFinished(500);
        }
    });
    connect(clear, &QPushButton::clicked, w, [refresh] {
        EngineHost::instance().callObj("clearTikTokLogin");
        refresh();
    });
    refresh();
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageOutput()
{
    QVBoxLayout *v;
    auto *w = page(&v);

    QFormLayout *f;
    v->addWidget(card(w, &f, "Obraz (NVIDIA NVENC H.264)", "Kodowanie w układzie NVENC karty graficznej — nie zabiera mocy grze. Obraz nie opuszcza pamięci GPU."));
    f->addRow("Typ przepływności", value("Stała przepływność (CBR)"));
    m_bitrate = track(new PaddedSpinBox);
    m_bitrate->setRange(1000, 20000);
    m_bitrate->setSingleStep(500);
    m_bitrate->setSuffix(" kb/s");
    m_bitrate->setValue(m_s["videoBitrateKbps"].toInt(6000));
    f->addRow("Przepływność (bitrate)", m_bitrate);
    m_keyframe = track(new PaddedSpinBox);
    m_keyframe->setRange(1, 10);
    m_keyframe->setSuffix(" s");
    m_keyframe->setValue(m_s["keyframeSec"].toInt(2));
    f->addRow("Interwał klatek kluczowych", m_keyframe);
    m_preset = track(new QComboBox);
    m_preset->addItems({"P1: Najszybszy (najniższa jakość)", "P2: Szybszy", "P3: Szybki", "P4: Średni",
                        "P5: Wolny (dobra jakość)", "P6: Wolniejszy", "P7: Najwolniejszy (najlepsza jakość)"});
    m_preset->setCurrentIndex(std::clamp(m_s["nvencPreset"].toString("p5").mid(1).toInt(), 1, 7) - 1);
    f->addRow("Preset", m_preset);
    m_profile = track(new QComboBox);
    m_profile->addItem("high (zalecany)", "high");
    m_profile->addItem("main", "main");
    m_profile->setCurrentIndex(m_s["nvencProfile"].toString("high") == "main" ? 1 : 0);
    f->addRow("Profil", m_profile);
    m_bframes = track(new PaddedSpinBox);
    m_bframes->setRange(0, 4);
    m_bframes->setValue(m_s["bFrames"].toInt(2));
    f->addRow("Klatki B", m_bframes);
    m_aq = track(new QCheckBox("Adaptacyjna kwantyzacja (lepsza jakość w ciemnych scenach)"));
    m_aq->setChecked(m_s["spatialAq"].toBool(true));
    m_lookahead = track(new QCheckBox("Look-ahead (lepszy rozkład bitrate, +opóźnienie)"));
    m_lookahead->setChecked(m_s["lookAhead"].toBool());
    m_zeroFpsImpact = track(new QCheckBox("Tryb Zero FPS Impact (1-Pass, bez AQ/Look-ahead — nie obciąża rdzeni CUDA gry)"));
    m_zeroFpsImpact->setChecked(m_s["nvencZeroFpsImpact"].toBool(false));
    auto updateZeroFps = [this](bool on) {
        m_aq->setEnabled(!m_live && !on);
        m_lookahead->setEnabled(!m_live && !on);
    };
    connect(m_zeroFpsImpact, &QCheckBox::toggled, this, updateZeroFps);
    f->addRow("", m_zeroFpsImpact);
    f->addRow("", m_aq);
    f->addRow("", m_lookahead);
    f->addRow("", hint("Tryb Zero FPS Impact wyłącza funkcje NVENC korzystające z rdzeni CUDA (AQ, Look-ahead, 2-Pass), chroniąc FPS przy obciążeniu GPU 98–100%."));

    QFormLayout *fa;
    v->addWidget(card(w, &fa, "Dźwięk"));
    fa->addRow("Enkoder", value("AAC-LC • 48 kHz • stereo"));
    m_audioBitrate = track(new QComboBox);
    for (int b : {128, 160, 192, 256, 320}) m_audioBitrate->addItem(QString("%1 kb/s").arg(b), b);
    m_audioBitrate->setCurrentIndex(std::max(0, m_audioBitrate->findData(m_s["audioBitrateKbps"].toInt(160))));
    fa->addRow("Przepływność", m_audioBitrate);

    QFormLayout *fr;
    v->addWidget(card(w, &fr, "Nagrywanie", "Nagranie używa tego samego enkodera co transmisja — bez dodatkowego obciążenia."));
    auto *row = new QHBoxLayout();
    m_folder = track(new QLineEdit(m_s["recordFolder"].toString()));
    auto *browse = new QPushButton("Przeglądaj");
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, "Folder nagrań", m_folder->text());
        if (!d.isEmpty()) m_folder->setText(QDir::toNativeSeparators(d));
    });
    row->addWidget(m_folder, 1);
    row->addWidget(browse);
    fr->addRow("Folder", row);
    m_recFormat = track(new QComboBox);
    m_recFormat->addItem("Matroska (.mkv) — odporny na awarie", "mkv");
    m_recFormat->addItem("MP4 (.mp4) — fragmentowany, gotowy do montażu", "mp4");
    m_recFormat->setCurrentIndex(m_s["recordFormat"].toString("mkv") == "mp4" ? 1 : 0);
    fr->addRow("Format", m_recFormat);

    for (QWidget *x : std::initializer_list<QWidget *>{m_bitrate, m_keyframe, m_preset, m_profile, m_bframes, m_zeroFpsImpact, m_aq, m_lookahead, m_audioBitrate, m_recFormat})
        x->setEnabled(!m_live);
    if (m_zeroFpsImpact->isChecked()) {
        m_aq->setEnabled(false);
        m_lookahead->setEnabled(false);
    }
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageVideo()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Obraz", "Rozmiar płótna i klatkaż obowiązują w podglądzie, transmisji i nagraniu."));
    auto *preset = new QComboBox;
    preset->addItem("Własna");
    for (const auto &size : {QSize(1920, 1080), QSize(1280, 720), QSize(1080, 1920), QSize(720, 1280)})
        preset->addItem(QString("%1 × %2").arg(size.width()).arg(size.height()), size);
    f->addRow("Rozdzielczość", preset);

    m_videoWidth = track(new PaddedSpinBox);
    m_videoHeight = track(new PaddedSpinBox);
    for (auto *field : {m_videoWidth, m_videoHeight}) {
        field->setRange(128, 4096);
        field->setSingleStep(2);
        field->setSuffix(" px");
        connect(field, &QSpinBox::editingFinished, this, [field] { field->setValue(field->value() & ~1); });
    }
    m_videoWidth->setValue(m_s["width"].toInt(1080));
    m_videoHeight->setValue(m_s["height"].toInt(1920));
    f->addRow("Szerokość", m_videoWidth);
    f->addRow("Wysokość", m_videoHeight);

    auto *aspect = value("");
    f->addRow("Proporcje", aspect);
    auto updateSize = [this, preset, aspect] {
        const QSize size(m_videoWidth->value(), m_videoHeight->value());
        const int divisor = std::gcd(size.width(), size.height());
        aspect->setText(QString("%1:%2").arg(size.width() / divisor).arg(size.height() / divisor));
        preset->setCurrentIndex(std::max(0, preset->findData(size)));
    };
    connect(m_videoWidth, &QSpinBox::valueChanged, this, updateSize);
    connect(m_videoHeight, &QSpinBox::valueChanged, this, updateSize);
    connect(preset, &QComboBox::activated, this, [this, preset](int index) {
        if (index == 0) return;
        const QSize size = preset->itemData(index).toSize();
        m_videoWidth->setValue(size.width());
        m_videoHeight->setValue(size.height());
    });
    updateSize();

    m_fps = track(new PaddedSpinBox);
    m_fps->setRange(1, 120);
    m_fps->setValue(m_s["fps"].toInt(60));
    f->addRow("Klatki na sekundę", m_fps);
    f->addRow(hint("NVENC wymaga parzystej szerokości i wysokości. Nieparzysta wartość zostanie zaokrąglona w dół."));
    for (QWidget *field : std::initializer_list<QWidget *>{preset, m_videoWidth, m_videoHeight, m_fps})
        field->setEnabled(!m_live);
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageAudio()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Główne urządzenia", "Dźwięk pulpitu i mikrofon działają we wszystkich scenach. Kolejne urządzenia dodasz przez „Dodaj źródło”."));
    f->addRow("Próbkowanie", value("48 kHz • stereo"));

    auto fill = [this](QComboBox *cb, const QJsonArray &devs, const QJsonObject &src) {
        cb->addItem("Wyłączone", "off");
        cb->addItem("Domyślne urządzenie systemu", "");
        for (const auto &d : devs) cb->addItem(d.toObject()["name"].toString(), d.toObject()["id"].toString());
        if (src.isEmpty()) cb->setCurrentIndex(0);
        else cb->setCurrentIndex(std::max(1, cb->findData(src["deviceId"].toString())));
        track(cb);
    };
    const QJsonArray sources = m_s["sources"].toArray();
    m_desktopDev = new QComboBox;
    fill(m_desktopDev, m_devices["outputs"].toArray(), firstSourceOfType(sources, "AudioOutput"));
    f->addRow("Dźwięk pulpitu", m_desktopDev);
    m_micDev = new QComboBox;
    fill(m_micDev, m_devices["inputs"].toArray(), firstSourceOfType(sources, "AudioInput"));
    f->addRow("Mikrofon", m_micDev);
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageHotkeys()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Skróty klawiszowe", "Działają globalnie — także gdy gra jest na pierwszym planie. Kliknij pole i naciśnij kombinację klawiszy."));
    const QJsonObject hk = m_s["hotkeys"].toObject();
    for (const auto &[id, label] : Hotkeys::actions()) {
        auto *row = new QHBoxLayout();
        auto *edit = new QKeySequenceEdit(QKeySequence(hk[id].toString(), QKeySequence::PortableText));
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        if (auto *le = edit->findChild<QLineEdit *>()) le->setPlaceholderText("Kliknij i naciśnij skrót…");
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, &SettingsDialog::markDirty);
        row->addWidget(edit, 1);
        f->addRow(label, row);
        m_hotkeys[id] = edit;
    }
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageAdvanced()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Wydajność"));
    m_priority = track(new QComboBox);
    m_priority->addItem("Normalny", "Normal");
    m_priority->addItem("Powyżej normalnego (zalecany)", "AboveNormal");
    m_priority->addItem("Wysoki", "High");
    m_priority->setCurrentIndex(std::max(0, m_priority->findData(m_s["processPriority"].toString("AboveNormal"))));
    f->addRow("Priorytet procesu", m_priority);
    m_preview = track(new QCheckBox("Podgląd na żywo (wyłącz dla maksymalnej wydajności)"));
    m_preview->setChecked(m_s["previewEnabled"].toBool(true));
    f->addRow("Podgląd", m_preview);

    QFormLayout *fn;
    v->addWidget(card(w, &fn, "Ponowne łączenie", "Gdy internet na chwilę padnie, program sam połączy się z serwerem i wznowi transmisję."));
    m_reconnect = track(new QCheckBox("Włącz"));
    m_reconnect->setChecked(m_s["autoReconnect"].toBool(true));
    fn->addRow("", m_reconnect);
    m_retryDelay = track(new PaddedSpinBox);
    m_retryDelay->setRange(1, 60);
    m_retryDelay->setSuffix(" s");
    m_retryDelay->setValue(m_s["reconnectDelaySec"].toInt(2));
    fn->addRow("Próbuj co", m_retryDelay);
    m_retryMax = track(new PaddedSpinBox);
    m_retryMax->setRange(1, 1000);
    m_retryMax->setValue(m_s["reconnectMaxRetries"].toInt(25));
    fn->addRow("Maksymalna liczba prób", m_retryMax);
    connect(m_reconnect, &QCheckBox::toggled, m_retryDelay, &QWidget::setEnabled);
    connect(m_reconnect, &QCheckBox::toggled, m_retryMax, &QWidget::setEnabled);
    m_retryDelay->setEnabled(m_reconnect->isChecked());
    m_retryMax->setEnabled(m_reconnect->isChecked());

    QFormLayout *fd;
    v->addWidget(card(w, &fd, "Diagnostyka"));
    auto *logBtn = new QPushButton("Otwórz plik logu");
    connect(logBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(EngineHost::instance().callObj("logPath")["path"].toString()));
    });
    fd->addRow("Log", logBtn);
    return scrollPage(w, v);
}

// =============================== wynik ===============================

QJsonObject SettingsDialog::result() const
{
    QJsonObject s = m_s;
    s["confirmStreamStop"] = m_confirmStop->isChecked();
    s["recordToFile"] = m_autoRecord->isChecked();
    s["snapping"] = m_snapping->isChecked();

    s["serverUrl"] = m_server->text().trimmed();
    s["streamKey"] = m_key->text().trimmed();
    s["secondStreamEnabled"] = m_secondEnabled->isChecked();
    s["secondStreamName"] = m_secondName->text().trimmed();
    s["secondServerUrl"] = m_secondServer->text().trimmed();
    s["secondStreamKey"] = m_secondKey->text().trimmed();

    s["videoBitrateKbps"] = m_bitrate->value();
    s["keyframeSec"] = m_keyframe->value();
    s["nvencPreset"] = QString("p%1").arg(m_preset->currentIndex() + 1);
    s["nvencProfile"] = m_profile->currentData().toString();
    s["bFrames"] = m_bframes->value();
    s["spatialAq"] = m_aq->isChecked();
    s["lookAhead"] = m_lookahead->isChecked();
    s["nvencZeroFpsImpact"] = m_zeroFpsImpact->isChecked();
    s["audioBitrateKbps"] = m_audioBitrate->currentData().toInt();
    s["recordFolder"] = m_folder->text().trimmed();
    s["recordFormat"] = m_recFormat->currentData().toString();

    s["width"] = m_videoWidth->value() & ~1;
    s["height"] = m_videoHeight->value() & ~1;
    s["fps"] = m_fps->value();

    // Główne urządzenia audio działają we wszystkich scenach.
    QJsonArray sources = s["sources"].toArray();
    auto applyDevice = [&sources](const QString &type, const QString &name, const QString &choice) {
        int idx = -1;
        for (int i = 0; i < sources.size(); ++i)
            if (sources[i].toObject()["type"].toString() == type && sources[i].toObject()["globalAudio"].toBool()) { idx = i; break; }
        if (choice == "off") {
            if (idx >= 0) sources.removeAt(idx);
            return;
        }
        QJsonObject src = idx >= 0 ? sources[idx].toObject()
                                   : QJsonObject{{"id", QUuid::createUuid().toString(QUuid::Id128)}, {"type", type}, {"name", name}, {"volume", 1.0}, {"muted", false}};
        src["deviceId"] = choice;
        src["globalAudio"] = true;
        if (idx >= 0) sources[idx] = src; else sources.append(src);
    };
    applyDevice("AudioOutput", "Dźwięk pulpitu", m_desktopDev->currentData().toString());
    applyDevice("AudioInput", "Mikrofon", m_micDev->currentData().toString());
    s["sources"] = sources;

    QJsonObject hk;
    for (auto it = m_hotkeys.begin(); it != m_hotkeys.end(); ++it) {
        const QString seq = it.value()->keySequence().toString(QKeySequence::PortableText);
        if (!seq.isEmpty()) hk[it.key()] = seq;
    }
    s["hotkeys"] = hk;

    s["processPriority"] = m_priority->currentData().toString();
    s["previewEnabled"] = m_preview->isChecked();
    s["autoReconnect"] = m_reconnect->isChecked();
    s["reconnectDelaySec"] = m_retryDelay->value();
    s["reconnectMaxRetries"] = m_retryMax->value();
    return s;
}
