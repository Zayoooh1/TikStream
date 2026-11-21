#include "AddSourceDialog.h"
#include "EngineHost.h"
#include "MainWindow.h"
#include "SettingsDialog.h"

#include <QListWidget>

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QMessageBox>
#include <QPalette>
#include <QScreen>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>

static void applyDarkPalette(QApplication &app)
{
    QPalette p;
    const QColor bg(0x1b, 0x1c, 0x23), base(0x15, 0x16, 0x1c), text(0xe6, 0xe7, 0xee), accent(0xfe, 0x2c, 0x55);
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, bg);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0x2a, 0x2c, 0x37));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, QColor(0x3d, 0x5a, 0xfe));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x2c, 0x37));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x6e, 0x72, 0x85));
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x5d, 0x60, 0x70));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x5d, 0x60, 0x70));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x5d, 0x60, 0x70));
    app.setPalette(p);
}

int main(int argc, char *argv[])
{
    const QSettings settings("TikStream", "UI2");
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR"))
        qputenv("QT_SCALE_FACTOR", QByteArray::number(settings.value("interfaceScale", 85).toInt() / 100.0));
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    app.setApplicationName("TikStream");
    app.setOrganizationName("TikStream");
    app.setStyle(QStyleFactory::create("Fusion"));
    applyDarkPalette(app);
    QFont f("Segoe UI", 9);
    f.setHintingPreference(QFont::PreferNoHinting);
    app.setFont(f);

    QFile qss(":/theme.qss");
    if (qss.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    QString err;
    auto &host = EngineHost::instance();
    if (!host.load(QCoreApplication::applicationDirPath() + "/engine", &err)) {
        QMessageBox::critical(nullptr, "TikStream", "Failed to start engine:\n\n" + err);
        return 1;
    }
    const QJsonObject init = host.callObj("init");
    if (init.contains("error")) {
        QMessageBox::critical(nullptr, "TikStream", "Engine error:\n\n" + init["error"].toString());
        return 1;
    }

    MainWindow w(init["canvasWidth"].toInt(1080), init["canvasHeight"].toInt(1920), init["gpu"].toString());
    const QStringList args = app.arguments();
    if (args.contains("--minimized")) w.showMinimized();
    else w.show();

    // --screenshot <file.png> : capture window screenshot after startup (for UI verification)
    const int si = args.indexOf("--screenshot");
    if (si >= 0 && si + 1 < args.size()) {
        const QString out = args[si + 1];
        QTimer::singleShot(4000, &w, [&w, out] {
            QScreen *scr = w.screen();
            const QRect g = w.frameGeometry();
            scr->grabWindow(0, g.x() - scr->geometry().x(), g.y() - scr->geometry().y(), g.width(), g.height()).save(out);
            w.close();
        });
    }

    // --test <folder> : save camera list, record 5 s through the full pipeline, capture screenshot, exit
    const int ti = args.indexOf("--test");
    if (ti >= 0 && ti + 1 < args.size()) {
        const QString dir = args[ti + 1];
        QTimer::singleShot(2500, &w, [&host, &w, dir] {
            QFile cams(dir + "/cameras.json");
            if (cams.open(QIODevice::WriteOnly)) cams.write(host.call("listCameras").toJson());
            QJsonObject s = host.callObj("getSettings");
            const QJsonValue oldFolder = s["recordFolder"];
            s["recordFolder"] = dir;
            host.call("setSettings", s);
            const auto r = host.callObj("startRecord");
            QFile log(dir + "/test.txt");
            log.open(QIODevice::WriteOnly);
            log.write("startRecord: " + QJsonDocument(r).toJson(QJsonDocument::Compact) + "\n");
            QTimer::singleShot(5000, &w, [&host, &w, dir, s, oldFolder] {
                QFile log(dir + "/test.txt");
                log.open(QIODevice::Append);
                log.write("stats: " + host.call("stats").toJson(QJsonDocument::Compact) + "\n");
                QScreen *scr = w.screen();
                const QRect g = w.frameGeometry();
                scr->grabWindow(0, g.x() - scr->geometry().x(), g.y() - scr->geometry().y(), g.width(), g.height()).save(dir + "/shot.png");
                log.write("stopRecord: " + host.call("stopRecord").toJson(QJsonDocument::Compact) + "\n");
                // dialog screenshots (rendered by Qt)
                {
                    AddSourceDialog add(host.callObj("getSettings")["sources"].toArray(), &w);
                    add.show();
                    QApplication::processEvents();
                    add.grab().save(dir + "/add.png");
                    SettingsDialog set(host.callObj("getSettings"), host.callObj("listAudioDevices"), false, &w);
                    set.show();
                    auto *nav = set.findChild<QListWidget *>("settingsNav");
                    for (int p : {0, 2, 3, 4, 5}) {
                        if (nav) nav->setCurrentRow(p);
                        QApplication::processEvents();
                        set.grab().save(dir + QString("/settings%1.png").arg(p));
                    }
                }
                QJsonObject restore = s;
                restore["recordFolder"] = oldFolder;
                host.call("setSettings", restore);
                QTimer::singleShot(2000, &w, [&w] { w.close(); });
            });
        });
    }

    const int rc = app.exec();
    host.call("shutdown");
    return rc;
}
