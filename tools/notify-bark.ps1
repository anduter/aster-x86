# notify-bark.ps1 - send a Bark push notification (iOS)
#
# Reads the Bark server URL (contains the device key) from:
#   1) -Url parameter
#   2) keys/bark.url  (gitignored)
#   3) $env:BARK_URL
#
# Uses POST JSON (https://api.day.app/<key>) so title/body are unambiguous.
#
# usage:
#   powershell -File tools\notify-bark.ps1 -Title "aster" -Body "8h soak PASS"
#   powershell -File tools\notify-bark.ps1 -Title "aster" -Body "audit: 3 bugs" `
#       -Group audit -Level timeSensitive -Ttl 86400 -Sound alarm -Badge 1
param(
    [string]$Title = 'aster',
    [string]$Body  = '',
    [string]$Group = 'aster',
    [ValidateSet('active', 'timeSensitive', 'passive')]
    [string]$Level = 'active',
    [int]$Ttl = 0,              # 0 = let Bark use its default
    [string]$Sound = '',
    [string]$Url = '',          # tap-through URL (optional)
    [int]$Badge = -1,           # -1 = unset
    [string]$ServerUrl = '',
    [int]$TimeoutSec = 15,
    [int]$Retries = 3
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $ServerUrl) {
    $f = Join-Path $repo 'keys\bark.url'
    if (Test-Path $f) { $ServerUrl = (Get-Content $f -Raw).Trim() }
    elseif ($env:BARK_URL) { $ServerUrl = $env:BARK_URL.Trim() }
}
if (-not $ServerUrl) {
    Write-Error 'notify-bark: no Bark URL (pass -ServerUrl, create keys/bark.url, or set BARK_URL)'
    exit 1
}

# 开关：keys/notify.flag 内容为 off/0/false/no 时静默跳过（-Force 可强制）
$flag = Join-Path $repo 'keys\notify.flag'
if ((-not $Force) -and (Test-Path $flag)) {
    $v = (Get-Content $flag -Raw).Trim()
    if ($v -match '^(off|0|false|no)$') {
        Write-Host 'notify-bark: disabled (keys/notify.flag)'
        exit 0
    }
}
$payload = [ordered]@{
    title = $Title
    body  = $Body
    group = $Group
    level = $Level
}
if ($Ttl -gt 0)   { $payload['ttl'] = $Ttl }
if ($Sound)       { $payload['sound'] = $Sound }
if ($Url)         { $payload['url'] = $Url }
if ($Badge -ge 0) { $payload['badge'] = $Badge }
$json = $payload | ConvertTo-Json -Compress

for ($i = 1; $i -le $Retries; $i++) {
    try {
        $r = Invoke-RestMethod -Uri $ServerUrl.TrimEnd('/') -Method Post `
            -ContentType 'application/json; charset=utf-8' -Body $json -TimeoutSec $TimeoutSec
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
