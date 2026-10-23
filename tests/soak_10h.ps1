# =============================================================================
# soak_10h.ps1 - 10 小时综合稳定性 soak
#
# 每轮：
#   1. build.ps1 -Test 完整构建 + QEMU 52 项回归
#   2. 记录 CSV（时间、轮次、退出码、结果、耗时）
#   3. 失败时保存 serial.log；连续 3 次失败则停止
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\soak_10h.ps1
#   powershell ... -Hours 0.1   # 短跑验证
# =============================================================================
param(
    [double]$Hours = 10.0,
    [int]$TimeoutSec = 18,
    [string]$RunDir = 'test_runs'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$runDir = Join-Path $root $RunDir
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$csvPath = Join-Path $runDir 'soak.csv'
$statusPath = Join-Path $runDir 'soak-status.txt'
$failDir = Join-Path $runDir 'failures'
New-Item -ItemType Directory -Force -Path $failDir | Out-Null

if (-not (Test-Path $csvPath)) {
    'timestamp,cycle,exit_code,result,seconds,serial' | Set-Content -Path $csvPath -Encoding UTF8
}

$start = Get-Date
$end = $start.AddHours($Hours)
$cycle = 0
$failCount = 0
$consecutiveFail = 0

while ((Get-Date) -lt $end) {
    $cycle++
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $outPath = Join-Path $runDir ('cycle-{0:D5}.out' -f $cycle)
    $sw = [Diagnostics.Stopwatch]::StartNew()

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
        -Test -Cpu max -TimeoutSec $TimeoutSec *> $outPath
    $exitCode = $LASTEXITCODE
    $sw.Stop()

    $text = if (Test-Path $outPath) { Get-Content $outPath -Raw } else { '' }
    $result = if ($text -match '=== TEST PASS \(\d+/\d+\) ===') { 'PASS' } else { 'FAIL' }

    $serialPath = ''
    if ($result -eq 'FAIL') {
        $failCount++
        $consecutiveFail++
        $serialPath = Join-Path $failDir ('serial-{0:D5}-{1}.log' -f $cycle, $stamp)
        if (Test-Path (Join-Path $root 'serial.log')) {
            Copy-Item (Join-Path $root 'serial.log') $serialPath -Force
        }
        $failOut = Join-Path $failDir ('cycle-{0:D5}-{1}.out' -f $cycle, $stamp)
        Copy-Item $outPath $failOut -Force
    } else {
        $consecutiveFail = 0
    }

    $secs = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
    $line = '{0},{1},{2},{3},{4},{5}' -f (Get-Date -Format 's'), $cycle, $exitCode, $result, $secs, $serialPath
    [IO.File]::AppendAllText($csvPath, $line + "`n", (New-Object System.Text.UTF8Encoding($false)))

    $status = @(
        "start=$($start.ToString('s'))",
        "now=$(Get-Date -Format s)",
        "end=$($end.ToString('s'))",
        "cycle=$cycle",
        "pass=$($cycle - $failCount)",
        "fail=$failCount",
        "last=$result exit=$exitCode secs=$secs",
        "fail_dir=$failDir"
    ) -join "`n"
    [IO.File]::WriteAllText($statusPath, $status + "`n", (New-Object System.Text.UTF8Encoding($false)))

    Remove-Item $outPath -Force -ErrorAction SilentlyContinue

    if ($consecutiveFail -ge 3) {
        [IO.File]::AppendAllText($csvPath,
            "STOP,consecutive_failures=$consecutiveFail`n",
            (New-Object System.Text.UTF8Encoding($false)))
        break
    }
}

$summary = @(
    "soak finished",
    "start=$($start.ToString('s'))",
    "end=$(Get-Date -Format s)",
    "cycles=$cycle",
    "failures=$failCount",
    "csv=$csvPath"
) -join "`n"
[IO.File]::WriteAllText($statusPath, $summary + "`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Host $summary
