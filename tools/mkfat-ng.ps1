# =============================================================================
# mkfat-ng.ps1 - ng WP2-2: 生成 1.44MB FAT16 测试镜像（HELLO.TXT + DIR/A.TXT）
#
# 为什么是 FAT16：ng 的 FAT16 后端只认 16 位 FAT 表项几何。1.44MB 软盘几何
# 下簇只有 2841 个，按微软原始规范会落进 FAT12 的簇数区间；本 WP 的判定标准
# 是 **BPB 与 16 位表项一致**（FAT 区必须覆盖 clusters+2 个 16 位项），所以
# 镜像用 fat_sectors=12（FAT12 的 1.44MB 软盘是 9）容纳 2843 个表项，其余
# 几何保持软盘标准（512B/扇区、1 扇区/簇、2 份 FAT、224 个根目录项）。
#
# 布局（2880 扇区 = 1474560 字节）：
#   [0]                 引导扇区 / BPB
#   [1..12]             FAT #1（12 扇区，可放 3072 个 16 位表项）
#   [13..24]            FAT #2（副本）
#   [25..38]            根目录（224 项 = 14 扇区）
#   [39..]              数据区：簇 2 = HELLO.TXT 内容
#                             簇 3 = DIR/A.TXT 内容
#                             簇 4 = DIR 子目录（".", "..", A.TXT）
#   clusters = (2880 - 39) / 1 = 2841
#
# 生成结果同时被 build-ng.ps1 用 .incbin 嵌进内核 .rodata，启动时复制到
# PMM RAM 盘（src/ng/fat_ramdisk.c）。
#
# usage: powershell -File tools/mkfat-ng.ps1 [-Repo <root>] [-Out obj-ng/fat16.img]
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$Out = 'obj-ng/fat16.img'
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

# ---- 几何（1.44MB 软盘，FAT16 表项）---------------------------------------
$BytesPerSector = 512
$SecPerCluster = 1
$Reserved = 1
$NumFats = 2
$RootEntries = 224
$FatSectors = 12
$TotalSectors = 2880
$RootStart = $Reserved + $NumFats * $FatSectors          # 25
$RootDirSectors = [int][Math]::Ceiling($RootEntries * 32 / $BytesPerSector) # 14
$DataStart = $RootStart + $RootDirSectors                # 39
$Clusters = [int](($TotalSectors - $DataStart) / $SecPerCluster) # 2841
$ImageBytes = $TotalSectors * $BytesPerSector

if ($Clusters -gt 65524) { Fail ("too many clusters: {0}" -f $Clusters) }
$FatNeeded = [int][Math]::Ceiling(($Clusters + 2) * 2 / $BytesPerSector)
if ($FatSectors -lt $FatNeeded) {
    Fail ("FAT too small: {0} sectors, need {1}" -f $FatSectors, $FatNeeded)
}

Write-Host '=== mkfat-ng (WP2-2) ==='
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
Set-U16 19 $TotalSectors
$img[21] = 0xF0                       # media：可移动盘
Set-U16 22 $FatSectors
Set-U16 24 18                         # sectors/track
Set-U16 26 2                          # heads
Set-U32 28 0                          # hidden sectors
Set-U32 32 0                          # total sectors (32-bit)；<= 65535 时为 0
$img[36] = 0x00                       # drive number（软盘）
$img[37] = 0x00
$img[38] = 0x29                       # extended boot signature
Set-U32 39 ([uint32]0x4E473230)       # volume id "NG02"
$label = [Text.Encoding]::ASCII.GetBytes('ASTERFAT16 ')
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
Set-DirEntry $RootStart 0 'ASTERFAT16 ' 0x08 0 0
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
if ([BitConverter]::ToUInt16($check, 19) -ne $TotalSectors) { Fail 'BPB total sectors mismatch' }

$hello = [Text.Encoding]::ASCII.GetString($check, ($DataStart + 0) * 512, 18)
if ($hello -ne "hello from fat16`r`n") { Fail 'HELLO.TXT content mismatch' }
$a = [Text.Encoding]::ASCII.GetString($check, ($DataStart + 1) * 512, 6)
if ($a -ne "dir a`n") { Fail 'DIR/A.TXT content mismatch' }

Write-Host ("files: HELLO.TXT (18 bytes), DIR/A.TXT (6 bytes)")
Write-Host ("mkfat-ng: PASS -> {0} ({1} bytes)" -f $Out, $check.Length)
exit 0
