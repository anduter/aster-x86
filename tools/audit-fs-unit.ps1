# =============================================================================
# tools/audit-fs-unit.ps1 - fs 审计的 host 单元测试（不启动 QEMU）
#
# 用仓库自带的 32 位 mingw gcc 编译真实 src/kernel/fs/fat16.c，运行
# tests/audit/fs/test_fat16_audit.c：坏 BPB / 坏簇链 / 非法首簇 / 截断忙 /
# 随机镜像 fuzz。宿主进程崩溃或 90s 超时都算 FAIL（用于发现死循环）。
#
# 用法：
#   powershell -NoProfile -File tools/audit-fs-unit.ps1
#   powershell -NoProfile -File tools/audit-fs-unit.ps1 -Fat16Src obj\audit\fat16_before.c
# =============================================================================
param(
    [string]$Repo = '',
    [string]$Gcc32 = '',
    [string]$Fat16Src = '',
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$root = (Resolve-Path $Repo).Path
Set-Location $root

if (-not $Gcc32) {
    $cand = Join-Path $root 'tools\w64devkit\bin\i686-w64-mingw32-gcc.exe'
    if (Test-Path $cand) {
        $Gcc32 = $cand
    } else {
        $cmd = Get-Command i686-w64-mingw32-gcc -ErrorAction SilentlyContinue
        if ($cmd) { $Gcc32 = $cmd.Source }
    }
}
if (-not $Gcc32 -or -not (Test-Path $Gcc32)) {
    throw 'audit-fs-unit: 找不到 32 位 gcc（tools/w64devkit/bin/i686-w64-mingw32-gcc.exe）'
}
if (-not $Fat16Src) { $Fat16Src = 'src/kernel/fs/fat16.c' }
if (-not (Test-Path $Fat16Src)) { throw ("audit-fs-unit: Fat16Src 不存在: {0}" -f $Fat16Src) }

$outDir = Join-Path $root 'obj/audit/fs'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir 'test-fat16-audit.exe'

$cflags = @(
    '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
    '-Wno-unused-parameter', '-Wl,--disable-dynamicbase',
    '-Itests/unit', '-Itests/unit/shim', '-Isrc/kernel/fs',
    '-Dmemcpy=k_memcpy', '-Dmemset=k_memset', '-Dmemmove=k_memmove',
    '-Dmemcmp=k_memcmp', '-Dstrlen=k_strlen', '-Dstrcmp=k_strcmp',
    '-Dstrncmp=k_strncmp', '-Dstrcpy=k_strcpy', '-Dstrncpy=k_strncpy',
    '-Dstrchr=k_strchr', '-Dutoa=k_utoa', '-Ditoa=k_itoa',
    '-Dkprintf=unit_kprintf'
)

Write-Host '=== audit-fs unit (FAT16) ===' -ForegroundColor Cyan
Write-Host ("  repo   : {0}" -f $root) -ForegroundColor DarkGray
Write-Host ("  gcc32  : {0}" -f $Gcc32) -ForegroundColor DarkGray
Write-Host ("  fat16  : {0}" -f $Fat16Src) -ForegroundColor DarkGray

$saved = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $compile = (& $Gcc32 @cflags -o $exe 'tests/audit/fs/test_fat16_audit.c' $Fat16Src 'src/kernel/lib/string.c' 2>&1 | Out-String)
    $compileCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $saved
}
if ($compileCode -ne 0) {
    Write-Host '  [FAIL] compile' -ForegroundColor Red
    foreach ($line in ($compile -split "`r?`n")) { if ($line.Trim()) { Write-Host ("         {0}" -f $line) -ForegroundColor DarkGray } }
    exit 1
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$proc = [System.Diagnostics.Process]::Start($psi)
$outTask = $proc.StandardOutput.ReadToEndAsync()
$errTask = $proc.StandardError.ReadToEndAsync()
$done = $proc.WaitForExit($TimeoutSec * 1000)
if (-not $done) {
    try { $proc.Kill() } catch { }
    Write-Host ("  [FAIL] timeout after {0}s (hang)" -f $TimeoutSec) -ForegroundColor Red
    exit 1
}
$proc.WaitForExit()
$out = $outTask.Result
$err = $errTask.Result
if ($out) { Write-Host $out.TrimEnd() -ForegroundColor DarkGray }
if ($err) { Write-Host $err.TrimEnd() -ForegroundColor DarkYellow }
if ($proc.ExitCode -ne 0) {
    Write-Host ("  [FAIL] exit={0}" -f $proc.ExitCode) -ForegroundColor Red
    exit 1
}
Write-Host '=== FAT16 AUDIT PASS ===' -ForegroundColor Green
exit 0
