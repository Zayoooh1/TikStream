#include "EngineHost.h"

#include <windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <coreclr_delegates.h>
#include <hostfxr.h>

EngineHost &EngineHost::instance()
{
    static EngineHost host;
    return host;
}

namespace {
using get_hostfxr_path_fn = int(__stdcall *)(char_t *buffer, size_t *buffer_size, const void *parameters);

std::wstring toW(const QString &s) { return QDir::toNativeSeparators(s).toStdWString(); }
} // namespace

bool EngineHost::load(const QString &engineDir, QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error) *error = msg;
        return false;
    };

    // 1. nethost.dll (next to exe) locates installed hostfxr
    HMODULE nethost = LoadLibraryW(toW(QCoreApplication::applicationDirPath() + "/nethost.dll").c_str());
    if (!nethost) nethost = LoadLibraryW(L"nethost.dll");
    if (!nethost) return fail("Missing nethost.dll next to the executable.");
    auto getPath = reinterpret_cast<get_hostfxr_path_fn>(GetProcAddress(nethost, "get_hostfxr_path"));
    if (!getPath) return fail("nethost.dll is missing get_hostfxr_path.");

    wchar_t buf[MAX_PATH * 2];
    size_t size = sizeof(buf) / sizeof(wchar_t);
    if (getPath(buf, &size, nullptr) != 0)
        return fail(".NET 10 runtime not found. Please install .NET 10 Desktop Runtime.");

    HMODULE fxr = LoadLibraryW(buf);
    if (!fxr) return fail("Failed to load hostfxr.dll.");
    auto init = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(GetProcAddress(fxr, "hostfxr_initialize_for_runtime_config"));
    auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(fxr, "hostfxr_get_runtime_delegate"));
    auto closeFxr = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(fxr, "hostfxr_close"));
    if (!init || !getDelegate || !closeFxr) return fail("hostfxr.dll is missing required exports.");

    // 2. Initialize CoreCLR using the engine library's runtimeconfig
    const std::wstring config = toW(engineDir + "/TikStream.Engine.runtimeconfig.json");
    const std::wstring assembly = toW(engineDir + "/TikStream.Engine.dll");
    hostfxr_handle ctx = nullptr;
    int rc = init(config.c_str(), nullptr, &ctx);
    if (rc < 0 || !ctx) return fail(QString("hostfxr_initialize_for_runtime_config: 0x%1").arg(uint(rc), 8, 16, QChar('0')));

    load_assembly_and_get_function_pointer_fn loadFn = nullptr;
    rc = getDelegate(ctx, hdt_load_assembly_and_get_function_pointer, reinterpret_cast<void **>(&loadFn));
    closeFxr(ctx);
    if (rc != 0 || !loadFn) return fail(QString("hostfxr_get_runtime_delegate: 0x%1").arg(uint(rc), 8, 16, QChar('0')));

    // 3. Resolve [UnmanagedCallersOnly] exports from TikStream.NativeApi
    auto get = [&](const wchar_t *method, void **out) {
        return loadFn(assembly.c_str(), L"TikStream.NativeApi, TikStream.Engine", method, UNMANAGEDCALLERSONLY_METHOD, nullptr, out);
    };
    if (get(L"Call", reinterpret_cast<void **>(&m_call)) != 0 ||
        get(L"FreeString", reinterpret_cast<void **>(&m_free)) != 0 ||
        get(L"SetSourcePixels", reinterpret_cast<void **>(&m_pixels)) != 0) {
        m_call = nullptr;
        return fail("Failed to load TikStream.Engine.dll.");
    }
    return true;
}

QJsonDocument EngineHost::call(const QString &method, const QJsonObject &args)
{
    if (!m_call) return {};
    const QByteArray m = method.toUtf8();
    const QByteArray a = QJsonDocument(args).toJson(QJsonDocument::Compact);
    char *res = m_call(m.constData(), a.constData());
    if (!res) return {};
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray(res));
    m_free(res);
    return doc;
}

void EngineHost::setSourcePixels(const QString &id, int w, int h, int stride, const uchar *bgraPremul)
{
    if (!m_pixels) return;
    const QByteArray i = id.toUtf8();
    m_pixels(i.constData(), w, h, stride, bgraPremul);
}
