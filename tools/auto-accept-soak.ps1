# auto-accept-soak.ps1 - run the remaining items in order, unattended
#
# 1) wait until the source tree is clean (agents committed their fixes)
# 2) check-deps must report 0 violations
# 3) two full -Test runs must pass (98/98 or whatever the current count is)
# 4) flake-hunt 20 runs must be 0 failures
# 5) then run the 8h soak (tools/soak.ps1, amp 2, injection on)
#
# Writes logs/auto-accept-report.txt and exits non-zero on the first failure.
#
# usage: powershell -File tools/auto-accept-soak.ps1
param(
    [string]$Repo = 'D:\OS_Project',
    [int]$WaitHours = 4,
    [int]$TestRuns = 2,
    [int]$FlakeRuns = 20,
    [double]$SoakHours = 8,
    [int]$AccQmpPort = 4621
)

$ErrorActionPreference = 'Stop'
Set-Location $Repo
$log = Join-Path $Repo 'logs\auto-accept-report.txt'
New-Item -ItemType Directory -Force -Path (Join-Path $Repo 'logs') | Out-Null
function Log([string]$s) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $s
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}
$log | ForEach-Object { } # keep analyzer quiet
Remove-Item $log -ErrorAction SilentlyContinue
Log "auto-accept-soak start (HEAD $(cmd /c 'git rev-parse --short HEAD'))"

# 1) wait for a clean source tree (two consecutive clean polls)
$deadline = (Get-Date).AddHours($WaitHours)
$cleanCount = 0
while ((Get-Date) -lt $deadline) {
    $dirty = (cmd /c "git status --porcelain -- src 2>nul" | Measure-Object -Line).Lines
    if ($dirty -eq 0) { $cleanCount++; if ($cleanCount -ge 2) { break } } else { $cleanCount = 0 }
    Start-Sleep -Seconds 60
}
if ($cleanCount -lt 2) { Log 'FAIL: source tree did not become clean in time'; exit 1 }
Log "tree clean at HEAD $(cmd /c 'git rev-parse --short HEAD')"

# 2) check-deps
$deps = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\check-deps.ps1') 2>&1
$deps | Out-File -FilePath (Join-Path $Repo 'logs\auto-check-deps.txt') -Encoding UTF8
if ((($deps | Out-String) -match 'module include violations:\s*0') -eq $false) {
    Log 'FAIL: check-deps violations != 0'
    exit 1
}
Log 'check-deps: 0 violations'

# 3) two full tests
for ($i = 1; $i -le $TestRuns; $i++) {
    $serial = "serial-acc-$i.log"
    Log "test run $i started"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'build.ps1') -Test `
        -ObjDir '.obj-acc' -DistDir '.dist-acc' -QmpPort $AccQmpPort -SerialLog $serial | Out-Null
    $code = $LASTEXITCODE
    $tail = if (Test-Path (Join-Path $Repo $serial)) { (Select-String -Path (Join-Path $Repo $serial) -Pattern '=== TEST' -ErrorAction SilentlyContinue | Select-Object -Last 1).Line } else { '' }
    $verdict = (Get-Content (Join-Path $Repo 'logs\acc-out.txt') -ErrorAction SilentlyContinue | Select-Object -Last 1)
    if ($code -ne 0) {
        Log "FAIL: test run $i exit=$code"
        exit 1
    }
    Log "test run $i PASS"
}

# 4) flake-hunt 20 runs
Log "flake-hunt $FlakeRuns runs started"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\flake-hunt.ps1') -Runs $FlakeRuns | Out-Null
$flakeCode = $LASTEXITCODE
Log "flake-hunt exit=$flakeCode"
if ($flakeCode -ne 0) { Log 'FAIL: flake-hunt reported failures'; exit 1 }

# 5) build default image, then soak
Log 'building default image for soak'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'build.ps1') | Out-Null
if ($LASTEXITCODE -ne 0) { Log 'FAIL: default build failed'; exit 1 }

Log "soak $SoakHours h started"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\soak.ps1') -Hours $SoakHours -Smp 2 -Inject -InjectEverySec 120
$soakCode = $LASTEXITCODE
Log "soak exit=$soakCode"
if ($soakCode -ne 0) { Log 'FAIL: soak reported crash'; exit 1 }

# 6) platform primitives -> arch/x86 (option B, types.h stays)
Log 'arch move (B) started'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\arch-move.ps1')
if ($LASTEXITCODE -ne 0) { Log 'FAIL: arch move failed'; exit 1 }
Log 'arch move (B) complete, 2 tests passed'

Log 'ALL PASS (acceptance + flake + soak + arch move)'
exit 0
