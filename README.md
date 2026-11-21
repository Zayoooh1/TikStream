# TikStream

A lightweight, high-performance Windows streaming and recording application built for **TikTok LIVE** (vertical `1080×1920 @ 60 FPS` and horizontal `1920×1080 @ 60 FPS`), engineered for NVIDIA RTX GPUs with a zero-copy VRAM → NVENC pipeline.

## Key Features

- **Zero-Copy D3D11 + NVENC Pipeline**: Screen capture (DXGI Desktop Duplication), window/game capture (Windows Graphics Capture 24H2+ / borderless DXGI), webcams (DirectShow/NV12), and media playback composited directly in GPU memory.
- **NVENC "Zero FPS Impact" Mode**: 1-Pass Low Latency encoding with Spatial/Temporal AQ and Look-ahead disabled — prevents CUDA core contention when games run at 98–100% GPU load.
- **HDR Support**: Automatic HDR display detection (`R16G16B16A16_FLOAT`) with real-time scRGB → SDR ACES filmic tone mapping in HLSL.
- **GPU Video Filters**: Brightness, contrast, saturation, CbCr Chroma Key with edge despill, and L/R/T/B edge cropping.
- **Advanced WASAPI Audio Engine**: Per-application audio capture (WASAPI Process Loopback), native **RNNoise** and **SpeexDSP** noise suppression, noise gate, compressor, limiter, and A/V sync offset (ms).
- **Multistreaming**: Stream simultaneously to a second RTMP destination sharing the same NVENC and AAC encoders.
- **TikTok LIVE Chat & Overlay**: Built-in LIVE chat dock for any `@username` with gift notifications, tap counter, and a **LIVE Chat Overlay** source (bundles portable `node.exe` in the installer — no separate Node.js installation required).
- **WiX 5 MSI Installer**: Clean default state on first install, preserves user settings on upgrades/reinstalls, and performs a complete user-data cleanup on uninstall.

## Architecture

| Component | Technology | Directory |
|---|---|---|
| User Interface | Qt 6 Widgets (C++, QSS) | `ui/` |
| Streaming Engine | C# / .NET 10 hosted in-process via `hostfxr` | `TikStream.Engine/` |
| Video Pipeline | DXGI / WGC → D3D11 (HLSL) → NV12 → `h264_nvenc` | `TikStream.Engine/Gpu/` |
| Audio Pipeline | WASAPI (NAudio) + RNNoise / SpeexDSP → AAC-LC | `TikStream.Engine/Capture/` |
| LIVE Chat | `tiktok-live-connector` background worker | `chat/` |
| Installer | WiX Toolset 5 (`TikStreamSetup.msi`) | `installer/` |

User configuration is stored locally in `%APPDATA%\TikStream\settings.json` (stream keys are encrypted via Windows DPAPI) and logs in `%APPDATA%\TikStream\log.txt`.

## Installation

Download the latest **`TikStreamSetup.msi`** from the [GitHub Releases](https://github.com/Zayoooh1/TikStream/releases) page and run the installer.

## Building from Source

### Prerequisites
- Windows 10 (19041+) or Windows 11 (x64)
- NVIDIA GPU with NVENC support
- [.NET 10 SDK](https://dotnet.microsoft.com/)
- [MSYS2 UCRT64](https://www.msys2.org/) with packages: `mingw-w64-ucrt-x86_64-qt6-base`, `qt6-svg`, `qt6-imageformats`, `qt6-tools`, `gcc`, `cmake`, `ninja`
- Node.js 20+ (to install `chat/` dependencies and bundle portable `node.exe`)
- **FFmpeg 8.x shared x64 DLLs** placed in `TikStream.Engine/native/` (`avcodec-62.dll`, `avdevice-62.dll`, `avfilter-11.dll`, `avformat-62.dll`, `avutil-60.dll`, `swresample-6.dll`, `swscale-9.dll`) — already bundled in the MSI release
- WiX Toolset 5 (`dotnet tool install -g wix --version 5.0.2`) for building the MSI installer

### Build Steps (PowerShell)

```powershell
# 1. C# Engine
cd TikStream.Engine
dotnet publish -c Release -o ../dist/engine
cd ..

# 2. Qt 6 UI (MSYS2 UCRT64 in PATH)
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S ui -B ui/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ui/build
Copy-Item ui\build\TikStream.exe dist\ -Force

# 3. LIVE Chat module + bundled portable node.exe
New-Item -ItemType Directory dist\chat -Force | Out-Null
Copy-Item chat\live.mjs,chat\events.mjs,chat\package.json,chat\package-lock.json dist\chat\ -Force
npm --prefix dist\chat ci --omit=dev --ignore-scripts
Copy-Item (Get-Command node).Source dist\chat\node.exe -Force

# 4. MSI Installer (optional)
powershell -ExecutionPolicy Bypass -File installer\build-msi.ps1
```

## Automated Self-Test

```powershell
dist\TikStream.exe --test .\test_out
```

Runs an automated startup check and a 5-second MKV recording, writing performance metrics to `test_out\test.txt`.

## License

Licensed under the **GNU General Public License v3.0 (GPL-3.0)** — see [LICENSE](LICENSE) for details.
