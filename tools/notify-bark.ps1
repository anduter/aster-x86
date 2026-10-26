# notify-bark.ps1 - send a Bark push notification (iOS)
#
# Reads the Bark URL (contains the device key) from:
#   1) -Url parameter
#   2) keys/bark.url  (gitignored)
#   3) $env:BARK_URL
#
# usage:
#   powershell -File tools\notify-bark.ps1 -Title "aster" -Body "8h 浸泡 PASS"
#   powershell -File tools\notify-bark.ps1 -Title "aster" -Body "audit: 3 bugs" -Group audit -Level timeSensitive
param(
    [string]$Title = 'aster',
    [string]$Body  = '',
    [string]$Group = 'aster',
    [ValidateSet('active', 'timeSensitive', 'passive')]
    [string]$Level = 'active',
    [string]$Url   = '',
    [int]$TimeoutSec = 15,
    [int]$Retries = 3
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Url) {
    $f = Join-Path $repo 'keys\bark.url'
    if (Test-Path $f) { $Url = (Get-Content $f -Raw).Trim() }
    elseif ($env:BARK_URL) { $Url = $env:BARK_URL.Trim() }
}
if (-not $Url) {
    Write-Error 'notify-bark: no Bark URL (pass -Url, create keys/bark.url, or set BARK_URL)'
    exit 1
}

$base = $Url.TrimEnd('/')
$t = [uri]::EscapeDataString($Title)
$b = [uri]::EscapeDataString($Body)
$q = 'group=' + [uri]::EscapeDataString($Group) + '&level=' + [uri]::EscapeDataString($Level)
$uri = "$base/$t/$b?$q"

for ($i = 1; $i -le $Retries; $i++) {
    try {
        $r = Invoke-RestMethod -Uri $uri -Method Get -TimeoutSec $TimeoutSec
        if ($r -and $r.code -eq 200) {
            Write-Host ("notify-bark: ok ({0})" -f $r.message)
            exit 0
        }
        Write-Warning ("notify-bark: unexpected response code={0}" -f $r.code)
    } catch {
        Write-Warning ("notify-bark: attempt {0}/{1} failed: {2}" -f $i, $Retries, $_.Exception.Message)
        Start-Sleep -Seconds 2
    }
}

Write-Error 'notify-bark: send failed after retries'
exit 1
