# TikStream

Lekki, wydajny program dla systemu Windows do streamowania i nagrywania **TikTok LIVE** (pionowe 1080×1920 @ 60 FPS oraz tryb poziomy 1920×1080 @ 60 FPS), zoptymalizowany pod karty NVIDIA RTX (zero-copy VRAM → NVENC).

## Najważniejsze funkcje

- **Silnik D3D11 Zero-Copy + NVENC**: przechwytywanie ekranu (DXGI Desktop Duplication), okien/gier (Windows Graphics Capture 24H2+ / DXGI bez żółtej ramki), kamerki (DirectShow/NV12) i mediów bezpośrednio w pamięci GPU.
- **Tryb „Zero FPS Impact” dla NVENC**: 1-Pass, bez Spatial/Temporal AQ i Look-ahead — nie walczy o rdzenie CUDA z grą przy obciążeniu GPU 98–100%.
- **Obsługa HDR**: automatyczne przechwytywanie monitorów HDR (`R16G16B16A16_FLOAT`) i tone-mapping scRGB → SDR w shaderze HLSL.
- **Filtry wideo GPU**: jasność, kontrast, nasycenie, kluczowanie koloru (Chroma Key z despillem) oraz kadrowanie krawędzi (L/P/G/D).
- **Zaawansowane audio WASAPI**: przechwytywanie dźwięku wybranych aplikacji (Per-Process Loopback), tłumienie hałasu **RNNoise** i **SpeexDSP**, bramka szumów, kompresor, limiter oraz synchronizacja A/V (opóźnienie w ms).
- **Multistream**: jednoczesne wysyłanie strumienia na drugie wyjście RTMP z tego samego enkodera NVENC.
- **Czat TikTok LIVE + Overlay**: panel czatu z dowolnym `@nickiem`, powiadomieniami o prezentach, licznikiem tapów i źródłem **Overlay czatu LIVE** do nałożenia na obraz (z wbudowanym `node.exe` — bez konieczności instalowania Node.js).
- **Instalator MSI (WiX 5)**: czysty start przy pierwszej instalacji, zachowanie ustawień przy aktualizacji/reinstalacji oraz pełne czyszczenie danych przy odinstalowaniu.

## Architektura

| Moduł | Technologia | Katalog |
|---|---|---|
| Interfejs (UI) | Qt 6 Widgets (C++, QSS) | `ui/` |
| Silnik wideo/audio | C# / .NET 10 ładowany przez `hostfxr` | `TikStream.Engine/` |
| Potok wideo | DXGI / WGC → D3D11 (HLSL) → NV12 → `h264_nvenc` | `TikStream.Engine/Gpu/` |
| Audio | WASAPI (NAudio) + RNNoise / SpeexDSP → AAC-LC | `TikStream.Engine/Capture/` |
| Czat LIVE | `tiktok-live-connector` (uruchamiany w tle) | `chat/` |
| Instalator | WiX Toolset 5 (`TikStreamSetup.msi`) | `installer/` |

Dane użytkownika są przechowywane lokalnie w `%APPDATA%\TikStream\settings.json` (klucze strumienia szyfrowane przez Windows DPAPI) oraz `%APPDATA%\TikStream\log.txt`.

## Wymagania i budowanie ze źródeł

Wymagania:
- Windows 10 (19041+) lub Windows 11 (x64)
- Karta graficzna NVIDIA ze wsparciem NVENC
- [.NET 10 SDK](https://dotnet.microsoft.com/)
- [MSYS2 UCRT64](https://www.msys2.org/) z pakietami: `mingw-w64-ucrt-x86_64-qt6-base`, `qt6-svg`, `qt6-imageformats`, `qt6-tools`, `gcc`, `cmake`, `ninja`
- Node.js 20+ (do zbudowania modułu `chat/` i spakowania `node.exe`)
- Biblioteki współdzielone **FFmpeg 8.x (x64)** umieszczone w `TikStream.Engine/native/` (`avcodec-62.dll`, `avdevice-62.dll`, `avfilter-11.dll`, `avformat-62.dll`, `avutil-60.dll`, `swresample-6.dll`, `swscale-9.dll`) — gotowy instalator `TikStreamSetup.msi` zawiera je już w paczce
- WiX Toolset 5 (`dotnet tool install -g wix --version 5.0.2`) do zbudowania instalatora MSI

### Kompilacja

```powershell
# 1. Silnik C#
cd TikStream.Engine
dotnet publish -c Release -o ../dist/engine
cd ..

# 2. Interfejs Qt (MSYS2 UCRT64 w PATH)
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S ui -B ui/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ui/build
Copy-Item ui\build\TikStream.exe dist\ -Force

# 3. Moduł czatu LIVE
New-Item -ItemType Directory dist\chat -Force | Out-Null
Copy-Item chat\live.mjs,chat\events.mjs,chat\package.json,chat\package-lock.json dist\chat\ -Force
npm --prefix dist\chat ci --omit=dev --ignore-scripts
Copy-Item (Get-Command node).Source dist\chat\node.exe -Force

# 4. Instalator MSI (opcjonalnie)
powershell -ExecutionPolicy Bypass -File installer\build-msi.ps1
```

## Test automatyczny

```powershell
dist\TikStream.exe --test .\test_out
```

Wykonuje automatyczny test uruchomienia, 5-sekundowego nagrania MKV oraz zapisuje raport w `test_out\test.txt`.

## Licencja

Projekt udostępniany na licencji **GNU General Public License v3.0 (GPL-3.0)** — szczegóły w pliku [LICENSE](LICENSE).
