# =============================================================================
# audit-smp-run.ps1 - boot the audit-smp image headless with N CPUs and capture
# the serial log; optionally inject shell commands after `ushell: ready`.
#
#   powershell -NoProfile -File tools\audit-smp-run.ps1 -Smp 2
#   powershell -NoProfile -File tools\audit-smp-run.ps1 -Smp 4 -Seconds 60
#   powershell -NoProfile -File tools\audit-smp-run.ps1 -Smp 4 -Keys "sectest`nfork" -SoakSec 120
#
# Isolation: QMP 4544 (never 12345), serial -> obj/audit/smp[-4].log, and a
# separate dist copy for -smp 4 (.dist-audit-smp4) so the default 2-core image
# is never mutated.
# =============================================================================
param(
    [int]$Smp = 2,
    [int]$Seconds = 40,
    [string]$DistDir = '',
    [string]$SerialLog = '',
    [int]$QmpPort = 4544,
    [string]$Keys = '',
    [int]$SoakSec = 0,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $DistDir) {
    $DistDir = if ($Smp -gt 2) { '.dist-audit-smp4' } else { '.dist-audit-smp' }
}
if (-not $SerialLog) {
    $SerialLog = if ($Smp -gt 2) { 'obj/audit/smp4.log' } else { 'obj/audit/smp.log' }
}
$dist = if ([IO.Path]::IsPathRooted($DistDir)) { $DistDir } else { Join-Path $root $DistDir }
$serial = if ([IO.Path]::IsPathRooted($SerialLog)) { $SerialLog } else { Join-Path $root $SerialLog }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $serial) | Out-Null

# -smp 4 uses its own image copy (the 2-core regression image stays untouched).
if ($Smp -gt 2) {
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    foreach ($f in @('floppy.img', 'disk.img', 'fat.img')) {
        Copy-Item (Join-Path (Join-Path $root '.dist-audit-smp') $f) (Join-Path $dist $f) -Force
    }
}

foreach ($f in @('floppy.img', 'disk.img', 'fat.img')) {
    if (-not (Test-Path (Join-Path $dist $f))) {
        throw ("missing image {0}\{1}; run build.ps1 -ObjDir .obj-audit-smp -DistDir .dist-audit-smp first" -f $DistDir, $f)
    }
}

function Test-PortFree([int]$Port) {
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', $Port)
        $c.Close()
        return $false
    } catch { return $true }
}
# Kill leftover audit QEMUs only (never touch other sessions' QEMUs).
Get-CimInstance Win32_Process -Filter "Name='qemu-system-i386.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match [regex]::Escape($serial) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
if (-not (Test-PortFree $QmpPort)) { throw ("QMP port {0} is busy" -f $QmpPort) }


Remove-Item -LiteralPath $serial -ErrorAction SilentlyContinue

$qargs = @(
    '-drive', ("file={0},format=raw,if=floppy" -f ((Join-Path $dist 'floppy.img') -replace '\\', '/')),
    '-boot', 'a',
    '-drive', ("file={0},format=raw,if=ide,index=0" -f ((Join-Path $dist 'disk.img') -replace '\\', '/')),
    '-drive', ("file={0},format=raw,if=ide,index=1" -f ((Join-Path $dist 'fat.img') -replace '\\', '/')),
    '-m', '256', '-vga', 'std', '-cpu', 'qemu32,+nx,+smep,+smap',
    '-smp', "$Smp",
    '-nic', 'user,model=e1000',
    '-display', 'none', '-no-reboot',
    '-serial', ("file:{0}" -f ($serial -replace '\\', '/')),
    '-qmp', ("tcp:127.0.0.1:{0},server,nowait" -f $QmpPort)
)
Write-Host ("[audit-smp] boot smp={0} dist={1} serial={2}" -f $Smp, $DistDir, $SerialLog) -ForegroundColor Cyan
$q = Start-Process -FilePath 'qemu-system-i386' -ArgumentList $qargs -PassThru -WindowStyle Hidden

function Get-Snap {
    if (Test-Path $serial) { return [string](Get-Content $serial -Raw -ErrorAction SilentlyContinue) }
    return ''
}
function Wait-For([string]$needle, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $snapNow = [string](Get-Snap)
        if ($snapNow.Contains($needle)) { return $true }
        if ($q.HasExited) { return $false }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

# QMP key injection (same HMP sendkey path as build.ps1).
function Send-Keys([string]$Text) {
    $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $QmpPort)
    $stream = $client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    $null = $reader.ReadLine()                       # greeting
    $writer.WriteLine('{"execute":"qmp_capabilities"}')
    $null = $reader.ReadLine()
    $map = @{ ' ' = 'spc'; '-' = 'minus'; '.' = 'dot'; '/' = 'slash'; ':' = 'shift-semicolon';
              '_' = 'shift-minus'; ',' = 'comma'; ';' = 'semicolon'; '=' = 'equal' }
    foreach ($ch in $Text.ToCharArray()) {
        $name = [string]$ch
        if ($ch -eq "`n") { $name = 'ret' }
        elseif ($map.ContainsKey($name)) { $name = $map[$name] }
        elseif ([char]::IsUpper($ch)) { $name = 'shift-' + $ch.ToString().ToLower() }
        $cmd = '{"execute":"human-monitor-command","arguments":{"command-line":"sendkey ' + $name + '"}}'
        $writer.WriteLine($cmd)
        $null = $reader.ReadLine()
        Start-Sleep -Milliseconds 25
    }
    $writer.WriteLine('{"execute":"quit"}')
    try { $null = $reader.ReadLine() } catch { }
    $client.Close()
}

$ok = $false
$ready = Wait-For 'audit: smp selftest done' $Seconds
if (-not $ready) {
    # Also accept a boot that stopped at login (production build) when the
    # caller only wants the boot-time selftests.
    $ready = ([string](Get-Snap)).Contains('audit: smp selftest done')
}

if ($Keys -and (Wait-For 'ushell: ready' 30)) {
    Start-Sleep -Milliseconds 500
    Send-Keys ($Keys + "`n")
    if ($SoakSec -gt 0) {
        $deadline = (Get-Date).AddSeconds($SoakSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 5
            if ($q.HasExited) { break }
        }
    }
} elseif ($SoakSec -gt 0) {
    Start-Sleep -Seconds $SoakSec
}

$snap = [string](Get-Snap)
if (-not $KeepRunning -and -not $q.HasExited) {
    try {
        $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $QmpPort)
        $stream = $client.GetStream()
        $reader = New-Object System.IO.StreamReader($stream)
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true
        $null = $reader.ReadLine()
        $writer.WriteLine('{"execute":"qmp_capabilities"}')
        $null = $reader.ReadLine()
        $writer.WriteLine('{"execute":"quit"}')
        try { $null = $reader.ReadLine() } catch { }
        $client.Close()
    } catch { }
    try { if (-not $q.HasExited) { $q.WaitForExit(5000) | Out-Null } } catch { }
    if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host '--- audit lines ---' -ForegroundColor Cyan
$lines = @()
if ($snap) { $lines = $snap -split "`r?`n" | Where-Object { $_ -match 'audit:|panic|oops|FAIL|PASS' } }
$lines | ForEach-Object { Write-Host $_ }
if (($snap -match 'panic:|oops:') ) {
    Write-Host '[audit-smp] kernel panic/oops detected' -ForegroundColor Red
    exit 2
}
if ($snap -match 'audit: smp selftest PASS') {
    Write-Host '[audit-smp] selftest PASS' -ForegroundColor Green
    exit 0
}
Write-Host '[audit-smp] no PASS verdict' -ForegroundColor Yellow
exit 1