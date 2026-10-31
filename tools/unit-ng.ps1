# =============================================================================
# unit-ng.ps1 - ng WP1-6 host 单测（64 位化）
#
# 把 PMM / heap / memmap / 调度器 / IPC 五组“纯逻辑”源码用本机 clang 编译成
# Windows PE 可执行文件并运行：
#   - pmm.c    : bitmap PMM（fake memory = 8 MiB 静态缓冲区）
#   - heap.c   : first-fit + 合并堆（NG_HOST_TEST 下用 fake 页分配器）
#   - memmap.c : PVH / multiboot2 内存图解析（合成结构）
#   - sched.c  : runqueue / BLOCKED / 定时唤醒 / zombie 回收（fake 时钟 +
#                fake 栈 + 不切栈的 fake switch，见 test_sched.c）
#   - handle.c : 句柄表（slot + generation / 类型 / 表满），见 test_ipc.c
#   - ipc_core.c: 端口队列 + 等待队列 + 超时/close 唤醒（fake ipc_env），
#                见 test_ipc.c
# 这些模块不 include 串口/端口 IO，所以能脱离内核直接跑。
#
# 输出必须包含 `N checks, M failures`；M=0 且 N>=20 才算 PASS。
#
# usage: powershell -File tools/unit-ng.ps1 [-Repo <root>] [-ObjDir obj-ng/unit]
# 退出码：0 = PASS；1 = FAIL（编译/运行失败、有 failure、check 数不足）。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$ObjDir = 'obj-ng/unit'
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path

$outDir = Join-Path $Repo $ObjDir
$exe = Join-Path $outDir 'unit-ng.exe'

function Fail([string]$msg) {
    Write-Host ("unit-ng: FAIL ({0})" -f $msg)
    exit 1
}

# ---- 本机 clang（Windows PE，能直接跑）-------------------------------------
$mingw = 'D:\msys64\mingw64\bin'
$clangCmd = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clangCmd -and (Test-Path $mingw)) {
    $env:PATH = $mingw + ';' + $env:PATH
    $clangCmd = Get-Command clang -ErrorAction SilentlyContinue
}
if (-not $clangCmd) { Fail 'clang not found; add D:\msys64\mingw64\bin to PATH' }
$clang = $clangCmd.Source

Write-Host '=== unit-ng (WP1-6 host tests) ==='
Write-Host ("clang: {0}" -f $clang)

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (Test-Path $exe) { Remove-Item -LiteralPath $exe -Force }

$sources = @(
    'src/ng/pmm.c',
    'src/ng/memmap.c',
    'src/ng/heap.c',
    'src/ng/sched.c',
    'src/ng/handle.c',
    'src/ng/ipc_core.c',
    'tests/unit-ng/unit_main.c',
    'tests/unit-ng/test_pmm.c',
    'tests/unit-ng/test_heap.c',
    'tests/unit-ng/test_memmap.c',
    'tests/unit-ng/test_sched.c',
    'tests/unit-ng/test_ipc.c'
)
$srcPaths = @()
foreach ($s in $sources) { $srcPaths += (Join-Path $Repo $s) }

$ccArgs = @(
    '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-DNG_HOST_TEST=1',
    '-I', (Join-Path $Repo 'src/ng'),
    '-I', (Join-Path $Repo 'tests/unit-ng'),
    '-o', $exe
) + $srcPaths

& $clang @ccArgs 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'host compile failed' }

# ---- 运行并解析 `N checks, M failures` -------------------------------------
$out = & $exe 2>&1 | Out-String
$runCode = $LASTEXITCODE
if ($out -and $out.Trim()) { Write-Host $out.TrimEnd() }

$checks = -1
$failures = -1
if ($out -match '(?m)^(\d+) checks, (\d+) failures\s*$') {
    $checks = [int]$Matches[1]
    $failures = [int]$Matches[2]
} else {
    Fail 'output does not contain "N checks, M failures"'
}
if ($runCode -ne 0) { Fail ("test executable exited {0}" -f $runCode) }
if ($failures -ne 0) { Fail ("{0} failure(s)" -f $failures) }
if ($checks -lt 20) { Fail ("only {0} checks, need at least 20" -f $checks) }

Write-Host ("unit-ng: PASS ({0} checks, 0 failures)" -f $checks)
exit 0
