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
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
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
    setWindowTitle("Settings");
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

    addPage("General", "gear", pageGeneral());
    addPage("Stream", "broadcast", pageStream());
    addPage("Output", "output", pageOutput());
    addPage("Video", "monitor", pageVideo());
    addPage("Audio", "speaker", pageAudio());
    addPage("Hotkeys", "keyboard", pageHotkeys());
    addPage("Advanced", "sliders", pageAdvanced());
    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    if (m_live) root->addWidget(hint("Streaming or recording is active — encoder and video settings can be changed after stopping."));

    auto *buttons = new QDialogButtonBox(this);
    auto *ok = buttons->addButton("OK", QDialogButtonBox::AcceptRole);
    auto *cancel = buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
    m_apply = buttons->addButton("Apply", QDialogButtonBox::ApplyRole);
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

// =============================== pages ===============================

QWidget *SettingsDialog::pageGeneral()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Streaming"));
    m_confirmStop = track(new QCheckBox("Ask for confirmation before stopping stream"));
    m_confirmStop->setChecked(m_s["confirmStreamStop"].toBool(true));
    m_autoRecord = track(new QCheckBox("Automatically record when streaming"));
    m_autoRecord->setChecked(m_s["recordToFile"].toBool());
    m_minToTray = track(new QCheckBox("Minimize to system tray when closing window"));
    m_minToTray->setChecked(m_s["minimizeToTray"].toBool(false));
    f->addRow(m_confirmStop);
    f->addRow(m_autoRecord);
    f->addRow(m_minToTray);

    QFormLayout *f2;
    v->addWidget(card(w, &f2, "Preview", "How items behave when dragging them with the mouse."));
    m_snapping = track(new QCheckBox("Snap to screen edges and center"));
    m_snapping->setChecked(m_s["snapping"].toBool(true));
    m_snapSources = track(new QCheckBox("Snap sources to other sources' edges and centers"));
    m_snapSources->setChecked(m_s["snapToSources"].toBool(true));
    f2->addRow(m_snapping);
    f2->addRow(m_snapSources);

    QFormLayout *f3;
    v->addWidget(card(w, &f3, "Updates", "Checks GitHub Releases for newer TikStream versions."));
    m_autoUpdate = track(new QCheckBox("Automatically check for updates on startup and periodically"));
    m_autoUpdate->setChecked(m_s["autoUpdateEnabled"].toBool(true));
    f3->addRow(m_autoUpdate);
    auto *checkRow = new QHBoxLayout();
    auto *checkBtn = new QPushButton("Check for Updates Now");
    checkBtn->setCursor(Qt::PointingHandCursor);
    connect(checkBtn, &QPushButton::clicked, this, &SettingsDialog::checkForUpdatesRequested);
    const QString ver = EngineHost::instance().callObj("appVersion")["version"].toString();
    checkRow->addWidget(checkBtn);
    if (!ver.isEmpty()) checkRow->addWidget(hint("Installed version: v" + ver));
    checkRow->addStretch();
    f3->addRow("", checkRow);
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageStream()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Stream Destination", "Paste the server URL and stream key from your streaming platform (e.g., TikTok LIVE, Twitch, or Kick)."));
    m_server = track(new QLineEdit(m_s["serverUrl"].toString()));
    m_server->setPlaceholderText("rtmps://server-address/app");
    f->addRow("Server", m_server);
    auto *keyRow = new QHBoxLayout();
    m_key = track(new QLineEdit(m_s["streamKey"].toString()));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText("stream key");
    auto *show = new QPushButton("Show");
    show->setCheckable(true);
    connect(show, &QPushButton::toggled, this, [this, show](bool on) {
        m_key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        show->setText(on ? "Hide" : "Show");
    });
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(show);
    f->addRow("Stream Key", keyRow);
    f->addRow("", hint("The key is stored encrypted (Windows DPAPI) — no one else on this computer can read it."));
    m_server->setEnabled(!m_live);
    m_key->setEnabled(!m_live);

    QFormLayout *f2;
    v->addWidget(card(w, &f2, "Multistream (Second RTMP Output)", "Sends the same stream from one NVENC encoder to a second platform simultaneously (e.g., Kick, YouTube Shorts LIVE, Twitch)."));
    m_secondEnabled = track(new QCheckBox("Enable second parallel output"));
    m_secondEnabled->setChecked(m_s["secondStreamEnabled"].toBool());
    m_secondName = track(new QLineEdit(m_s["secondStreamName"].toString("Second Channel")));
    m_secondName->setPlaceholderText("e.g. Kick / YouTube");
    m_secondServer = track(new QLineEdit(m_s["secondServerUrl"].toString()));
    m_secondServer->setPlaceholderText("rtmps://second-server-address/app");
    auto *keyRow2 = new QHBoxLayout();
    m_secondKey = track(new QLineEdit(m_s["secondStreamKey"].toString()));
    m_secondKey->setEchoMode(QLineEdit::Password);
    m_secondKey->setPlaceholderText("second stream key");
    auto *show2 = new QPushButton("Show");
    show2->setCheckable(true);
    connect(show2, &QPushButton::toggled, this, [this, show2](bool on) {
        m_secondKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        show2->setText(on ? "Hide" : "Show");
    });
    keyRow2->addWidget(m_secondKey, 1);
    keyRow2->addWidget(show2);
    f2->addRow(m_secondEnabled);
    f2->addRow("Output Name", m_secondName);
    f2->addRow("RTMP Server", m_secondServer);
    f2->addRow("Stream Key", keyRow2);
    auto updateSecondEnabled = [this](bool on) {
        const bool e = on && !m_live;
        m_secondName->setEnabled(e);
        m_secondServer->setEnabled(e);
        m_secondKey->setEnabled(e);
    };
    connect(m_secondEnabled, &QCheckBox::toggled, this, updateSecondEnabled);
    m_secondEnabled->setEnabled(!m_live);
    updateSecondEnabled(m_secondEnabled->isChecked());

    // ---------- TikTok LIVE Native Account (QR + LIVE Studio Import) ----------
    QFormLayout *ttForm;
    v->addWidget(card(w, &ttForm, "Konto TikTok LIVE (QR / LIVE Studio)",
        "Zaloguj się bezpośrednio kodem QR w aplikacji TikTok lub zaimportuj aktywną sesję z TikTok LIVE Studio. Sesja jest szyfrowana lokalnie (Windows DPAPI)."));

    auto *ttAccountLabel = new QLabel("Sprawdzanie statusu konta…");
    ttAccountLabel->setTextFormat(Qt::PlainText);
    ttAccountLabel->setWordWrap(true);
    ttAccountLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    ttAccountLabel->setStyleSheet("color: #e2e8f0; font-weight: 500;");

    auto *ttStatusLabel = new QLabel("");
    ttStatusLabel->setTextFormat(Qt::PlainText);
    ttStatusLabel->setWordWrap(true);
    ttStatusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    ttStatusLabel->setStyleSheet("color: #94a3b8;");

    auto *qrImageLabel = new QLabel;
    qrImageLabel->setAlignment(Qt::AlignCenter);
    qrImageLabel->setFixedSize(224, 224);
    qrImageLabel->setStyleSheet("background: #ffffff; border-radius: 8px; padding: 6px;");
    qrImageLabel->setVisible(false);

    auto *btnQr = new QPushButton("Zaloguj przez kod QR");
    auto *btnQrCancel = new QPushButton("Anuluj QR");
    btnQrCancel->setVisible(false);
    auto *btnImport = new QPushButton("Importuj z LIVE Studio");
    auto *btnCheckLive = new QPushButton("Sprawdź dostęp LIVE");
    auto *btnLogout = new QPushButton("Wyloguj");

    auto *ttBtnRow = new QHBoxLayout;
    ttBtnRow->addWidget(btnQr);
    ttBtnRow->addWidget(btnQrCancel);
    ttBtnRow->addWidget(btnImport);
    ttBtnRow->addWidget(btnCheckLive);
    ttBtnRow->addWidget(btnLogout);
    ttBtnRow->addStretch();

    ttForm->addRow("Konto", ttAccountLabel);
    ttForm->addRow(ttBtnRow);
    ttForm->addRow("", qrImageLabel);
    ttForm->addRow("", ttStatusLabel);

    // Optional advanced signer fields (collapsed by default — not needed for QR login)
    auto *advToggle = new QCheckBox("Zaawansowane: zewnętrzny signer API (opcjonalne)");
    ttForm->addRow("", advToggle);

    auto *advBox = new QWidget;
    auto *advForm = new QFormLayout(advBox);
    advForm->setContentsMargins(0, 4, 0, 0);
    m_signerKey = track(new QLineEdit(m_s["tikTokSignerKey"].toString()));
    m_signerKey->setEchoMode(QLineEdit::Password);
    m_signerKey->setPlaceholderText("Opcjonalny klucz X-RapidAPI-Key");
    auto *signerRow = new QHBoxLayout;
    signerRow->addWidget(m_signerKey, 1);
    auto *showSigner = new QToolButton;
    showSigner->setText("Pokaż");
    showSigner->setCheckable(true);
    connect(showSigner, &QToolButton::toggled, m_signerKey, [this, showSigner](bool on) {
        m_signerKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        showSigner->setText(on ? "Ukryj" : "Pokaż");
    });
    signerRow->addWidget(showSigner);
    advForm->addRow("Klucz Signera", signerRow);

    const QString defaultSignerUrl = QStringLiteral("https://tiktok-live-studio-api-signer1.p.rapidapi.com/");
    m_signerUrl = track(new QLineEdit(m_s["tikTokSignerUrl"].toString(defaultSignerUrl)));
    m_signerUrl->setPlaceholderText(defaultSignerUrl);
    advForm->addRow("URL Signera", m_signerUrl);

    advBox->setVisible(false);
    connect(advToggle, &QCheckBox::toggled, advBox, &QWidget::setVisible);
    ttForm->addRow(advBox);

    auto *qrTimer = new QTimer(w);
    auto qrRendered = std::make_shared<QString>();

    auto refreshTikTokAccount = [ttAccountLabel, btnCheckLive, btnLogout, btnQr, btnImport, qrTimer] {
        const auto info = EngineHost::instance().callObj("tiktokAccountStatus");
        const auto acct = info["account"].toObject();
        const bool loggedIn = acct["loggedIn"].toBool();
        const bool qrActive = qrTimer->isActive();
        if (loggedIn) {
            const QString name = acct["displayName"].toString();
            const QString src = acct["source"].toString() == "live_studio" ? "LIVE Studio" : "QR";
            const QString reg = acct["region"].toString();
            QString text = QString("Zalogowano: %1 (sesja: %2)").arg(name.isEmpty() ? acct["userId"].toString() : name, src);
            if (!reg.isEmpty()) text += " · region: " + reg;
            ttAccountLabel->setText(text);
        } else {
            ttAccountLabel->setText("Nie zalogowano.");
        }
        btnCheckLive->setEnabled(loggedIn && !qrActive);
        btnLogout->setVisible(loggedIn);
        btnQr->setEnabled(!qrActive);
        btnImport->setEnabled(!qrActive);
    };

    auto stopQrUi = [qrTimer, qrImageLabel, btnQrCancel, qrRendered, refreshTikTokAccount] {
        qrTimer->stop();
        qrImageLabel->clear();
        qrImageLabel->setVisible(false);
        btnQrCancel->setVisible(false);
        qrRendered->clear();
        refreshTikTokAccount();
    };

    connect(qrTimer, &QTimer::timeout, w, [ttStatusLabel, qrImageLabel, qrRendered, stopQrUi] {
        const auto st = EngineHost::instance().callObj("tiktokQrPoll");
        const QString status = st["status"].toString();
        const QString text = st["statusText"].toString();
        if (!text.isEmpty()) ttStatusLabel->setText(text);

        const QString b64 = st["qrcodeB64"].toString();
        if (!b64.isEmpty() && b64 != *qrRendered) {
            QPixmap px;
            if (px.loadFromData(QByteArray::fromBase64(b64.toLatin1()))) {
                qrImageLabel->setPixmap(px.scaled(210, 210, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                qrImageLabel->setVisible(true);
                *qrRendered = b64;
            }
        }

        if (status == "confirmed" || status == "expired" || status == "error" || status == "idle") {
            if (status == "error" && st.contains("error"))
                ttStatusLabel->setText("Błąd: " + st["error"].toString());
            stopQrUi();
        }
    });

    connect(btnQr, &QPushButton::clicked, w, [this, ttStatusLabel, btnQrCancel, qrTimer, qrRendered, refreshTikTokAccount] {
        const auto res = EngineHost::instance().callObj("tiktokQrBegin", {
            {"signerKey", m_signerKey->text().trimmed()},
            {"signerUrl", m_signerUrl->text().trimmed()},
        });
        if (res.contains("error")) {
            ttStatusLabel->setText("Błąd: " + res["error"].toString());
            return;
        }
        qrRendered->clear();
        ttStatusLabel->setText(res["statusText"].toString("Generowanie kodu QR…"));
        btnQrCancel->setVisible(true);
        qrTimer->start(1000);
        refreshTikTokAccount();
    });

    connect(btnQrCancel, &QPushButton::clicked, w, [ttStatusLabel, stopQrUi] {
        EngineHost::instance().callObj("tiktokQrCancel");
        ttStatusLabel->setText("Anulowano logowanie QR.");
        stopQrUi();
    });

    connect(btnImport, &QPushButton::clicked, w, [ttStatusLabel, refreshTikTokAccount] {
        const auto res = EngineHost::instance().callObj("tiktokImportStudio");
        if (res.contains("error")) {
            ttStatusLabel->setText("Błąd importu: " + res["error"].toString());
        } else {
            const auto acct = res["account"].toObject();
            ttStatusLabel->setText("Zaimportowano sesję LIVE Studio: " + acct["displayName"].toString());
        }
        refreshTikTokAccount();
    });

    connect(btnLogout, &QPushButton::clicked, w, [ttStatusLabel, stopQrUi] {
        EngineHost::instance().callObj("tiktokLogout");
        ttStatusLabel->setText("Wylogowano z konta TikTok.");
        stopQrUi();
    });

    connect(btnCheckLive, &QPushButton::clicked, w, [this, w, btnCheckLive, ttStatusLabel] {
        btnCheckLive->setEnabled(false);
        ttStatusLabel->setText("Sprawdzanie webcast/room/create_info/…");
        const QString sk = m_signerKey->text().trimmed();
        const QString su = m_signerUrl->text().trimmed();
        QPointer<QWidget> guard(w);
        auto *th = QThread::create([sk, su, guard, btnCheckLive, ttStatusLabel] {
            const auto res = EngineHost::instance().callObj("tiktokCheckCreateInfo", {
                {"signerKey", sk},
                {"signerUrl", su},
            });
            if (!guard) return;
            QMetaObject::invokeMethod(guard, [res, guard, btnCheckLive, ttStatusLabel] {
                if (!guard) return;
                btnCheckLive->setEnabled(true);
                if (res["ok"].toBool())
                    ttStatusLabel->setText("OK: " + res["message"].toString());
                else
                    ttStatusLabel->setText("Błąd: " + res["error"].toString("Nieznany błąd create_info."));
            }, Qt::QueuedConnection);
        });
        connect(th, &QThread::finished, th, &QObject::deleteLater);
        th->start();
    });

    connect(this, &QDialog::finished, w, [qrTimer] {
        if (qrTimer->isActive()) {
            qrTimer->stop();
            EngineHost::instance().callObj("tiktokQrCancel");
        }
    });
    refreshTikTokAccount();

    QFormLayout *loginForm;
    v->addWidget(card(w, &loginForm, "TikTok · Sandbox", "Sign in via browser to verify connection with a Sandbox test account."));
    auto *login = new QPushButton("Sign in with TikTok");
    auto *configure = new QPushButton("Sandbox Credentials");
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
    // Isolated profile allows closing only the window opened by TikStream.
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
        clear->setText(pending ? "Cancel" : "Clear Profile");
        status->setText(pending ? "Complete sign-in in your browser." :
            name.isEmpty() ? info["error"].toString() : "TikTok Profile: " + name);
    };
    connect(loginTimer, &QTimer::timeout, w, refresh);
    auto configureLogin = [this, refresh, status] {
        QDialog dialog(this);
        dialog.setWindowTitle("TikTok Sandbox Credentials");
        auto *form = new QFormLayout(&dialog);
        auto *key = new QLineEdit;
        auto *secret = new QLineEdit;
        secret->setEchoMode(QLineEdit::Password);
        form->addRow("Client key", key);
        form->addRow("Client secret", secret);
        form->addRow(hint("Use credentials from the Sandbox tab. They are kept in memory only until TikStream closes."));
        auto *buttons = new QDialogButtonBox(&dialog);
        auto *ok = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
        buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
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
            status->setText("Chrome, Edge, or Brave is required for the isolated login window.");
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
            status->setText("Failed to create temporary browser profile.");
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
        status->setText("Failed to open the login window.");
    });
    connect(browser, &QProcess::finished, w, [this, profile, refresh, status] {
        profile->reset();
        auto &host = EngineHost::instance();
        const bool wasPending = host.callObj("tiktokLoginStatus")["pending"].toBool();
        if (wasPending) host.callObj("clearTikTokLogin");
        refresh();
        if (wasPending) status->setText("Login window was closed.");
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
    v->addWidget(card(w, &f, "Video (NVIDIA NVENC H.264)", "Hardware encoding on the GPU's NVENC engine — frames never leave VRAM."));
    f->addRow("Rate Control", value("Constant Bitrate (CBR)"));
    m_bitrate = track(new PaddedSpinBox);
    m_bitrate->setRange(1000, 20000);
    m_bitrate->setSingleStep(500);
    m_bitrate->setSuffix(" kbps");
    m_bitrate->setValue(m_s["videoBitrateKbps"].toInt(6000));
    f->addRow("Bitrate", m_bitrate);
    m_keyframe = track(new PaddedSpinBox);
    m_keyframe->setRange(1, 10);
    m_keyframe->setSuffix(" s");
    m_keyframe->setValue(m_s["keyframeSec"].toInt(2));
    f->addRow("Keyframe Interval", m_keyframe);
    m_preset = track(new QComboBox);
    m_preset->addItems({"P1: Fastest (lowest quality)", "P2: Faster", "P3: Fast", "P4: Medium",
                        "P5: Slow (good quality)", "P6: Slower", "P7: Slowest (best quality)"});
    m_preset->setCurrentIndex(std::clamp(m_s["nvencPreset"].toString("p5").mid(1).toInt(), 1, 7) - 1);
    f->addRow("Preset", m_preset);
    m_profile = track(new QComboBox);
    m_profile->addItem("high (recommended)", "high");
    m_profile->addItem("main", "main");
    m_profile->setCurrentIndex(m_s["nvencProfile"].toString("high") == "main" ? 1 : 0);
    f->addRow("Profile", m_profile);
    m_bframes = track(new PaddedSpinBox);
    m_bframes->setRange(0, 4);
    m_bframes->setValue(m_s["bFrames"].toInt(2));
    f->addRow("B-frames", m_bframes);
    m_aq = track(new QCheckBox("Adaptive Quantization (better quality in dark scenes)"));
    m_aq->setChecked(m_s["spatialAq"].toBool(true));
    m_lookahead = track(new QCheckBox("Look-ahead (better bitrate distribution, +latency)"));
    m_lookahead->setChecked(m_s["lookAhead"].toBool());
    m_zeroFpsImpact = track(new QCheckBox("Zero FPS Impact Mode (1-Pass, no AQ/Look-ahead — zero CUDA contention)"));
    m_zeroFpsImpact->setChecked(m_s["nvencZeroFpsImpact"].toBool(false));
    auto updateZeroFps = [this](bool on) {
        m_aq->setEnabled(!m_live && !on);
        m_lookahead->setEnabled(!m_live && !on);
    };
    connect(m_zeroFpsImpact, &QCheckBox::toggled, this, updateZeroFps);
    f->addRow("", m_zeroFpsImpact);
    f->addRow("", m_aq);
    f->addRow("", m_lookahead);
    f->addRow("", hint("Zero FPS Impact disables CUDA-based NVENC features (AQ, Look-ahead, 2-Pass), protecting game FPS at 98–100% GPU load."));

    QFormLayout *fa;
    v->addWidget(card(w, &fa, "Audio"));
    fa->addRow("Encoder", value("AAC-LC • 48 kHz • stereo"));
    m_audioBitrate = track(new QComboBox);
    for (int b : {128, 160, 192, 256, 320}) m_audioBitrate->addItem(QString("%1 kbps").arg(b), b);
    m_audioBitrate->setCurrentIndex(std::max(0, m_audioBitrate->findData(m_s["audioBitrateKbps"].toInt(160))));
    fa->addRow("Bitrate", m_audioBitrate);

    QFormLayout *fr;
    v->addWidget(card(w, &fr, "Recording", "Recording shares the same encoder as streaming — no extra overhead."));
    auto *row = new QHBoxLayout();
    m_folder = track(new QLineEdit(m_s["recordFolder"].toString()));
    auto *browse = new QPushButton("Browse");
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, "Recording Folder", m_folder->text());
        if (!d.isEmpty()) m_folder->setText(QDir::toNativeSeparators(d));
    });
    row->addWidget(m_folder, 1);
    row->addWidget(browse);
    fr->addRow("Folder", row);
    m_recFormat = track(new QComboBox);
    m_recFormat->addItem("Matroska (.mkv) — crash-resistant", "mkv");
    m_recFormat->addItem("MP4 (.mp4) — fragmented, ready for editing", "mp4");
    m_recFormat->setCurrentIndex(m_s["recordFormat"].toString("mkv") == "mp4" ? 1 : 0);
    fr->addRow("Format", m_recFormat);
    m_recCodec = track(new QComboBox);
    m_recCodec->addItem("H.264 NVENC (shared with stream — zero extra GPU load)", "h264");
    m_recCodec->addItem("HEVC / H.265 NVENC (better compression)", "hevc");
    m_recCodec->addItem("AV1 NVENC (RTX 40xx+ — best quality per bitrate)", "av1");
    m_recCodec->setCurrentIndex(std::max(0, m_recCodec->findData(m_s["recordCodec"].toString("h264"))));
    fr->addRow("Recording Codec", m_recCodec);
    m_multiTrack = track(new QCheckBox("Multi-track audio (MKV only — Track 1: Full Mix, Tracks 2+: separate audio sources)"));
    m_multiTrack->setChecked(m_s["multiTrackAudio"].toBool(false));
    auto updateMultiTrack = [this] {
        m_multiTrack->setEnabled(!m_live && m_recFormat->currentData().toString() == "mkv");
    };
    connect(m_recFormat, &QComboBox::currentIndexChanged, this, updateMultiTrack);
    fr->addRow("", m_multiTrack);

    QFormLayout *fb;
    v->addWidget(card(w, &fb, "Replay Buffer", "Keeps the last N seconds of encoded video and audio in RAM with zero extra encoding overhead."));
    m_replaySec = track(new PaddedSpinBox);
    m_replaySec->setRange(10, 120);
    m_replaySec->setSingleStep(5);
    m_replaySec->setSuffix(" s");
    m_replaySec->setValue(m_s["replayBufferSec"].toInt(30));
    fb->addRow("Buffer Length", m_replaySec);
    fb->addRow("", hint("Use the Controls dock or Hotkeys to start/stop the replay buffer and save instant clips to your recording folder."));

    for (QWidget *x : std::initializer_list<QWidget *>{m_bitrate, m_keyframe, m_preset, m_profile, m_bframes, m_zeroFpsImpact, m_aq, m_lookahead, m_audioBitrate, m_recFormat, m_recCodec, m_replaySec})
        x->setEnabled(!m_live);
    updateMultiTrack();
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
    v->addWidget(card(w, &f, "Video", "Canvas size and frame rate apply to preview, streaming, and recording."));
    auto *preset = new QComboBox;
    preset->addItem("Custom");
    for (const auto &size : {QSize(1920, 1080), QSize(1280, 720), QSize(1080, 1920), QSize(720, 1280)})
        preset->addItem(QString("%1 × %2").arg(size.width()).arg(size.height()), size);
    f->addRow("Resolution", preset);

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
    f->addRow("Width", m_videoWidth);
    f->addRow("Height", m_videoHeight);

    auto *aspect = value("");
    f->addRow("Aspect Ratio", aspect);
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
    f->addRow("Frames per second", m_fps);
    m_transitionMs = track(new PaddedSpinBox);
    m_transitionMs->setRange(0, 1500);
    m_transitionMs->setSingleStep(50);
    m_transitionMs->setSuffix(" ms");
    m_transitionMs->setValue(m_s["sceneTransitionMs"].toInt(250));
    f->addRow("Scene Transition (Fade)", m_transitionMs);
    f->addRow(hint("0 ms = instant cut. GPU crossfade runs directly on the canvas with zero extra CPU cost."));
    f->addRow(hint("NVENC requires even width and height. Odd values will be rounded down."));

    QFormLayout *fd;
    v->addWidget(card(w, &fd, "Dual-Canvas Recording (Horizontal + Vertical)", "Record a second resolution (e.g. 1920×1080 horizontal VOD) simultaneously while streaming vertical 9:16."));
    m_dualCanvas = track(new QCheckBox("Enable Dual-Canvas recording output"));
    m_dualCanvas->setChecked(m_s["dualCanvasEnabled"].toBool(false));
    fd->addRow("", m_dualCanvas);
    m_dualPreview = track(new QCheckBox("Show horizontal Dual-Canvas preview in detached side dock"));
    m_dualPreview->setChecked(m_s["dualCanvasPreviewEnabled"].toBool(true));
    fd->addRow("", m_dualPreview);
    m_dualRecordTarget = track(new QComboBox);
    m_dualRecordTarget->addItem("Horizontal only (Dual-Canvas)", "Horizontal");
    m_dualRecordTarget->addItem("Vertical only (main canvas)", "Vertical");
    m_dualRecordTarget->addItem("Both Vertical & Horizontal simultaneously", "Both");
    m_dualRecordTarget->setCurrentIndex(std::max(0, m_dualRecordTarget->findData(m_s["dualRecordTarget"].toString("Horizontal"))));
    fd->addRow("Record View", m_dualRecordTarget);
    auto *dualRow = new QHBoxLayout();
    m_dualW = track(new PaddedSpinBox);
    m_dualW->setRange(128, 4096); m_dualW->setSingleStep(2); m_dualW->setSuffix(" px"); m_dualW->setValue(m_s["dualCanvasWidth"].toInt(1920));
    m_dualH = track(new PaddedSpinBox);
    m_dualH->setRange(128, 4096); m_dualH->setSingleStep(2); m_dualH->setSuffix(" px"); m_dualH->setValue(m_s["dualCanvasHeight"].toInt(1080));
    dualRow->addWidget(m_dualW, 1);
    dualRow->addWidget(new QLabel("×"));
    dualRow->addWidget(m_dualH, 1);
    fd->addRow("Recording Canvas", dualRow);
    auto updateDual = [this] {
        const bool dualOn = m_dualCanvas->isChecked();
        const bool anyDual = dualOn || (m_secondEnabled && m_secondEnabled->isChecked());
        m_dualW->setEnabled(!m_live && dualOn);
        m_dualH->setEnabled(!m_live && dualOn);
        m_dualRecordTarget->setEnabled(!m_live && dualOn);
        m_dualPreview->setEnabled(anyDual);
    };
    connect(m_dualCanvas, &QCheckBox::toggled, this, [updateDual](bool) { updateDual(); });
    if (m_secondEnabled) connect(m_secondEnabled, &QCheckBox::toggled, this, [updateDual](bool) { updateDual(); });
    m_dualCanvas->setEnabled(!m_live);
    updateDual();

    for (QWidget *field : std::initializer_list<QWidget *>{preset, m_videoWidth, m_videoHeight, m_fps})
        field->setEnabled(!m_live);
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageAudio()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Global Audio Devices", "Desktop audio and microphone are active across all scenes. Add extra devices via \"Add Source\"."));
    f->addRow("Sample Rate", value("48 kHz • stereo"));

    auto fill = [this](QComboBox *cb, const QJsonArray &devs, const QJsonObject &src) {
        cb->addItem("Disabled", "off");
        cb->addItem("System default device", "");
        for (const auto &d : devs) cb->addItem(d.toObject()["name"].toString(), d.toObject()["id"].toString());
        if (src.isEmpty()) cb->setCurrentIndex(0);
        else cb->setCurrentIndex(std::max(1, cb->findData(src["deviceId"].toString())));
        track(cb);
    };
    const QJsonArray sources = m_s["sources"].toArray();
    m_desktopDev = new QComboBox;
    fill(m_desktopDev, m_devices["outputs"].toArray(), firstSourceOfType(sources, "AudioOutput"));
    f->addRow("Desktop Audio", m_desktopDev);
    m_micDev = new QComboBox;
    fill(m_micDev, m_devices["inputs"].toArray(), firstSourceOfType(sources, "AudioInput"));
    f->addRow("Microphone", m_micDev);
    return scrollPage(w, v);
}

QWidget *SettingsDialog::pageHotkeys()
{
    QVBoxLayout *v;
    auto *w = page(&v);
    QFormLayout *f;
    v->addWidget(card(w, &f, "Hotkeys", "Work globally — even when a game is in the foreground. Click a field and press a key combination."));
    const QJsonObject hk = m_s["hotkeys"].toObject();
    for (const auto &[id, label] : Hotkeys::actions()) {
        auto *row = new QHBoxLayout();
        auto *edit = new QKeySequenceEdit(QKeySequence(hk[id].toString(), QKeySequence::PortableText));
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        if (auto *le = edit->findChild<QLineEdit *>()) le->setPlaceholderText("Click and press shortcut…");
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
    v->addWidget(card(w, &f, "Performance"));
    m_priority = track(new QComboBox);
    m_priority->addItem("Normal", "Normal");
    m_priority->addItem("Above Normal (recommended)", "AboveNormal");
    m_priority->addItem("High", "High");
    m_priority->setCurrentIndex(std::max(0, m_priority->findData(m_s["processPriority"].toString("AboveNormal"))));
    f->addRow("Process Priority", m_priority);
    m_preview = track(new QCheckBox("Live preview (disable for maximum performance)"));
    m_preview->setChecked(m_s["previewEnabled"].toBool(true));
    f->addRow("Preview", m_preview);

    QFormLayout *fn;
    v->addWidget(card(w, &fn, "Auto-Reconnect", "Automatically reconnects to the server and resumes streaming if the connection drops."));
    m_reconnect = track(new QCheckBox("Enable"));
    m_reconnect->setChecked(m_s["autoReconnect"].toBool(true));
    fn->addRow("", m_reconnect);
    m_retryDelay = track(new PaddedSpinBox);
    m_retryDelay->setRange(1, 60);
    m_retryDelay->setSuffix(" s");
    m_retryDelay->setValue(m_s["reconnectDelaySec"].toInt(2));
    fn->addRow("Retry Delay", m_retryDelay);
    m_retryMax = track(new PaddedSpinBox);
    m_retryMax->setRange(1, 1000);
    m_retryMax->setValue(m_s["reconnectMaxRetries"].toInt(25));
    fn->addRow("Maximum Retries", m_retryMax);
    connect(m_reconnect, &QCheckBox::toggled, m_retryDelay, &QWidget::setEnabled);
    connect(m_reconnect, &QCheckBox::toggled, m_retryMax, &QWidget::setEnabled);
    m_retryDelay->setEnabled(m_reconnect->isChecked());
    m_retryMax->setEnabled(m_reconnect->isChecked());

    QFormLayout *fd;
    v->addWidget(card(w, &fd, "Diagnostics"));
    auto *logBtn = new QPushButton("Open Log File");
    connect(logBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(EngineHost::instance().callObj("logPath")["path"].toString()));
    });
    fd->addRow("Log", logBtn);
    return scrollPage(w, v);
}

// =============================== result ===============================

QJsonObject SettingsDialog::result() const
{
    QJsonObject s = m_s;
    s["confirmStreamStop"] = m_confirmStop->isChecked();
    s["recordToFile"] = m_autoRecord->isChecked();
    s["minimizeToTray"] = m_minToTray->isChecked();
    s["snapping"] = m_snapping->isChecked();
    s["snapToSources"] = m_snapSources->isChecked();
    s["autoUpdateEnabled"] = m_autoUpdate->isChecked();

    s["serverUrl"] = m_server->text().trimmed();
    s["streamKey"] = m_key->text().trimmed();
    s["tikTokSignerKey"] = m_signerKey->text().trimmed();
    s["tikTokSignerUrl"] = m_signerUrl->text().trimmed();
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
    s["recordCodec"] = m_recCodec->currentData().toString();
    s["multiTrackAudio"] = m_multiTrack->isChecked();
    s["replayBufferSec"] = m_replaySec->value();

    s["width"] = m_videoWidth->value() & ~1;
    s["height"] = m_videoHeight->value() & ~1;
    s["fps"] = m_fps->value();
    s["sceneTransitionMs"] = m_transitionMs->value();
    s["dualCanvasEnabled"] = m_dualCanvas->isChecked();
    s["dualCanvasPreviewEnabled"] = m_dualPreview->isChecked();
    s["dualRecordTarget"] = m_dualRecordTarget->currentData().toString();
    s["dualCanvasWidth"] = m_dualW->value() & ~1;
    s["dualCanvasHeight"] = m_dualH->value() & ~1;

    // Global audio devices are active across all scenes.
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
    applyDevice("AudioOutput", "Desktop Audio", m_desktopDev->currentData().toString());
    applyDevice("AudioInput", "Microphone", m_micDev->currentData().toString());
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
