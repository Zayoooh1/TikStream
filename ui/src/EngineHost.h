#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// Bridge to the C# engine: loads the .NET runtime in-process (hostfxr) and invokes NativeApi exports.
class EngineHost
{
public:
    static EngineHost &instance();

    bool load(const QString &engineDir, QString *error);
    bool isLoaded() const { return m_call != nullptr; }

    QJsonDocument call(const QString &method, const QJsonObject &args = {});
    QJsonObject callObj(const QString &method, const QJsonObject &args = {}) { return call(method, args).object(); }
    QJsonArray callArr(const QString &method, const QJsonObject &args = {}) { return call(method, args).array(); }

    void setSourcePixels(const QString &id, int w, int h, int stride, const uchar *bgraPremul);

private:
    using CallFn = char *(__stdcall *)(const char *, const char *);
    using FreeFn = void(__stdcall *)(char *);
    using PixelsFn = void(__stdcall *)(const char *, int, int, int, const uchar *);

    CallFn m_call = nullptr;
    FreeFn m_free = nullptr;
    PixelsFn m_pixels = nullptr;
};
