# =============================================================================
# build-ng.ps1 - ng WP3-1: 构建 64 位内核（ELF64 x86_64，PMM/heap + 页表 +
#               trap/syscall + 线程/调度 + IPC/句柄 + ELF64 加载器 + ring3 hello
#               + VFS/ramfs（L1 服务域）+ FAT16（L1，RAM 盘镜像）+
#               PCI/VBE/帧缓冲控制台 + PS/2 键盘（L1D 设备域）+
#               ATA PIO 块设备 + 真实磁盘镜像上的 FAT16（L1D/L0，WP2-4）+
#               auth/log/pkg（L1，WP2-5）+ 用户态 shell（L3，WP2-6）+ 多进程（WP3-0）
#               + display server / demo（L2a/L3，WP3-1））
#
# 输入：src/ng/entry.S（multiboot2 + PVH 头、32 位入口、引导页表、长模式）、
#       src/ng/isr.S（32 个异常 stub、受控探测、syscall 入口、ring3 往返）、
#       src/ng/serial.c（COM1）、src/ng/memmap.c（PVH/multiboot2 内存图）、
#       src/ng/pmm.c（bitmap PMM）、src/ng/heap.c（first-fit + 合并堆）、
#       src/ng/paging.c（4 级页表/W^X/NX/SMEP/SMAP，页表页来自 PMM）、
#       src/ng/gdt.c（GDT/TSS/IST）、src/ng/trap.c（IDT/异常/panic）、
#       src/ng/syscall.c（STAR/LSTAR/FMASK + ring0/ring3 + IPC/VFS 调用号）、
#       src/ng/mem_selftest.c（内存图/PMM/heap/页表池自检）、
#       src/ng/switch.S + sched_arch.c + sched.c + sched_selftest.c（WP1-5）、
#       src/ng/handle.c + ipc_core.c + ipc.c + ipc_selftest.c（WP1-6）、
#       src/ng/elf.c + useras.c + uaccess.c + elf_selftest.c（WP1-7）、
#       src/ng/fs_syscall.c（20..27 的用户指针胶水）+ src/ng/fs_selftest.c
#       （WP2-1）、src/ng/fat16_selftest.c + src/ng/fat_ramdisk.c（WP2-2）、
#       src/ng/wp23_selftest.c（WP2-3：PCI/VBE/fb/PS2 接线 + 自检）、
#       src/ng/ata_blk.c + src/ng/wp24_selftest.c（WP2-4：ATA -> blk_env 适配
#         + PCI PIIX3/真盘挂载自检）、src/drivers/block/ata.c（L1D：ATA PIO）、
#       src/ng/svc_syscall.c + src/ng/wp25_selftest.c（WP2-5：31..35 胶水 +
#         auth/log/pkg 自检）、src/ng/wp26_selftest.c（WP2-6：加载/运行用户态
#         shell + 会话证据）、src/apps/shell/shell.c（L3：64 位 shell）、
#       src/services/fs/vfs.c + ramfs.c + fat16.c（L1：VFS/ramfs/FAT16）、
#       src/services/auth/{sha256,auth}.c + log/log.c + pkg/pkg.c（L1：
#         SHA-256 / 用户登录 / 64KB log 环 / 包清单与校验）、
#       src/drivers/pci/pci.c + video/vbe.c + video/fb.c + video/font8x16.c
#         + video/vga_text.c + input/ps2.c（L1D：PCI/VBE/帧缓冲/PS2）、
#       src/ng/main.c（64 位 C 入口）、src/ng/link.ld。
# 用户程序：src/ng/user/hello.S 用 src/ng/user/user.ld 链到 0x40000000，
#           生成 obj-ng/hello.elf，再用 .incbin 嵌进内核 .rodata
#           （ng_user_elf_start..end），内核 ELF64 加载器从内存镜像读取。
#           WP2-6 的 shell（src/apps/shell/shell.c）同样链到 0x40000000，
#           生成 obj-ng/shell.elf 并以 ng_shell_elf_start..end 嵌入；默认启动
#           在 WP1-7 ring3 hello 之后加载 shell，`-Smoke` 编出的内核跳过它。
# FAT16 镜像：tools/mkfat-ng.ps1 生成 obj-ng/fat16.img（1.44MB），同样用
#           .incbin 嵌进 .rodata（ng_fat16_img_start..end），启动时由
#           src/ng/fat_ramdisk.c 复制到 PMM RAM 盘再挂到 /mnt/fat。
# WP2-4 磁盘：tools/mkfat-ng.ps1 -SizeMB 8 生成 dist-ng/disk.img（8MB FAT16），
#           不嵌入内核；run-ng.ps1 用 QEMU `-drive file=...,if=ide` 挂成
#           PIIX3 IDE 主盘，内核 ATA PIO 直接读写，FAT16 挂到 /mnt/disk。
# 输出：dist-ng/aster-ng.elf，对象文件在 obj-ng/。
#
# 工具链（WP0-4 已验证）：
#   clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
#   -mno-red-zone -fno-stack-protector
#
# 校验：ELF64 / x86_64 / ET_EXEC / 入口地址；PT_LOAD 4 KiB 对齐（按段分页的
#       前提）；multiboot2 头在前 32 KiB 且 8 字节对齐、checksum 为 0；
#       PT_NOTE 里有 Xen PVH 入口（QEMU -kernel 用）；用户 ELF 的
#       ET_EXEC/EM_X86_64/0x40000000 基址/段权限 W^X。
#
# usage: powershell -File tools/build-ng.ps1 [-Repo <root>] [-DistDir dist-ng]
#                                             [-ObjDir obj-ng] [-PanicTest]
#                                             [-Smoke]
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$DistDir = 'dist-ng',
    [string]$ObjDir = 'obj-ng',
    [switch]$PanicTest,
    [switch]$Smoke
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path

$srcDir = Join-Path $Repo 'src/ng'
$distDir = Join-Path $Repo $DistDir
$objDir = Join-Path $Repo $ObjDir
$elf = Join-Path $distDir 'aster-ng.elf'

function Fail([string]$msg) {
    Write-Host ("build-ng: FAIL ({0})" -f $msg)
    exit 1
}

# ---- 工具链 ----------------------------------------------------------------
$mingw = 'D:\msys64\mingw64\bin'
$clangCmd = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clangCmd -and (Test-Path $mingw)) {
    $env:PATH = $mingw + ';' + $env:PATH
    $clangCmd = Get-Command clang -ErrorAction SilentlyContinue
}
if (-not $clangCmd) { Fail 'clang not found; add D:\msys64\mingw64\bin to PATH' }
$clang = $clangCmd.Source

$lldCmd = Get-Command ld.lld -ErrorAction SilentlyContinue
if (-not $lldCmd -and (Test-Path $mingw)) {
    $env:PATH = $mingw + ';' + $env:PATH
    $lldCmd = Get-Command ld.lld -ErrorAction SilentlyContinue
}
if (-not $lldCmd) { Fail 'ld.lld not found; install lld (WP0-4) or add its dir to PATH' }

Write-Host '=== build-ng (WP3-3) ==='
Write-Host ("clang: {0}" -f $clang)
Write-Host ("lld:   {0}" -f $lldCmd.Source)
if ($PanicTest) { Write-Host 'mode:  panic-test build (-DNG_PANIC_TEST=1)' }
if ($Smoke) { Write-Host 'mode:  smoke build (-DNG_SMOKE_MODE=1: 跳过 WP2-6 shell，只跑 ring3 hello)' }

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
New-Item -ItemType Directory -Force -Path $objDir | Out-Null
if (Test-Path $elf) { Remove-Item -LiteralPath $elf -Force }

$target = '--target=x86_64-elf'
$common = @($target, '-ffreestanding', '-nostdlib', '-mno-red-zone',
    '-fno-stack-protector', '-fcf-protection=none')
$asmFlags = $common + @('-c')
$cFlags = $common + @('-mno-sse', '-mgeneral-regs-only', '-fno-builtin',
    '-fno-pic', '-fno-pie', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-I', $srcDir,
    '-I', (Join-Path $Repo 'src/services/fs/include'),
    '-I', (Join-Path $Repo 'src/services/auth/include'),
    '-I', (Join-Path $Repo 'src/services/log/include'),
    '-I', (Join-Path $Repo 'src/services/pkg/include'),
    '-I', (Join-Path $Repo 'src/drivers/include'),
    '-I', (Join-Path $Repo 'src/desktop/include'),
    '-I', (Join-Path $Repo 'tests/parity'),
    '-c')
if ($PanicTest) { $cFlags += '-DNG_PANIC_TEST=1' }
if ($Smoke) { $cFlags += '-DNG_SMOKE_MODE=1' }

# 用户程序（src/apps/shell/shell.c）的编译选项：ring3、无 libc、无 SSE。
# WP3-1：displayd/demo 用 L2 的纯逻辑模块（src/desktop/{include,src}），所以
# 用户程序也要有 src/ng（types.h）与 src/desktop/include 两个 include 路径。
$userCFlags = @($target, '-ffreestanding', '-nostdlib', '-mno-red-zone',
    '-fno-stack-protector', '-fcf-protection=none', '-mno-sse',
    '-mgeneral-regs-only', '-fno-builtin', '-fno-pic', '-fno-pie', '-std=c11',
    '-Os', '-Wall', '-Wextra', '-Werror',
    '-ffunction-sections', '-fdata-sections',
    '-I', $srcDir,
    '-I', (Join-Path $Repo 'src/desktop/include'),
    '-c')

# ---- WP1-7：用户程序 hello.S -> hello.elf（0x40000000）-> .incbin 嵌内核 ----
# 用户 ELF 不是最终交付物，但它是"内核 ELF64 加载器"的真实输入：先按 0x40000000
# 链接，再把整个文件嵌进内核 .rodata（ng_user_elf_start..end），避免依赖 FS。
Write-Host ''
Write-Host '=== user-elf (WP1-7) ==='
$userDir = Join-Path $srcDir 'user'
$userSrc = Join-Path $userDir 'hello.S'
$userLd = Join-Path $userDir 'user.ld'
$userObj = Join-Path $objDir 'user_hello.o'
$userElf = Join-Path $objDir 'hello.elf'
$userBlobObj = Join-Path $objDir 'user_elf_blob.o'
$userBlobAsm = Join-Path $objDir 'user_elf_blob.S'
foreach ($p in @($userSrc, $userLd)) {
    if (-not (Test-Path -LiteralPath $p)) { Fail ("missing user source: {0}" -f $p) }
}
Write-Host ("  hello.S -> {0}" -f 'obj-ng/user_hello.o')
& $clang @($asmFlags + @($userSrc, '-o', $userObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'user hello.S compile failed' }
Write-Host ("  link    -> {0}" -f 'obj-ng/hello.elf (base 0x40000000)')
$userLinkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-Wl,--gc-sections', '-o', $userElf, $userObj)
& $clang @userLinkArgs 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'user hello.elf link failed' }

# ---- 校验用户 ELF：ELF64/ET_EXEC/EM_X86_64、入口与段权限 ----
$ub = [IO.File]::ReadAllBytes($userElf)
$uProblems = New-Object System.Collections.Generic.List[string]
$uLoadCount = 0
$uLoadEnd = 0
if ($ub.Length -lt 64) { $uProblems.Add('user ELF smaller than ELF64 header') }
if ($uProblems.Count -eq 0) {
    if (-not ($ub[0] -eq 0x7F -and $ub[1] -eq 0x45 -and $ub[2] -eq 0x4C -and $ub[3] -eq 0x46)) {
        $uProblems.Add('user ELF bad magic')
    }
    if ($ub[4] -ne 2) { $uProblems.Add(("user ELF class={0}, want 2" -f $ub[4])) }
    if ($ub[5] -ne 1) { $uProblems.Add(("user ELF data={0}, want 1 (LSB)" -f $ub[5])) }
    $uType = [BitConverter]::ToUInt16($ub, 16)
    $uMachine = [BitConverter]::ToUInt16($ub, 18)
    $uEntry = [BitConverter]::ToUInt64($ub, 24)
    $uPhOff = [BitConverter]::ToUInt64($ub, 32)
    $uPhEnt = [BitConverter]::ToUInt16($ub, 54)
    $uPhNum = [BitConverter]::ToUInt16($ub, 56)
    if ($uType -ne 2) { $uProblems.Add(("user ELF e_type={0}, want 2 (ET_EXEC)" -f $uType)) }
    if ($uMachine -ne 62) { $uProblems.Add(("user ELF e_machine={0}, want 62" -f $uMachine)) }
    if ($uPhEnt -ne 56) { $uProblems.Add(("user ELF e_phentsize={0}, want 56" -f $uPhEnt)) }
    if ($uPhNum -lt 1) { $uProblems.Add('user ELF has no program headers') }
    if ($uEntry -lt 0x40000000 -or $uEntry -ge 0x40010000) {
        $uProblems.Add(("user ELF e_entry=0x{0:x} outside 0x40000000..0x40010000" -f $uEntry))
    }
    for ($i = 0; $i -lt $uPhNum; $i++) {
        $ph = [int]$uPhOff + ($i * $uPhEnt)
        if ($ph + 56 -gt $ub.Length) { $uProblems.Add('user ELF phdr outside file'); break }
        if ([BitConverter]::ToUInt32($ub, $ph) -ne 1) { continue }  # PT_LOAD
        $uLoadCount++
        $pFlags = [BitConverter]::ToUInt32($ub, $ph + 4)
        $pVaddr = [BitConverter]::ToUInt64($ub, $ph + 16)
        $pFilesz = [BitConverter]::ToUInt64($ub, $ph + 32)
        $pMemsz = [BitConverter]::ToUInt64($ub, $ph + 40)
        $pAlign = [BitConverter]::ToUInt64($ub, $ph + 48)
        if (($pVaddr % 4096) -ne 0) { $uProblems.Add(("user PT_LOAD vaddr=0x{0:x} not 4 KiB aligned" -f $pVaddr)) }
        if ($pAlign -lt 4096) { $uProblems.Add(("user PT_LOAD align=0x{0:x} < 4096" -f $pAlign)) }
        if ($pFilesz -gt $pMemsz) { $uProblems.Add('user PT_LOAD filesz > memsz') }
        if (($pFlags -band 3) -eq 3) { $uProblems.Add(("user PT_LOAD 0x{0:x} is W+X (W^X violated)" -f $pVaddr)) }
        if ($pVaddr -lt 0x40000000) { $uProblems.Add(("user PT_LOAD vaddr=0x{0:x} below base" -f $pVaddr)) }
        if (($pVaddr + $pMemsz) -gt 0x40010000) {
            $uProblems.Add(("user PT_LOAD ends 0x{0:x} above 0x40010000" -f ($pVaddr + $pMemsz)))
        }
        if (($pVaddr + $pMemsz) -gt $uLoadEnd) { $uLoadEnd = $pVaddr + $pMemsz }
    }
    if ($uLoadCount -lt 3) {
        $uProblems.Add(("user PT_LOAD count {0} < 3 (text/rodata/data+bss)" -f $uLoadCount))
    }
}
if ($uProblems.Count -gt 0) {
    foreach ($p in $uProblems) { Write-Host ("  ! {0}" -f $p) }
    Fail 'user ELF verification failed'
}

# ---- 生成 .incbin 汇编并编译成内核对象 ----
$incPath = ($userElf -replace '\\', '/')
$blobText = @"
/* generated by tools/build-ng.ps1 (WP1-7): embed user ELF into kernel .bigdata */
    .section .bigdata, "a"
    .align 16
    .global ng_user_elf_start
ng_user_elf_start:
    .incbin "$incPath"
    .global ng_user_elf_end
ng_user_elf_end:
    .align 16
"@
[IO.File]::WriteAllText($userBlobAsm, $blobText, (New-Object Text.UTF8Encoding $false))
& $clang @($asmFlags + @($userBlobAsm, '-o', $userBlobObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'user ELF embed compile failed' }
Write-Host ("user-elf: PASS ({0} bytes, {1} PT_LOAD, entry 0x{2:x}, end 0x{3:x})" -f $ub.Length, $uLoadCount, $uEntry, $uLoadEnd)
Write-Host ''

# ---- WP2-6：用户态 shell（src/apps/shell/shell.c）-> ELF64 -> .incbin ------
# shell 是独立 ELF64（L3 应用），链接到 0x40000000；内核用 WP1-7 的加载器
# 从嵌入镜像加载。校验项与 hello 相同：ELF64/ET_EXEC/EM_X86_64、段 4 KiB
# 对齐且 W^X、入口在 [0x40000000,0x40010000)、总大小不超过 64 KiB 窗口。
Write-Host '=== user-shell (WP2-6) ==='
$shellSrc = Join-Path $Repo 'src/apps/shell/shell.c'
$shellObj = Join-Path $objDir 'user_shell.o'
$shellElf = Join-Path $objDir 'shell.elf'
$shellBlobObj = Join-Path $objDir 'shell_elf_blob.o'
$shellBlobAsm = Join-Path $objDir 'shell_elf_blob.S'
if (-not (Test-Path -LiteralPath $shellSrc)) { Fail ("missing shell source: {0}" -f $shellSrc) }
Write-Host ("  shell.c -> {0}" -f 'obj-ng/user_shell.o')
& $clang @($userCFlags + @($shellSrc, '-o', $shellObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'user shell.c compile failed' }
Write-Host ("  link    -> {0}" -f 'obj-ng/shell.elf (base 0x40000000)')
$shellLinkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-Wl,--gc-sections', '-o', $shellElf, $shellObj)
& $clang @shellLinkArgs 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'user shell.elf link failed' }

$sb = [IO.File]::ReadAllBytes($shellElf)
$sProblems = New-Object System.Collections.Generic.List[string]
$sLoadCount = 0
$sLoadEnd = 0
$sEntry = 0
if ($sb.Length -lt 64) {
    $sProblems.Add('shell ELF smaller than ELF64 header')
} elseif (-not ($sb[0] -eq 0x7F -and $sb[1] -eq 0x45 -and $sb[2] -eq 0x4C -and $sb[3] -eq 0x46)) {
    $sProblems.Add('shell ELF bad magic')
} else {
    if ($sb[4] -ne 2) { $sProblems.Add(("shell ELF class={0}, want 2" -f $sb[4])) }
    if ($sb[5] -ne 1) { $sProblems.Add(("shell ELF data={0}, want 1 (LSB)" -f $sb[5])) }
    $sType = [BitConverter]::ToUInt16($sb, 16)
    $sMachine = [BitConverter]::ToUInt16($sb, 18)
    $sEntry = [BitConverter]::ToUInt64($sb, 24)
    $sPhOff = [BitConverter]::ToUInt64($sb, 32)
    $sPhEnt = [BitConverter]::ToUInt16($sb, 54)
    $sPhNum = [BitConverter]::ToUInt16($sb, 56)
    if ($sType -ne 2) { $sProblems.Add(("shell ELF e_type={0}, want 2 (ET_EXEC)" -f $sType)) }
    if ($sMachine -ne 62) { $sProblems.Add(("shell ELF e_machine={0}, want 62" -f $sMachine)) }
    if ($sPhEnt -ne 56) { $sProblems.Add(("shell ELF e_phentsize={0}, want 56" -f $sPhEnt)) }
    if ($sPhNum -lt 1) { $sProblems.Add('shell ELF has no program headers') }
    if ($sEntry -lt 0x40000000 -or $sEntry -ge 0x40010000) {
        $sProblems.Add(("shell ELF e_entry=0x{0:x} outside 0x40000000..0x40010000" -f $sEntry))
    }
    for ($i = 0; $i -lt $sPhNum; $i++) {
        $ph = [int]$sPhOff + ($i * $sPhEnt)
        if ($ph + 56 -gt $sb.Length) { $sProblems.Add('shell ELF phdr outside file'); break }
        if ([BitConverter]::ToUInt32($sb, $ph) -ne 1) { continue }  # PT_LOAD
        $sLoadCount++
        $pFlags = [BitConverter]::ToUInt32($sb, $ph + 4)
        $pVaddr = [BitConverter]::ToUInt64($sb, $ph + 16)
        $pFilesz = [BitConverter]::ToUInt64($sb, $ph + 32)
        $pMemsz = [BitConverter]::ToUInt64($sb, $ph + 40)
        $pAlign = [BitConverter]::ToUInt64($sb, $ph + 48)
        if (($pVaddr % 4096) -ne 0) { $sProblems.Add(("shell PT_LOAD vaddr=0x{0:x} not 4 KiB aligned" -f $pVaddr)) }
        if ($pAlign -lt 4096) { $sProblems.Add(("shell PT_LOAD align=0x{0:x} < 4096" -f $pAlign)) }
        if ($pFilesz -gt $pMemsz) { $sProblems.Add('shell PT_LOAD filesz > memsz') }
        if (($pFlags -band 3) -eq 3) { $sProblems.Add(("shell PT_LOAD 0x{0:x} is W+X (W^X violated)" -f $pVaddr)) }
        if ($pVaddr -lt 0x40000000) { $sProblems.Add(("shell PT_LOAD vaddr=0x{0:x} below base" -f $pVaddr)) }
        if (($pVaddr + $pMemsz) -gt 0x40010000) {
            $sProblems.Add(("shell PT_LOAD ends 0x{0:x} above 0x40010000" -f ($pVaddr + $pMemsz)))
        }
        if (($pVaddr + $pMemsz) -gt $sLoadEnd) { $sLoadEnd = $pVaddr + $pMemsz }
    }
    if ($sLoadCount -lt 3) {
        $sProblems.Add(("shell PT_LOAD count {0} < 3 (text/rodata/data+bss)" -f $sLoadCount))
    }
}
if ($sProblems.Count -gt 0) {
    foreach ($p in $sProblems) { Write-Host ("  ! {0}" -f $p) }
    Fail 'user shell ELF verification failed'
}
$shellInc = ($shellElf -replace '\\', '/')
$shellBlobText = @"
/* generated by tools/build-ng.ps1 (WP2-6): embed user shell ELF into .bigdata */
    .section .bigdata, "a"
    .align 16
    .global ng_shell_elf_start
ng_shell_elf_start:
    .incbin "$shellInc"
    .global ng_shell_elf_end
ng_shell_elf_end:
    .align 16
"@
[IO.File]::WriteAllText($shellBlobAsm, $shellBlobText, (New-Object Text.UTF8Encoding $false))
& $clang @($asmFlags + @($shellBlobAsm, '-o', $shellBlobObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'shell ELF embed compile failed' }
Write-Host ("user-shell: PASS ({0} bytes, {1} PT_LOAD, entry 0x{2:x}, end 0x{3:x})" -f $sb.Length, $sLoadCount, $sEntry, $sLoadEnd)
Write-Host ''

# ---- WP3-0：多进程/IPC 用户程序（src/apps/proc/proc.c）-> ELF64 -> .incbin --
# 同一个 ELF 多个角色（hello/chain/relay/server/hold），由 argv 决定；内核
# proc_init 把它种到 ramfs 的 /bin/proc，spawn 再从 VFS 读回来加载。
Write-Host '=== user-proc (WP3-0) ==='
$procSrc = Join-Path $Repo 'src/apps/proc/proc.c'
$procObj = Join-Path $objDir 'user_proc.o'
$procElf = Join-Path $objDir 'proc.elf'
$procBlobObj = Join-Path $objDir 'proc_elf_blob.o'
$procBlobAsm = Join-Path $objDir 'proc_elf_blob.S'
if (-not (Test-Path -LiteralPath $procSrc)) { Fail ("missing proc source: {0}" -f $procSrc) }
Write-Host ("  proc.c  -> {0}" -f 'obj-ng/user_proc.o')
& $clang @($userCFlags + @($procSrc, '-o', $procObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'user proc.c compile failed' }
Write-Host ("  link    -> {0}" -f 'obj-ng/proc.elf (base 0x40000000)')
$procLinkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-Wl,--gc-sections', '-o', $procElf, $procObj)
& $clang @procLinkArgs 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'user proc.elf link failed' }

$pb = [IO.File]::ReadAllBytes($procElf)
$pProblems = New-Object System.Collections.Generic.List[string]
$pLoadCount = 0
$pLoadEnd = 0
$pEntry = 0
if ($pb.Length -lt 64) {
    $pProblems.Add('proc ELF smaller than ELF64 header')
} elseif (-not ($pb[0] -eq 0x7F -and $pb[1] -eq 0x45 -and $pb[2] -eq 0x4C -and $pb[3] -eq 0x46)) {
    $pProblems.Add('proc ELF bad magic')
} else {
    if ($pb[4] -ne 2) { $pProblems.Add(("proc ELF class={0}, want 2" -f $pb[4])) }
    if ($pb[5] -ne 1) { $pProblems.Add(("proc ELF data={0}, want 1 (LSB)" -f $pb[5])) }
    $pType = [BitConverter]::ToUInt16($pb, 16)
    $pMachine = [BitConverter]::ToUInt16($pb, 18)
    $pEntry = [BitConverter]::ToUInt64($pb, 24)
    $pPhOff = [BitConverter]::ToUInt64($pb, 32)
    $pPhEnt = [BitConverter]::ToUInt16($pb, 54)
    $pPhNum = [BitConverter]::ToUInt16($pb, 56)
    if ($pType -ne 2) { $pProblems.Add(("proc ELF e_type={0}, want 2 (ET_EXEC)" -f $pType)) }
    if ($pMachine -ne 62) { $pProblems.Add(("proc ELF e_machine={0}, want 62" -f $pMachine)) }
    if ($pPhEnt -ne 56) { $pProblems.Add(("proc ELF e_phentsize={0}, want 56" -f $pPhEnt)) }
    if ($pPhNum -lt 1) { $pProblems.Add('proc ELF has no program headers') }
    if ($pEntry -lt 0x40000000 -or $pEntry -ge 0x40010000) {
        $pProblems.Add(("proc ELF e_entry=0x{0:x} outside 0x40000000..0x40010000" -f $pEntry))
    }
    for ($i = 0; $i -lt $pPhNum; $i++) {
        $ph = [int]$pPhOff + ($i * $pPhEnt)
        if ($ph + 56 -gt $pb.Length) { $pProblems.Add('proc ELF phdr outside file'); break }
        if ([BitConverter]::ToUInt32($pb, $ph) -ne 1) { continue }  # PT_LOAD
        $pLoadCount++
        $pFlags = [BitConverter]::ToUInt32($pb, $ph + 4)
        $pVaddr = [BitConverter]::ToUInt64($pb, $ph + 16)
        $pFilesz = [BitConverter]::ToUInt64($pb, $ph + 32)
        $pMemsz = [BitConverter]::ToUInt64($pb, $ph + 40)
        $pAlign = [BitConverter]::ToUInt64($pb, $ph + 48)
        if (($pVaddr % 4096) -ne 0) { $pProblems.Add(("proc PT_LOAD vaddr=0x{0:x} not 4 KiB aligned" -f $pVaddr)) }
        if ($pAlign -lt 4096) { $pProblems.Add(("proc PT_LOAD align=0x{0:x} < 4096" -f $pAlign)) }
        if ($pFilesz -gt $pMemsz) { $pProblems.Add('proc PT_LOAD filesz > memsz') }
        # W^X：只要求 text 段（PF_X）可执行，data 段可写但不可执行。
        if (($pFlags -band 3) -eq 3) { $pProblems.Add(("proc PT_LOAD 0x{0:x} is W+X (W^X violated)" -f $pVaddr)) }
        if ($pVaddr -lt 0x40000000) { $pProblems.Add(("proc PT_LOAD vaddr=0x{0:x} below base" -f $pVaddr)) }
        if (($pVaddr + $pMemsz) -gt 0x40010000) {
            $pProblems.Add(("proc PT_LOAD ends 0x{0:x} above 0x40010000" -f ($pVaddr + $pMemsz)))
        }
        if (($pVaddr + $pMemsz) -gt $pLoadEnd) { $pLoadEnd = $pVaddr + $pMemsz }
    }
    # proc.c 很小，text+rodata 可能落在同一个 R+X 段：允许 2 段（W^X 仍强制）。
    if ($pLoadCount -lt 2) {
        $pProblems.Add(("proc PT_LOAD count {0} < 2" -f $pLoadCount))
    }
}
if ($pProblems.Count -gt 0) {
    foreach ($p in $pProblems) { Write-Host ("  ! {0}" -f $p) }
    Fail 'user proc ELF verification failed'
}
$procInc = ($procElf -replace '\\', '/')
$procBlobText = @"
/* generated by tools/build-ng.ps1 (WP3-0): embed user proc ELF into .bigdata */
    .section .bigdata, "a"
    .align 16
    .global ng_proc_elf_start
ng_proc_elf_start:
    .incbin "$procInc"
    .global ng_proc_elf_end
ng_proc_elf_end:
    .align 16
"@
[IO.File]::WriteAllText($procBlobAsm, $procBlobText, (New-Object Text.UTF8Encoding $false))
& $clang @($asmFlags + @($procBlobAsm, '-o', $procBlobObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'proc ELF embed compile failed' }
Write-Host ("user-proc: PASS ({0} bytes, {1} PT_LOAD, entry 0x{2:x}, end 0x{3:x})" -f $pb.Length, $pLoadCount, $pEntry, $pLoadEnd)
Write-Host ''

# ---- WP3-1：displayd（L2a）+ demo（L3）-------------------------------------
# 两个用户 ELF 都链接到 0x40000000，共用 L2 的纯逻辑模块（协议编解码、surface
# 表、合成器、绘制、8x8 字模）。displayd 是 ring3 display server，demo 是它的
# 第一个客户端；内核把两个 ELF 都 .incbin 进 .rodata，proc_init 种到 /bin。
function Test-NgUserElf {
    param([string]$Path, [string]$Label, [int]$MinLoad)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $probs = New-Object System.Collections.Generic.List[string]
    $load = 0
    $end = 0
    $entry = 0
    if ($bytes.Length -lt 64) {
        $probs.Add('smaller than ELF64 header')
    } elseif (-not ($bytes[0] -eq 0x7F -and $bytes[1] -eq 0x45 -and $bytes[2] -eq 0x4C -and $bytes[3] -eq 0x46)) {
        $probs.Add('bad magic')
    } else {
        if ($bytes[4] -ne 2) { $probs.Add(("class={0}, want 2" -f $bytes[4])) }
        if ($bytes[5] -ne 1) { $probs.Add(("data={0}, want 1 (LSB)" -f $bytes[5])) }
        $eType = [BitConverter]::ToUInt16($bytes, 16)
        $eMachine = [BitConverter]::ToUInt16($bytes, 18)
        $entry = [BitConverter]::ToUInt64($bytes, 24)
        $phOff = [BitConverter]::ToUInt64($bytes, 32)
        $phEnt = [BitConverter]::ToUInt16($bytes, 54)
        $phNum = [BitConverter]::ToUInt16($bytes, 56)
        if ($eType -ne 2) { $probs.Add(("e_type={0}, want 2 (ET_EXEC)" -f $eType)) }
        if ($eMachine -ne 62) { $probs.Add(("e_machine={0}, want 62" -f $eMachine)) }
        if ($phEnt -ne 56) { $probs.Add(("e_phentsize={0}, want 56" -f $phEnt)) }
        if ($phNum -lt 1) { $probs.Add('no program headers') }
        if ($entry -lt 0x40000000 -or $entry -ge 0x40010000) {
            $probs.Add(("e_entry=0x{0:x} outside 0x40000000..0x40010000" -f $entry))
        }
        for ($i = 0; $i -lt $phNum; $i++) {
            $ph = [int]$phOff + ($i * $phEnt)
            if ($ph + 56 -gt $bytes.Length) { $probs.Add('phdr outside file'); break }
            if ([BitConverter]::ToUInt32($bytes, $ph) -ne 1) { continue }  # PT_LOAD
            $load++
            $pFlags = [BitConverter]::ToUInt32($bytes, $ph + 4)
            $pVaddr = [BitConverter]::ToUInt64($bytes, $ph + 16)
            $pFilesz = [BitConverter]::ToUInt64($bytes, $ph + 32)
            $pMemsz = [BitConverter]::ToUInt64($bytes, $ph + 40)
            $pAlign = [BitConverter]::ToUInt64($bytes, $ph + 48)
            if (($pVaddr % 4096) -ne 0) { $probs.Add(("PT_LOAD vaddr=0x{0:x} not 4 KiB aligned" -f $pVaddr)) }
            if ($pAlign -lt 4096) { $probs.Add(("PT_LOAD align=0x{0:x} < 4096" -f $pAlign)) }
            if ($pFilesz -gt $pMemsz) { $probs.Add('PT_LOAD filesz > memsz') }
            if (($pFlags -band 3) -eq 3) { $probs.Add(("PT_LOAD 0x{0:x} is W+X (W^X violated)" -f $pVaddr)) }
            if ($pVaddr -lt 0x40000000) { $probs.Add(("PT_LOAD vaddr=0x{0:x} below base" -f $pVaddr)) }
            if (($pVaddr + $pMemsz) -gt 0x40010000) {
                $probs.Add(("PT_LOAD ends 0x{0:x} above 0x40010000" -f ($pVaddr + $pMemsz)))
            }
            if (($pVaddr + $pMemsz) -gt $end) { $end = $pVaddr + $pMemsz }
        }
        if ($load -lt $MinLoad) {
            $probs.Add(("PT_LOAD count {0} < {1}" -f $load, $MinLoad))
        }
    }
    if ($probs.Count -gt 0) {
        foreach ($p in $probs) { Write-Host ("  ! {0}" -f $p) }
        Fail ("user {0} ELF verification failed" -f $Label)
    }
    Write-Host ("user-{0}: PASS ({1} bytes, {2} PT_LOAD, entry 0x{3:x}, end 0x{4:x})" -f `
        $Label, $bytes.Length, $load, $entry, $end)
    return @{ Bytes = $bytes; LoadCount = $load; Entry = $entry; End = $end }
}

function New-NgUserBlob {
    param([string]$ElfPath, [string]$SymbolPrefix, [string]$ObjPath, [string]$AsmPath, [string]$Label)
    $inc = ($ElfPath -replace '\\', '/')
    $text = @"
/* generated by tools/build-ng.ps1 (WP3-1): embed $Label ELF into .bigdata */
    .section .bigdata, "a"
    .align 16
    .global ${SymbolPrefix}_start
${SymbolPrefix}_start:
    .incbin "$inc"
    .global ${SymbolPrefix}_end
${SymbolPrefix}_end:
    .align 16
"@
    [IO.File]::WriteAllText($AsmPath, $text, (New-Object Text.UTF8Encoding $false))
    & $clang @($asmFlags + @($AsmPath, '-o', $ObjPath)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0} ELF embed compile failed" -f $Label) }
}

Write-Host '=== user-desktop (WP3-3) ==='
$dsInclude = Join-Path $Repo 'src/desktop/include'
$dsSrcDir = Join-Path $Repo 'src/desktop/src'
$dsUserSources = @('ds_protocol.c', 'ds_surface.c', 'ds_composite.c',
    'ds_blend.c', 'ds_font8x8.c', 'ds_draw.c', 'ds_wm.c',
    'session_protocol.c', 'session_table.c')
$dsUserObjs = New-Object System.Collections.Generic.List[string]
$dsObjByName = @{}
foreach ($name in $dsUserSources) {
    $src = Join-Path $dsSrcDir $name
    if (-not (Test-Path -LiteralPath $src)) { Fail ("missing desktop source: {0}" -f $src) }
    $leaf = [IO.Path]::GetFileNameWithoutExtension($name)
    $obj = Join-Path $objDir ('ds_' + $leaf + '.o')
    Write-Host ("  {0} -> {1}" -f $name, ('obj-ng/' + [IO.Path]::GetFileName($obj)))
    & $clang @($userCFlags + @($src, '-o', $obj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0} compile failed" -f $name) }
    $dsUserObjs.Add($obj)
    $dsObjByName[$leaf] = $obj
}

# 每个程序只链自己用到的 L2 模块（内核 .rodata 里嵌入 6 个用户 ELF，
# 2 MiB 映像窗口很紧：displayd 不需要 ds_draw/ds_font8x8/ds_wm，
# demo/wm 不需要 ds_surface/ds_composite）。
$desktopPrograms = @(
    @{ Name = 'displayd'; Src = (Join-Path $Repo 'src/desktop/display/displayd.c');
       Obj = (Join-Path $objDir 'user_displayd.o'); Elf = (Join-Path $objDir 'displayd.elf');
       Sym = 'ng_displayd_elf'; Label = 'displayd';
       Libs = @('ds_protocol', 'ds_surface', 'ds_composite', 'ds_blend',
                'session_protocol') },
    @{ Name = 'demo'; Src = (Join-Path $Repo 'src/apps/demo/demo.c');
       Obj = (Join-Path $objDir 'user_demo.o'); Elf = (Join-Path $objDir 'demo.elf');
       Sym = 'ng_demo_elf'; Label = 'demo';
       Libs = @('ds_protocol', 'session_protocol', 'ds_blend', 'ds_font8x8',
                'ds_draw') },
    @{ Name = 'wm'; Src = (Join-Path $Repo 'src/desktop/shell/wm.c');
       Obj = (Join-Path $objDir 'user_wm.o'); Elf = (Join-Path $objDir 'wm.elf');
       Sym = 'ng_wm_elf'; Label = 'wm';
       Libs = @('ds_protocol', 'session_protocol', 'ds_blend', 'ds_font8x8',
                'ds_draw', 'ds_wm') },
    @{ Name = 'sessiond'; Src = (Join-Path $Repo 'src/desktop/session/sessiond.c');
       Obj = (Join-Path $objDir 'user_sessiond.o'); Elf = (Join-Path $objDir 'sessiond.elf');
       Sym = 'ng_sessiond_elf'; Label = 'sessiond';
       Libs = @('ds_protocol', 'session_protocol', 'session_table') }
)
$desktopBlobObjs = New-Object System.Collections.Generic.List[string]
foreach ($prog in $desktopPrograms) {
    if (-not (Test-Path -LiteralPath $prog.Src)) { Fail ("missing desktop program: {0}" -f $prog.Src) }
    Write-Host ("  {0}.c -> {1}" -f $prog.Name, ('obj-ng/' + [IO.Path]::GetFileName($prog.Obj)))
    & $clang @($userCFlags + @($prog.Src, '-o', $prog.Obj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0}.c compile failed" -f $prog.Name) }
    Write-Host ("  link    -> {0} (base 0x40000000)" -f ('obj-ng/' + [IO.Path]::GetFileName($prog.Elf)))
    $progLibObjs = @()
    foreach ($lib in $prog.Libs) {
        if (-not $dsObjByName.ContainsKey($lib)) { Fail ("missing desktop object: {0}" -f $lib) }
        $progLibObjs += $dsObjByName[$lib]
    }
    $linkUserArgs = @($target, '-fuse-ld=lld', '-nostdlib',
        ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
        '-Wl,--gc-sections',
        '-o', $prog.Elf, $prog.Obj) + $progLibObjs
    & $clang @linkUserArgs 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0}.elf link failed" -f $prog.Name) }
    $info = Test-NgUserElf -Path $prog.Elf -Label $prog.Label -MinLoad 2
    $blobObj = Join-Path $objDir ($prog.Name + '_elf_blob.o')
    $blobAsm = Join-Path $objDir ($prog.Name + '_elf_blob.S')
    New-NgUserBlob -ElfPath $prog.Elf -SymbolPrefix $prog.Sym -ObjPath $blobObj -AsmPath $blobAsm -Label $prog.Label
    $desktopBlobObjs.Add($blobObj)
}
Write-Host ''

# ---- WP2-2：FAT16 测试镜像 -> .incbin 嵌内核 .rodata ----------------------
Write-Host '=== fat16 image (WP2-2) ==='
$fatImg = Join-Path $objDir 'fat16.img'
$mkfat = Join-Path $PSScriptRoot 'mkfat-ng.ps1'
$psHost = $null
try { $psHost = (Get-Process -Id $PID).Path } catch { }
if (-not $psHost) { $psHost = 'powershell' }
$mkArgs = @('-NoProfile')
if ($env:OS -eq 'Windows_NT') { $mkArgs += @('-ExecutionPolicy', 'Bypass') }
$mkArgs += @('-File', $mkfat, '-Repo', $Repo, '-Out', $fatImg, '-SizeMB', '0')
& $psHost @mkArgs
if ($LASTEXITCODE -ne 0) { Fail 'mkfat-ng.ps1 failed' }
if (-not (Test-Path -LiteralPath $fatImg)) { Fail 'fat16 image not produced' }
$fatBytes = [IO.File]::ReadAllBytes($fatImg)
$fatProblems = New-Object System.Collections.Generic.List[string]
if ($fatBytes.Length -ne 1474560) {
    $fatProblems.Add(("fat16 image size {0} != 1474560" -f $fatBytes.Length))
}
if ($fatBytes.Length -ge 512) {
    if ($fatBytes[510] -ne 0x55 -or $fatBytes[511] -ne 0xAA) { $fatProblems.Add('fat16 boot signature missing') }
    if ([BitConverter]::ToUInt16($fatBytes, 11) -ne 512) { $fatProblems.Add('fat16 BPB bytes/sector != 512') }
    if ($fatBytes[13] -ne 1) { $fatProblems.Add('fat16 BPB sectors/cluster != 1') }
    if ([BitConverter]::ToUInt16($fatBytes, 14) -lt 1) { $fatProblems.Add('fat16 BPB reserved == 0') }
    if ($fatBytes[16] -lt 1) { $fatProblems.Add('fat16 BPB FAT count == 0') }
    if ([BitConverter]::ToUInt16($fatBytes, 19) -ne 2880) { $fatProblems.Add('fat16 BPB total sectors != 2880') }
}
if ($fatProblems.Count -gt 0) {
    foreach ($p in $fatProblems) { Write-Host ("  ! {0}" -f $p) }
    Fail 'FAT16 image verification failed'
}
$fatBlobObj = Join-Path $objDir 'fat16_img_blob.o'
$fatBlobAsm = Join-Path $objDir 'fat16_img_blob.S'
$fatInc = ($fatImg -replace '\\', '/')
$fatBlobText = @"
/* generated by tools/build-ng.ps1 (WP2-2): embed FAT16 image at 0x200000 */
    .section .fat16, "a"
    .align 16
    .global ng_fat16_img_start
ng_fat16_img_start:
    .incbin "$fatInc"
    .global ng_fat16_img_end
ng_fat16_img_end:
    .align 16
"@
[IO.File]::WriteAllText($fatBlobAsm, $fatBlobText, (New-Object Text.UTF8Encoding $false))
& $clang @($asmFlags + @($fatBlobAsm, '-o', $fatBlobObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'FAT16 image embed compile failed' }
Write-Host ("fat16 image: PASS ({0} bytes, {1} sectors, {2} clusters)" -f $fatBytes.Length, 2880, 2841)
Write-Host ''

# ---- WP2-4：真实磁盘镜像 dist-ng/disk.img（8MB FAT16）----------------------
Write-Host '=== disk image (WP2-4) ==='
$diskImg = Join-Path $distDir 'disk.img'
$mkDiskArgs = @('-NoProfile')
if ($env:OS -eq 'Windows_NT') { $mkDiskArgs += @('-ExecutionPolicy', 'Bypass') }
$mkDiskArgs += @('-File', $mkfat, '-Repo', $Repo, '-Out', $diskImg, '-SizeMB', '8')
& $psHost @mkDiskArgs
if ($LASTEXITCODE -ne 0) { Fail 'mkfat-ng.ps1 -SizeMB 8 failed' }
if (-not (Test-Path -LiteralPath $diskImg)) { Fail 'disk.img not produced' }
$diskBytes = [IO.File]::ReadAllBytes($diskImg)
$diskProblems = New-Object System.Collections.Generic.List[string]
if ($diskBytes.Length -ne 8388608) {
    $diskProblems.Add(("disk image size {0} != 8388608" -f $diskBytes.Length))
}
if ($diskBytes.Length -ge 512) {
    if ($diskBytes[510] -ne 0x55 -or $diskBytes[511] -ne 0xAA) { $diskProblems.Add('disk boot signature missing') }
    if ([BitConverter]::ToUInt16($diskBytes, 11) -ne 512) { $diskProblems.Add('disk BPB bytes/sector != 512') }
    if ($diskBytes[13] -ne 2) { $diskProblems.Add(('disk BPB sectors/cluster != 2 (got {0})' -f $diskBytes[13])) }
    if ([BitConverter]::ToUInt16($diskBytes, 17) -ne 512) { $diskProblems.Add('disk BPB root entries != 512') }
    if ([BitConverter]::ToUInt16($diskBytes, 19) -ne 16384) { $diskProblems.Add('disk BPB total sectors != 16384') }
    if ($diskBytes[38] -ne 0x29) { $diskProblems.Add('disk BPB extended signature missing') }
    $diskHello = [Text.Encoding]::ASCII.GetString($diskBytes, 97 * 512, 18)
    if ($diskHello -ne "hello from fat16`r`n") { $diskProblems.Add('disk HELLO.TXT content mismatch') }
}
if ($diskProblems.Count -gt 0) {
    foreach ($p in $diskProblems) { Write-Host ("  ! {0}" -f $p) }
    Fail 'disk image verification failed'
}
$diskFatSec = [BitConverter]::ToUInt16($diskBytes, 22)
$diskRootSec = 32
$diskDataStart = 1 + 2 * $diskFatSec + $diskRootSec
$diskClusters = [int][Math]::Floor((16384 - $diskDataStart) / 2)
Write-Host ("disk image: PASS ({0} bytes, 16384 sectors, fats={1}x{2}, data.start={3}, clusters={4})" -f `
    $diskBytes.Length, 2, $diskFatSec, $diskDataStart, $diskClusters)
Write-Host ''

# WP1-5 additions: switch.S, sched_arch.c, sched.c, sched_selftest.c.
# WP1-6 additions: handle.c, ipc_core.c, ipc.c, ipc_selftest.c.
# WP1-7 additions: mem.c（freestanding memcpy/memset）、elf.c、useras.c、
#                  uaccess.c、elf_selftest.c + user_elf_blob.o。
# WP2-1 additions: fs_syscall.c、fs_selftest.c（src/ng）+ vfs.c、ramfs.c
#                  （src/services/fs，L1 服务域；只依赖 types.h 与 fs_env）。
# WP2-2 additions: fat_ramdisk.c、fat16_selftest.c（src/ng）+ fat16.c
#                  （src/services/fs，L1；只依赖 types.h/vfs.h/blk.h）。
# WP2-3 additions: wp23_selftest.c（src/ng 接线）+ src/drivers/{pci,video,input}
#                  （L1D；只依赖 types.h / 自己的头，端口 IO 由内核注入）。
# WP2-7 additions: parity_selftest.c（src/ng，100 项 parity harness）+
#                  tests/parity/{parity_cases.h,parity_body.inc}（-I）+ 一份
#                  -DNG_HOST_TEST 的 src/apps/shell/shell.c（tokenizer 回归）。
# 汇编源；新增 .S 只要加进这个列表。
$asmSources = @('entry.S', 'isr.S', 'switch.S')
# C 源文件按顺序编译；新增 .c 只要加进这个列表。
$cSources = @('serial.c', 'memmap.c', 'pmm.c', 'heap.c', 'mem.c', 'paging.c', 'gdt.c', 'trap.c', 'syscall.c', 'mem_selftest.c', 'sched_arch.c', 'sched.c', 'sched_selftest.c', 'handle.c', 'ipc_core.c', 'ipc.c', 'ipc_selftest.c', 'elf.c', 'useras.c', 'uaccess.c', 'elf_selftest.c', 'fs_syscall.c', 'svc_syscall.c', 'fs_selftest.c', 'fat_ramdisk.c', 'fat16_selftest.c', 'wp23_selftest.c', 'ata_blk.c', 'wp24_selftest.c', 'wp25_selftest.c', 'wp26_selftest.c', 'proc_table.c', 'proc.c', 'proc_selftest.c', 'shm_core.c', 'shm.c', 'fbmap.c', 'wp31_selftest.c', 'wp32_selftest.c', 'wp33_selftest.c', 'parity_selftest.c', 'main.c')
# L1 服务域源码（相对仓库根；-I src/ng 与各 src/services/*/include 已加）。
$serviceSources = @('src/services/fs/vfs.c', 'src/services/fs/ramfs.c',
    'src/services/fs/fat16.c',
    'src/services/auth/sha256.c', 'src/services/auth/auth.c',
    'src/services/log/log.c', 'src/services/pkg/pkg.c')
# L1D 设备域源码（相对仓库根；-I src/drivers/include 已加）。
$driverSources = @('src/drivers/pci/pci.c', 'src/drivers/video/vbe.c',
    'src/drivers/video/fb.c', 'src/drivers/video/font8x16.c',
    'src/drivers/video/vga_text.c', 'src/drivers/input/ps2.c',
    'src/drivers/block/ata.c')
$steps = $asmSources.Count + $cSources.Count + $serviceSources.Count + $driverSources.Count + 2
$step = 0
$asmObjs = New-Object System.Collections.Generic.List[string]

# ---- 编译汇编 --------------------------------------------------------------
foreach ($src in $asmSources) {
    $step++
    Write-Host ("  [{0}/{1}] {2}" -f $step, $steps, $src)
    $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
    & $clang @($asmFlags + @((Join-Path $srcDir $src), '-o', $obj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0} compile failed" -f $src) }
    $asmObjs.Add($obj)
}

$cObjs = New-Object System.Collections.Generic.List[string]
foreach ($src in $cSources) {
    $step++
    Write-Host ("  [{0}/{1}] {2}" -f $step, $steps, $src)
    $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.o')
    & $clang @($cFlags + @((Join-Path $srcDir $src), '-o', $obj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0} compile failed" -f $src) }
    $cObjs.Add($obj)
}

# ---- L1 服务域（src/services/fs）-------------------------------------------
$serviceObjs = New-Object System.Collections.Generic.List[string]
foreach ($rel in $serviceSources) {
    $step++
    $leaf = [IO.Path]::GetFileName($rel)
    Write-Host ("  [{0}/{1}] {2}" -f $step, $steps, $rel)
    $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($rel) + '.o')
    & $clang @($cFlags + @((Join-Path $Repo $rel), '-o', $obj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0} compile failed" -f $leaf) }
    $serviceObjs.Add($obj)
}

# ---- L1D 设备域（src/drivers）-----------------------------------------------
$driverObjs = New-Object System.Collections.Generic.List[string]
foreach ($rel in $driverSources) {
    $step++
    $leaf = [IO.Path]::GetFileName($rel)
    Write-Host ("  [{0}/{1}] {2}" -f $step, $steps, $rel)
    $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($rel) + '.o')
    & $clang @($cFlags + @((Join-Path $Repo $rel), '-o', $obj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
    if ($LASTEXITCODE -ne 0) { Fail ("{0} compile failed" -f $leaf) }
    $driverObjs.Add($obj)
}

# ---- WP2-7：shell.c 的 parity 对象（只取 tokenizer / cwd 解析钩子）---------
# 内核 parity 不跑 shell_main（那是 ring3 的 WP2-6 路径），但要用**同一份**
# 解析代码做回归，所以用 -DNG_HOST_TEST 编一份：它保留 sh_test_tokenize /
# sh_test_resolve 钩子、去掉 _start，并且不依赖 libc。sh_host_syscall 由
# src/ng/parity_selftest.c 提供一个 -ENOSYS 兜底（tokenizer 不会调用它）。
$step++
$shellParityObj = Join-Path $objDir 'shell_parity.o'
Write-Host ("  [{0}/{1}] src/apps/shell/shell.c (parity tokenizer, -DNG_HOST_TEST)" -f $step, $steps)
& $clang @($cFlags + @('-DNG_HOST_TEST=1', (Join-Path $Repo 'src/apps/shell/shell.c'), '-o', $shellParityObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'shell parity object compile failed' }

# ---- 链接 ------------------------------------------------------------------
$step++
Write-Host ("  [{0}/{1}] link -> dist-ng/aster-ng.elf" -f $step, $steps)
$linkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + (Join-Path $srcDir 'link.ld')),
    '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-o', $elf) + $asmObjs.ToArray() + $cObjs.ToArray() + $serviceObjs.ToArray() + $driverObjs.ToArray() + @($shellParityObj, $userBlobObj, $shellBlobObj, $procBlobObj, $fatBlobObj) + $desktopBlobObjs.ToArray()
& $clang @linkArgs 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) {
    if (Test-Path $elf) { Remove-Item -LiteralPath $elf -Force }
    Fail 'link failed'
}

# ---- 校验 ELF64 / 启动头 / 段对齐 -----------------------------------------
$b = [IO.File]::ReadAllBytes($elf)
$problems = New-Object System.Collections.Generic.List[string]
$loadCount = 0
$segAlignOk = $true

if ($b.Length -lt 64) { $problems.Add('file smaller than ELF64 header') }

if ($problems.Count -eq 0) {
    if (-not ($b[0] -eq 0x7F -and $b[1] -eq 0x45 -and $b[2] -eq 0x4C -and $b[3] -eq 0x46)) {
        $problems.Add('bad ELF magic')
    }
    if ($b[4] -ne 2) { $problems.Add(("e_ident[EI_CLASS]={0}, want 2 (ELF64)" -f $b[4])) }
    if ($b[5] -ne 1) { $problems.Add(("e_ident[EI_DATA]={0}, want 1 (little endian)" -f $b[5])) }
    $eType = [BitConverter]::ToUInt16($b, 16)
    $eMachine = [BitConverter]::ToUInt16($b, 18)
    $eEntry = [BitConverter]::ToUInt64($b, 24)
    $ePhOff = [BitConverter]::ToUInt64($b, 32)
    $ePhentSize = [BitConverter]::ToUInt16($b, 54)
    $ePhNum = [BitConverter]::ToUInt16($b, 56)
    if ($eType -ne 2) { $problems.Add(("e_type={0}, want 2 (ET_EXEC)" -f $eType)) }
    if ($eMachine -ne 62) { $problems.Add(("e_machine={0}, want 62 (EM_X86_64)" -f $eMachine)) }
    if ($eEntry -lt 0x100000 -or $eEntry -ge 0x110000) {
        $problems.Add(("e_entry=0x{0:x} outside 0x100000..0x110000" -f $eEntry))
    }

    # ---- PT_LOAD：每段 4 KiB 对齐（.text/.rodata/.data/.bss 分页的前提）----
    for ($i = 0; $i -lt $ePhNum; $i++) {
        $ph = [int]$ePhOff + ($i * $ePhentSize)
        if ($ph + 56 -gt $b.Length) { break }
        if ([BitConverter]::ToUInt32($b, $ph) -ne 1) { continue }   # PT_LOAD
        $loadCount++
        $pVaddr = [BitConverter]::ToUInt64($b, $ph + 16)
        $pAlign = [BitConverter]::ToUInt64($b, $ph + 48)
        if (($pVaddr % 4096) -ne 0) { $segAlignOk = $false }
        if ($pAlign -lt 4096) { $segAlignOk = $false }
    }
    if (-not $segAlignOk) {
        $problems.Add('PT_LOAD segment not 4 KiB aligned (section pages would be shared)')
    }
    if ($loadCount -lt 4) {
        $problems.Add(("PT_LOAD count {0} < 4 (want headers/text/rodata/data)" -f $loadCount))
    }

    # ---- multiboot2 header：前 32 KiB、8 字节对齐、checksum = 0 ----
    $mbOff = -1
    $limit = [Math]::Min($b.Length, 32768) - 16
    for ($off = 0; $off -le $limit; $off += 8) {
        # magic 0xE85250D6 以 uint32 小端存放：D6 50 52 E8
        if ($b[$off] -eq 0xD6 -and $b[$off + 1] -eq 0x50 -and $b[$off + 2] -eq 0x52 -and $b[$off + 3] -eq 0xE8) {
            $mbOff = $off
            break
        }
    }
    if ($mbOff -lt 0) {
        $problems.Add('multiboot2 header magic not found in first 32 KiB (8-byte aligned)')
    } else {
        $mbLen = [BitConverter]::ToUInt32($b, $mbOff + 8)
        $sum = [uint64]0
        $sum += [BitConverter]::ToUInt32($b, $mbOff)
        $sum += [BitConverter]::ToUInt32($b, $mbOff + 4)
        $sum += [uint64]$mbLen
        $sum += [BitConverter]::ToUInt32($b, $mbOff + 12)
        if (($sum % 0x100000000) -ne 0) { $problems.Add('multiboot2 checksum != 0') }
        if ($mbLen -lt 16) { $problems.Add(("multiboot2 header length {0} < 16" -f $mbLen)) }
        if (($mbOff + $mbLen) -gt 32768) { $problems.Add('multiboot2 header crosses 32 KiB limit') }
    }

    # ---- PT_NOTE 里的 Xen PVH 入口（type 18）----
    $pvhEntry = $null
    for ($i = 0; $i -lt $ePhNum; $i++) {
        $ph = [int]$ePhOff + ($i * $ePhentSize)
        if ($ph + 56 -gt $b.Length) { break }
        if ([BitConverter]::ToUInt32($b, $ph) -ne 4) { continue }   # PT_NOTE
        $noteOff = [BitConverter]::ToUInt64($b, $ph + 8)
        $noteSize = [BitConverter]::ToUInt64($b, $ph + 32)
        $n = [int]$noteOff
        $end = [int]($noteOff + $noteSize)
        while ($n + 12 -le $end) {
            $namesz = [BitConverter]::ToUInt32($b, $n)
            $descsz = [BitConverter]::ToUInt32($b, $n + 4)
            $ntype = [BitConverter]::ToUInt32($b, $n + 8)
            $nameOff = $n + 12
            $name = [Text.Encoding]::ASCII.GetString($b, $nameOff, [int]$namesz)
            $descOff = $nameOff + ((([int]$namesz + 3) -shr 2) -shl 2)
            if ($descOff + $descsz -gt $end) { break }
            if ($name -eq "Xen`0" -and $ntype -eq 18) {
                $pvhEntry = [BitConverter]::ToUInt32($b, $descOff)
                break
            }
            $n = $descOff + ((([int]$descsz + 3) -shr 2) -shl 2)
        }
        if ($null -ne $pvhEntry) { break }
    }
    if ($null -eq $pvhEntry) {
        $problems.Add('Xen PVH note (type 18) not found in PT_NOTE')
    } elseif ([uint64]$pvhEntry -ne $eEntry) {
        $problems.Add(("PVH entry 0x{0:x} != ELF e_entry 0x{1:x}" -f $pvhEntry, $eEntry))
    }
}

if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Host ("  ! {0}" -f $p) }
    if (Test-Path $elf) { Remove-Item -LiteralPath $elf -Force }
    Fail 'output verification failed'
}

Write-Host ''
Write-Host ("ELF64: PASS ({0} bytes)" -f $b.Length)
Write-Host '  magic    7f 45 4c 46'
Write-Host ("  class    {0} (ELF64)" -f $b[4])
Write-Host ("  machine  {0} (EM_X86_64)" -f $eMachine)
Write-Host ("  type     {0} (ET_EXEC)" -f $eType)
Write-Host ("  entry    {0}" -f ('0x{0:x16}' -f $eEntry))
Write-Host ("  phnum    {0}" -f $ePhNum)
Write-Host 'segments: PASS'
Write-Host ("  {0} PT_LOAD, 4 KiB aligned (per-section pages)" -f $loadCount)
Write-Host 'multiboot2: PASS'
Write-Host ("  header   file offset 0x{0:x}, length {1}, checksum 0, 8-byte aligned" -f $mbOff, $mbLen)
Write-Host 'pvh-note: PASS'
Write-Host ("  entry    {0} (PT_NOTE, Xen type 18; QEMU -kernel)" -f ('0x{0:x16}' -f $pvhEntry))
Write-Host ''
Write-Host ("build-ng: PASS -> {0}" -f (Join-Path $DistDir 'aster-ng.elf'))
exit 0
