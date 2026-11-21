#pragma once
#include <QImage>
#include <QJsonObject>

// Rasteryzuje źródło „Tekst” albo „Obraz” do QImage ARGB32_Premultiplied — w pamięci to BGRA z premultiplikacją,
// czyli dokładnie format tekstury, której oczekuje silnik.
QImage rasterizeSource(const QJsonObject &source);
QString rasterKey(const QJsonObject &source);
