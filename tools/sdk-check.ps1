# =============================================================================
# sdk-check.ps1 - Aster SDK alpha 语法 + 宿主单测门禁（WP3-4）
#
# 两步：
#   1. 对 sdk/include/aster/*.h 与 sdk/examples/**/*.c 逐个跑
#      clang -fsyntax-only -std=c11 -ffreestanding -fno-builtin -Wall -Wextra
#            -Werror -I sdk/include <file>
#      （header-only + 示例不需要链接/运行；抓类型、结构布局、内联汇编约束）
#   2. 用**宿主** clang 编译并运行 tests/unit-ng/test_sdk.c（纯函数/线格式，
#      不跑 QEMU、不碰内核），要求输出 `sdk-test: N checks, 0 failures` 且
#      N >= 30。它覆盖 display 协议 v3（CREATE/LIST/FOCUS token、v1/v2 解码）
#      与 session 协议/argv 解析。
#
# 为什么宿主单测放在 sdk-check 而不是 tools/unit-ng.ps1：WP3-5（S1 故障注入）
# 并行会话拥有 tests/unit-ng/unit_main.c 与 tools/unit-ng.ps1；WP3-4 的 SDK
# 测试独立成文件、独立运行，避免同文件冲突。
#
# 退出码：0 = 全部通过（打印 `sdk-check: PASS (...)`）；
#         1 = 有文件/单测失败 / 找不到 clang / 目录结构不对。
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
$testSrc = Join-Path $root 'tests/unit-ng/test_sdk.c'

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

# ---- 1. 语法门禁 -----------------------------------------------------------
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

# ---- 2. 宿主单测（tests/unit-ng/test_sdk.c） ------------------------------
if (-not (Test-Path -LiteralPath $testSrc)) {
    Write-Host ("sdk-check: FAIL (missing host test {0})" -f $testSrc) -ForegroundColor Red
    exit 1
}
$tmpExe = Join-Path ([IO.Path]::GetTempPath()) ("aster-sdk-test-{0}.exe" -f $PID)
Remove-Item -LiteralPath $tmpExe -Force -ErrorAction SilentlyContinue
$testArgs = @(
    '-std=c11',
    '-O1',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-I', $incDir,
    $testSrc,
    '-o', $tmpExe
)
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$testBuildOut = @(& $Clang @testArgs 2>&1)
$testBuildCode = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($testBuildCode -ne 0) {
    Write-Host '[FAIL] tests/unit-ng/test_sdk.c (compile)' -ForegroundColor Red
    foreach ($line in $testBuildOut) {
        Write-Host ("       {0}" -f $line) -ForegroundColor DarkRed
    }
    Write-Host 'sdk-check: FAIL (host test compile)' -ForegroundColor Red
    exit 1
}

$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$testRunOut = @(& $tmpExe 2>&1)
$testRunCode = $LASTEXITCODE
$ErrorActionPreference = $prevEap
Remove-Item -LiteralPath $tmpExe -Force -ErrorAction SilentlyContinue
foreach ($line in $testRunOut) {
    Write-Host ("       {0}" -f $line)
}
if ($testRunCode -ne 0) {
    Write-Host 'sdk-check: FAIL (host test run)' -ForegroundColor Red
    exit 1
}
$checks = 0
foreach ($line in $testRunOut) {
    $m = [regex]::Match([string]$line, 'sdk-test:\s*(\d+)\s+checks,\s*0\s+failures')
    if ($m.Success) { $checks = [int]$m.Groups[1].Value }
}
if ($checks -lt 30) {
    Write-Host ("sdk-check: FAIL (SDK host checks = {0}, want >= 30)" -f $checks) -ForegroundColor Red
    exit 1
}

Write-Host ("sdk-check: PASS ({0} files + {1} SDK host checks)" -f $n, $checks) -ForegroundColor Green
exit 0
