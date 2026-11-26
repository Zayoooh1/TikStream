#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QGridLayout;
class QScrollArea;

class SoundboardWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit SoundboardWidget(QWidget *parent = nullptr);

    void setPads(const QJsonArray &pads);
    QJsonArray pads() const { return m_pads; }

signals:
    void padTriggered(const QString &id, const QString &filePath, double volume);
    void stopAllRequested();
    void padsChanged(const QJsonArray &pads);

private:
    void rebuildGrid();
    bool editPadDialog(QJsonObject &pad, bool isNew);
    void addPad();
    void editPad(int index);
    void duplicatePad(int index);
    void deletePad(int index);

    QJsonArray m_pads;
    QWidget *m_gridHost = nullptr;
    QGridLayout *m_grid = nullptr;
};
