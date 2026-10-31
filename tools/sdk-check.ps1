# =============================================================================
# sdk-check.ps1 - Aster SDK alpha 语法门禁
#
# 对 sdk/include/aster/*.h 与 sdk/examples/**/*.c 逐个跑：
#   clang -fsyntax-only -std=c11 -ffreestanding -fno-builtin -Wall -Wextra -Werror
#         -I sdk/include <file>
#
# 为什么是 -fsyntax-only：SDK 是 header-only + 示例，不需要链接/运行；这一步
# 足够抓出类型、结构布局、内联汇编约束和 API 使用错误，且不会碰 QEMU/构建目录。
#
# 退出码：0 = 全部通过（打印 `sdk-check: PASS (n files)`）；
#         1 = 有文件失败 / 找不到 clang / 目录结构不对。
#
# usage:
#   powershell -NoProfile -File tools/sdk-check.ps1
#   pwsh -NoProfile -File tools/sdk-check.ps1 -Repo /path/to/OS_Project
#   powershell -NoProfile -File tools/sdk-check.ps1 -Clang C:\msys64\mingw64\bin\clang.exe
# =============================================================================
param(
    [string]$Repo = '',
    [string]$Clang = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) {
    $Repo = Split-Path -Parent $PSScriptRoot
}
if (-not (Test-Path -LiteralPath $Repo)) {
    Write-Host ("sdk-check: FAIL (repo not found: {0})" -f $Repo) -ForegroundColor Red
    exit 1
}
$root = (Resolve-Path -LiteralPath $Repo).Path
$incDir = Join-Path $root 'sdk/include'
$asterDir = Join-Path $incDir 'aster'
$exDir = Join-Path $root 'sdk/examples'

if (-not (Test-Path -LiteralPath $asterDir)) {
    Write-Host ("sdk-check: FAIL (missing {0})" -f $asterDir) -ForegroundColor Red
    exit 1
}

# ---- 找 clang --------------------------------------------------------------
if (-not $Clang) {
    $cmd = Get-Command clang -ErrorAction SilentlyContinue
    if ($cmd) {
        $Clang = $cmd.Source
    } else {
        $candidates = @(
            'D:\msys64\mingw64\bin\clang.exe',
            'C:\msys64\mingw64\bin\clang.exe',
            '/usr/bin/clang',
            '/usr/local/bin/clang'
        )
        foreach ($p in $candidates) {
            if (Test-Path -LiteralPath $p) { $Clang = $p; break }
        }
    }
}
if (-not $Clang -or -not (Test-Path -LiteralPath $Clang)) {
    Write-Host 'sdk-check: FAIL (clang not found; pass -Clang <path>)' -ForegroundColor Red
    exit 1
}

# ---- 收集输入 --------------------------------------------------------------
$files = @()
$files += @(Get-ChildItem -LiteralPath $asterDir -Filter '*.h' -File |
    Sort-Object Name)
if (Test-Path -LiteralPath $exDir) {
    $files += @(Get-ChildItem -LiteralPath $exDir -Recurse -Filter '*.c' -File |
        Sort-Object FullName)
}
if ($files.Count -eq 0) {
    Write-Host 'sdk-check: FAIL (no headers/examples found)' -ForegroundColor Red
    exit 1
}

$clangArgs = @(
    '-fsyntax-only',
    '-std=c11',
    '-ffreestanding',
    '-fno-builtin',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-I', $incDir
)

# ---- 逐个检查 --------------------------------------------------------------
$failed = 0
$n = 0
Write-Host ("sdk-check: clang = {0}" -f $Clang)
Write-Host ("sdk-check: include = {0}" -f $incDir)
foreach ($f in $files) {
    $n++
    $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
    # PS 5.1：ErrorActionPreference=Stop 会把 clang 的 stderr 变成 terminating
    # error，导致拿不到诊断文本。检查期间临时放宽，只认退出码。
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = @(& $Clang @clangArgs $f.FullName 2>&1)
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    if ($exitCode -ne 0) {
        $failed++
        Write-Host ("[FAIL] {0}" -f $rel) -ForegroundColor Red
        foreach ($line in $output) {
            Write-Host ("       {0}" -f $line) -ForegroundColor DarkRed
        }
    } else {
        Write-Host ("[ ok ] {0}" -f $rel) -ForegroundColor DarkGreen
    }
}

if ($failed -ne 0) {
    Write-Host ("sdk-check: FAIL ({0}/{1} files)" -f $failed, $n) -ForegroundColor Red
    exit 1
}

Write-Host ("sdk-check: PASS ({0} files)" -f $n) -ForegroundColor Green
exit 0
