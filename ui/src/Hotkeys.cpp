#include "Hotkeys.h"

#include <windows.h>

#include <QCoreApplication>

namespace {
UINT toVk(Qt::Key k)
{
    if (k >= Qt::Key_A && k <= Qt::Key_Z) return UINT('A' + (k - Qt::Key_A));
    if (k >= Qt::Key_0 && k <= Qt::Key_9) return UINT('0' + (k - Qt::Key_0));
    if (k >= Qt::Key_F1 && k <= Qt::Key_F24) return UINT(VK_F1 + (k - Qt::Key_F1));
    switch (k) {
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Insert: return VK_INSERT;
    case Qt::Key_Delete: return VK_DELETE;
    case Qt::Key_Home: return VK_HOME;
    case Qt::Key_End: return VK_END;
    case Qt::Key_PageUp: return VK_PRIOR;
    case Qt::Key_PageDown: return VK_NEXT;
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Pause: return VK_PAUSE;
    case Qt::Key_ScrollLock: return VK_SCROLL;
    case Qt::Key_Minus: return VK_OEM_MINUS;
    case Qt::Key_Plus: case Qt::Key_Equal: return VK_OEM_PLUS;
    case Qt::Key_Comma: return VK_OEM_COMMA;
    case Qt::Key_Period: return VK_OEM_PERIOD;
    default: return 0;
    }
}
} // namespace

Hotkeys::Hotkeys(QObject *parent) : QObject(parent) { QCoreApplication::instance()->installNativeEventFilter(this); }

Hotkeys::~Hotkeys()
{
    clear();
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

QList<QPair<QString, QString>> Hotkeys::actions()
{
    return {
        {"toggleStream", "Start / stop streaming"},
        {"toggleRecord", "Start / stop recording"},
        {"toggleReplay", "Start / stop replay buffer"},
        {"saveReplay", "Save replay clip"},
        {"toggleMic", "Mute / unmute microphone"},
        {"nextScene", "Next scene"},
        {"prevScene", "Previous scene"},
    };
}

void Hotkeys::clear()
{
    for (auto it = m_ids.begin(); it != m_ids.end(); ++it) UnregisterHotKey(nullptr, it.key());
    m_ids.clear();
}

QStringList Hotkeys::apply(const QHash<QString, QString> &map)
{
    clear();
    QStringList failed;
    int id = 0x5100;
    for (auto it = map.begin(); it != map.end(); ++it) {
        const QKeySequence seq(it.value(), QKeySequence::PortableText);
        if (seq.isEmpty()) continue;
        const QKeyCombination kc = seq[0];
        const UINT vk = toVk(kc.key());
        if (!vk) { failed << it.key(); continue; }
        UINT mods = MOD_NOREPEAT;
        if (kc.keyboardModifiers() & Qt::ControlModifier) mods |= MOD_CONTROL;
        if (kc.keyboardModifiers() & Qt::ShiftModifier) mods |= MOD_SHIFT;
        if (kc.keyboardModifiers() & Qt::AltModifier) mods |= MOD_ALT;
        if (kc.keyboardModifiers() & Qt::MetaModifier) mods |= MOD_WIN;
        if (RegisterHotKey(nullptr, id, mods, vk)) m_ids[id++] = it.key();
        else failed << it.key();
    }
    return failed;
}

bool Hotkeys::nativeEventFilter(const QByteArray &type, void *message, qintptr *)
{
    if (type != "windows_generic_MSG") return false;
    auto *msg = static_cast<MSG *>(message);
    if (msg->message == WM_HOTKEY && m_ids.contains(int(msg->wParam))) {
        emit triggered(m_ids.value(int(msg->wParam)));
        return true;
    }
    return false;
}
