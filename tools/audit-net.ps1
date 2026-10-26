# =============================================================================
# audit-net.ps1 - 网络协议栈审计会话（audit-net）的独立回归入口
#
# 隔离约定（与其它会话并行跑不打架）：
#   obj  = .obj-audit-net
#   dist = .dist-audit-net
#   QMP  = 4543
#   串口 = obj/audit/net.log
#   回显端口 = 12399（**不占用 12345**，那是 build.ps1 -Test 的默认端口）
#
# 它做两件事：
#   1) 用上面这套隔离参数跑一遍 build.ps1 -Test（225 项全量检查，含 net 组）；
#   2) 再从串口日志里断言 audit-net 新增的畸形包负向自检行（net: audit ...），
#      这些行是本次修复的回归证据（见 docs/audit/net.md）。
#
# 关于重试：本仓库的 `tcptest` 线程在启动负载下有**已知**的尾部延迟
# （见 commit b79ca12 / 1a6998e；audit-net 在未改动的基线 38161af 上实测
# 3 次里出现过 1 次 "net: tcp selftest PASS" 晚于 60s 窗口），表现为全量检查
# 只有 "TCP 健壮性自检通过" 这一项失败。此时脚本会整轮重跑（-Retries 控制，
# 默认 2 次），并在最后的报告里如实打印每一轮的结果。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/audit-net.ps1
#   ... -NoFull      只编译+启动+断言 net: audit 行（迭代用，不当 225 项门禁）
#   ... -Retries 1   不重试
# 退出码：0 = 全 PASS，1 = 有失败
# =============================================================================
param(
    [int]$QmpPort = 4543,
    [int]$EchoPort = 12399,
    [string]$ObjDir = '.obj-audit-net',
    [string]$DistDir = '.dist-audit-net',
    [string]$SerialLog = 'obj/audit/net.log',
    [switch]$NoFull,
    [int]$TimeoutSec = 12,
    [int]$Retries = 2
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$auditDir = Join-Path $root 'obj/audit'
New-Item -ItemType Directory -Force -Path $auditDir | Out-Null

# audit-net 修复对应的回归行（缺一行即判失败）
$auditLines = @(
    'net: audit arp spoof rejected ok',
    'net: audit udp ingress rejects malformed ok (rejected=5)',
    'net: audit net ok (udp+arp)',
    'net: audit tcp forged SYN ignored ok',
    'net: audit tcp receive accounting ok (partial segment not over-ACKed)',
    'net: audit tcp handshake seq validated ok',
    'net: audit tcp ip ingress rejects malformed ok (bad=3)',
    'net: audit listen socket owned by pid ok',
    'net: audit tcp ok'
)

# 既有 net 正向断言（抽关键项，防止修复把正向链路弄坏）
$netLines = @(
    'net: ARP reply gw=',
    'net: ICMP echo reply seq=',
    'net: UDP echo reply',
    'net: TCP echo reply',
    'net: dhcp ok',
    'net: dns ok',
    'net: ping ok',
    'net: tcp handshake ok',
    'net: tcp large transfer ok',
    'net: tcp listen/accept selftest PASS',
    'net: tcp selftest PASS'
)

function Invoke-AuditRun([int]$attempt) {
    $serialAbs = Join-Path $root $SerialLog
    Remove-Item $serialAbs -ErrorAction SilentlyContinue

    $buildArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'build.ps1',
        '-Test',
        '-ObjDir', $ObjDir, '-DistDir', $DistDir,
        '-QmpPort', "$QmpPort", '-EchoPort', "$EchoPort",
        '-SerialLog', $SerialLog, '-TimeoutSec', "$TimeoutSec"
    )
    $buildLog = Join-Path $auditDir ("build-$attempt.log")
    Write-Host ("=== audit-net run {0}: build + qemu (obj={1} qmp={2} echo={3} serial={4}) ===" -f `
                $attempt, $ObjDir, $QmpPort, $EchoPort, $SerialLog) -ForegroundColor Cyan
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $buildArgs -Wait -PassThru `
            -NoNewWindow -RedirectStandardOutput $buildLog `
            -RedirectStandardError (Join-Path $auditDir ("build-$attempt.err.log"))

    $serialText = ''
    if (Test-Path $serialAbs) { $serialText = [string](Get-Content $serialAbs -Raw) }
    return @{ Exit = $p.ExitCode; Text = $serialText; Log = $buildLog }
}

function Show-Lines([string]$text, [string[]]$lines, [string]$title) {
    Write-Host ''
    Write-Host ("--- {0} ---" -f $title) -ForegroundColor Cyan
    $miss = @()
    foreach ($l in $lines) {
        if ($text.Contains($l)) {
            Write-Host ("  [PASS] {0}" -f $l) -ForegroundColor Green
        } else {
            Write-Host ("  [FAIL] {0}" -f $l) -ForegroundColor Red
            $miss += $l
        }
    }
    return $miss
}

$attempt = 0
$finalMiss = @()
$finalExit = 1
$fullOk = $false
do {
    $attempt++
    $run = Invoke-AuditRun $attempt
    if (-not $run.Text) {
        Write-Host '=== audit-net: FAIL（串口日志为空，QEMU 可能没起来） ===' -ForegroundColor Red
        $finalMiss = @('serial log empty')
        $finalExit = 1
        break
    }

    $missAudit = @(Show-Lines $run.Text $auditLines 'net: audit 回归行')
    $missNet = @(Show-Lines $run.Text $netLines '既有 net 正向断言')
    $fullOk = $run.Text.Contains('=== TEST PASS (225/225) ===')
    if ($NoFull) { $fullOk = $true }

    $finalMiss = @($missAudit) + @($missNet)
    $finalExit = $run.Exit

    Write-Host ''
    if ($fullOk) {
        $m = [regex]::Match($run.Text, '=== TEST PASS \((\d+)/(\d+)\) ===')
        if ($m.Success) {
            Write-Host ("--- 全量检查：{0}/{1} PASS ---" -f $m.Groups[1].Value, $m.Groups[2].Value) -ForegroundColor Green
        } else {
            Write-Host '--- 全量检查：跳过（-NoFull） ---' -ForegroundColor DarkGray
        }
    } else {
        Write-Host '--- 全量检查：没有 TEST PASS 行（可能只是已知的 tcptest 尾部延迟） ---' -ForegroundColor Yellow
    }

    $onlyFlakyTail = ($finalMiss.Count -eq 0) -and (-not $fullOk) -and
                     ($run.Text.Contains('net: audit net ok')) -and
                     (-not $run.Text.Contains('net: tcp selftest PASS'))
    if ($onlyFlakyTail -and $attempt -lt $Retries) {
        Write-Host ''
        Write-Host ("[retry] 只有 'net: tcp selftest PASS' 落在 60s 窗口之外（仓库已知 flake：基线同样复现），整轮重跑 {0}/{1}" -f ($attempt + 1), $Retries) -ForegroundColor Yellow
        continue
    }
    break
} while ($true)

Write-Host ''
if ($finalMiss.Count -eq 0 -and $fullOk) {
    Write-Host ("=== audit-net: PASS（{0} 轮；修复回归行齐全 + 225/225 + 正向链路未回归） ===" -f $attempt) -ForegroundColor Green
    exit 0
}
if ($finalMiss.Count -eq 0 -and -not $fullOk) {
    Write-Host '=== audit-net: FAIL（回归行齐全，但全量检查未通过；见 obj/audit/build-*.log） ===' -ForegroundColor Red
    exit 1
}
Write-Host ("=== audit-net: FAIL（缺 {0} 行） ===" -f $finalMiss.Count) -ForegroundColor Red
foreach ($m in $finalMiss) { Write-Host ("  * {0}" -f $m) -ForegroundColor Red }
exit 1
