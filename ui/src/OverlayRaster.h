#pragma once
#include <QImage>
#include <QJsonObject>

// Rasterizes a "Text" or "Image" source into a premultiplied ARGB32 QImage — premultiplied BGRA in memory,
// matching the exact texture format expected by the engine.
QImage rasterizeSource(const QJsonObject &source);
QString rasterKey(const QJsonObject &source);
