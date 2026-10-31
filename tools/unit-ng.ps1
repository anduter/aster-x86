# =============================================================================
# unit-ng.ps1 - ng WP1-8 host 单测（64 位化）
#
# 把 PMM / heap / memmap / 调度器 / IPC / ELF64 / uaccess / syscall 八组
# “纯逻辑”源码用本机 clang 编译成本机可执行文件并运行（Windows PE 或 Linux
# ELF，取决于宿主）：
#   - pmm.c    : bitmap PMM（fake memory = 8 MiB 静态缓冲区）
#   - heap.c   : first-fit + 合并堆（NG_HOST_TEST 下用 fake 页分配器）
#   - memmap.c : PVH / multiboot2 内存图解析（合成结构）
#   - sched.c  : runqueue / BLOCKED / 定时唤醒 / zombie 回收（fake 时钟 +
#                fake 栈 + 不切栈的 fake switch，见 test_sched.c）
#   - handle.c : 句柄表（slot + generation / 类型 / 表满），见 test_ipc.c
#   - ipc_core.c: 端口队列 + 等待队列 + 超时/close 唤醒（fake ipc_env），
#                见 test_ipc.c
#   - elf.c    : ELF64 头/段校验 + 逐段加载/回滚（fake 页池，test_elf.c）
#   - uaccess.c: copyin/copyout/copyin_str 的页级校验与页边界（fake 页表，
#                test_uaccess.c；拷贝真的读写宿主内存）
#   - syscall.c: 调用号分发 + ring3 执行入口 + per-thread 内核栈
#                （fake 串口/页表/GDT/IPC 胶水 + fake ring3 入口，
#                  test_syscall.c；fake_ng_env.c 提供这些替身）
#   - src/services/fs/vfs.c + ramfs.c : VFS 路径/挂载/节点/fd/权限 + ramfs
#                页链（tests/unit-ng/test_vfs.c；fake 页池 8 页）
#   - src/ng/fs_syscall.c : 20..27 的 syscall 胶水（路径 copyin_str、缓冲
#                copyin/copyout，见 test_syscall.c 的 FS 分发用例）
#   - src/drivers/{pci,video,input} : WP2-3 的 L1D 驱动（PCI 地址/BAR、Bochs VBE
#                寄存器、8x16 字体、帧缓冲绘制/读回、VGA 文本、PS/2 scancode/
#                环形缓冲），端口 IO 全部注入 fake ops（tests/unit-ng/test_drivers.c）
#   - src/drivers/block/ata.c + src/ng/ata_blk.c : WP2-4 的 ATA PIO 驱动与
#                blk_env 适配（寄存器/LBA/BSY-DRQ-ERR-超时状态机、FAT16 在
#                fake ATA 盘上读写，tests/unit-ng/test_ata.c）
#   - src/services/auth/{sha256,auth}.c + log/log.c + pkg/pkg.c +
#                src/ng/svc_syscall.c : WP2-5 的 SHA-256/登录/64KB log 环/
#                包清单校验与 31..35 胶水（test_auth/test_log/test_pkg +
#                test_syscall 的 SVC 分发用例）
#   - src/apps/shell/shell.c : WP2-6 的用户态 shell（命令 tokenizer/重定向、
#                cwd 解析、行编辑、15 个内建命令、错误分支；-DNG_HOST_TEST 把
#                sh_sys() 换成 test_shell.c 的 sh_host_syscall()，后端直接跑
#                真实的 VFS/ramfs + auth/log/pkg）
#   - tests/unit-ng/test_parity.c : WP2-7 parity 100 项里可 host 化的 95 项。
#                与内核共用 tests/parity/parity_cases.h（编号/case 表）和
#                parity_body.inc（断言体）；ATA 走 fake 控制器，显示/输入 5 项
#                需要 QEMU，由内核自检覆盖。
# 这些模块不 include 串口/端口 IO，所以能脱离内核直接跑。
#
# 输出必须包含 `N checks, M failures`；M=0 且 N>=20 才算 PASS。
#
# usage: powershell -File tools/unit-ng.ps1 [-Repo <root>] [-ObjDir obj-ng/unit]
#        pwsh -File tools/ci-ng.ps1 -Quick     # CI 里由 ci-ng.ps1 调用
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
if (-not $clangCmd) { Fail 'clang not found; install clang or add D:\msys64\mingw64\bin to PATH' }
$clang = $clangCmd.Source

Write-Host '=== unit-ng (WP1-8 host tests) ==='
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
    'src/ng/elf.c',
    'src/ng/uaccess.c',
    'src/ng/syscall.c',
    'src/ng/fs_syscall.c',
    'src/ng/svc_syscall.c',
    'src/services/fs/vfs.c',
    'src/services/fs/ramfs.c',
    'src/services/fs/fat16.c',
    'src/services/auth/sha256.c',
    'src/services/auth/auth.c',
    'src/services/log/log.c',
    'src/services/pkg/pkg.c',
    'src/ng/ata_blk.c',
    'src/drivers/block/ata.c',
    'src/drivers/pci/pci.c',
    'src/drivers/video/vbe.c',
    'src/drivers/video/fb.c',
    'src/drivers/video/font8x16.c',
    'src/drivers/video/vga_text.c',
    'src/drivers/input/ps2.c',
    'src/apps/shell/shell.c',
    'tests/unit-ng/fake_ng_env.c',
    'tests/unit-ng/unit_main.c',
    'tests/unit-ng/test_pmm.c',
    'tests/unit-ng/test_heap.c',
    'tests/unit-ng/test_memmap.c',
    'tests/unit-ng/test_sched.c',
    'tests/unit-ng/test_ipc.c',
    'tests/unit-ng/test_elf.c',
    'tests/unit-ng/test_uaccess.c',
    'tests/unit-ng/test_syscall.c',
    'tests/unit-ng/test_vfs.c',
    'tests/unit-ng/test_fat16.c',
    'tests/unit-ng/test_ata.c',
    'tests/unit-ng/test_drivers.c',
    'tests/unit-ng/test_auth.c',
    'tests/unit-ng/test_log.c',
    'tests/unit-ng/test_pkg.c',
    'tests/unit-ng/test_shell.c',
    'tests/unit-ng/test_parity.c'
)
$srcPaths = @()
foreach ($s in $sources) { $srcPaths += (Join-Path $Repo $s) }

$ccArgs = @(
    '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-DNG_HOST_TEST=1',
    '-I', (Join-Path $Repo 'src/ng'),
    '-I', (Join-Path $Repo 'src/services/fs/include'),
    '-I', (Join-Path $Repo 'src/services/auth/include'),
    '-I', (Join-Path $Repo 'src/services/log/include'),
    '-I', (Join-Path $Repo 'src/services/pkg/include'),
    '-I', (Join-Path $Repo 'src/drivers/include'),
    '-I', (Join-Path $Repo 'tests/unit-ng'),
    '-I', (Join-Path $Repo 'tests/parity'),
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
