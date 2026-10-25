# error-path-check.ps1 - independent verification of the fs error-path selftest
#
# Why: build.ps1 is owned by another session, so the "failure must fail as a
# whole and must not corrupt existing state" assertions live in
# fs/vfs_selftest.c (runs on every -Test) and this script checks the serial
# markers independently.
#
# usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\error-path-check.ps1
#       runs an isolated regression (.obj-errap / .dist-errap / serial-errap.log,
#       QMP 4511, waits for .session/test.lock) and checks the markers.
#   powershell ... -ReuseLog
#       only analyses the existing serial-errap.log, does not start QEMU.
#
# exit code: 0 = all checks pass; 1 = build/regression failed or markers missing.
param(
    [string]$Repo = 'D:\OS_Project',
    [string]$Name = 'errap',
    [switch]$ReuseLog,
    [string]$SerialLog = '',
    [int]$WaitSec = 900
)

Set-Location $Repo

if (-not $SerialLog) { $SerialLog = "serial-$Name.log" }

$exit = 0
if (-not $ReuseLog) {
    Write-Host ("[error-path] running isolated regression (-Name {0}) ..." -f $Name) -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name $Name -Test
    $exit = $LASTEXITCODE
    Write-Host ("[error-path] session-run exit={0}" -f $exit)
} else {
    Write-Host ("[error-path] reuse log {0} (skip QEMU)" -f $SerialLog) -ForegroundColor Cyan
}

if (-not (Test-Path $SerialLog)) {
    Write-Host ("[error-path] FAIL: serial log not found: {0}" -f $SerialLog) -ForegroundColor Red
    exit 1
}

$raw = Get-Content $SerialLog -Raw

$checks = @(
    @{ Name = 'regression exit code is 0';            Ok = ($exit -eq 0) },
    @{ Name = 'no error-path FAILED marker';          Ok = -not ($raw -match 'error-path selftest FAILED') },
    @{ Name = 'error-path selftest passed';           Ok = ($raw -match 'fs: error-path selftest ok') },
    @{ Name = 'VFS backend selftest still passed';    Ok = ($raw -match 'fs: backend selftest ok') },
    @{ Name = 'no backend selftest FAILED marker';    Ok = -not ($raw -match 'fs: backend selftest FAILED') }
)

Write-Host '[error-path] checks:'
$failed = 0
foreach ($c in $checks) {
    if ($c.Ok) {
        Write-Host ("  PASS  " + $c.Name) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL  " + $c.Name) -ForegroundColor Red
        $failed++
    }
}

if ($raw -match 'fs: error-path selftest ok') {
    Write-Host '[error-path] marker:'
    Write-Host '    fs: error-path selftest ok'
}

# verdict line from the regression (serial log usually has it via build.ps1 echo)
$verdict = (Select-String -Path $SerialLog -Pattern '=== TEST (PASS|FAIL)[^=]*===' -ErrorAction SilentlyContinue |
            Select-Object -Last 1).Line
if (-not $verdict) {
    $buildLog = "logs-session-$Name.log"
    if (Test-Path $buildLog) {
        $verdict = (Select-String -Path $buildLog -Pattern '=== TEST (PASS|FAIL)[^=]*===' -ErrorAction SilentlyContinue |
                    Select-Object -Last 1).Line
    }
}
if ($verdict) { Write-Host ("[error-path] verdict: " + $verdict.Trim()) }

if ($failed -eq 0) {
    Write-Host '[error-path] PASS' -ForegroundColor Green
    exit 0
}

Write-Host ("[error-path] FAIL ({0} check(s))" -f $failed) -ForegroundColor Red
exit 1
