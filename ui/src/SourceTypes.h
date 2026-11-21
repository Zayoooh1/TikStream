#pragma once
#include <QList>
#include <QString>

// Source type metadata — shared across the source list, Add Source dialog, and properties panel.
struct SourceTypeInfo
{
    QString type;        // engine enum name (SourceType)
    QString name;        // user-facing display name
    QString icon;        // icon name from :/icons
    QString description; // one-sentence description
    bool audio = false;
};

inline const QList<SourceTypeInfo> &sourceTypes()
{
    static const QList<SourceTypeInfo> list = {
        {"Screen", "Display Capture", "monitor", "Entire monitor — without the yellow Windows capture border. Great for fullscreen games and desktop."},
        {"Window", "Window Capture", "window", "Video feed from a specific application window."},
        {"Game", "Game Capture", "gamepad", "Fullscreen application or a specific game window."},
        {"Camera", "Camera", "camera", "Webcam or capture card. Supports all device modes up to 1080p 120 fps."},
        {"Image", "Image", "image", "PNG, JPG, or GIF file — logo, frame, or transparent overlay."},
        {"Text", "Text", "text", "Text with outline and background — handle, stream title, or info label."},
        {"Media", "Media Source (Video)", "play", "Video file with audio: intro, break screen, or animation. Supports looping."},
        {"Color", "Color Source", "palette", "Solid color rectangle — background, bar, or dimmer."},
        {"Browser", "Browser Source", "globe", "Web page, alert box, or HTML overlay (StreamElements, TikFinity, Streamlabs) with transparent background."},
        {"ChatOverlay", "LIVE Chat Overlay", "text", "TikTok LIVE chat, gifts, and taps rendered directly on the stream."},
        {"AudioInput", "Audio Input / Microphone", "mic", "Microphone or other audio input device in this scene.", true},
        {"AudioApplication", "Application Audio", "speaker", "Audio from a specific application and its child processes.", true},
        {"AudioOutput", "Desktop Audio / Output", "speaker", "What you hear in your speakers or headphones (game, music, Discord).", true},
    };
    return list;
}

inline SourceTypeInfo sourceTypeInfo(const QString &type)
{
    for (const auto &t : sourceTypes())
        if (t.type == type) return t;
    return {type, type, "image", ""};
}
