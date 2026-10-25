# mkfat16.ps1 - create/populate/verify a FAT16 image without external tools
#
#   .\tools\mkfat16.ps1 -Path dist\fat.img -SizeMB 8
#   .\tools\mkfat16.ps1 -Path dist\fat.img -File HOST.TXT -Content "hello from host"
#   .\tools\mkfat16.ps1 -Path dist\fat.img -List
#   .\tools\mkfat16.ps1 -Path dist\fat.img -Extract HOST.TXT
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$SizeMB = 8,
    [string]$File,
    [string]$Content,
    [switch]$List,
    [string]$Extract
)

$ErrorActionPreference = 'Stop'
$sector = 512
$totalSectors = $SizeMB * 1024 * 2       # 8MB -> 16384
$reserved = 1
$fats = 2
$rootEntries = 512
$rootSectors = [int]($rootEntries * 32 / $sector)

# FAT size: pick the smallest that covers (clusters + 2) * 2 bytes
$fatSectors = 64
for ($i = 0; $i -lt 10; $i++) {
    $dataStart = $reserved + $fats * $fatSectors + $rootSectors
    $clusters = [int](($totalSectors - $dataStart) / 1)
    $need = [int][Math]::Ceiling((($clusters + 2) * 2) / $sector)
    if ($need -le $fatSectors) { break }
    $fatSectors = $need
}
$dataStart = $reserved + $fats * $fatSectors + $rootSectors
$clusters = $totalSectors - $dataStart
$rootStart = $reserved + $fats * $fatSectors

if ($List -or $Extract) {
    if (-not (Test-Path $Path)) { throw "image not found: $Path" }
    $img = [IO.File]::ReadAllBytes((Resolve-Path $Path))

    $bps = [BitConverter]::ToUInt16($img, 11)
    $spc = $img[13]
    $rsv = [BitConverter]::ToUInt16($img, 14)
    $nfats = $img[16]
    $rootEnt = [BitConverter]::ToUInt16($img, 17)
    $fatSz = [BitConverter]::ToUInt16($img, 22)
    $rootSec = [int][Math]::Ceiling($rootEnt * 32 / $bps)
    $rootLba = $rsv + $nfats * $fatSz
    $dataLba = $rootLba + $rootSec

    $found = $false
    for ($i = 0; $i -lt $rootEnt; $i++) {
        $off = ($rootLba * $bps) + ($i * 32)
        if ($img[$off] -eq 0) { break }
        if ($img[$off] -eq 0xE5) { continue }
        $attr = $img[$off + 11]
        if (($attr -band 0x0F) -eq 0x0F) { continue }
        if (($attr -band 0x08) -ne 0) { continue }

        $raw = [Text.Encoding]::ASCII.GetString($img, $off, 11)
        $name = $raw.Substring(0, 8).Trim()
        $ext = $raw.Substring(8, 3).Trim()
        if ($ext) { $name = "$name.$ext" }
        $cluster = [BitConverter]::ToUInt16($img, $off + 26)
        $size = [BitConverter]::ToUInt32($img, $off + 28)

        if ($List) {
            Write-Host ("{0,-12} {1,8} bytes  cluster={2}" -f $name, $size, $cluster)
        }

        if ($Extract -and $name -eq $Extract.ToUpper()) {
            $start = ($dataLba + ($cluster - 2) * $spc) * $bps
            $text = [Text.Encoding]::ASCII.GetString($img, $start, $size)
            Write-Output $text
            $found = $true
            break
        }
        $found = $found -or ($Extract -eq $null)
    }
    if ($Extract -and -not $found) { throw "file not found: $Extract" }
    return
}

# ---- build a fresh image ----
$bytes = New-Object byte[] ($totalSectors * $sector)
$bytes[0] = 0xEB; $bytes[1] = 0x3C; $bytes[2] = 0x90
[Text.Encoding]::ASCII.GetBytes('MSDOS5.0') | ForEach-Object -Begin { $i = 3 } -Process { $bytes[$i++] = $_ }

function Set-U16([byte[]]$a, [int]$o, [int]$v) { $a[$o] = $v -band 0xFF; $a[$o + 1] = ($v -shr 8) -band 0xFF }
function Set-U32([byte[]]$a, [int]$o, [long]$v) {
    $a[$o] = $v -band 0xFF; $a[$o + 1] = ($v -shr 8) -band 0xFF
    $a[$o + 2] = ($v -shr 16) -band 0xFF; $a[$o + 3] = ($v -shr 24) -band 0xFF
}

Set-U16 $bytes 11 $sector
$bytes[13] = 1                          # sectors per cluster
Set-U16 $bytes 14 $reserved
$bytes[16] = $fats
Set-U16 $bytes 17 $rootEntries
Set-U16 $bytes 19 $totalSectors    # total16（8MB = 16384 扇区，放得下）
$bytes[21] = 0xF8
Set-U16 $bytes 22 $fatSectors
Set-U16 $bytes 24 18
Set-U16 $bytes 26 2
Set-U32 $bytes 32 0
$bytes[36] = 0x80
$bytes[38] = 0x29
Set-U32 $bytes 39 0x12345678
[Text.Encoding]::ASCII.GetBytes('NO NAME    ') | ForEach-Object -Begin { $i = 43 } -Process { $bytes[$i++] = $_ }
[Text.Encoding]::ASCII.GetBytes('FAT16   ') | ForEach-Object -Begin { $i = 54 } -Process { $bytes[$i++] = $_ }
$bytes[62] = 0xF4                       # hlt
$bytes[510] = 0x55; $bytes[511] = 0xAA

# FAT: entry0 media, entry1 EOC
for ($f = 0; $f -lt $fats; $f++) {
    $base = ($reserved + $f * $fatSectors) * $sector
    Set-U16 $bytes $base 0xFFF8
    Set-U16 $bytes ($base + 2) 0xFFFF
}

# 可选：往根目录写一个文件（占 1 个簇就够测）
if ($File -and $Content) {
    $raw = [Text.Encoding]::ASCII.GetBytes($Content)
    $name = $File.ToUpper()
    $dot = $name.LastIndexOf('.')
    if ($dot -ge 0) { $baseName = $name.Substring(0, $dot); $ext = $name.Substring($dot + 1) } else { $baseName = $name; $ext = '' }
    $baseName = $baseName.PadRight(8).Substring(0, 8)
    $ext = $ext.PadRight(3).Substring(0, 3)

    $slot = $rootStart * $sector
    [Text.Encoding]::ASCII.GetBytes(($baseName + $ext)) | ForEach-Object -Begin { $i = 0 } -Process { $bytes[$slot + $i] = $_; $i++ }
    $bytes[$slot + 11] = 0x20
    Set-U16 $bytes ($slot + 26) 2                       # 起始簇 2
    Set-U32 $bytes ($slot + 28) $raw.Length

    # 簇 2 数据
    $dataOff = ($dataStart + 0) * $sector
    [Array]::Copy($raw, 0, $bytes, $dataOff, $raw.Length)
    # FAT[2] = EOF
    for ($f = 0; $f -lt $fats; $f++) {
        $base = ($reserved + $f * $fatSectors) * $sector
        Set-U16 $bytes ($base + 4) 0xFFFF
    }
}

[IO.File]::WriteAllBytes($Path, $bytes)
Write-Host ("[OK] FAT16 image: {0} ({1} MB, {2} clusters, data_lba={3})" -f $Path, $SizeMB, $clusters, $dataStart)
