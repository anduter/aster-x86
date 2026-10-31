# =============================================================================
# build-ng.ps1 - ng WP1-5: 构建 64 位内核（ELF64 x86_64，PMM/heap + 页表 + trap/syscall）
#
# 输入：src/ng/entry.S（multiboot2 + PVH 头、32 位入口、引导页表、长模式）、
#       src/ng/isr.S（32 个异常 stub、受控探测、syscall 入口、ring3 往返）、
#       src/ng/serial.c（COM1）、src/ng/memmap.c（PVH/multiboot2 内存图）、
#       src/ng/pmm.c（bitmap PMM）、src/ng/heap.c（first-fit + 合并堆）、
#       src/ng/paging.c（4 级页表/W^X/NX/SMEP/SMAP，页表页来自 PMM）、
#       src/ng/gdt.c（GDT/TSS/IST）、src/ng/trap.c（IDT/异常/panic）、
#       src/ng/syscall.c（STAR/LSTAR/FMASK + ring0/ring3 测试）、
#       src/ng/mem_selftest.c（内存图/PMM/heap/页表池自检）、
#       src/ng/switch.S + sched_arch.c + sched.c + sched_selftest.c（WP1-5）、
#       src/ng/main.c（64 位 C 入口）、src/ng/link.ld。
# 输出：dist-ng/aster-ng.elf，对象文件在 obj-ng/。
#
# 工具链（WP0-4 已验证）：
#   clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
#   -mno-red-zone -fno-stack-protector
#
# 校验：ELF64 / x86_64 / ET_EXEC / 入口地址；PT_LOAD 4 KiB 对齐（按段分页的
#       前提）；multiboot2 头在前 32 KiB 且 8 字节对齐、checksum 为 0；
#       PT_NOTE 里有 Xen PVH 入口（QEMU -kernel 用）。
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

Write-Host '=== build-ng (WP1-5) ==='
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
    '-fno-pic', '-fno-pie', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-c')
if ($PanicTest) { $cFlags += '-DNG_PANIC_TEST=1' }

# WP1-5 additions: switch.S, sched_arch.c, sched.c, sched_selftest.c.
# 汇编源；新增 .S 只要加进这个列表。
$asmSources = @('entry.S', 'isr.S', 'switch.S')
# C 源文件按顺序编译；新增 .c 只要加进这个列表。
$cSources = @('serial.c', 'memmap.c', 'pmm.c', 'heap.c', 'paging.c', 'gdt.c', 'trap.c', 'syscall.c', 'mem_selftest.c', 'sched_arch.c', 'sched.c', 'sched_selftest.c', 'main.c')
$steps = $asmSources.Count + $cSources.Count + 1
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

# ---- 链接 ------------------------------------------------------------------
$step++
Write-Host ("  [{0}/{1}] link -> dist-ng/aster-ng.elf" -f $step, $steps)
$linkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + (Join-Path $srcDir 'link.ld')),
    '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-o', $elf) + $asmObjs.ToArray() + $cObjs.ToArray()
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
