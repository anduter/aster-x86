# =============================================================================
# run-ng.ps1 - ng WP1-2: 在 QEMU 里启动 dist-ng/aster-ng.elf，等一组期望行
#
# 启动命令（WP1-1/WP1-2 出口）：
#   qemu-system-x86_64 -kernel dist-ng/aster-ng.elf -serial file:<log> \
#                      -display none -no-reboot -m 256 -cpu max
#
# 用 file: 串口而不是 stdio：脚本要在 QEMU 不退出（内核 hlt 循环）的情况下
# 轮询串口内容，命中所有期望行后主动结束 QEMU。
#
# -cpu 默认 max：默认的 qemu64 不报 SMEP/SMAP，WP1-2 的 CR4 置位路径就测不到；
# 想看"CPU 不支持时打印 off"的检测路径，用 -Cpu qemu64（见 claims）。
#
# 期望行（全部命中才 PASS）：
#   ng: hello 64-bit
#   ng: paging 4-level ok
#   ng: wx layout ok (text rx, rodata ro, data rw-nx)
#   ng: nx on
#   ng: smep=<on|off> smap=<on|off>
#   ng: WP1-2 selftest PASS
#
# 若 ELF 不存在会先调用 tools/build-ng.ps1（干净检出也能一条命令 PASS）。
#
# usage: powershell -File tools/run-ng.ps1 [-Repo <root>] [-ElfPath <elf>]
#                                           [-TimeoutSec 20] [-SerialLog <path>] [-Cpu max]
# 退出码：0 = PASS；1 = FAIL（缺 QEMU / 启动失败 / 超时 / 期望行缺失）。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$ElfPath = '',
    [int]$TimeoutSec = 20,
    [string]$SerialLog = '',
    [string]$Cpu = 'max'
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path

# ---- 期望的串口行 ----------------------------------------------------------
$required = @(
    'ng: hello 64-bit',
    'ng: paging 4-level ok',
    'ng: wx layout ok (text rx, rodata ro, data rw-nx)',
    'ng: nx on',
    'ng: WP1-2 selftest PASS'
)
$smepPattern = 'ng: smep=(on|off) smap=(on|off)'
$failPattern = 'ng: WP1-2 selftest FAIL'

function Test-Serial([string]$text) {
    if (-not $text) { return $false }
    foreach ($line in $required) {
        if (-not $text.Contains($line)) { return $false }
    }
    if ($text -notmatch $smepPattern) { return $false }
    return $true
}

function Fail([string]$msg) {
    Write-Host ("run-ng: FAIL ({0})" -f $msg)
    exit 1
}

# ---- 内核 ELF --------------------------------------------------------------
if ($ElfPath) {
    if (-not (Test-Path -LiteralPath $ElfPath)) { Fail ("ELF not found: {0}" -f $ElfPath) }
    $elf = (Resolve-Path -LiteralPath $ElfPath).Path
} else {
    $elf = Join-Path $Repo 'dist-ng\aster-ng.elf'
}

if (-not (Test-Path -LiteralPath $elf)) {
    Write-Host 'run-ng: kernel not built yet, running build-ng.ps1 first'
    $build = Join-Path $PSScriptRoot 'build-ng.ps1'
    & powershell -NoProfile -ExecutionPolicy Bypass -File $build -Repo $Repo
    if ($LASTEXITCODE -ne 0) { Fail 'build-ng.ps1 failed' }
    if (-not (Test-Path -LiteralPath $elf)) { Fail 'build-ng.ps1 did not produce the ELF' }
}

# ---- QEMU ------------------------------------------------------------------
$qemu = $null
$qcmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if ($qcmd) {
    $qemu = $qcmd.Source
} elseif (Test-Path 'D:\Program Files\qemu\qemu-system-x86_64.exe') {
    $qemu = 'D:\Program Files\qemu\qemu-system-x86_64.exe'
}
if (-not $qemu) { Fail 'qemu-system-x86_64 not found (PATH or D:\Program Files\qemu)' }

$qemuVer = ''
$verOut = & $qemu --version 2>&1
if ($LASTEXITCODE -eq 0 -and $verOut) { $qemuVer = (@($verOut)[0]).ToString() }

# ---- 串口日志（默认放临时目录，避免污染仓库）------------------------------
$tmpBase = Join-Path ([IO.Path]::GetTempPath()) ('aster-ng-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmpBase | Out-Null
$keepLog = [bool]$SerialLog
if (-not $SerialLog) { $SerialLog = Join-Path $tmpBase 'serial.log' }
$errLog = Join-Path $tmpBase 'qemu.err'
$outLog = Join-Path $tmpBase 'qemu.out'
Remove-Item -LiteralPath $SerialLog -Force -ErrorAction SilentlyContinue

Write-Host '=== run-ng (WP1-2) ==='
Write-Host ("elf:  {0} ({1} bytes)" -f $elf, (Get-Item -LiteralPath $elf).Length)
Write-Host ("qemu: {0}" -f $qemu)
if ($qemuVer) { Write-Host ("      {0}" -f $qemuVer) }

# -kernel 之后是 ELF；-serial file: 让脚本可轮询；headless、不自动重启、256 MiB
$qemuArgs = @(
    '-kernel', ('"' + $elf + '"'),
    '-serial', ('"file:' + $SerialLog + '"'),
    '-display', 'none',
    '-no-reboot',
    '-m', '256',
    '-cpu', $Cpu
)
Write-Host ("args: -kernel <elf> -serial file:<log> -display none -no-reboot -m 256 -cpu {0} (timeout {1}s)" -f $Cpu, $TimeoutSec)

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $qemu -ArgumentList $qemuArgs -PassThru `
    -RedirectStandardError $errLog -RedirectStandardOutput $outLog -WindowStyle Hidden

$matched = $false
$failedEarly = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    if ($p.HasExited) { break }
    if (Test-Path -LiteralPath $SerialLog) {
        $text = Get-Content -LiteralPath $SerialLog -Raw -ErrorAction SilentlyContinue
        if ($text) {
            if ($text -match $failPattern) { $failedEarly = $true; break }
            if (Test-Serial $text) { $matched = $true; break }
        }
    }
    Start-Sleep -Milliseconds 100
}
$sw.Stop()

# 内核是 hlt 循环，检测完/超时后必须主动结束 QEMU
$exitedEarly = $p.HasExited
if (-not $exitedEarly) {
    try { $p.Kill() } catch { }
    $p.WaitForExit(3000) | Out-Null
}

$serialText = ''
if (Test-Path -LiteralPath $SerialLog) {
    $serialText = (Get-Content -LiteralPath $SerialLog -Raw -ErrorAction SilentlyContinue)
}
$qemuErr = ''
if (Test-Path -LiteralPath $errLog) { $qemuErr = (Get-Content -LiteralPath $errLog -Raw -ErrorAction SilentlyContinue) }

function Remove-TmpBase {
    if (-not $keepLog) { Remove-Item -Recurse -Force $tmpBase -ErrorAction SilentlyContinue }
}

if ($matched) {
    Write-Host 'run-ng: serial output:'
    Write-Host ('-' * 48)
    Write-Host ($serialText.TrimEnd())
    Write-Host ('-' * 48)
    $missingCount = @($required | Where-Object { -not $serialText.Contains($_) }).Count
    Write-Host ("run-ng: matched {0}/{1} expected lines + smep/smap line" -f ($required.Count - $missingCount), $required.Count)
    Write-Host ("run-ng: PASS (all expected lines in {0:N1}s, cpu {1})" -f $sw.Elapsed.TotalSeconds, $Cpu)
    Remove-TmpBase
    exit 0
}

# 失败：把串口/QEMU 输出都打出来，便于定位页表/长模式/far jump 的坑
Write-Host 'run-ng: serial output:'
Write-Host ('-' * 48)
if ($serialText) { Write-Host ($serialText.TrimEnd()) } else { Write-Host '(empty)' }
Write-Host ('-' * 48)
$missing = @($required | Where-Object { -not $serialText -or -not $serialText.Contains($_) })
if ($missing.Count -gt 0) {
    foreach ($m in $missing) { Write-Host ("run-ng: missing line: {0}" -f $m) }
}
if ($serialText -and $serialText -notmatch $smepPattern) {
    Write-Host 'run-ng: missing line: ng: smep=<on|off> smap=<on|off>'
}
if ($qemuErr -and $qemuErr.Trim()) {
    Write-Host 'run-ng: qemu stderr:'
    Write-Host ($qemuErr.TrimEnd())
}
if ($failedEarly) { Fail 'kernel reported ng: WP1-2 selftest FAIL' }
if ($exitedEarly) {
    # Start-Process 返回的进程对象有时读不到 ExitCode；读不到就不打，stderr 已足够定位
    $code = $null
    try { $p.WaitForExit(); $code = $p.ExitCode } catch { }
    $suffix = ''
    if ("$code" -ne '') { $suffix = " (code $code)" }
    Fail ("QEMU exited before all expected lines{0}" -f $suffix)
}
Fail ("timeout {0}s, expected lines not all found" -f $TimeoutSec)
