# =============================================================================
# parity-ng.ps1 - ng WP2-7 / E2 鍑哄彛锛?00 椤?parity 闂ㄧ
#
# 涓ょ鐢ㄦ硶锛?#   1) 鐙珛璺戯紙榛樿锛夛細鍚姩 QEMU 璺?tools/run-ng.ps1锛屽啀鏂█ parity 缁撴灉锛?#   2) 澶嶇敤鏃ュ織锛坈i-ng 鐢級锛?LogFile <run-ng 鏃ュ織>锛屼笉鍐嶉噸澶嶅惎鍔?QEMU銆?#
# 鏂█锛?#   - 涓插彛閲屾湁 `ng: parity 100/100 PASS`锛?#   - 閫愰」琛?`parity: NNN/100 [domain] ... ok` 鎭板ソ 100 鏉°€佺紪鍙?001..100 杩炵画锛?#   - 娌℃湁浠讳綍 `... FAIL` 琛?/ `ng: parity FAIL`锛?#   - 鐙珛璺戞椂杩樿姹?run-ng 鏈韩 PASS锛堟棫鐨?108 鏉℃湡鏈涜 + 姝ｅ垯锛夈€?#
# 杈撳嚭锛歚parity-ng: PASS (100/100 items, 0 FAIL, 10 domains)` 鎴?#       `parity-ng: FAIL (...)`锛涢€€鍑虹爜 0/1銆?#
# usage:
#   powershell -NoProfile -File tools/parity-ng.ps1
#   powershell -NoProfile -File tools/parity-ng.ps1 -Cpu qemu64
#   powershell -NoProfile -File tools/parity-ng.ps1 -LogFile C:\tmp\run.log
#   powershell -NoProfile -File tools/parity-ng.ps1 -Fallback     # -vga none 璺緞
#
# 鐜锛氶渶瑕?D:\msys64\mingw64\bin 鍦?PATH锛坮un-ng / build-ng 浼氳嚜宸辫ˉ锛夈€侭OM銆?# =============================================================================
param(
    [string]$Repo = '',
    [string]$LogFile = '',
    [int]$TimeoutSec = 20,
    [string]$Cpu = 'max',
    [switch]$Fallback,
    [switch]$KeepLog
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $Repo)) { throw ("parity-ng: repo not found: {0}" -f $Repo) }
$Repo = (Resolve-Path -LiteralPath $Repo).Path

function Fail([string]$msg) {
    Write-Host ("parity-ng: FAIL ({0})" -f $msg)
    exit 1
}

$OnWindows = ($env:OS -eq 'Windows_NT')
$ownLog = $false
$text = ''

if ($LogFile) {
    if (-not (Test-Path -LiteralPath $LogFile)) {
        Fail ("log file not found: {0}" -f $LogFile)
    }
    $text = Get-Content -LiteralPath $LogFile -Raw
    if (-not $text) { Fail 'empty log file' }
    Write-Host '=== parity-ng (WP2-7, 100 items) ==='
    Write-Host ("log:    {0} (reused)" -f $LogFile)
} else {
    $psHost = $null
    try { $psHost = (Get-Process -Id $PID).Path } catch { }
    if (-not $psHost) {
        $cmd = Get-Command pwsh -ErrorAction SilentlyContinue
        if (-not $cmd) { $cmd = Get-Command powershell -ErrorAction SilentlyContinue }
        if (-not $cmd) { Fail 'cannot locate a PowerShell host (pwsh/powershell)' }
        $psHost = $cmd.Source
    }
    $runNg = Join-Path $Repo 'tools/run-ng.ps1'
    if (-not (Test-Path -LiteralPath $runNg)) { Fail 'tools/run-ng.ps1 missing' }

    $log = Join-Path ([IO.Path]::GetTempPath()) ('ng-parity-' + [guid]::NewGuid().ToString('N').Substring(0, 8) + '.log')
    $argList = @('-NoProfile')
    if ($OnWindows) { $argList += @('-ExecutionPolicy', 'Bypass') }
    $argList += @('-File', $runNg, '-Repo', $Repo, '-Cpu', $Cpu, '-TimeoutSec', "$TimeoutSec")
    if ($Fallback) { $argList += '-Fallback' }

    Write-Host '=== parity-ng (WP2-7, 100 items) ==='
    Write-Host ("run:    {0}" -f $runNg)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $psHost @argList *> $log
        $runCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
    }
    $sw.Stop()
    $text = Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
    if (-not $text) { $text = '' }
    $ownLog = $true

    if ($runCode -ne 0 -or $text -notmatch 'run-ng: PASS') {
        $tail = @($text -split "`r?`n" | Select-Object -Last 25)
        foreach ($l in $tail) { Write-Host ('  | ' + $l) }
        if (-not $KeepLog) { Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue }
        Fail ("run-ng did not PASS (exit={0}, {1:N1}s)" -f $runCode, $sw.Elapsed.TotalSeconds)
    }
    Write-Host ("run-ng: PASS ({0:N1}s)" -f $sw.Elapsed.TotalSeconds)
}

# ---- 閫愰」琛?----------------------------------------------------------------
$itemRx = [regex]'(?m)^parity: (\d{3})/100 \[([a-z0-9]+)\] (.*?) \.\.\. (ok|FAIL)\r?$'
$items = @($itemRx.Matches($text))
$okCount = 0
$failCount = 0
$seenIds = New-Object System.Collections.Generic.List[int]
$domains = @{}
$failLines = New-Object System.Collections.Generic.List[string]

foreach ($m in $items) {
    $id = [int]$m.Groups[1].Value
    $dom = $m.Groups[2].Value
    $state = $m.Groups[4].Value
    $seenIds.Add($id)
    if (-not $domains.ContainsKey($dom)) { $domains[$dom] = 0 }
    $domains[$dom] = $domains[$dom] + 1
    if ($state -eq 'ok') {
        $okCount++
    } else {
        $failCount++
        $failLines.Add($m.Value)
    }
}

# ---- 姹囨€昏 / 澶辫触鏍囪 -----------------------------------------------------
$summaryOk = ($text -match 'ng: parity 100/100 PASS')
$anyFailMarker = ($text -match 'ng: parity FAIL')
$detailFail = ($text -match '(?m)^\s+parity detail: ')

Write-Host ''
Write-Host '--- parity items ---'
foreach ($d in ($domains.Keys | Sort-Object)) {
    Write-Host ("  {0,-6} {1,3}" -f $d, $domains[$d])
}
Write-Host ("  total  {0,3}" -f $items.Count)
Write-Host ''

$problems = New-Object System.Collections.Generic.List[string]
if (-not $summaryOk) { $problems.Add('missing "ng: parity 100/100 PASS"') }
if ($anyFailMarker) { $problems.Add('found "ng: parity FAIL"') }
if ($items.Count -ne 100) { $problems.Add(("item lines {0} != 100" -f $items.Count)) }
if ($failCount -ne 0 -or $detailFail) { $problems.Add(("failed items {0}" -f $failCount)) }
for ($i = 0; $i -lt $seenIds.Count; $i++) {
    if ($seenIds[$i] -ne ($i + 1)) {
        $problems.Add(("id sequence broken at index {0} (id={1})" -f $i, $seenIds[$i]))
        break
    }
}

if ($problems.Count -gt 0) {
    if ($failLines.Count -gt 0) {
        Write-Host '--- failed items ---'
        foreach ($l in $failLines) { Write-Host ('  ' + $l) -ForegroundColor Red }
    }
    foreach ($p in $problems) { Write-Host ('  ! ' + $p) -ForegroundColor Red }
    if ($ownLog -and -not $KeepLog) { Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue }
    Fail ($problems -join '; ')
}

Write-Host ("parity-ng: PASS ({0}/100 items, 0 FAIL, {1} domains)" -f $okCount, $domains.Count) -ForegroundColor Green
if ($ownLog) {
    if ($KeepLog) { Write-Host ("log kept: {0}" -f $log) }
    else { Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue }
}
exit 0
