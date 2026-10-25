# session-run.ps1 - run build.ps1 with per-session isolation and a test lock
#
# Isolation: -ObjDir/-DistDir/-QmpPort/-SerialLog are derived from -Name, so two
# sessions do not overwrite each other's artifacts.
# Lock: only -Test needs the host echo port 12345, so -Test acquires
# .session/test.lock and waits for it instead of failing.
#
# usage:
#   powershell -File tools/session-run.ps1 -Name a -Test
#   powershell -File tools/session-run.ps1 -Name b            # build only
param(
    [string]$Repo = 'D:\OS_Project',
    [string]$Name = 'session',
    [string]$ObjDir = '',
    [string]$DistDir = '',
    [int]$QmpPort = 0,
    [string]$SerialLog = '',
    [switch]$Test,
    [int]$WaitSec = 600
)

Set-Location $Repo
if (-not $ObjDir)  { $ObjDir = ".obj-$Name" }
if (-not $DistDir) { $DistDir = ".dist-$Name" }
if ($QmpPort -eq 0) {
    $map = @{ 'a' = 4481; 'b' = 4491; 'c' = 4501 }
    $QmpPort = if ($map.ContainsKey($Name)) { $map[$Name] } else { 4511 }
}
if (-not $SerialLog) { $SerialLog = "serial-$Name.log" }

$sessionDir = Join-Path $Repo '.session'
New-Item -ItemType Directory -Force -Path $sessionDir | Out-Null
$lockPath = Join-Path $sessionDir 'test.lock'

function Write-History([string]$line) {
    Add-Content -Path (Join-Path $sessionDir 'history.log') -Value $line -Encoding UTF8
}

$lockStream = $null
if ($Test) {
    $deadline = (Get-Date).AddSeconds($WaitSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $lockStream = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
            break
        } catch {
            # stale lock: holder process no longer running -> remove and retry
            $holderGone = $true
            if (Test-Path $lockPath) {
                $txt = Get-Content $lockPath -Raw -ErrorAction SilentlyContinue
                if ($txt -match '"pid":\s*(\d+)') {
                    $holderPid = [int]$matches[1]
                    if (Get-Process -Id $holderPid -ErrorAction SilentlyContinue) { $holderGone = $false }
                }
            }
            if ($holderGone) { Remove-Item $lockPath -Force -ErrorAction SilentlyContinue; continue }
            Write-Host ("[session-run] waiting for test lock (held by {0})" -f $txt.Trim()) -ForegroundColor Yellow
            Start-Sleep -Seconds 2
        }
    }
    if (-not $lockStream) { throw "test lock not acquired within $WaitSec s" }
    $info = @{ name = $Name; pid = $PID; start = (Get-Date -Format o) } | ConvertTo-Json -Compress
    $lockStream.SetLength(0)
    $w = New-Object IO.StreamWriter($lockStream)
    $w.Write($info); $w.Flush()
    Write-Host ("[session-run] test lock acquired by {0} (pid {1})" -f $Name, $PID) -ForegroundColor Green
}

try {
    $buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'build.ps1',
              '-ObjDir', $ObjDir, '-DistDir', $DistDir, '-QmpPort', "$QmpPort", '-SerialLog', $SerialLog)
    if ($Test) { $buildArgs += '-Test' }

    Write-Host ("[session-run] {0}: obj={1} dist={2} qmp={3} serial={4} test={5}" -f `
                $Name, $ObjDir, $DistDir, $QmpPort, $SerialLog, $Test.IsPresent) -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $log = Join-Path $Repo ("logs-session-$Name.log")
    & powershell @buildArgs *> $log
    $code = $LASTEXITCODE
    $sw.Stop()

    $verdict = '(build only)'
    if ($Test) {
        $line = (Select-String -Path $log -Pattern '=== TEST (PASS|FAIL)[^=]*===' -ErrorAction SilentlyContinue | Select-Object -Last 1).Line
        if ($line) { $verdict = $line.Trim() } else { $verdict = '(no verdict captured)' }
    }
    Write-History ("{0} name={1} test={2} exit={3} elapsed={4:N1}s {5}" -f `
                   (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Name, $Test.IsPresent, $code, $sw.Elapsed.TotalSeconds, $verdict)
    Write-Host ("[session-run] exit={0} elapsed={1:N1}s {2}" -f $code, $sw.Elapsed.TotalSeconds, $verdict) -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Red' })
    exit $code
}
finally {
    if ($lockStream) {
        $lockStream.Dispose()
        Remove-Item $lockPath -Force -ErrorAction SilentlyContinue
    }
}
