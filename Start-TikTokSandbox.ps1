$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'dist/TikStream.exe'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw 'Build TikStream into the dist folder first.'
}

$key = Read-Host 'TikTok Sandbox Client Key'
$secret = Read-Host 'TikTok Sandbox Client Secret' -AsSecureString
if ([string]::IsNullOrWhiteSpace($key) -or $secret.Length -eq 0) {
    throw 'Please enter both fields from the TikTok Developer Sandbox tab.'
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new($exe)
$startInfo.UseShellExecute = $false
$startInfo.WorkingDirectory = Split-Path -Parent $exe
$startInfo.Environment['TIKSTREAM_TIKTOK_SANDBOX_KEY'] = $key.Trim()
$startInfo.Environment['TIKSTREAM_TIKTOK_SANDBOX_SECRET'] = [System.Net.NetworkCredential]::new('', $secret).Password
try {
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.Dispose()
} finally {
    [void]$startInfo.Environment.Remove('TIKSTREAM_TIKTOK_SANDBOX_SECRET')
    $secret.Dispose()
}
