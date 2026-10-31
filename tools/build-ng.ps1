# =============================================================================
# build-ng.ps1 - ng WP1-1: 构建 64 位 boot 内核（ELF64 x86_64）
#
# 输入：src/ng/entry.S（multiboot2 + PVH 头、32 位入口、页表、长模式）、
#       src/ng/main.c（64 位 C 入口：COM1 串口 hello）、src/ng/link.ld。
# 输出：dist-ng/aster-ng.elf，对象文件在 obj-ng/。
#
# 工具链（WP0-4 已验证）：
#   clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
#   -mno-red-zone -fno-stack-protector
#
# 校验：ELF64 / x86_64 / ET_EXEC / 入口地址；multiboot2 头在前 32 KiB 且
#       8 字节对齐、checksum 为 0；PT_NOTE 里有 Xen PVH 入口（QEMU -kernel 用）。
#
# usage: powershell -File tools/build-ng.ps1 [-Repo <root>] [-DistDir dist-ng] [-ObjDir obj-ng]
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$DistDir = 'dist-ng',
    [string]$ObjDir = 'obj-ng'
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

Write-Host '=== build-ng (WP1-1) ==='
Write-Host ("clang: {0}" -f $clang)
Write-Host ("lld:   {0}" -f $lldCmd.Source)

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
New-Item -ItemType Directory -Force -Path $objDir | Out-Null
if (Test-Path $elf) { Remove-Item -LiteralPath $elf -Force }

$entryObj = Join-Path $objDir 'entry.o'
$mainObj = Join-Path $objDir 'main.o'

$target = '--target=x86_64-elf'
$common = @($target, '-ffreestanding', '-nostdlib', '-mno-red-zone',
    '-fno-stack-protector', '-fcf-protection=none')
$asmFlags = $common + @('-c')
$cFlags = $common + @('-mno-sse', '-mgeneral-regs-only', '-fno-builtin',
    '-fno-pic', '-fno-pie', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-c')

# ---- 编译 ------------------------------------------------------------------
Write-Host '  [1/3] entry.S'
& $clang @($asmFlags + @((Join-Path $srcDir 'entry.S'), '-o', $entryObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'entry.S compile failed' }

Write-Host '  [2/3] main.c'
& $clang @($cFlags + @((Join-Path $srcDir 'main.c'), '-o', $mainObj)) 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) { Fail 'main.c compile failed' }

# ---- 链接 ------------------------------------------------------------------
Write-Host '  [3/3] link -> dist-ng/aster-ng.elf'
$linkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + (Join-Path $srcDir 'link.ld')),
    '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-o', $elf, $entryObj, $mainObj)
& $clang @linkArgs 2>&1 | Out-String | ForEach-Object { if ($_.Trim()) { Write-Host $_.Trim() } }
if ($LASTEXITCODE -ne 0) {
    if (Test-Path $elf) { Remove-Item -LiteralPath $elf -Force }
    Fail 'link failed'
}

# ---- 校验 ELF64 / 启动头 --------------------------------------------------
$b = [IO.File]::ReadAllBytes($elf)
$problems = New-Object System.Collections.Generic.List[string]

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
Write-Host 'multiboot2: PASS'
Write-Host ("  header   file offset 0x{0:x}, length {1}, checksum 0, 8-byte aligned" -f $mbOff, $mbLen)
Write-Host 'pvh-note: PASS'
Write-Host ("  entry    {0} (PT_NOTE, Xen type 18; QEMU -kernel)" -f ('0x{0:x16}' -f $pvhEntry))
Write-Host ''
Write-Host ("build-ng: PASS -> {0}" -f (Join-Path $DistDir 'aster-ng.elf'))
exit 0
