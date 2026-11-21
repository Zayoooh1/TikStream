#include "AddSourceDialog.h"

#include "SourceTypes.h"
#include "Widgets.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

AddSourceDialog::AddSourceDialog(const QJsonArray &sources, QWidget *parent) : QDialog(parent), m_sources(sources)
{
    setWindowTitle("Add Source");
    resize(860, 560);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    // ---------- left side: types ----------
    m_types = new QListWidget(this);
    m_types->setObjectName("settingsNav");
    m_types->setFixedWidth(280);
    m_types->setIconSize(QSize(20, 20));
    auto header = [this](const QString &t) {
        auto *it = new QListWidgetItem(t.toUpper(), m_types);
        it->setFlags(Qt::NoItemFlags);
        it->setSizeHint(QSize(0, 30));
        QFont f = it->font();
        f.setPointSizeF(7.5);
        f.setBold(true);
        it->setFont(f);
        it->setForeground(QColor(0x6e, 0x72, 0x85));
    };
    header("Video");
    bool audioHeader = false;
    for (const auto &t : sourceTypes()) {
        if (t.audio && !audioHeader) { header("Audio"); audioHeader = true; }
        auto *it = new QListWidgetItem(themedIcon(t.icon), t.name, m_types);
        it->setData(Qt::UserRole, t.type);
        it->setSizeHint(QSize(0, 38));
    }
    root->addWidget(m_types);

    // ---------- right side ----------
    auto *right = new QFrame(this);
    right->setObjectName("card");
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(20, 18, 20, 18);
    rv->setSpacing(12);

    auto *head = new QHBoxLayout();
    m_icon = new QLabel;
    m_icon->setFixedSize(52, 52);
    m_icon->setAlignment(Qt::AlignCenter);
    m_icon->setObjectName("bigIcon");
    auto *ht = new QVBoxLayout();
    ht->setSpacing(3);
    m_title = new QLabel;
    m_title->setObjectName("dialogTitle");
    m_desc = new QLabel;
    m_desc->setObjectName("hint");
    m_desc->setWordWrap(true);
    ht->addWidget(m_title);
    ht->addWidget(m_desc);
    head->addWidget(m_icon);
    head->addSpacing(8);
    head->addLayout(ht, 1);
    rv->addLayout(head);

    auto *line = new QWidget;
    line->setObjectName("sectionLine");
    line->setFixedHeight(1);
    rv->addWidget(line);

    m_new = new QRadioButton("Create new source");
    m_new->setChecked(true);
    rv->addWidget(m_new);
    m_name = new QLineEdit;
    m_name->setPlaceholderText("Source name");
    auto *nameRow = new QHBoxLayout();
    nameRow->setContentsMargins(26, 0, 0, 0);
    nameRow->addWidget(m_name);
    rv->addLayout(nameRow);

    m_existingBox = new QWidget;
    auto *ev = new QVBoxLayout(m_existingBox);
    ev->setContentsMargins(0, 6, 0, 0);
    m_existing = new QRadioButton("Add existing source (reuse the same device/file in another scene)");
    auto *modeGroup = new QButtonGroup(this);
    modeGroup->addButton(m_new);
    modeGroup->addButton(m_existing);
    ev->addWidget(m_existing);
    m_existingList = new QListWidget;
    m_existingList->setObjectName("existingList");
    m_existingList->setIconSize(QSize(18, 18));
    auto *exRow = new QHBoxLayout();
    exRow->setContentsMargins(26, 0, 0, 0);
    exRow->addWidget(m_existingList);
    ev->addLayout(exRow);
    rv->addWidget(m_existingBox, 1);
    rv->addStretch();

    m_visible = new QCheckBox("Make source visible");
    m_visible->setChecked(true);
    rv->addWidget(m_visible);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch();
    auto *cancel = new QPushButton("Cancel");
    m_ok = new QPushButton("Add Source");
    m_ok->setObjectName("primary");
    m_ok->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(m_ok);
    rv->addLayout(buttons);
    root->addWidget(right, 1);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_ok, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_types, &QListWidget::currentRowChanged, this, &AddSourceDialog::onTypeChanged);
    connect(m_types, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) { if (it->flags() & Qt::ItemIsEnabled) accept(); });
    connect(m_existingList, &QListWidget::itemClicked, this, [this] { m_existing->setChecked(true); });
    connect(m_existingList, &QListWidget::itemDoubleClicked, this, [this] { m_existing->setChecked(true); accept(); });
    connect(m_name, &QLineEdit::textEdited, this, [this] { m_new->setChecked(true); });
    m_types->setCurrentRow(1);
}

QString AddSourceDialog::uniqueName(const QString &base) const
{
    auto taken = [this](const QString &n) {
        for (const auto &v : m_sources)
            if (v.toObject()["name"].toString() == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i)
        if (!taken(QString("%1 %2").arg(base).arg(i))) return QString("%1 %2").arg(base).arg(i);
}

void AddSourceDialog::onTypeChanged()
{
    auto *it = m_types->currentItem();
    if (!it || !(it->flags() & Qt::ItemIsEnabled)) return;
    const auto info = sourceTypeInfo(it->data(Qt::UserRole).toString());
    m_icon->setPixmap(themedIcon(info.icon).pixmap(30, 30));
    m_title->setText(info.name);
    m_desc->setText(info.description);
    m_name->setText(uniqueName(info.name));
    m_new->setChecked(true);

    m_existingList->clear();
    for (const auto &v : m_sources) {
        const auto s = v.toObject();
        if (s["type"].toString() != info.type || s["globalAudio"].toBool()) continue;
        auto *e = new QListWidgetItem(themedIcon(info.icon), s["name"].toString(), m_existingList);
        e->setData(Qt::UserRole, s["id"].toString());
    }
    m_existingBox->setVisible(m_existingList->count() > 0);
    m_visible->setText(info.audio ? "Make source active (audio enabled)" : "Make source visible");
    m_name->setFocus();
    m_name->selectAll();
}

AddSourceResult AddSourceDialog::result() const
{
    AddSourceResult r;
    if (auto *it = m_types->currentItem()) r.type = it->data(Qt::UserRole).toString();
    r.visible = m_visible->isChecked();
    if (m_existing->isChecked() && m_existingList->currentItem()) r.existingId = m_existingList->currentItem()->data(Qt::UserRole).toString();
    r.name = m_name->text().trimmed().isEmpty() ? sourceTypeInfo(r.type).name : m_name->text().trimmed();
    return r;
}
