# =============================================================================
# tools/stress.ps1 - long-running stress run (file-backed serial, robust)
#
# Why not tools/soak.ps1: its serial is a TCP socket, and in this QEMU build the
# guest output can stall mid-run (observed: output stops ~15s after boot while
# the guest keeps running in a file-backed run). A stalled socket looks exactly
# like a guest hang, so a multi-hour run on it is not trustworthy.
#
# Here the serial goes to a file and we tail it by byte offset:
#   - crash patterns  -> CRASH.txt + exit 2
#   - no growth 300s  -> hang report + exit 2
#   - QEMU exits      -> CRASH.txt + exit 2
# Commands are injected through the HMP monitor with sendkey every N seconds.
#
# Usage:
#   .\tools\stress.ps1 -Hours 8
#   .\tools\stress.ps1 -Hours 0.05 -InjectEverySec 15      # 3 min smoke
#   stop early: New-Item stress-run\STOP
# =============================================================================
param(
    [double]$Hours = 8,
    [int]$InjectEverySec = 60,
    [string]$OutDir = 'stress-run',
    [string]$Image = 'dist/floppy.img',
    [int]$Smp = 2,
    [int]$MonitorPort = 4480,
    [string]$Commands = 'ls,echo stress,pwd,fork,cat system:/docs/hello.txt'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$serialPath = Join-Path $OutDir 'serial.log'
$progress   = Join-Path $OutDir 'progress.log'
$stopFile   = Join-Path $OutDir 'STOP'
$crashTxt   = Join-Path $OutDir 'CRASH.txt'
Remove-Item $serialPath, $progress, $stopFile, $crashTxt -ErrorAction SilentlyContinue

$start = Get-Date
$deadline = $start.AddHours($Hours)
"stress: start {0} hours={1} image={2} smp={3}" -f $start.ToString('s'), $Hours, $Image, $Smp |
    Add-Content $progress

if (-not (Test-Path $Image)) { throw "missing image $Image" }

$qemu = Start-Process -FilePath 'qemu-system-i386' -ArgumentList @(
    '-fda', (Resolve-Path $Image).Path, '-boot', 'a', '-display', 'none',
    '-smp', "$Smp", '-m', '256', '-cpu', 'max', '-no-reboot',
    '-serial', ("file:{0}" -f $serialPath),
    '-monitor', ("tcp:127.0.0.1:{0},server=on,wait=off" -f $MonitorPort)
) -PassThru -WindowStyle Hidden

# ---- monitor (kept open for the whole run) ----
$monitor = $null
$monitorWriter = $null
for ($i = 0; $i -lt 60; $i++) {
    try {
        $monitor = New-Object Net.Sockets.TcpClient('127.0.0.1', $MonitorPort)
        $monitorWriter = New-Object IO.StreamWriter($monitor.GetStream())
        $monitorWriter.AutoFlush = $true
        break
    } catch { Start-Sleep -Milliseconds 200 }
}

function Send-Keys([string]$text) {
    if ($monitorWriter -eq $null) { throw 'monitor not connected' }
    foreach ($ch in $text.ToCharArray()) {
        $k = switch -CaseSensitive ($ch) {
            ' ' { 'spc' }
            '/' { 'slash' }
            ':' { 'shift-semicolon' }
            '.' { 'dot' }
            '-' { 'minus' }
            default { [string]$ch }
        }
        $monitorWriter.WriteLine("sendkey $k")
        Start-Sleep -Milliseconds 60
    }
    $monitorWriter.WriteLine('sendkey ret')
}

# ---- tail the serial file by byte offset ----
$fs = $null
for ($i = 0; $i -lt 50; $i++) {
    try {
        $fs = [IO.File]::Open($serialPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        break
    } catch { Start-Sleep -Milliseconds 200 }
}

$offset = 0
$booted = $false
$injected = 0
$heartbeats = 0
$bytesTotal = 0
$lastGrowth = Get-Date
$nextInject = (Get-Date).AddSeconds($InjectEverySec)
$nextReport = (Get-Date).AddSeconds(300)
$crashPattern = 'KERNEL PANIC|CPU exception|user: exception|Triple fault'
$commands = $Commands -split ','
$tailText = ''

function Save-Crash([string]$reason) {
    $t = @()
    $t += ("time:   " + (Get-Date).ToString('s'))
    $t += ("reason: " + $reason)
    $t += ("ran:    " + [Math]::Round(((Get-Date) - $start).TotalMinutes, 1) + " min")
    $t += ("inject: " + $injected + "  heartbeats: " + $heartbeats + "  bytes: " + $bytesTotal)
    $t += ''
    $t += '---- serial tail ----'
    $t += $tailText
    $t | Set-Content -Path $crashTxt -Encoding UTF8
}

while ((Get-Date) -lt $deadline) {
    if (Test-Path $stopFile) { "stress: STOP file" | Add-Content $progress; break }
    if ($qemu.HasExited) { Save-Crash "qemu exited (code $($qemu.ExitCode))"; exit 2 }

    # read whatever the guest appended
    if ($fs -ne $null) {
        $len = $fs.Length
        if ($len -gt $offset) {
            $fs.Seek($offset, [IO.SeekOrigin]::Begin) | Out-Null
            $n = $len - $offset
            if ($n -gt 65536) { $n = 65536 }
            $buf = New-Object byte[] $n
            $read = $fs.Read($buf, 0, $n)
            if ($read -gt 0) {
                $chunk = [Text.Encoding]::ASCII.GetString($buf, 0, $read)
                $offset += $read
                $bytesTotal += $read
                $lastGrowth = Get-Date
                $tailText = ($tailText + $chunk)
                if ($tailText.Length -gt 8000) { $tailText = $tailText.Substring($tailText.Length - 4000) }
                $heartbeats += ([regex]::Matches($chunk, 'thread: heartbeat')).Count
                if (-not $booted -and $chunk.Contains('ushell: ready')) {
                    $booted = $true
                    "stress: booted {0}" -f (Get-Date).ToString('s') | Add-Content $progress
                }
                if ($chunk -match $crashPattern) {
                    Save-Crash 'crash pattern in serial output'
                    if (-not $qemu.HasExited) { Stop-Process -Id $qemu.Id -Force }
                    exit 2
                }
            }
        }
    }

    # hang detection: a live guest prints a heartbeat every second
    if ($booted -and ((Get-Date) - $lastGrowth).TotalSeconds -gt 300) {
        Save-Crash 'hang: no serial growth for 300s'
        if (-not $qemu.HasExited) { Stop-Process -Id $qemu.Id -Force }
        exit 2
    }

    if ($booted -and (Get-Date) -ge $nextInject) {
        $cmd = $commands[$injected % $commands.Count]
        try { Send-Keys $cmd } catch { "stress: inject failed: $_" | Add-Content $progress }
        $injected++
        $nextInject = (Get-Date).AddSeconds($InjectEverySec)
    }

    if ((Get-Date) -ge $nextReport) {
        "stress: alive {0} ran={1}min injected={2} hb={3} bytes={4}" -f `
            (Get-Date).ToString('s'), [Math]::Round(((Get-Date) - $start).TotalMinutes, 1), `
            $injected, $heartbeats, $bytesTotal | Add-Content $progress
        $nextReport = (Get-Date).AddSeconds(300)
    }

    Start-Sleep -Milliseconds 500
}

if ($fs -ne $null) { $fs.Close() }
if ($monitor -ne $null) { try { $monitorWriter.WriteLine('quit'); Start-Sleep -Milliseconds 500; $monitor.Close() } catch {} }
if (-not $qemu.HasExited) {
    for ($i = 0; $i -lt 20; $i++) { if ($qemu.HasExited) { break }; Start-Sleep -Milliseconds 250 }
}
if (-not $qemu.HasExited) { Stop-Process -Id $qemu.Id -Force }
if (Test-Path $crashTxt) { Get-Content $crashTxt | Select-Object -First 4 | Add-Content $progress }

"stress: done {0} ran={1}min injected={2} hb={3} bytes={4}" -f `
    (Get-Date).ToString('s'), [Math]::Round(((Get-Date) - $start).TotalMinutes, 1), `
    $injected, $heartbeats, $bytesTotal | Add-Content $progress
exit 0
