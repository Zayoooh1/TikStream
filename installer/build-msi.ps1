$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$stage = Join-Path $PSScriptRoot "stage"
$icoPath = Join-Path $PSScriptRoot "TikStream.ico"
$msiOut = Join-Path $PSScriptRoot "TikStreamSetup.msi"

# 1. Generate TikStream.ico (256x256 PNG inside ICO container) if missing
if (-not (Test-Path $icoPath)) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 256, 256
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $r = 48; $x = 12; $y = 12; $w = 232; $h = 232
    $path.AddArc($x, $y, $r, $r, 180, 90)
    $path.AddArc($x + $w - $r, $y, $r, $r, 270, 90)
    $path.AddArc($x + $w - $r, $y + $h - $r, $r, $r, 0, 90)
    $path.AddArc($x, $y + $h - $r, $r, $r, 90, 90)
    $path.CloseFigure()

    $bgBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 27, 28, 35))
    $g.FillPath($bgBrush, $path)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 254, 44, 85), 10)
    $g.DrawPath($pen, $path)

    $font = New-Object System.Drawing.Font ("Segoe UI", 120, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $accentBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 254, 44, 85))
    $whiteBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $g.DrawString("T", $font, $accentBrush, [System.Drawing.RectangleF]::new(6, 6, 256, 256), $sf)
    $g.DrawString("T", $font, $whiteBrush, [System.Drawing.RectangleF]::new(0, 0, 256, 256), $sf)
    $g.Dispose()

    $pngMs = New-Object System.IO.MemoryStream
    $bmp.Save($pngMs, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBytes = $pngMs.ToArray()
    $bmp.Dispose()

    $fs = [System.IO.File]::Create($icoPath)
    $bw = New-Object System.IO.BinaryWriter $fs
    $bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]1)
    $bw.Write([Byte]0); $bw.Write([Byte]0); $bw.Write([Byte]0); $bw.Write([Byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngBytes.Length)
    $bw.Write([UInt32]22)
    $bw.Write($pngBytes)
    $bw.Close(); $fs.Close()
    Write-Host "Generated $icoPath"
}

# 2. Prepare clean stage directory (production runtime files from dist only, excluding test_* folders)
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage -Force | Out-Null

# Top-level dist binaries (exe + dll)
Get-ChildItem $dist -File | Where-Object { $_.Extension -in '.exe','.dll' } | ForEach-Object {
    Copy-Item $_.FullName $stage -Force
}

# Required runtime directories
$runtimeDirs = @('engine', 'chat', 'platforms', 'styles', 'imageformats', 'iconengines', 'generic', 'networkinformation', 'tls')
foreach ($d in $runtimeDirs) {
    $src = Join-Path $dist $d
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $stage $d) -Recurse -Force
    }
}

# Bundle portable node.exe into chat/ so users don't need to install Node.js
$stageNode = Join-Path $stage "chat\node.exe"
if (-not (Test-Path $stageNode)) {
    $sysNode = (Get-Command node -ErrorAction SilentlyContinue).Source
    if ($sysNode -and (Test-Path $sysNode)) {
        Copy-Item $sysNode $stageNode -Force
        Write-Host "Bundled node.exe -> $stageNode"
    }
}

# Remove any debug symbols (pdb) from stage
Get-ChildItem $stage -Recurse -Include *.pdb -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

$fileCount = (Get-ChildItem $stage -Recurse -File).Count
$totalMb = [Math]::Round(((Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host "Staged $fileCount files ($totalMb MB) -> $stage"

# 3. Build TikStreamSetup.msi with WiX 5
$env:PATH = "$env:USERPROFILE\.dotnet\tools;$env:PATH"
Push-Location $PSScriptRoot
try {
    & wix build -arch x64 -culture pl-PL -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext TikStream.wxs TikStreamUI.wxs -o $msiOut
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

$msiInfo = Get-Item $msiOut
Write-Host ("MSI READY: {0} ({1:F1} MB)" -f $msiInfo.FullName, ($msiInfo.Length / 1MB))
