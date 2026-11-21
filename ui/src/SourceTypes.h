#pragma once
#include <QList>
#include <QString>

// Metadane typów źródeł — wspólne dla listy źródeł, okna dodawania i właściwości.
struct SourceTypeInfo
{
    QString type;        // nazwa jak w silniku (SourceType)
    QString name;        // nazwa dla użytkownika
    QString icon;        // ikona z :/icons
    QString description; // jedno zdanie: do czego to jest
    bool audio = false;
};

inline const QList<SourceTypeInfo> &sourceTypes()
{
    static const QList<SourceTypeInfo> list = {
        {"Screen", "Przechwytywanie ekranu", "monitor", "Cały monitor — bez żółtej ramki Windows. Dobre do gier w pełnym ekranie i pulpitu."},
        {"Window", "Przechwytywanie okna", "window", "Obraz z wybranego okna aplikacji."},
        {"Game", "Przechwytywanie gry", "gamepad", "Aplikacja pełnoekranowa lub wybrane okno gry."},
        {"Camera", "Kamerka", "camera", "Kamerka internetowa lub karta przechwytywania. Wszystkie tryby urządzenia, aż do 1080p 120 fps."},
        {"Image", "Obraz", "image", "Plik PNG, JPG lub GIF — logo, ramka, nakładka z przezroczystością."},
        {"Text", "Tekst", "text", "Napis z obrysem i tłem — nick, tytuł streamu, informacja."},
        {"Media", "Multimedia (wideo)", "play", "Plik wideo z dźwiękiem: intro, przerwa, animacja. Może się zapętlać."},
        {"Color", "Kolor", "palette", "Jednolity prostokąt koloru — tło, pasek, przyciemnienie."},
        {"ChatOverlay", "Overlay czatu LIVE", "text", "Czat, prezenty i tapy TikTok LIVE bezpośrednio na streamie."},
        {"AudioInput", "Mikrofon / wejście dźwięku", "mic", "Mikrofon lub inne wejście dźwięku w tej scenie.", true},
        {"AudioApplication", "Dźwięk aplikacji", "speaker", "Dźwięk wybranej aplikacji i jej podprocesów.", true},
        {"AudioOutput", "Dźwięk pulpitu / wyjście", "speaker", "To, co słyszysz w głośnikach lub słuchawkach (gra, muzyka, Discord).", true},
    };
    return list;
}

inline SourceTypeInfo sourceTypeInfo(const QString &type)
{
    for (const auto &t : sourceTypes())
        if (t.type == type) return t;
    return {type, type, "image", ""};
}
