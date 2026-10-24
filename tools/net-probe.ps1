# =============================================================================
# tools/net-probe.ps1 - 从串口日志断言新增的网络自检
#
# 默认：先跑一次 tools/session-run.ps1 -Name net -Test（需要回显端口 12345），
#       再分析 serial-net.log。
# -ReuseLog：不跑 QEMU，只分析现有日志。
#
# 断言：
#   net: dhcp ok ip=A gw=B dns=C       （或 -AllowDegraded 时接受 keep static）
#   net: dns ok <name>=<ip>            （或 -AllowDegraded 时接受 dns timeout）
#   net: ping ok seq=N rtt=X.Yms       （或 -AllowDegraded 时接受 ping timeout）
#
# 用法：
#   powershell -File tools/net-probe.ps1                 # 跑测试 + 断言
#   powershell -File tools/net-probe.ps1 -ReuseLog       # 只分析 serial-net.log
#   powershell -File tools/net-probe.ps1 -ReuseLog -AllowDegraded -Log serial-a.log
# =============================================================================
param(
    [string]$Repo = 'D:\OS_Project',
    [string]$Log = 'serial-net.log',
    [switch]$ReuseLog,
    [switch]$AllowDegraded,
    [int]$WaitSec = 600
)

$ErrorActionPreference = 'Stop'
Set-Location $Repo

if (-not $ReuseLog) {
    Write-Host '[net-probe] running tools/session-run.ps1 -Name net -Test ...' -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/session-run.ps1 -Name net -Test -WaitSec $WaitSec
    if ($LASTEXITCODE -ne 0) {
        Write-Host ("[net-probe] FAIL: session-run exit={0} (see logs-session-net.log)" -f $LASTEXITCODE) -ForegroundColor Red
        exit 1
    }
}

$logPath = $Log
if (-not [IO.Path]::IsPathRooted($logPath)) { $logPath = Join-Path $Repo $logPath }
if (-not (Test-Path $logPath)) {
    Write-Host ("[net-probe] FAIL: log not found: {0}" -f $logPath) -ForegroundColor Red
    exit 1
}
$text = Get-Content -Path $logPath -Raw
if (-not $text) {
    Write-Host ("[net-probe] FAIL: empty log: {0}" -f $logPath) -ForegroundColor Red
    exit 1
}

$results = @()

function Add-Result([string]$Name, [bool]$Ok, [string]$Detail) {
    $script:results += [pscustomobject]@{ Name = $Name; Ok = $Ok; Detail = $Detail }
}

# ---- DHCP ----
$m = [regex]::Match($text, 'net: dhcp ok ip=(\d+\.\d+\.\d+\.\d+) gw=(\d+\.\d+\.\d+\.\d+) dns=(\d+\.\d+\.\d+\.\d+)')
if ($m.Success) {
    Add-Result 'dhcp ok' $true $m.Value
} elseif ($AllowDegraded -and $text -match 'net: dhcp timeout, keep static') {
    Add-Result 'dhcp degraded' $true 'net: dhcp timeout, keep static (静态 10.0.2.15 保留)'
} else {
    Add-Result 'dhcp ok' $false '未找到 net: dhcp ok 或降级行'
}

# ---- DNS ----
$m = [regex]::Match($text, 'net: dns ok ([A-Za-z0-9][A-Za-z0-9\.\-]*)=(\d+\.\d+\.\d+\.\d+)')
if ($m.Success) {
    Add-Result 'dns ok' $true $m.Value
} elseif ($AllowDegraded -and $text -match 'net: dns timeout') {
    Add-Result 'dns degraded' $true 'net: dns timeout (允许降级)'
} else {
    Add-Result 'dns ok' $false '未找到 net: dns ok 或降级行'
}

# ---- ping ----
$pings = [regex]::Matches($text, 'net: ping ok seq=(\d+) rtt=(\d+\.\d+)ms')
if ($pings.Count -gt 0) {
    Add-Result 'ping ok' $true ("{0} 条: {1}" -f $pings.Count, (($pings | ForEach-Object { $_.Value }) -join '; '))
} elseif ($AllowDegraded -and $text -match 'net: ping timeout') {
    Add-Result 'ping degraded' $true 'net: ping timeout (允许降级)'
} else {
    Add-Result 'ping ok' $false '未找到 net: ping ok 或降级行'
}

# ---- 回归总数（日志里有判定行才检查） ----
$verdict = [regex]::Match($text, '=== TEST (PASS|FAIL)[^=]*===')
if ($verdict.Success) {
    Add-Result 'regression' ($verdict.Groups[1].Value -eq 'PASS') $verdict.Value
}

$failed = 0
foreach ($r in $results) {
    if ($r.Ok) {
        Write-Host ("[PASS] {0,-12} {1}" -f $r.Name, $r.Detail) -ForegroundColor Green
    } else {
        $failed++
        Write-Host ("[FAIL] {0,-12} {1}" -f $r.Name, $r.Detail) -ForegroundColor Red
    }
}

if ($failed -gt 0) {
    Write-Host ("[net-probe] FAIL ({0} check(s) failed) log={1}" -f $failed, $logPath) -ForegroundColor Red
    exit 1
}
Write-Host ("[net-probe] PASS log={0}" -f $logPath) -ForegroundColor Green
exit 0
