# record-screen.ps1 - 用已安装的 ffmpeg(gdigrab) 录制全屏，供反馈排查用
param(
    [int]$Seconds = 60,
    [string]$Out = "",
    [int]$Fps = 30
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'obj-ng\feedback'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path $dir ("feedback-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".mp4")
}
$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) { $ffmpeg = 'D:\Program Files\ffmpeg\bin\ffmpeg.exe' }
if (-not (Test-Path $ffmpeg)) { Write-Host 'ffmpeg not found'; exit 1 }
Write-Host ("recording {0}s -> {1}" -f $Seconds, $Out)
& $ffmpeg -y -f gdigrab -framerate $Fps -i desktop -t $Seconds -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p $Out 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host 'ffmpeg failed'; exit 1 }
Write-Host ('saved: ' + $Out)
Write-Host ('size: ' + (Get-Item $Out).Length + ' bytes')