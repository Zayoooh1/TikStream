#pragma once
#include <QDialog>
#include <QJsonArray>

class QListWidget;
class QLabel;
class QLineEdit;
class QRadioButton;
class QCheckBox;
class QPushButton;
class QWidget;

struct AddSourceResult
{
    QString type;
    QString name;        // new source
    QString existingId;  // or existing source (empty = new)
    bool visible = true;
};

class AddSourceDialog : public QDialog
{
    Q_OBJECT
public:
    AddSourceDialog(const QJsonArray &sources, QWidget *parent = nullptr);
    AddSourceResult result() const;

private:
    void onTypeChanged();
    QString uniqueName(const QString &base) const;

    QJsonArray m_sources;
    QListWidget *m_types = nullptr;
    QLabel *m_icon = nullptr, *m_title = nullptr, *m_desc = nullptr;
    QRadioButton *m_new = nullptr, *m_existing = nullptr;
    QLineEdit *m_name = nullptr;
    QListWidget *m_existingList = nullptr;
    QWidget *m_existingBox = nullptr;
    QCheckBox *m_visible = nullptr;
    QPushButton *m_ok = nullptr;
};
