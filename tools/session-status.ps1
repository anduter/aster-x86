# session-status.ps1 - one-screen view of parallel development sessions
#
# Shows: running build.ps1 processes, QEMU instances, busy echo/QMP ports,
# per-session obj dirs with their kernel.bin, newest serial logs and their last
# test verdict, and the git HEAD / dirty count.
#
# usage: powershell -File tools/session-status.ps1 [-Repo D:\OS_Project]
param([string]$Repo = 'D:\OS_Project')

Set-Location $Repo

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

Section 'git'
$head = (cmd /c "git rev-parse --short HEAD 2>nul")
$dirty = (cmd /c "git status --porcelain 2>nul" | Measure-Object -Line).Lines
Write-Host ("HEAD {0}   dirty files: {1}" -f $head, $dirty)

Section 'build.ps1 processes'
$procs = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
         Where-Object { $_.CommandLine -match 'build\.ps1' }
if (-not $procs) { Write-Host '(none)' }
else {
    $procs | ForEach-Object {
        $cmd = $_.CommandLine
        $obj = if ($cmd -match '-ObjDir\s+(\S+)') { $matches[1] } else { 'obj' }
        $qmp = if ($cmd -match '-QmpPort\s+(\d+)') { $matches[1] } else { '4444' }
        $test = if ($cmd -match '-Test') { 'TEST' } else { 'build' }
        Write-Host ("pid {0,-6} {1,-5} obj={2,-10} qmp={3}  since {4}" -f $_.ProcessId, $test, $obj, $qmp, $_.CreationDate)
    }
}

Section 'QEMU'
$q = Get-Process qemu-system-i386 -ErrorAction SilentlyContinue
if (-not $q) { Write-Host '(none)' } else { $q | ForEach-Object { Write-Host ("pid {0} started {1}" -f $_.Id, $_.StartTime) } }

Section 'ports (echo 12345 / QMP 4444,4481,4491)'
foreach ($p in 4444, 4481, 4491, 12345) {
    $busy = Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue
    $state = if ($busy) { 'LISTEN' } else { 'free' }
    Write-Host ("  {0,-6} {1}" -f $p, $state)
}

Section 'per-session artifacts'
Get-ChildItem -Directory -Filter '.obj-*' -ErrorAction SilentlyContinue | ForEach-Object {
    $kb = Join-Path $_.FullName 'kernel.bin'
    if (Test-Path $kb) {
        $i = Get-Item $kb
        Write-Host ("  {0,-12} kernel.bin {1,8} bytes  {2:HH:mm:ss}" -f $_.Name, $i.Length, $i.LastWriteTime)
    } else { Write-Host ("  {0,-12} (no kernel.bin)" -f $_.Name) }
}
if (Test-Path 'obj/kernel.bin') {
    $i = Get-Item 'obj/kernel.bin'
    Write-Host ("  {0,-12} kernel.bin {1,8} bytes  {2:HH:mm:ss}" -f 'obj', $i.Length, $i.LastWriteTime)
}

Section 'recent test verdicts (logs-session-*.log / test*.log)'
Get-ChildItem -File -Include 'logs-session-*.log','logs-*.log','test*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 6 | ForEach-Object {
        $v = (Select-String -Path $_.FullName -Pattern '=== TEST (PASS|FAIL)[^=]*===' -ErrorAction SilentlyContinue |
              Select-Object -Last 1).Line
        if ($v) { Write-Host ("  {0,-26} {1:HH:mm:ss}  {2}" -f $_.Name, $_.LastWriteTime, $v.Trim()) }
    }
Section 'recent serial logs'
Get-ChildItem -File -Filter 'serial*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 5 | ForEach-Object {
        Write-Host ("  {0,-20} {1:HH:mm:ss}  {2} bytes" -f $_.Name, $_.LastWriteTime, $_.Length)
    }

Section 'lock'
$lock = '.session/test.lock'
if (Test-Path $lock) {
    Write-Host ((Get-Content $lock -Raw).Trim())
} else { Write-Host '(no test lock held)' }
