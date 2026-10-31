# =============================================================================
# fat-add.ps1 - WP3-5: 往 FAT16 磁盘镜像里加一个 8.3 文件（仅测试用）
#
# 为什么需要它：S1 故障注入的客户机代理（tests/s1/s1fault.c）不能编进内核
# （本 WP 不改 build-ng/内核镜像），所以 host 把它编译成 ELF，再用本脚本
# 写进 tools/mkfat-ng.ps1 生成的 8MB FAT16 镜像；guest 里 ring3 shell 用
# `spawn /mnt/disk/s1fault.elf <a|b|c|d>` 从 VFS 读它（proc_spawn 支持任意
# VFS 路径）。不修改 dist-ng/disk.img 本身：s1-fault.ps1 总是先复制一份。
#
# 只做最小 FAT16 追加：
#   - 解析 BPB（bytes/sector、spc、reserved、FAT 数、root entries、FAT size、
#     总扇区），算出 root/data 区；
#   - 找根目录里的空项（0x00；没有就用 0xE5 删除项）和第一段足够长的空闲
#     簇链（从 2 开始扫 FAT#1），同时写 FAT#1/FAT#2；
#   - 写目录项（8.3 大写、属性 0x20、首簇、字节数）与数据；
#   - 重新读回镜像做结构/内容自校验。
#
# 不支持：子目录、LFN、追加到已存在文件、碎片整理（8MB 镜像够用）。
#
# usage: powershell -File tests/s1/fat-add.ps1 -Image obj-ng/s1/last/disk-s1.img \
#            -File obj-ng/s1/last/s1fault.elf -Name S1FAULT.ELF
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$File,
    [Parameter(Mandatory = $true)][string]$Name
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host ("fat-add: FAIL ({0})" -f $msg)
    exit 1
}

if (-not (Test-Path -LiteralPath $Image)) { Fail ("image not found: {0}" -f $Image) }
if (-not (Test-Path -LiteralPath $File)) { Fail ("file not found: {0}" -f $File) }

$Image = (Resolve-Path -LiteralPath $Image).Path
$File = (Resolve-Path -LiteralPath $File).Path

$img = [IO.File]::ReadAllBytes($Image)
$bytes = [IO.File]::ReadAllBytes($File)
if ($bytes.Length -eq 0) { Fail 'input file is empty' }
if ($img.Length -lt 512) { Fail 'image smaller than one sector' }
if ($img[510] -ne 0x55 -or $img[511] -ne 0xAA) { Fail 'no 0x55AA boot signature' }

function Get-U16([int]$off) {
    return [int]([BitConverter]::ToUInt16($script:img, $off))
}

function Get-U32([int]$off) {
    return [long]([BitConverter]::ToUInt32($script:img, $off))
}

function Set-U16([int]$off, [int]$value) {
    $script:img[$off] = [byte]($value -band 0xFF)
    $script:img[$off + 1] = [byte](($value -shr 8) -band 0xFF)
}

function Set-U32([int]$off, [long]$value) {
    $script:img[$off] = [byte]($value -band 0xFF)
    $script:img[$off + 1] = [byte](($value -shr 8) -band 0xFF)
    $script:img[$off + 2] = [byte](($value -shr 16) -band 0xFF)
    $script:img[$off + 3] = [byte](($value -shr 24) -band 0xFF)
}

# ---- BPB -------------------------------------------------------------------
$bps = Get-U16 11
$spc = [int]$img[13]
$reserved = Get-U16 14
$numFats = [int]$img[16]
$rootEntries = Get-U16 17
$total16 = Get-U16 19
$fatSectors = Get-U16 22
$total32 = Get-U32 32
$total = if ($total16 -ne 0) { $total16 } else { $total32 }

if ($bps -lt 512 -or $bps -gt 4096 -or ($bps % 512) -ne 0) { Fail ("bad bytes/sector {0}" -f $bps) }
if ($spc -lt 1 -or $spc -gt 128) { Fail ("bad sectors/cluster {0}" -f $spc) }
if ($numFats -lt 1 -or $numFats -gt 4) { Fail ("bad FAT count {0}" -f $numFats) }
if ($rootEntries -lt 16 -or $rootEntries -gt 4096) { Fail ("bad root entries {0}" -f $rootEntries) }
if ($fatSectors -le 0) { Fail 'bad FAT size' }
if ($total -le 0) { Fail 'bad total sectors' }

$rootDirSectors = [int][Math]::Ceiling($rootEntries * 32 / $bps)
$rootStart = $reserved + $numFats * $fatSectors
$dataStart = $rootStart + $rootDirSectors
if ($dataStart -ge $total) { Fail 'no data area' }
$clusters = [int][Math]::Floor(($total - $dataStart) / $spc)
if ($clusters -lt 2) { Fail ("too few clusters ({0})" -f $clusters) }
$clusterSize = $spc * $bps

# ---- 8.3 名字 --------------------------------------------------------------
$upper = $Name.ToUpperInvariant()
$dot = $upper.LastIndexOf('.')
if ($dot -gt 0) {
    $base = $upper.Substring(0, $dot)
    $ext = $upper.Substring($dot + 1)
} else {
    $base = $upper
    $ext = ''
}
if ($base.Length -lt 1 -or $base.Length -gt 8) { Fail ("8.3 base must be 1..8 chars: '{0}'" -f $Name) }
if ($ext.Length -gt 3) { Fail ("8.3 ext must be <=3 chars: '{0}'" -f $Name) }
$allowed = '^[A-Z0-9_\-]+$'
if ($base -notmatch $allowed -or ($ext -ne '' -and $ext -notmatch $allowed)) {
    Fail ("unsupported 8.3 characters: '{0}'" -f $Name)
}
$name83 = $base.PadRight(8) + $ext.PadRight(3)
if ($name83.Length -ne 11) { Fail 'internal 8.3 packing error' }

# ---- 根目录：找空项 / 查重 -------------------------------------------------
$rootOff = $rootStart * $bps
$freeOff = -1
$freeDeleted = -1
for ($i = 0; $i -lt $rootEntries; $i++) {
    $off = $rootOff + $i * 32
    $first = [int]$img[$off]
    if ($first -eq 0x00) {
        if ($freeOff -lt 0) { $freeOff = $off }
        break
    }
    if ($first -eq 0xE5) {
        if ($freeDeleted -lt 0) { $freeDeleted = $off }
        continue
    }
    $slotName = [Text.Encoding]::ASCII.GetString($img, $off, 11)
    if ($slotName -eq $name83) { Fail ("file already exists in image: {0}" -f $Name) }
}
if ($freeOff -lt 0) { $freeOff = $freeDeleted }
if ($freeOff -lt 0) { Fail 'root directory is full' }

# ---- 空闲簇（优先连续段）--------------------------------------------------
function Get-FatEntry([int]$cluster) {
    $off = $reserved * $bps + $cluster * 2
    return [int]([BitConverter]::ToUInt16($script:img, $off))
}

$need = [int][Math]::Ceiling($bytes.Length / $clusterSize)
if ($need -lt 1) { $need = 1 }
$chain = New-Object System.Collections.Generic.List[int]
$runStart = -1
$runLen = 0
for ($c = 2; $c -le $clusters + 1; $c++) {
    if ((Get-FatEntry $c) -eq 0) {
        if ($runStart -lt 0) { $runStart = $c; $runLen = 0 }
        $runLen++
        if ($runLen -eq $need) { break }
    } else {
        $runStart = -1
        $runLen = 0
    }
}
if ($runLen -lt $need) { Fail ("not enough free clusters (need {0})" -f $need) }
for ($k = 0; $k -lt $need; $k++) { $chain.Add($runStart + $k) }

# ---- FAT 链（两份拷贝）----------------------------------------------------
for ($k = 0; $k -lt $need; $k++) {
    $value = if ($k -eq ($need - 1)) { 0xFFFF } else { $chain[$k + 1] }
    for ($f = 0; $f -lt $numFats; $f++) {
        $off = ($reserved + $f * $fatSectors) * $bps + $chain[$k] * 2
        Set-U16 $off $value
    }
}

# ---- 数据 ------------------------------------------------------------------
for ($k = 0; $k -lt $need; $k++) {
    $src = $k * $clusterSize
    $chunk = [Math]::Min($clusterSize, $bytes.Length - $src)
    if ($chunk -le 0) { break }
    $dst = ($dataStart + ($chain[$k] - 2) * $spc) * $bps
    [Array]::Copy($bytes, $src, $img, $dst, $chunk)
}

# ---- 目录项 ----------------------------------------------------------------
for ($i = 0; $i -lt 32; $i++) { $img[$freeOff + $i] = 0 }
[Array]::Copy([Text.Encoding]::ASCII.GetBytes($name83), 0, $img, $freeOff, 11)
$img[$freeOff + 11] = 0x20            # ATTR_ARCHIVE
Set-U16 ($freeOff + 20) 0              # cluster_hi（FAT16 恒 0）
Set-U16 ($freeOff + 26) $chain[0]      # cluster_lo
Set-U32 ($freeOff + 28) ([long]$bytes.Length)

[IO.File]::WriteAllBytes($Image, $img)

# ---- 自校验（重新读回镜像）------------------------------------------------
$check = [IO.File]::ReadAllBytes($Image)
$foundOff = -1
for ($i = 0; $i -lt $rootEntries; $i++) {
    $off = $rootOff + $i * 32
    if ([int]$check[$off] -eq 0x00) { break }
    if ([int]$check[$off] -eq 0xE5) { continue }
    $slotName = [Text.Encoding]::ASCII.GetString($check, $off, 11)
    if ($slotName -eq $name83) { $foundOff = $off; break }
}
if ($foundOff -lt 0) { Fail 'verification: directory entry missing' }
$checkSize = [long]([BitConverter]::ToUInt32($check, $foundOff + 28))
if ($checkSize -ne $bytes.Length) { Fail ("verification: size {0} != {1}" -f $checkSize, $bytes.Length) }
$firstCluster = [int]([BitConverter]::ToUInt16($check, $foundOff + 26))
if ($firstCluster -ne $chain[0]) { Fail 'verification: first cluster mismatch' }
$readBack = New-Object byte[] ([Math]::Min(64, $bytes.Length))
$dataOff = ($dataStart + ($firstCluster - 2) * $spc) * $bps
[Array]::Copy($check, $dataOff, $readBack, 0, $readBack.Length)
for ($i = 0; $i -lt $readBack.Length; $i++) {
    if ($readBack[$i] -ne $bytes[$i]) { Fail 'verification: data mismatch' }
}
$lastCluster = $chain[$need - 1]
$lastFat = [BitConverter]::ToUInt16($check, $reserved * $bps + $lastCluster * 2)
if ($lastFat -ne 0xFFFF) { Fail 'verification: FAT end marker missing' }

Write-Host '=== fat-add (WP3-5) ==='
Write-Host ("image:  {0} ({1} bytes, {2} clusters x {3} B)" -f $Image, $check.Length, $clusters, $clusterSize)
Write-Host ("root:   sector {0} ({1} entries), data sector {2}" -f $rootStart, $rootEntries, $dataStart)
Write-Host ("file:   {0} -> {1} ({2} bytes, {3} clusters, first={4})" -f $File, $Name, $bytes.Length, $need, $chain[0])
Write-Host ("fat-add: PASS -> {0}" -f $Image)
exit 0
