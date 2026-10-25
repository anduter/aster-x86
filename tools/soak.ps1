# =============================================================================
# soak.ps1 - long-running stability run / crash watchdog
#
# QEMU runs headless; the serial line is a TCP socket that this script drains
# itself (a plain "file:" backend buffers 4KB and loses the tail).
#
# Every $InjectEverySec it types a shell command (ls / echo / pwd / fork /
# cat), every $ShotEverySec it takes a screenshot, and it continuously scans
# the serial stream for:
#   KERNEL PANIC | CPU exception | user: exception | sched: no runnable
# On a hit (or QEMU exit) it writes CRASH.txt + crash.ppm and exits 2.
#
# Usage:
#   .\tools\soak.ps1 -Hours 16
#   .\tools\soak.ps1 -Hours 0.03 -InjectEverySec 15    # smoke run
#   stop early:  New-Item obj\soak\STOP
# =============================================================================
param(
    [double]$Hours = 16,
    [int]$InjectEverySec = 300,
    [int]$ShotEverySec = 3600,
    [int]$MonitorPort = 4446,
    [int]$SerialPort = 4450,
    [int]$ReportEverySec = 300,
    [string]$OutDir = 'obj/soak',
    [string]$Image = 'dist/floppy.img',
    [string]$CommandList = 'ls,echo soak,pwd,fork,cat /docs/hello.txt',
    [switch]$Inject = $false,
    [int]$Smp = 1
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$serialFile = Join-Path $OutDir 'serial.log'
$progress   = Join-Path $OutDir 'progress.log'
$stopFile   = Join-Path $OutDir 'STOP'
$crashTxt   = Join-Path $OutDir 'CRASH.txt'

Remove-Item $serialFile, $progress, $stopFile, $crashTxt -ErrorAction SilentlyContinue

$startTime = Get-Date
$deadline = $startTime.AddHours($Hours)
Add-Content $progress ("soak: start {0} hours={1} image={2}" -f (Get-Date -Format s), $Hours, $Image)

$qemu = Start-Process -FilePath 'qemu-system-i386' -ArgumentList @(
    '-fda', (Resolve-Path $Image).Path, '-boot', 'a', '-display', 'none',
    '-smp', "$Smp",
    '-serial', ("tcp:127.0.0.1:{0},server=on,wait=on" -f $SerialPort),
    '-monitor', ("tcp:127.0.0.1:{0},server=on,wait=off" -f $MonitorPort)
) -PassThru -WindowStyle Hidden

# One persistent HMP connection: opening/closing a monitor socket before every
# sendkey makes QEMU drop serial output (observed: log stops right after the
# first injection), so keep it open for the whole run.
$script:monitorClient = $null
$script:monitorWriter = $null

function Connect-Monitor {
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $script:monitorClient = New-Object Net.Sockets.TcpClient('127.0.0.1', $MonitorPort)
            $script:monitorWriter = New-Object IO.StreamWriter($script:monitorClient.GetStream())
            return $true
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    return $false
}

function Send-Monitor {
    param([string[]]$Commands)

    if ($script:monitorWriter -eq $null) {
        if (-not (Connect-Monitor)) { return $false }
    }
    foreach ($cmd in $Commands) {
        $script:monitorWriter.WriteLine($cmd)
        $script:monitorWriter.Flush()
        Start-Sleep -Milliseconds 120
    }
    return $true
}

function To-Key {
    param([string]$Text)
    $keys = @()
    foreach ($ch in $Text.ToCharArray()) {
        switch -CaseSensitive ($ch) {
            ' '   { $keys += 'spc' }
            '/'   { $keys += 'slash' }
            ':'   { $keys += 'shift-semicolon' }
            '.'   { $keys += 'dot' }
            '-'   { $keys += 'minus' }
            default { $keys += [string]$ch }
        }
    }
    return $keys
}

function Stop-Qemu {
    param($Process)
    Send-Monitor -Commands @('quit') | Out-Null
    for ($i = 0; $i -lt 20; $i++) {
        if ($Process.HasExited) { return }
        Start-Sleep -Milliseconds 250
    }
    if (-not $Process.HasExited) { Stop-Process -Id $Process.Id -Force }
}

function Save-Crash {
    param([string]$Reason)

    Add-Content $progress ("soak: CRASH {0} - {1}" -f (Get-Date -Format s), $Reason)

    Send-Monitor -Commands @(("screendump {0}" -f (Join-Path $OutDir 'crash.ppm'))) | Out-Null

    $tail = $script:serialText
    if ($tail.Length -gt 16000) { $tail = $tail.Substring($tail.Length - 16000) }
    $text = @()
    $text += ("time:    " + (Get-Date -Format s))
    $text += ("reason:  " + $Reason)
    $text += ("ran:     " + [Math]::Round(((Get-Date) - $script:startTime).TotalMinutes, 1) + " min")
    $text += ''
    $text += '---- serial tail ----'
    $text += $tail
    $text | Set-Content -Path $crashTxt -Encoding UTF8
}

# ---- connect to the serial socket (QEMU waits for us) ----
Connect-Monitor | Out-Null

$serialClient = New-Object Net.Sockets.TcpClient
for ($i = 0; $i -lt 100; $i++) {
    try { $serialClient.Connect('127.0.0.1', $SerialPort); break }
    catch { Start-Sleep -Milliseconds 200 }
}
if (-not $serialClient.Connected) {
    Add-Content $progress 'soak: cannot connect serial socket'
    Stop-Qemu $qemu
    exit 2
}
$serialStream = $serialClient.GetStream()
$serialStream.ReadTimeout = 200
$buffer = New-Object byte[] 8192

$script:serialText = ''
$script:lastData = Get-Date
$crashPattern = 'KERNEL PANIC|CPU exception|user: exception|sched: no runnable|Triple fault'
$commands = $CommandList -split ','
$booted = $false
$injected = 0

$nextInject = (Get-Date).AddSeconds($InjectEverySec)
$nextShot   = (Get-Date).AddSeconds($ShotEverySec)
$nextReport = (Get-Date).AddSeconds($ReportEverySec)
$nextFlush  = (Get-Date).AddSeconds(10)

function Receive-Serial {
    try {
        $n = $serialStream.Read($buffer, 0, $buffer.Length)
    } catch {
        return 0
    }
    if ($n -le 0) { return 0 }

    $chunk = [Text.Encoding]::ASCII.GetString($buffer, 0, $n)
    $script:serialText += $chunk
    if ($script:serialText.Length -gt 200000) {
        $script:serialText = $script:serialText.Substring($script:serialText.Length - 100000)
    }
    return $n
}

while ((Get-Date) -lt $deadline) {
    if (Test-Path $stopFile) {
        Add-Content $progress ("soak: stop file at {0}" -f (Get-Date -Format s))
        break
    }
    if ($qemu.HasExited) {
        Save-Crash 'qemu exited'
        exit 2
    }

    $got = Receive-Serial
    if ($got -gt 0) {
        $script:lastData = Get-Date
    } else {
        Start-Sleep -Milliseconds 50
    }

    if (-not $booted -and $script:serialText.Contains('ushell: ready')) {
        $booted = $true
        $script:lastData = Get-Date
        Add-Content $progress ("soak: booted at {0}" -f (Get-Date -Format s))
    }

    # liveness: the kernel prints a heartbeat every second, so silence for
    # 3 minutes means the guest hung (even if it did not panic).
    if ($booted -and ((Get-Date) - $script:lastData).TotalSeconds -gt 180) {
        Save-Crash 'hang: no serial output for 180s'
        Stop-Qemu $qemu
        exit 2
    }

    if ($script:serialText -match $crashPattern) {
        Save-Crash 'crash pattern in serial output'
        Stop-Qemu $qemu
        exit 2
    }

    if ($booted -and $Inject -and (Get-Date) -ge $nextInject) {
        $cmd = $commands[$injected % $commands.Count]
        $injected++
        Send-Monitor -Commands ((To-Key $cmd) + @('ret')) | Out-Null
        $nextInject = (Get-Date).AddSeconds($InjectEverySec)
    }

    if ($booted -and (Get-Date) -ge $nextShot) {
        $shot = Join-Path $OutDir ("shot-{0}.ppm" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        Send-Monitor -Commands @(("screendump {0}" -f $shot)) | Out-Null
        $nextShot = (Get-Date).AddSeconds($ShotEverySec)
    }

    if ((Get-Date) -ge $nextFlush) {
        if ($script:serialText.Length -gt 0) {
            $script:serialText | Add-Content $serialFile -NoNewline
            $script:serialText = ''
        }
        $nextFlush = (Get-Date).AddSeconds(10)
    }

    if ((Get-Date) -ge $nextReport) {
        $last = ''
        $m = [regex]::Matches($script:serialText, 'soak: uptime=[^\r\n]*')
        if ($m.Count -gt 0) { $last = $m[$m.Count - 1].Value }
        Add-Content $progress ("soak: alive {0} injected={1} chars={2} {3}" -f (Get-Date -Format s), $injected, $script:serialText.Length, $last)
        $nextReport = (Get-Date).AddSeconds($ReportEverySec)
    }
}

# flush remaining output
if ($script:serialText.Length -gt 0) {
    $script:serialText | Add-Content $serialFile -NoNewline
    $script:serialText = ''
}
Add-Content $progress ("soak: done {0} injected={1}" -f (Get-Date -Format s), $injected)
Stop-Qemu $qemu
exit 0
