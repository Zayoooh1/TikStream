# TikStream — instrukcje dla Codexa

## Kontekst
Aplikacja Windows do TikTok LIVE, pionowe 1080×1920, 60 FPS. UI i raporty po polsku.
- `ui/`: Qt 6 Widgets, C++, QSS, MSYS2 UCRT64/MinGW. Główne pliki: `src/MainWindow.cpp/.h`, `src/EngineHost.cpp`, `src/PreviewWidget.cpp`, `src/AddSourceDialog.cpp`, `src/SettingsDialog.cpp`, `res/theme.qss`.
- `TikStream.Engine/`: C#/.NET 10, ładowany przez `hostfxr` w procesie UI. `NativeApi.cs`: `Call(method,json)->json`. `Engine/StreamEngine.cs`: render thread D3D11; `Engine/Sources.cs`, `Gpu/Compositor.cs`: źródła i swapchain; `Capture/CameraCapture.cs`: FFmpeg/dshow/NV12; `Capture/AudioEngine.cs` oraz `Capture/ApplicationAudioCapture.cs`: WASAPI/audio.
- `Core/Settings.cs`: `AppSettings`, `SourceDef`, `Scene`, `SceneItem`, camelCase JSON. Kopia ustawień w `MainWindow::m_s`.
- Zachowaj architekturę Qt + hostfxr, Desktop Duplication -> D3D11 -> NV12 -> NVENC zero-copy i kompatybilność danych/scen.

## Nienaruszalne zasady
- Nie blokuj render thread przez I/O, JSON, inicjalizację kodeków ani długie locki. Synchronizuj bezpiecznie stan między Qt i C#; bez zbędnych kopii GPU->CPU i alokacji per-frame.
- Drag/resize: lekkie aktualizacje transformacji najwyżej raz na klatkę, pełne `setSettings` po zakończeniu interakcji, jeśli odpowiada to bieżącej implementacji.
- Nie ukrywaj błędów przez obniżenie rozdzielczości/FPS/jakości. Rozróżniaj spadki renderu, podglądu i enkodera.
- Audio: odróżniaj urządzenia globalne od źródeł sceny; zachowaj wybór urządzenia i hot-plug. Diagnozuj tłumienie sygnału zamiast dodawać sztuczny boost.
- Zachowaj polskie UI, skale 70/85/100%, układy docków, mikser pionowy/poziomy. Nie resetuj ustawień ani scen.
- Nie przerywaj aktywnego nagrania/streamu, nie usuwaj danych i nie cofaj cudzych zmian bez zgody.

## Praca oszczędzająca kontekst
1. Zakres najpierw: dla małej poprawki znajdź symbole (`rg -n`), przeczytaj tylko istotne pliki i fragmenty. Nie skanuj ponownie całego projektu.
2. Jeśli to repozytorium Git: `git status --short` i `git diff --stat`; szczegółowy diff wyłącznie dla plików zadania. Uwzględnij zmiany po przerwanych sesjach.
3. Nie wklejaj pełnych plików ani długich logów do kontekstu; pokazuj istotne błędy/metryki i ograniczone fragmenty. Nie odczytuj ponownie niezmienionych danych bez przyczyny.
4. Dla usterek wydajności zbierz baseline CPU per-thread/frame times/FPS/`skippedFrames`, ustal przyczynę, popraw i porównaj pomiary. Bez losowych eksperymentów.
5. Testy i buildy wykonuj raz po logicznym pakiecie zmian, nie po każdej pojedynczej edycji. Po kolejnym błędzie odczytaj odpowiedni fragment logu. Nie pomijaj testów wymaganych przez ryzyko zmiany.
6. Nie uruchamiaj subagentów ani `$token-optimization` automatycznie. Skill do osobnego audytu, jeśli zostanie zlecony.
7. Przy długim/niedokończonym zadaniu aktualizuj krótki `CODEX_HANDOFF.md`: cel, potwierdzone ustalenia, zmienione pliki, testy, następny krok. Odczytuj go przy wznawianiu tego zadania, nie przy każdej rozmowie.

## Kompilacja i walidacja
- Tylko C#: `cd TikStream.Engine && dotnet publish -c Release -o ../dist/engine`.
- Tylko Qt/QSS, w MSYS2 UCRT64: `cd ui && PATH=/c/msys64/ucrt64/bin:$PATH cmake --build build`.
- Zmiany na styku Qt/C#, wspólnego API lub końcowa wersja do uruchomienia: zbuduj oba komponenty i wykonaj test integracyjny. Przed podmianą EXE zamknij aplikację (nie podczas streamu). Kopia UI: `cp ui/build/TikStream.exe dist/` z katalogu głównego w odpowiedniej powłoce.
- Test: `dist/TikStream.exe --test <folder>`; wyniki `test.txt`, zrzuty i log `%APPDATA%\TikStream\log.txt`. Zweryfikuj nagranie, wygląd UI, ~60 FPS przy ustawieniu 60 i `skippedFrames=0`. Testy fizycznych urządzeń/hot-plug wymagają osobnego potwierdzenia.
- Nie deklaruj pomyślnego buildu, pomiaru ani testu, którego nie wykonano.

## Raport
Zwięźle po polsku: przyczyna/dowody, zmiany, testy, nierozwiązane ryzyka. Bez długiej narracji i pełnych logów.
