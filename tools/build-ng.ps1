# =============================================================================
# build-ng.ps1 - ng WP2-3: 构建 64 位内核（ELF64 x86_64，PMM/heap + 页表 +
#               trap/syscall + 线程/调度 + IPC/句柄 + ELF64 加载器 + ring3 hello
#               + VFS/ramfs（L1 服务域）+ FAT16（L1，RAM 盘镜像）+
#               PCI/VBE/帧缓冲控制台 + PS/2 键盘（L1D 设备域））
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
#       src/services/fs/vfs.c + ramfs.c + fat16.c（L1：VFS/ramfs/FAT16）、
#       src/drivers/pci/pci.c + video/vbe.c + video/fb.c + video/font8x16.c
#         + video/vga_text.c + input/ps2.c（L1D：PCI/VBE/帧缓冲/PS2）、
#       src/ng/main.c（64 位 C 入口）、src/ng/link.ld。
# 用户程序：src/ng/user/hello.S 用 src/ng/user/user.ld 链到 0x40000000，
#           生成 obj-ng/hello.elf，再用 .incbin 嵌进内核 .rodata
#           （ng_user_elf_start..end），内核 ELF64 加载器从内存镜像读取。
# FAT16 镜像：tools/mkfat-ng.ps1 生成 obj-ng/fat16.img（1.44MB），同样用
#           .incbin 嵌进 .rodata（ng_fat16_img_start..end），启动时由
#           src/ng/fat_ramdisk.c 复制到 PMM RAM 盘再挂到 /mnt/fat。
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
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$DistDir = 'dist-ng',
    [string]$ObjDir = 'obj-ng',
    [switch]$PanicTest
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

Write-Host '=== build-ng (WP2-3) ==='
Write-Host ("clang: {0}" -f $clang)
Write-Host ("lld:   {0}" -f $lldCmd.Source)
if ($PanicTest) { Write-Host 'mode:  panic-test build (-DNG_PANIC_TEST=1)' }

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
    '-I', (Join-Path $Repo 'src/drivers/include'),
    '-c')
if ($PanicTest) { $cFlags += '-DNG_PANIC_TEST=1' }

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
    '-o', $userElf, $userObj)
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
/* generated by tools/build-ng.ps1 (WP1-7): embed user ELF into kernel .rodata */
    .section .rodata
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

# ---- WP2-2：FAT16 测试镜像 -> .incbin 嵌内核 .rodata ----------------------
Write-Host '=== fat16 image (WP2-2) ==='
$fatImg = Join-Path $objDir 'fat16.img'
$mkfat = Join-Path $PSScriptRoot 'mkfat-ng.ps1'
$psHost = $null
try { $psHost = (Get-Process -Id $PID).Path } catch { }
if (-not $psHost) { $psHost = 'powershell' }
$mkArgs = @('-NoProfile')
if ($env:OS -eq 'Windows_NT') { $mkArgs += @('-ExecutionPolicy', 'Bypass') }
$mkArgs += @('-File', $mkfat, '-Repo', $Repo, '-Out', $fatImg)
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
# 汇编源；新增 .S 只要加进这个列表。
$asmSources = @('entry.S', 'isr.S', 'switch.S')
# C 源文件按顺序编译；新增 .c 只要加进这个列表。
$cSources = @('serial.c', 'memmap.c', 'pmm.c', 'heap.c', 'mem.c', 'paging.c', 'gdt.c', 'trap.c', 'syscall.c', 'mem_selftest.c', 'sched_arch.c', 'sched.c', 'sched_selftest.c', 'handle.c', 'ipc_core.c', 'ipc.c', 'ipc_selftest.c', 'elf.c', 'useras.c', 'uaccess.c', 'elf_selftest.c', 'fs_syscall.c', 'fs_selftest.c', 'fat_ramdisk.c', 'fat16_selftest.c', 'wp23_selftest.c', 'main.c')
# L1 服务域源码（相对仓库根；-I src/ng 与 -I src/services/fs/include 已加）。
$serviceSources = @('src/services/fs/vfs.c', 'src/services/fs/ramfs.c', 'src/services/fs/fat16.c')
# L1D 设备域源码（相对仓库根；-I src/drivers/include 已加）。
$driverSources = @('src/drivers/pci/pci.c', 'src/drivers/video/vbe.c',
    'src/drivers/video/fb.c', 'src/drivers/video/font8x16.c',
    'src/drivers/video/vga_text.c', 'src/drivers/input/ps2.c')
$steps = $asmSources.Count + $cSources.Count + $serviceSources.Count + $driverSources.Count + 1
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

# ---- 链接 ------------------------------------------------------------------
$step++
Write-Host ("  [{0}/{1}] link -> dist-ng/aster-ng.elf" -f $step, $steps)
$linkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + (Join-Path $srcDir 'link.ld')),
    '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-o', $elf) + $asmObjs.ToArray() + $cObjs.ToArray() + $serviceObjs.ToArray() + $driverObjs.ToArray() + @($userBlobObj, $fatBlobObj)
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
