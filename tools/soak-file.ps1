# soak-file.ps1 - soak harness using a file-backed serial port.
#
# The TCP serial backend used by tools/soak.ps1 can stop draining (harness
# side), which looks like a guest hang. This variant writes the serial stream
# to a file (no backpressure) and, on a stall, dumps both CPU register sets via
# the QEMU monitor so the failure is diagnosable.
#
# usage: powershell -File tools\soak-file.ps1 -Hours 0.08
param(
    [string]$Repo = 'D:\OS_Project',
    [double]$Hours = 0.08,
    [int]$Smp = 2,
    [string]$Image = 'dist/floppy.img',
    [int]$MonitorPort = 4495,
    [string]$SerialLog = 'obj/soak-file/serial.log',
    [int]$StallSec = 120
)
$ErrorActionPreference = 'Stop'
Set-Location $Repo
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SerialLog) | Out-Null
Remove-Item $SerialLog -ErrorAction SilentlyContinue
$crash = Join-Path (Split-Path -Parent $SerialLog) 'CRASH.txt'
Remove-Item $crash -ErrorAction SilentlyContinue

function HMP([string]$cmd) {
    try {
        $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $MonitorPort)
        $s = $c.GetStream()
        $r = New-Object IO.StreamReader($s); $w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
        Start-Sleep -Milliseconds 200
        $w.WriteLine($cmd); Start-Sleep -Milliseconds 500
        $o = ''
        while ($s.DataAvailable) { $b = New-Object byte[] 65536; $n = $s.Read($b, 0, $b.Length); $o += [Text.Encoding]::ASCII.GetString($b, 0, $n) }
        $c.Close()
        return $o
    } catch { return '' }
}

$q = Start-Process -FilePath 'qemu-system-i386' -ArgumentList @(
    '-fda', $Image, '-boot', 'a', '-display', 'none',
    '-smp', "$Smp", '-m', '128',
    '-nic', 'user,model=e1000',
    '-serial', "file:$SerialLog",
    '-monitor', ("tcp:127.0.0.1:{0},server=on,wait=off" -f $MonitorPort),
    '-no-reboot'
) -PassThru -WindowStyle Hidden

$deadline = (Get-Date).AddSeconds($Hours * 3600)
$lastLen = 0
$stall = 0
$lastHb = 0
while ((Get-Date) -lt $deadline) {
    if ($q.HasExited) { "qemu exited early"; break }
    Start-Sleep -Seconds 10
    $len = if (Test-Path $SerialLog) { (Get-Item $SerialLog).Length } else { 0 }
    if ($len -gt $lastLen) {
        $lastLen = $len; $stall = 0
        $t = Get-Content $SerialLog -Raw -ErrorAction SilentlyContinue
        foreach ($m in [regex]::Matches($t, 'thread: heartbeat (\d+)')) { $lastHb = [int]$m.Groups[1].Value }
    } else {
        $stall++
        if ($stall -ge ($StallSec / 10)) {
            "STALL: no serial growth for $StallSec s, last hb=$lastHb"
            Remove-Item $crash -ErrorAction SilentlyContinue
            Add-Content $crash ("stall at {0}, last heartbeat {1}" -f (Get-Date -Format s), $lastHb)
            $h0 = HMP 'info registers'; $c0 = (($h0 -split "`n") | Where-Object { $_ -match 'EIP=|HLT=' }) -join ' | '
            HMP 'cpu 1' | Out-Null
            $h1 = HMP 'info registers'; $c1 = (($h1 -split "`n") | Where-Object { $_ -match 'EIP=|HLT=' }) -join ' | '
            Add-Content $crash "cpu0: $c0"
            Add-Content $crash "cpu1: $c1"
            Get-Content $crash
            if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force }
            exit 2
        }
    }
}
if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force }
"soak-file done: elapsed=$Hours h, last heartbeat=$lastHb"
exit 0
