# =============================================================================
# mkfat-ng.ps1 - ng WP2-2/2-4: 生成 FAT16 测试镜像
#
# 两种模式（同一份镜像构造代码）：
#   - 默认 / -SizeMB >= 1：真实磁盘镜像 dist-ng/disk.img（默认 8MB，FAT16），
#     由 run-ng.ps1 以 `-drive file=...,format=raw,if=ide` 挂到 QEMU 的
#     PIIX3 IDE 主盘；内核 ATA PIO 驱动读它，FAT16 挂到 /mnt/disk。
#   - -SizeMB 0：1.44MB 软盘几何（WP2-2 的 RAM 盘镜像 obj-ng/fat16.img），
#     由 build-ng.ps1 以 .incbin 嵌进内核、启动时复制到 PMM RAM 盘挂在 /mnt/fat。
#
# 为什么 FAT16 判定是“16 位 FAT 表项几何”而不是簇数 >= 4085：WP2-2 的 1.44MB
# 镜像只有 2841 个簇（按微软原始规范属于 FAT12 区间），但 FAT 区用 12 个扇区
# 容纳了 clusters+2 个 16 位项，所以 FAT16 后端（fat16.c）按表项几何接受它。
# 8MB 默认镜像因为 2 扇区/簇得到 8143 个簇，本身就落在 FAT16 区间（>= 4085）。
#
# 8MB 布局（16384 扇区 = 8388608 字节，2 扇区/簇 = 1024B/簇）：
#   [0]                 引导扇区 / BPB
#   [1..32]             FAT #1（32 扇区 = 8192 个 16 位表项）
#   [33..64]            FAT #2（副本）
#   [65..96]            根目录（512 项 = 32 扇区）
#   [97..]              数据区：簇 2 = HELLO.TXT、簇 3 = DIR/A.TXT、
#                             簇 4 = DIR 子目录
#   clusters = (16384 - 97) / 2 = 8143
#
# 内容与 1.44MB 镜像一致：HELLO.TXT（18B "hello from fat16\r\n"）、
# DIR/A.TXT（6B "dir a\n"）；ring3 demo 会另建 DISK.TXT。
#
# usage: powershell -File tools/mkfat-ng.ps1 [-Repo <root>] [-Out dist-ng/disk.img]
#                                            [-SizeMB 8]     # 0 = 1.44MB legacy
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$Out = 'dist-ng/disk.img',
    [int]$SizeMB = 8
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $Repo $Out }
$OutDir = Split-Path -Parent $Out
if ($OutDir -and -not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

function Fail([string]$msg) {
    Write-Host ("mkfat-ng: FAIL ({0})" -f $msg)
    exit 1
}

# ---- 几何 ------------------------------------------------------------------
$BytesPerSector = 512
$Reserved = 1
$NumFats = 2

if ($SizeMB -eq 0) {
    # WP2-2：1.44MB 软盘几何 + 16 位表项几何
    $ModeText = 'legacy 1.44MB'
    $TotalSectors = 2880
    $SecPerCluster = 1
    $RootEntries = 224
    $Media = 0xF0
    $DriveNumber = 0x00
    $VolumeLabel = 'ASTERFAT16 '
    $VolumeId = [uint32]0x4E473230        # "NG02"
} elseif ($SizeMB -ge 1 -and $SizeMB -le 64) {
    # WP2-4：真实磁盘（8MB 默认）——2 扇区/簇得到 >= 4085 个簇，是真 FAT16。
    $ModeText = ("disk {0}MB" -f $SizeMB)
    $TotalSectors = [int]($SizeMB * 1024 * 1024 / $BytesPerSector)
    $SecPerCluster = 2
    $RootEntries = 512
    $Media = 0xF8
    $DriveNumber = 0x80
    $VolumeLabel = 'ASTERDISK  '
    $VolumeId = [uint32]0x4E473234        # "NG24"
} else {
    Fail ("-SizeMB out of range: {0} (0 = 1.44MB, 1..64 = FAT16 disk)" -f $SizeMB)
}

$RootDirSectors = [int][Math]::Ceiling($RootEntries * 32 / $BytesPerSector)

# FAT 区大小与簇数互相依赖：迭代到不动点（最多 8 轮）。
$FatSectors = 1
$DataStart = 0
$Clusters = 0
for ($it = 0; $it -lt 8; $it++) {
    $DataStart = $Reserved + $NumFats * $FatSectors + $RootDirSectors
    if ($DataStart -ge $TotalSectors) { Fail ("no data area: data.start={0}" -f $DataStart) }
    $Clusters = [int][Math]::Floor(($TotalSectors - $DataStart) / $SecPerCluster)
    $FatNeeded = [int][Math]::Ceiling(($Clusters + 2) * 2 / $BytesPerSector)
    if ($FatSectors -eq $FatNeeded) { break }
    $FatSectors = $FatNeeded
}
$DataStart = $Reserved + $NumFats * $FatSectors + $RootDirSectors
$Clusters = [int][Math]::Floor(($TotalSectors - $DataStart) / $SecPerCluster)
$FatNeeded = [int][Math]::Ceiling(($Clusters + 2) * 2 / $BytesPerSector)

if ($Clusters -lt 2 -or $Clusters -gt 65524) {
    Fail ("clusters out of FAT16 range: {0}" -f $Clusters)
}
if ($FatSectors -lt $FatNeeded) {
    Fail ("FAT too small: {0} sectors, need {1}" -f $FatSectors, $FatNeeded)
}
if ($FatSectors -gt 0xFFFF) { Fail ("FAT too large: {0} sectors" -f $FatSectors) }

$ImageBytes = $TotalSectors * $BytesPerSector

Write-Host '=== mkfat-ng (WP2-2/2-4) ==='
Write-Host ("mode: {0}" -f $ModeText)
Write-Host ("geometry: {0} sectors x {1}, spc={2}, FATS={3}x{4}, root={5} ({6} sec)" -f `
    $TotalSectors, $BytesPerSector, $SecPerCluster, $NumFats, $FatSectors, $RootEntries, $RootDirSectors)
Write-Host ("          data.start={0}, clusters={1}, bytes/cluster={2}" -f `
    $DataStart, $Clusters, ($SecPerCluster * $BytesPerSector))

$img = New-Object byte[] $ImageBytes

function Set-U16([int]$off, [int]$value) {
    $script:img[$off] = [byte]($value -band 0xFF)
    $script:img[$off + 1] = [byte](($value -shr 8) -band 0xFF)
}

function Set-U32([int]$off, [uint32]$value) {
    $script:img[$off] = [byte]($value -band 0xFF)
    $script:img[$off + 1] = [byte](($value -shr 8) -band 0xFF)
    $script:img[$off + 2] = [byte](($value -shr 16) -band 0xFF)
    $script:img[$off + 3] = [byte](($value -shr 24) -band 0xFF)
}

function Set-Fat([int]$cluster, [int]$value) {
    for ($f = 0; $f -lt $NumFats; $f++) {
        $off = ($Reserved + $f * $FatSectors) * $BytesPerSector + $cluster * 2
        Set-U16 $off $value
    }
}

# 目录项：$name11 必须正好 11 字节（8.3，空格填充；"." / ".." 用原始形状）。
function Set-DirEntry([int]$sector, [int]$index, [string]$name11, [byte]$attr,
                      [int]$cluster, [int]$size) {
    if ($name11.Length -ne 11) { Fail ("name must be 11 bytes: '{0}'" -f $name11) }
    $off = $sector * $BytesPerSector + $index * 32
    for ($i = 0; $i -lt 32; $i++) { $script:img[$off + $i] = 0 }
    $bytes = [Text.Encoding]::ASCII.GetBytes($name11)
    [Array]::Copy($bytes, 0, $script:img, $off, 11)
    $script:img[$off + 11] = $attr
    # cluster_hi = 0（FAT16），cluster_lo 在 26，size 在 28
    Set-U16 ($off + 20) 0
    Set-U16 ($off + 26) $cluster
    Set-U32 ($off + 28) ([uint32]$size)
}

function Set-ClusterData([int]$cluster, [string]$text) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($text)
    $off = ($DataStart + ($cluster - 2) * $SecPerCluster) * $BytesPerSector
    [Array]::Copy($bytes, 0, $script:img, $off, $bytes.Length)
}

# ---- 引导扇区 / BPB -------------------------------------------------------
$img[0] = 0xEB; $img[1] = 0x3C; $img[2] = 0x90
$oem = [Text.Encoding]::ASCII.GetBytes('ASTER   ')
[Array]::Copy($oem, 0, $img, 3, 8)
Set-U16 11 $BytesPerSector
$img[13] = [byte]$SecPerCluster
Set-U16 14 $Reserved
$img[16] = [byte]$NumFats
Set-U16 17 $RootEntries
if ($TotalSectors -le 0xFFFF) {
    Set-U16 19 $TotalSectors
    Set-U32 32 0
} else {
    Set-U16 19 0
    Set-U32 32 ([uint32]$TotalSectors)
}
$img[21] = [byte]$Media
Set-U16 22 $FatSectors
Set-U16 24 18                         # sectors/track
Set-U16 26 2                          # heads
Set-U32 28 0                          # hidden sectors
$img[36] = [byte]$DriveNumber         # drive number（0x00 软盘 / 0x80 硬盘）
$img[37] = 0x00
$img[38] = 0x29                       # extended boot signature
Set-U32 39 $VolumeId
$label = [Text.Encoding]::ASCII.GetBytes($VolumeLabel)
[Array]::Copy($label, 0, $img, 43, 11)
$fstype = [Text.Encoding]::ASCII.GetBytes('FAT16   ')
[Array]::Copy($fstype, 0, $img, 54, 8)
$img[510] = 0x55; $img[511] = 0xAA

# ---- FAT：保留项 + 已用簇 --------------------------------------------------
Set-Fat 0 0xFFF8
Set-Fat 1 0xFFFF
Set-Fat 2 0xFFFF
Set-Fat 3 0xFFFF
Set-Fat 4 0xFFFF

# ---- 根目录 ----------------------------------------------------------------
$RootStart = $Reserved + $NumFats * $FatSectors
Set-DirEntry $RootStart 0 $VolumeLabel 0x08 0 0
Set-DirEntry $RootStart 1 'HELLO   TXT' 0x20 2 18
Set-DirEntry $RootStart 2 'DIR        ' 0x10 4 0
# index 3.. 保持 0x00：目录结束

# ---- DIR 子目录（簇 4）-----------------------------------------------------
$dirSector = $DataStart + (4 - 2) * $SecPerCluster
Set-DirEntry $dirSector 0 '.          ' 0x10 4 0
Set-DirEntry $dirSector 1 '..         ' 0x10 0 0
Set-DirEntry $dirSector 2 'A       TXT' 0x20 3 6

# ---- 文件内容 --------------------------------------------------------------
Set-ClusterData 2 "hello from fat16`r`n"
Set-ClusterData 3 "dir a`n"

# ---- 写盘 + 自校验 ---------------------------------------------------------
[IO.File]::WriteAllBytes($Out, $img)

$check = [IO.File]::ReadAllBytes($Out)
if ($check.Length -ne $ImageBytes) { Fail ("output size {0} != {1}" -f $check.Length, $ImageBytes) }
if ($check[510] -ne 0x55 -or $check[511] -ne 0xAA) { Fail 'boot signature missing' }
if ([BitConverter]::ToUInt16($check, 11) -ne 512) { Fail 'BPB bytes/sector != 512' }
if ($check[13] -ne $SecPerCluster) { Fail 'BPB sectors/cluster mismatch' }
if ([BitConverter]::ToUInt16($check, 17) -ne $RootEntries) { Fail 'BPB root entries mismatch' }
if ([BitConverter]::ToUInt16($check, 22) -ne $FatSectors) { Fail 'BPB FAT size mismatch' }
$bpbTotal = if ($TotalSectors -le 0xFFFF) {
    [BitConverter]::ToUInt16($check, 19)
} else {
    [BitConverter]::ToUInt32($check, 32)
}
if ($bpbTotal -ne $TotalSectors) { Fail 'BPB total sectors mismatch' }

$hello = [Text.Encoding]::ASCII.GetString($check, ($DataStart + 0) * 512, 18)
if ($hello -ne "hello from fat16`r`n") { Fail 'HELLO.TXT content mismatch' }
$a = [Text.Encoding]::ASCII.GetString($check, ($DataStart + 1 * $SecPerCluster) * 512, 6)
if ($a -ne "dir a`n") { Fail 'DIR/A.TXT content mismatch' }

Write-Host ("files: HELLO.TXT (18 bytes), DIR/A.TXT (6 bytes)")
Write-Host ("mkfat-ng: PASS -> {0} ({1} bytes)" -f $Out, $check.Length)
exit 0
