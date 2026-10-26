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
    'net: audit dns txid entropy ok',
    'net: audit tcp forged ACK ignored ok',
    'net: audit tcp ok'
)

# 既有 net 正向断言：只放"启动/自检阶段必然产生"的行（不依赖 shell 注入键，
# 宿主被占满时 shell 键注入类检查会成批失败，那是环境问题不是 net 回归）。
$netLines = @(
    'net: ARP reply gw=',
    'net: ICMP echo reply seq=',
    'net: UDP echo reply',
    'net: dhcp ok',
    'net: dns ok',
    'net: ping ok',
    'net: tcp handshake ok',
    'net: tcp large transfer ok'
)

# 尾部（shell 注入相关 + sleep 密集段）：仓库既有 flake 与宿主争用都会命中，
# 单独判定，只有这些缺失时才整轮重试（见文件头注释与 docs/audit/net.md §5-8）。
$tailLines = @(
    'net: TCP echo reply',
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
    $buildText = ''
    if (Test-Path $buildLog) { $buildText = [string](Get-Content $buildLog -Raw) }
    return @{ Exit = $p.ExitCode; Text = $serialText; Build = $buildText; Log = $buildLog }
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
    $missTail = @(Show-Lines $run.Text $tailLines 'tcptest 尾部（已知 flake，单独判定）')
    # 225 项断言行在 build.ps1 的 stdout（build-*.log），不在串口日志里
    $fullMatch = [regex]::Match($run.Build, '=== TEST PASS \((\d+)/(\d+)\) ===')
    $fullOk = $fullMatch.Success -and ($fullMatch.Groups[1].Value -eq $fullMatch.Groups[2].Value)
    $fullCount = if ($fullMatch.Success) { $fullMatch.Groups[2].Value } else { '?' }
    if ($NoFull) { $fullOk = $true }

    $finalMiss = @($missAudit) + @($missNet) + @($missTail)
    $finalExit = $run.Exit

    Write-Host ''
    if ($fullOk) {
        $m = [regex]::Match($run.Build, '=== TEST PASS \((\d+)/(\d+)\) ===')
        if ($m.Success) {
            Write-Host ("--- 全量检查：{0}/{1} PASS ---" -f $m.Groups[1].Value, $m.Groups[2].Value) -ForegroundColor Green
        } else {
            Write-Host '--- 全量检查：跳过（-NoFull） ---' -ForegroundColor DarkGray
        }
    } else {
        $failedChecks = @([regex]::Matches($run.Build, '\[FAIL\] ([^\r\n]+)') | ForEach-Object { $_.Groups[1].Value.Trim() })
        Write-Host '--- 全量检查：没有 TEST PASS 行 ---' -ForegroundColor Yellow
        foreach ($fc in $failedChecks) { Write-Host ("  [FAIL] {0}" -f $fc) -ForegroundColor Red }
    }

    # 只有"既有 tcptest 尾部"缺失（审计行与其它正向行全在）时才重试：
    # 这是仓库已知的尾部延迟/卡住，与本会话 net 改动无关（基线同样复现）。
    $onlyFlakyTail = ($missAudit.Count -eq 0) -and ($missNet.Count -eq 0) -and
                     ($missTail.Count -ge 1) -and (-not $fullOk)
    if ($onlyFlakyTail -and $attempt -lt $Retries) {
        Write-Host ''
        Write-Host ("[retry] 只有 tcptest 尾部（'net: tcp selftest PASS'）缺失 = 仓库已知 flake，整轮重跑 {0}/{1}" -f ($attempt + 1), $Retries) -ForegroundColor Yellow
        continue
    }
    break
} while ($true)

Write-Host ''
if ($finalMiss.Count -eq 0 -and $fullOk) {
    Write-Host ("=== audit-net: PASS（{0} 轮；修复回归行齐全 + {1}/{1} 全量检查 + 正向链路未回归） ===" -f $attempt, $fullCount) -ForegroundColor Green
    exit 0
}
if ($finalMiss.Count -eq 0 -and -not $fullOk) {
    Write-Host '=== audit-net: FAIL（回归行齐全，但全量检查未通过；见 obj/audit/build-*.log） ===' -ForegroundColor Red
    exit 1
}
Write-Host ("=== audit-net: FAIL（缺 {0} 行） ===" -f $finalMiss.Count) -ForegroundColor Red
foreach ($m in $finalMiss) { Write-Host ("  * {0}" -f $m) -ForegroundColor Red }
exit 1
