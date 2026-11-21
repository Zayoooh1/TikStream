$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'dist/TikStream.exe'
if (!(Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw 'Najpierw zbuduj TikStream w folderze dist.'
}

$key = Read-Host 'Client key z Sandbox'
$secret = Read-Host 'Client secret z Sandbox' -AsSecureString
if ([string]::IsNullOrWhiteSpace($key) -or $secret.Length -eq 0) {
    throw 'Podaj oba pola z zakladki Sandbox.'
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
