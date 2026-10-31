# =============================================================================
# ci-ng.ps1 - ng 分支门禁：一条命令跑完 64 位内核的静态检查 + host 单测 +
#             构建 + ring3 回归 + parity 100 项 + 旧 32 位 parity
#
# 步骤（按顺序）：
#   1. tools/check-domains.ps1     域边界（向上依赖必须失败），期望 0 违规
#   2. tools/check-deps.ps1        旧树模块边界，期望 0 违规
#   3. tools/unit-ng.ps1           host 单测（PMM/heap/memmap/sched/ipc/elf/
#                                  uaccess/syscall/fs/fat/ata/drivers/auth/
#                                  log/pkg/shell/parity），期望 N checks, 0 failures
#   4. tools/build-ng.ps1          构建 dist-ng/aster-ng.elf（ELF64 + multiboot2
#                                  + PVH + 用户 ELF 校验）
#   5. tools/run-ng.ps1            QEMU 启动，等 108 条期望行 + 42 条正则（含
#                                  `ng: ring3 hello`、`ng: parity 100/100 PASS`）
#   6. tools/parity-ng.ps1         WP2-7 parity 门禁（复用第 5 步日志，断言
#                                  100 项逐条 ok / 无 FAIL，不重复起 QEMU）
#   7. build.ps1 -Test             旧 32 位回归（默认跑，保持 parity；
#                                  -Quick / -NoLegacy 跳过；非 Windows 跳过）
#
# 输出：每步一行 PASS/FAIL/SKIP；失败时打印该步日志尾部；最后
#       `=== NG CI PASS ===`（退出码 0）或 `=== NG CI FAIL (n) ===`（退出码 1）。
#
# 跨平台：Windows PowerShell 5.1 与 ubuntu `pwsh` 都能跑。
#   - 仓库根默认取本脚本上级目录，不写死盘符；
#   - 子脚本用“当前 PowerShell 宿主”启动（pwsh / powershell 通吃）；
#   - -ExecutionPolicy 只在 Windows 上传递（Linux 不支持该参数）。
#
# usage:
#   powershell -NoProfile -File tools/ci-ng.ps1                  # 全量（含 QEMU）
#   powershell -NoProfile -File tools/ci-ng.ps1 -Quick           # 无 QEMU / legacy
#   pwsh -NoProfile -File tools/ci-ng.ps1 -Quick                 # ubuntu runner
#   powershell -NoProfile -File tools/ci-ng.ps1 -Cpu qemu64      # SMAP=off 路径
#   powershell -NoProfile -File tools/ci-ng.ps1 -KeepLogs        # 保留每步日志
#
# 退出码：0 = PASS；1 = 有失败步骤（或仓库结构不对）。
# =============================================================================
param(
    [string]$Repo = '',
    [switch]$Quick,
    [switch]$Legacy,
    [switch]$NoLegacy,
    [string]$Cpu = 'max',
    [int]$RunTimeoutSec = 20,
    [switch]$KeepLogs
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

# ---- 仓库根 ----------------------------------------------------------------
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $Repo)) { throw ("ci-ng: repo not found: {0}" -f $Repo) }
$Repo = (Resolve-Path -LiteralPath $Repo).Path

$OnWindows = ($env:OS -eq 'Windows_NT')
$Mode = if ($Quick) { 'quick' } else { 'full' }

# 用当前 PowerShell 宿主跑子脚本：本地 PS5.1 / CI pwsh / Linux pwsh 都不写死解释器名。
$psHost = $null
try { $psHost = (Get-Process -Id $PID).Path } catch { }
if (-not $psHost) {
    $cmd = Get-Command pwsh -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command powershell -ErrorAction SilentlyContinue }
    if (-not $cmd) { throw 'ci-ng: cannot locate a PowerShell host (pwsh/powershell)' }
    $psHost = $cmd.Source
}

$requiredScripts = @(
    'tools/check-domains.ps1',
    'tools/check-deps.ps1',
    'tools/unit-ng.ps1',
    'tools/build-ng.ps1',
    'tools/run-ng.ps1',
    'tools/parity-ng.ps1',
    'build.ps1'
)
foreach ($rel in $requiredScripts) {
    $p = Join-Path $Repo $rel
    if (-not (Test-Path -LiteralPath $p)) { throw ("ci-ng: missing script: {0}" -f $rel) }
}

# 旧 32 位 parity：默认开；-Quick / -NoLegacy 关。-Legacy 是显式开关（默认已开，
# 传了等价于确认），保持与 evolution.md 门禁描述一致。
$runLegacy = -not $Quick -and -not $NoLegacy
if ($Legacy -and $Quick) {
    Write-Host 'ci-ng: -Legacy 与 -Quick 同时给出，-Quick 优先（跳过 legacy）' -ForegroundColor DarkYellow
}

$logDir = Join-Path ([IO.Path]::GetTempPath()) ('ng-ci-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$results = New-Object System.Collections.Generic.List[object]
$swAll = [Diagnostics.Stopwatch]::StartNew()

function Write-Ng([string]$text) { Write-Host $text }

function Add-Result([string]$label, [string]$status, [string]$detail, [string]$log) {
    $results.Add([pscustomobject]@{ Label = $label; Status = $status; Detail = $detail; Log = $log })
}

function Show-Tail([string]$path, [int]$lines = 30) {
    if (-not $path -or -not (Test-Path -LiteralPath $path)) { return }
    $text = Get-Content -LiteralPath $path -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return }
    $all = @($text -split "`r?`n")
    Write-Host ('    ' + ('-' * 56)) -ForegroundColor DarkGray
    foreach ($l in (@($all | Select-Object -Last $lines))) {
        Write-Host ('    | ' + $l) -ForegroundColor DarkGray
    }
    Write-Host ('    ' + ('-' * 56)) -ForegroundColor DarkGray
}

# 跑一个子脚本；Parse 返回 @{ Ok = $bool; Detail = '...' }
function Invoke-NgStep {
    param(
        [string]$Label,
        [string]$RelativePath,
        [string[]]$ScriptArgs = @(),
        [scriptblock]$Parse = $null,
        [string]$SkipReason = ''
    )

    if ($SkipReason) {
        Write-Host ("  [SKIP] {0}  ({1})" -f $Label, $SkipReason) -ForegroundColor DarkYellow
        Add-Result $Label 'skip' $SkipReason ''
        return $false
    }

    Write-Host ("  [ .. ] {0}" -f $Label)
    $log = Join-Path $logDir (($Label -replace '[^A-Za-z0-9_.-]', '_') + '.log')
    $argList = @('-NoProfile')
    if ($OnWindows) { $argList += @('-ExecutionPolicy', 'Bypass') }
    $argList += @('-File', (Join-Path $Repo $RelativePath)) + $ScriptArgs

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $psHost @argList *> $log
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
    }
    $sw.Stop()

    $text = ''
    if (Test-Path -LiteralPath $log) {
        $text = Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue
    }
    $ok = ($code -eq 0)
    $detail = ("exit={0}, {1:N1}s" -f $code, $sw.Elapsed.TotalSeconds)
    if ($Parse) {
        $r = & $Parse $text
        if ($null -ne $r) {
            if (-not $r.Ok) { $ok = $false }
            if ($r.Detail) { $detail = $r.Detail + '; ' + $detail }
        }
    }

    if ($ok) {
        Write-Host ("  [PASS] {0}  ({1})" -f $Label, $detail) -ForegroundColor Green
        Add-Result $Label 'pass' $detail $log
    } else {
        Write-Host ("  [FAIL] {0}  ({1})" -f $Label, $detail) -ForegroundColor Red
        Add-Result $Label 'fail' $detail $log
        Show-Tail $log 30
    }
    return $ok
}

Write-Host ''
Write-Host '=== ci-ng (E1 gate) ===' -ForegroundColor Cyan
Write-Host ("repo:   {0}" -f $Repo)
Write-Host ("host:   {0}" -f $psHost)
Write-Host ("mode:   {0}{1}" -f $Mode, $(if ($OnWindows) { '' } else { ' (non-Windows)' }))
Write-Host ("legacy: {0}" -f $(if ($runLegacy) { 'run' } else { 'skip' }))
Write-Host ''

# ---- 1/6、2/6：静态边界检查 ------------------------------------------------
Write-Ng '--- 1/7 domain boundaries ---'
$null = Invoke-NgStep -Label 'check-domains' -RelativePath 'tools/check-domains.ps1' `
    -ScriptArgs @('-Repo', $Repo) -Parse {
        param($text)
        if ($text -match 'domain include violations:\s*(\d+)') {
            $n = [int]$Matches[1]
            return @{ Ok = ($n -eq 0); Detail = ("{0} violations" -f $n) }
        }
        return @{ Ok = $false; Detail = 'no violation summary in output' }
    }

Write-Ng '--- 2/7 module boundaries ---'
$null = Invoke-NgStep -Label 'check-deps' -RelativePath 'tools/check-deps.ps1' `
    -ScriptArgs @('-Repo', $Repo) -Parse {
        param($text)
        if ($text -match 'module include violations:\s*(\d+)') {
            $n = [int]$Matches[1]
            return @{ Ok = ($n -eq 0); Detail = ("{0} violations" -f $n) }
        }
        return @{ Ok = $false; Detail = 'no violation summary in output' }
    }

# ---- 3/6：host 单测 --------------------------------------------------------
Write-Ng '--- 3/7 host unit tests (unit-ng) ---'
$unitOk = Invoke-NgStep -Label 'unit-ng' -RelativePath 'tools/unit-ng.ps1' `
    -ScriptArgs @('-Repo', $Repo) -Parse {
        param($text)
        if ($text -match 'unit-ng: PASS \((\d+) checks, 0 failures\)') {
            return @{ Ok = $true; Detail = ("{0} checks" -f $Matches[1]) }
        }
        if ($text -match 'unit-ng: FAIL \(([^)]*)\)') {
            return @{ Ok = $false; Detail = ("unit-ng FAIL: {0}" -f $Matches[1]) }
        }
        return @{ Ok = $false; Detail = 'unit-ng PASS line missing' }
    }

# ---- 4/6：构建 ------------------------------------------------------------
Write-Ng '--- 4/7 build kernel (build-ng) ---'
$buildOk = Invoke-NgStep -Label 'build-ng' -RelativePath 'tools/build-ng.ps1' `
    -ScriptArgs @('-Repo', $Repo) -Parse {
        param($text)
        if ($text -notmatch 'build-ng: PASS') {
            return @{ Ok = $false; Detail = 'build-ng PASS line missing' }
        }
        $d = 'kernel built'
        if ($text -match 'ELF64: PASS \((\d+) bytes\)') { $d = ("{0} bytes" -f $Matches[1]) }
        if ($text -match 'user-elf: PASS \((\d+) bytes') { $d = $d + (", user-elf {0} bytes" -f $Matches[1]) }
        return @{ Ok = $true; Detail = $d }
    }

# ---- 5/7：QEMU ring3 回归 --------------------------------------------------
Write-Ng '--- 5/7 QEMU ring3 regression (run-ng) ---'
$runSkip = ''
$runOk = $false
if ($Quick) {
    $runSkip = '-Quick 跳过 QEMU'
} elseif (-not $buildOk) {
    $runSkip = 'build-ng 失败，跳过'
}
$runOk = Invoke-NgStep -Label 'run-ng' -RelativePath 'tools/run-ng.ps1' `
    -ScriptArgs @('-Repo', $Repo, '-Cpu', $Cpu, '-TimeoutSec', "$RunTimeoutSec") `
    -SkipReason $runSkip -Parse {
        param($text)
        if ($text -match 'run-ng: PASS') {
            $d = 'expected lines matched'
            if ($text -match 'matched (\d+)/(\d+) expected lines \+ (\d+) patterns') {
                $d = ("{0}/{1} lines + {2} patterns" -f $Matches[1], $Matches[2], $Matches[3])
            }
            return @{ Ok = $true; Detail = $d }
        }
        if ($text -match 'run-ng: FAIL \(([^)]*)\)') {
            return @{ Ok = $false; Detail = ("run-ng FAIL: {0}" -f $Matches[1]) }
        }
        return @{ Ok = $false; Detail = 'run-ng PASS line missing' }
    }

# ---- 6/7：parity 100 项（复用 run-ng 的日志，不重复启动 QEMU）--------------
Write-Ng '--- 6/7 parity 100 items (WP2-7) ---'
$paritySkip = ''
if ($runSkip) {
    $paritySkip = $runSkip
} elseif (-not $runOk) {
    $paritySkip = 'run-ng 失败，跳过'
}
$runEntry = @($results | Where-Object { $_.Label -eq 'run-ng' })[0]
$runLog = ''
if ($runEntry) { $runLog = $runEntry.Log }
$parityOk = Invoke-NgStep -Label 'parity-ng' -RelativePath 'tools/parity-ng.ps1' `
    -ScriptArgs @('-Repo', $Repo, '-LogFile', $runLog) -SkipReason $paritySkip -Parse {
        param($text)
        if ($text -match 'parity-ng: PASS \((\d+)/100 items, (\d+) FAIL, (\d+) domains\)') {
            return @{ Ok = $true; Detail = ("{0}/100 items, {1} domains" -f $Matches[1], $Matches[3]) }
        }
        if ($text -match 'parity-ng: FAIL \(([^)]*)\)') {
            return @{ Ok = $false; Detail = ("parity-ng FAIL: {0}" -f $Matches[1]) }
        }
        return @{ Ok = $false; Detail = 'parity-ng PASS line missing' }
    }

# ---- 7/7：旧 32 位 parity --------------------------------------------------
Write-Ng '--- 7/7 legacy 32-bit parity (build.ps1 -Test) ---'
$legacySkip = ''
if ($Quick) {
    $legacySkip = '-Quick 跳过 legacy'
} elseif (-not $runLegacy) {
    $legacySkip = '-NoLegacy'
} elseif (-not $OnWindows) {
    # 旧树构建依赖 vendored i686-elf 工具链与 Windows 路径，托管 Linux runner 不跑。
    $legacySkip = '旧 32 位套件需要 Windows 工具链（非 Windows 跳过）'
}
$legacyOk = Invoke-NgStep -Label 'legacy-32bit' -RelativePath 'build.ps1' `
    -ScriptArgs @('-Test') -SkipReason $legacySkip -Parse {
        param($text)
        if ($text -match '=== TEST PASS \((\d+)/(\d+)\) ===') {
            return @{ Ok = $true; Detail = ("{0}/{1} parity checks" -f $Matches[1], $Matches[2]) }
        }
        if ($text -match '=== TEST FAIL \(([^)]*)\) ===') {
            return @{ Ok = $false; Detail = ("legacy TEST FAIL: {0}" -f $Matches[1]) }
        }
        return @{ Ok = $false; Detail = 'TEST PASS line missing' }
    }

$swAll.Stop()

# ---- 汇总 ------------------------------------------------------------------
$failed = @($results | Where-Object { $_.Status -eq 'fail' })
$skipped = @($results | Where-Object { $_.Status -eq 'skip' })
$passed = @($results | Where-Object { $_.Status -eq 'pass' })

Write-Host ''
Write-Host '=== ci-ng summary ===' -ForegroundColor Cyan
foreach ($r in $results) {
    $tag = $r.Status.ToUpper().PadRight(4)
    $color = switch ($r.Status) {
        'pass' { 'Green' }
        'fail' { 'Red' }
        default { 'DarkYellow' }
    }
    Write-Host ("  {0} {1,-16} {2}" -f $tag, $r.Label, $r.Detail) -ForegroundColor $color
}
Write-Host ("  {0} passed, {1} failed, {2} skipped, {3:N1}s" -f `
    $passed.Count, $failed.Count, $skipped.Count, $swAll.Elapsed.TotalSeconds)

if (-not $KeepLogs) {
    Remove-Item -LiteralPath $logDir -Recurse -Force -ErrorAction SilentlyContinue
} else {
    Write-Host ("logs: {0}" -f $logDir)
}

if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host ("=== NG CI FAIL ({0}) ===" -f $failed.Count) -ForegroundColor Red
    foreach ($f in $failed) { Write-Host ("  - {0}: {1}" -f $f.Label, $f.Detail) -ForegroundColor Red }
    exit 1
}

Write-Host ''
Write-Host '=== NG CI PASS ===' -ForegroundColor Green
exit 0
