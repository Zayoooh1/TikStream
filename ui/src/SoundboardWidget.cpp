#include "SoundboardWidget.h"

#include "Widgets.h"

#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString makePadId() { return QUuid::createUuid().toString(QUuid::Id128); }
} // namespace

SoundboardWidget::SoundboardWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumWidth(210);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(6);
    auto *addBtn = new QPushButton("+ Dodaj pad", this);
    addBtn->setCursor(Qt::PointingHandCursor);
    auto *stopBtn = new QPushButton("Stop All", this);
    stopBtn->setCursor(Qt::PointingHandCursor);
    stopBtn->setToolTip("Stop all currently playing soundboard clips");
    bar->addWidget(addBtn, 1);
    bar->addWidget(stopBtn);
    root->addLayout(bar);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_gridHost = new QWidget(scroll);
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(2, 2, 2, 2);
    m_grid->setSpacing(6);
    scroll->setWidget(m_gridHost);
    root->addWidget(scroll, 1);

    connect(addBtn, &QPushButton::clicked, this, &SoundboardWidget::addPad);
    connect(stopBtn, &QPushButton::clicked, this, &SoundboardWidget::stopAllRequested);

    rebuildGrid();
}

void SoundboardWidget::setPads(const QJsonArray &pads)
{
    m_pads = pads;
    rebuildGrid();
}

void SoundboardWidget::rebuildGrid()
{
    while (QLayoutItem *it = m_grid->takeAt(0)) {
        if (QWidget *w = it->widget()) w->deleteLater();
        delete it;
    }

    if (m_pads.isEmpty()) {
        auto *empty = new QLabel("Brak padów.\nKliknij \"+ Dodaj pad\", aby dodać dźwięk pod przycisk, hotkey lub gift TikTok.", m_gridHost);
        empty->setObjectName("hint");
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        m_grid->addWidget(empty, 0, 0, 1, 3);
        m_grid->setRowStretch(1, 1);
        return;
    }

    constexpr int cols = 3;
    for (int i = 0; i < m_pads.size(); ++i) {
        const QJsonObject pad = m_pads[i].toObject();
        const QString id = pad["id"].toString();
        const QString name = pad["name"].toString("Pad");
        const QString path = pad["filePath"].toString();
        const double vol = pad["volume"].toDouble(1.0);
        const QString colorHex = pad["colorHex"].toString("#2563EB");
        const QString hotkey = pad["hotkey"].toString();
        const QString giftName = pad["giftTriggerName"].toString().trimmed();
        const int minDiamonds = pad["giftMinDiamonds"].toInt(0);

        QColor bg(colorHex);
        if (!bg.isValid()) bg = QColor(0x25, 0x63, 0xEB);
        const QColor bgHover = bg.lighter(115);
        const QColor bgPress = bg.darker(118);
        const QString textColor = bg.lightness() > 160 ? QStringLiteral("#0f172a") : QStringLiteral("#ffffff");

        QString label = name;
        QStringList badges;
        if (!hotkey.isEmpty()) badges << hotkey;
        if (!giftName.isEmpty()) badges << (QStringLiteral("🎁 ") + giftName);
        else if (minDiamonds > 0) badges << QStringLiteral("🎁 ≥%1💎").arg(minDiamonds);
        if (!badges.isEmpty()) label += "\n" + badges.join(" · ");

        auto *btn = new QPushButton(label, m_gridHost);
        btn->setMinimumHeight(58);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        btn->setToolTip(path.isEmpty() ? QStringLiteral("Right-click to assign an audio file") : path);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: %2; border: 1px solid rgba(255,255,255,48); "
            "border-radius: 8px; padding: 6px; font-weight: 600; font-size: 12px; }"
            "QPushButton:hover { background: %3; }"
            "QPushButton:pressed { background: %4; }")
            .arg(bg.name(), textColor, bgHover.name(), bgPress.name()));

        connect(btn, &QPushButton::clicked, this, [this, id, path, vol] {
            emit padTriggered(id, path, vol);
        });
        connect(btn, &QWidget::customContextMenuRequested, this, [this, btn, i, id, path, vol](const QPoint &pos) {
            QMenu m(btn);
            m.addAction("Odtwórz", this, [this, id, path, vol] { emit padTriggered(id, path, vol); });
            m.addAction("Edytuj pad…", this, [this, i] { editPad(i); });
            m.addAction("Duplikuj", this, [this, i] { duplicatePad(i); });
            m.addSeparator();
            m.addAction("Usuń", this, [this, i] { deletePad(i); });
            m.exec(btn->mapToGlobal(pos));
        });

        m_grid->addWidget(btn, i / cols, i % cols);
    }
    m_grid->setRowStretch((m_pads.size() + cols - 1) / cols + 1, 1);
}

bool SoundboardWidget::editPadDialog(QJsonObject &pad, bool isNew)
{
    QDialog dlg(this);
    dlg.setWindowTitle(isNew ? "Nowy pad Soundboardu" : "Edytuj pad Soundboardu");
    dlg.resize(460, 0);

    auto *v = new QVBoxLayout(&dlg);
    auto *f = new QFormLayout;
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    v->addLayout(f);

    auto *nameEdit = new QLineEdit(pad["name"].toString(isNew ? "Airhorn" : "Pad"));
    f->addRow("Nazwa", nameEdit);

    auto *pathRow = new QHBoxLayout;
    auto *pathEdit = new QLineEdit(pad["filePath"].toString());
    pathEdit->setPlaceholderText("Plik .mp3 / .wav / .ogg");
    auto *browseBtn = new QPushButton("Przeglądaj…");
    pathRow->addWidget(pathEdit, 1);
    pathRow->addWidget(browseBtn);
    f->addRow("Plik audio", pathRow);
    connect(browseBtn, &QPushButton::clicked, &dlg, [&dlg, pathEdit, nameEdit] {
        const QString p = QFileDialog::getOpenFileName(&dlg, "Wybierz dźwięk", {}, "Audio (*.mp3 *.wav *.ogg);;All Files (*.*)");
        if (!p.isEmpty()) {
            pathEdit->setText(QDir::toNativeSeparators(p));
            if (nameEdit->text().trimmed().isEmpty() || nameEdit->text() == "Airhorn")
                nameEdit->setText(QFileInfo(p).completeBaseName());
        }
    });

    auto *volSpin = new PaddedSpinBox;
    volSpin->setRange(0, 200);
    volSpin->setSuffix(" %");
    volSpin->setValue(int(std::lround(pad["volume"].toDouble(1.0) * 100.0)));
    f->addRow("Głośność", volSpin);

    auto *colorBtn = new ColorButton("Kolor kafelka");
    QColor initCol(pad["colorHex"].toString("#2563EB"));
    if (!initCol.isValid()) initCol = QColor(0x25, 0x63, 0xEB);
    colorBtn->setColor(initCol);
    f->addRow("Kolor", colorBtn);

    auto *hkEdit = new QKeySequenceEdit(QKeySequence(pad["hotkey"].toString(), QKeySequence::PortableText));
    hkEdit->setMaximumSequenceLength(1);
    hkEdit->setClearButtonEnabled(true);
    f->addRow("Globalny hotkey", hkEdit);

    auto *giftNameEdit = new QLineEdit(pad["giftTriggerName"].toString());
    giftNameEdit->setPlaceholderText("np. Rose, Lion (puste = wyłączone)");
    f->addRow("Wyzwalacz giftu (nazwa)", giftNameEdit);

    auto *giftMinSpin = new PaddedSpinBox;
    giftMinSpin->setRange(0, 1000000);
    giftMinSpin->setSuffix(" 💎");
    giftMinSpin->setSpecialValueText("Wyłączone");
    giftMinSpin->setValue(pad["giftMinDiamonds"].toInt(0));
    f->addRow("Min. diamentów (gift)", giftMinSpin);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return false;

    if (pad["id"].toString().isEmpty()) pad["id"] = makePadId();
    pad["name"] = nameEdit->text().trimmed().isEmpty() ? QStringLiteral("Pad") : nameEdit->text().trimmed();
    pad["filePath"] = pathEdit->text().trimmed();
    pad["volume"] = volSpin->value() / 100.0;
    pad["colorHex"] = colorBtn->color().name(QColor::HexRgb);
    pad["hotkey"] = hkEdit->keySequence().toString(QKeySequence::PortableText);
    pad["giftTriggerName"] = giftNameEdit->text().trimmed();
    pad["giftMinDiamonds"] = giftMinSpin->value();
    return true;
}

void SoundboardWidget::addPad()
{
    QJsonObject pad{
        {"id", makePadId()},
        {"name", "New Pad"},
        {"filePath", ""},
        {"volume", 1.0},
        {"colorHex", "#2563EB"},
        {"hotkey", ""},
        {"giftTriggerName", ""},
        {"giftMinDiamonds", 0},
    };
    if (!editPadDialog(pad, true)) return;
    m_pads.append(pad);
    rebuildGrid();
    emit padsChanged(m_pads);
}

void SoundboardWidget::editPad(int index)
{
    if (index < 0 || index >= m_pads.size()) return;
    QJsonObject pad = m_pads[index].toObject();
    if (!editPadDialog(pad, false)) return;
    m_pads[index] = pad;
    rebuildGrid();
    emit padsChanged(m_pads);
}

void SoundboardWidget::duplicatePad(int index)
{
    if (index < 0 || index >= m_pads.size()) return;
    QJsonObject copy = m_pads[index].toObject();
    copy["id"] = makePadId();
    copy["name"] = copy["name"].toString() + " (kopia)";
    copy["hotkey"] = "";
    m_pads.append(copy);
    rebuildGrid();
    emit padsChanged(m_pads);
}

void SoundboardWidget::deletePad(int index)
{
    if (index < 0 || index >= m_pads.size()) return;
    m_pads.removeAt(index);
    rebuildGrid();
    emit padsChanged(m_pads);
}
