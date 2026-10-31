# record-feedback.ps1 - start screen recording (ffmpeg/gdigrab) + key timeline together
param(
    [int]$Seconds = 60,
    [int]$Fps = 30
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'obj-ng\feedback'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$mp4 = Join-Path $dir ("feedback-" + $stamp + ".mp4")
$csv = Join-Path $dir ("keys-" + $stamp + ".csv")
$keysScript = Join-Path $PSScriptRoot 'record-keys.ps1'
$recScript = Join-Path $PSScriptRoot 'record-screen.ps1'
Write-Host ("start " + (Get-Date -Format 'HH:mm:ss.fff'))
$keyJob = Start-Job -ScriptBlock {
    param($s, $o, $sec)
    & powershell -NoProfile -ExecutionPolicy Bypass -File $s -Seconds $sec -Out $o
} -ArgumentList $keysScript, $csv, $Seconds
Start-Sleep -Milliseconds 300
& powershell -NoProfile -ExecutionPolicy Bypass -File $recScript -Seconds $Seconds -Out $mp4 -Fps $Fps
Wait-Job $keyJob -Timeout ($Seconds + 15) | Out-Null
Receive-Job $keyJob | Out-Null
Remove-Job $keyJob -Force
Write-Host ("end " + (Get-Date -Format 'HH:mm:ss.fff'))
Get-ChildItem $mp4, $csv -ErrorAction SilentlyContinue | Select-Object Name, Length
