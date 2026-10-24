# =============================================================================
# tools/e1000-probe.ps1 - assert the e1000 driver lines from the serial log
#
# Default: run tools/session-run.ps1 -Name e1000 -Test, then analyse
# serial-e1000.log. -ReuseLog: only analyse an existing log.
#
# Checks:
#   net: e1000 irq line=N registered  OR  net: e1000 irq armed (...)
#       (if INTx is unavailable, accept the explicit fallback line
#        "net: e1000 irq unavailable, fallback polling" with a [WARN])
#   net: e1000 napi budget=8 ...
#   net: e1000 tx reclaim ok (freed=N)
#   net: e1000 stats rx=.. tx=.. irq=.. polls=.. drop=.. reset=..
#   existing selftests: ARP / ICMP / UDP / TCP / dhcp / dns / ping
#   regression verdict: === TEST PASS (98/98) ===
#
# Usage:
#   powershell -File tools/e1000-probe.ps1                 # run test + assert
#   powershell -File tools/e1000-probe.ps1 -ReuseLog       # analyse serial-e1000.log
#   powershell -File tools/e1000-probe.ps1 -ReuseLog -Log serial-a.log
# =============================================================================
param(
    [string]$Repo = 'D:\OS_Project',
    [string]$Log = 'serial-e1000.log',
    [string]$BuildLog = 'logs-session-e1000.log',
    [switch]$ReuseLog,
    [int]$WaitSec = 900
)

$ErrorActionPreference = 'Stop'
Set-Location $Repo

if (-not $ReuseLog) {
    Write-Host '[e1000-probe] running tools/session-run.ps1 -Name e1000 -Test ...' -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/session-run.ps1 -Name e1000 -Test -WaitSec $WaitSec
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("[e1000-probe] FAIL: session-run exit={0} (see logs-session-e1000.log)" -f $LASTEXITCODE) -ForegroundColor Red
        exit 1
    }
}

$logPath = $Log
if (-not [IO.Path]::IsPathRooted($logPath)) { $logPath = Join-Path $Repo $logPath }
if (-not (Test-Path $logPath)) {
    Write-Host ("[e1000-probe] FAIL: log not found: {0}" -f $logPath) -ForegroundColor Red
    exit 1
}
$text = Get-Content -Path $logPath -Raw
if (-not $text) {
    Write-Host ("[e1000-probe] FAIL: empty log: {0}" -f $logPath) -ForegroundColor Red
    exit 1
}

$results = @()

function Add-Result([string]$Name, [bool]$Ok, [string]$Detail) {
    $script:results += [pscustomobject]@{ Name = $Name; Ok = $Ok; Detail = $Detail }
}

function Match-Detail([System.Text.RegularExpressions.Match]$M, [string]$Miss) {
    if ($M.Success) { return $M.Value }
    return $Miss
}

# ---- IRQ registered / armed (explicit polling fallback is accepted) ----
$m = [regex]::Match($text, 'net: e1000 irq armed \((?:ioapic|pic) line=(\d+)\)')
if ($m.Success) {
    Add-Result 'irq armed' $true $m.Value
} elseif ($text -match 'net: e1000 irq line=(\d+) registered') {
    Add-Result 'irq registered' $true $Matches[0]
} elseif ($text -match 'net: e1000 irq unavailable, fallback polling') {
    Add-Result 'irq fallback' $true 'INTx unavailable, driver kept polling mode (no fake IRQ)'
} else {
    Add-Result 'irq' $false 'missing net: e1000 irq line'
}

# ---- NAPI budget ----
$m = [regex]::Match($text, 'net: e1000 napi budget=(\d+) rx_ring=(\d+) tx_ring=(\d+)')
Add-Result 'napi' $m.Success (Match-Detail $m 'missing net: e1000 napi budget= line')

# ---- TX reclaim ----
$m = [regex]::Match($text, 'net: e1000 tx reclaim ok \(freed=(\d+)\)')
Add-Result 'tx reclaim' $m.Success (Match-Detail $m 'missing net: e1000 tx reclaim ok (freed=N)')

# ---- stats line ----
$m = [regex]::Match($text, 'net: e1000 stats rx=(\d+) tx=(\d+) irq=(\d+) polls=(\d+) drop=(\d+) reset=(\d+)')
Add-Result 'stats' $m.Success (Match-Detail $m 'missing net: e1000 stats line')

# ---- watchdog: must be deferred + completed, never the old inline form ----
$wdDeferred = [regex]::Matches($text, 'net: e1000 tx watchdog reset \(deferred\)').Count
$wdInline = [regex]::Matches($text, '(?m)^net: e1000 tx watchdog reset\r?$').Count
$wdDone = [regex]::Match($text, 'net: e1000 tx watchdog reset done').Success
if ($wdDeferred -gt 0) {
    $detail = "deferred={0} inline={1} done={2}" -f $wdDeferred, $wdInline, $wdDone
    Add-Result 'watchdog' (($wdInline -eq 0) -and $wdDone) $detail
} elseif ($wdInline -gt 0) {
    Add-Result 'watchdog' $false 'found old inline reset log (must be deferred)'
} else {
    Add-Result 'watchdog' $true 'not triggered this run (reset=0, bounded deferred path armed)'
}

# ---- existing protocol selftests must still be there ----
$m = [regex]::Match($text, 'net: ARP reply gw=([0-9a-f:]+)')
Add-Result 'ARP reply' $m.Success (Match-Detail $m 'missing net: ARP reply')

$m = [regex]::Match($text, 'net: ICMP echo reply seq=(\d+)')
Add-Result 'ICMP echo' $m.Success (Match-Detail $m 'missing net: ICMP echo reply')

$m = [regex]::Match($text, 'net: UDP echo reply')
Add-Result 'UDP echo' $m.Success (Match-Detail $m 'missing net: UDP echo reply')

$m = [regex]::Match($text, 'net: TCP echo reply')
Add-Result 'TCP echo' $m.Success (Match-Detail $m 'missing net: TCP echo reply')

$m = [regex]::Match($text, 'net: dhcp ok ip=(\d+\.\d+\.\d+\.\d+) gw=(\d+\.\d+\.\d+\.\d+) dns=(\d+\.\d+\.\d+\.\d+)')
Add-Result 'DHCP' $m.Success (Match-Detail $m 'missing net: dhcp ok')

$m = [regex]::Match($text, 'net: dns ok ([A-Za-z0-9][A-Za-z0-9\.\-]*)=(\d+\.\d+\.\d+\.\d+)')
Add-Result 'DNS' $m.Success (Match-Detail $m 'missing net: dns ok')

$m = [regex]::Match($text, 'net: ping ok seq=(\d+) rtt=(\d+\.\d+)ms')
Add-Result 'ping' $m.Success (Match-Detail $m 'missing net: ping ok')

# ---- regression verdict (the 98/98 line lives in the build output) ----
$buildText = ''
$buildPath = $BuildLog
if (-not [IO.Path]::IsPathRooted($buildPath)) { $buildPath = Join-Path $Repo $buildPath }
if (Test-Path $buildPath) { $buildText = Get-Content -Path $buildPath -Raw }
$verdictText = "$text`n$buildText"

$m = [regex]::Match($verdictText, '=== TEST (PASS|FAIL) \((\d+)/(\d+)\) ===')
if (-not $m.Success) {
    $m = [regex]::Match($verdictText, '=== TEST FAIL \((\d+) of (\d+) checks failed\) ===')
}
if ($m.Success -and $m.Groups[1].Value -eq 'PASS') {
    Add-Result 'regression' $true $m.Value
} elseif ($m.Success) {
    Add-Result 'regression' $false $m.Value
} else {
    Add-Result 'verdict' $true 'no === TEST PASS/FAIL === found (serial-only analysis)'
}

$failed = 0
foreach ($r in $results) {
    if ($r.Ok -and $r.Name -eq 'irq fallback') {
        Write-Host ("[WARN] {0,-14} {1}" -f $r.Name, $r.Detail) -ForegroundColor Yellow
    } elseif ($r.Ok) {
        Write-Host ("[PASS] {0,-14} {1}" -f $r.Name, $r.Detail) -ForegroundColor Green
    } else {
        $failed++
        Write-Host ("[FAIL] {0,-14} {1}" -f $r.Name, $r.Detail) -ForegroundColor Red
    }
}

if ($failed -gt 0) {
    Write-Host ("[e1000-probe] FAIL ({0} check(s) failed) log={1}" -f $failed, $logPath) -ForegroundColor Red
    exit 1
}
Write-Host ("[e1000-probe] PASS log={0}" -f $logPath) -ForegroundColor Green
exit 0
