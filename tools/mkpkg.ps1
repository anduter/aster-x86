# =============================================================================
# mkpkg.ps1 - 把一组文件打成一个签名的 .pkg 容器（并可选生成内核 C 数组）
#
# 输出格式见 src/kernel/include/pkg.h：
#   header(64) | manifest 文本 | 文件表(76/项) | 数据区 | RSA-2048 签名(256)
# 签名覆盖 [0, sig_off) 的全部字节（OpenSSL dgst -sha256 -sign，PKCS#1 v1.5）。
# 私钥永不进 git（keys/shell_private.pem，见 keys/README.md）。
#
# 用法（build.ps1 会为 echosvc 等包各调一次）：
#   powershell -NoProfile -File tools/mkpkg.ps1 `
#       -Name echosvc -Version 1.0.0 -Type service -Entry echosvc `
#       -Requires "service:supervisor,abi:1.3" -Capabilities "CAP_SVC_REGISTER" `
#       -Account root -RootDir build/pkg/echosvc -Files echosvc.conf `
#       -Out obj/pkg/echosvc-1.0.0.pkg -OutC obj/pkg/echosvc_pkg.c
#
#   -ModulusC obj/pkg_modulus.c    # 生成内核用的公钥模数数组（pkg_modulus[256]）
# =============================================================================
param(
    [Parameter(Mandatory=$true)][string]$Name,
    [Parameter(Mandatory=$true)][string]$Version,
    [ValidateSet('service','app')][string]$Type = 'service',
    [Parameter(Mandatory=$true)][string]$Entry,
    [string]$Requires = '',
    [string]$Capabilities = '',
    [string]$Account = 'root',
    [string[]]$Files = @(),
    [string]$RootDir = '.',
    [string]$Key = 'keys/shell_private.pem',
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$OutC = '',
    [string]$ModulusC = '',
    [string]$ArrayPrefix = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

function Fail([string]$msg) { Write-Host ("mkpkg: {0}" -f $msg) -ForegroundColor Red; exit 1 }

# -Files "a,b" 被 -File 调用时是单个字符串，这里统一拆开
$Files = @($Files | ForEach-Object { $_ -split '[,;]' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

if ($Name -notmatch '^[a-z0-9][a-z0-9._-]*$') { Fail "包名非法（小写字母/数字/._-）：$Name" }
if ($Name.Length -ge 32) { Fail "包名太长（<32）：$Name" }
if ($Version -notmatch '^[0-9][0-9A-Za-z._-]*$') { Fail "版本号非法：$Version" }
if ($Version.Length -ge 16) { Fail "版本号太长（<16）：$Version" }
if ($Entry -notmatch '^[A-Za-z0-9][A-Za-z0-9._/-]*$') { Fail "entry 非法：$Entry" }
if ($Files.Count -eq 0) { Fail '至少要给一个 -Files' }
if ($Files.Count -gt 16) { Fail '-Files 最多 16 个（与内核 PKG_FILE_MAX 一致）' }
if ($Account -notmatch '^[a-z_][a-z0-9_-]*$') { Fail "account 非法：$Account" }
if (-not (Test-Path -LiteralPath $Key)) { Fail "签名私钥不存在：$Key（先跑 tools/gen-key.ps1）" }
if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) { Fail '找不到 openssl' }

# ---- 读文件、算哈希 ----
$entries = @()
foreach ($f in $Files) {
    if ($f -match '(^|/)\.\.(/|$)' -or $f.StartsWith('/') -or $f.Contains(':')) {
        Fail "文件路径必须是相对路径且不含 ..：$f"
    }
    $src = Join-Path $RootDir $f
    if (-not (Test-Path -LiteralPath $src)) { Fail "文件不存在：$src" }
    $bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $src).Path)
    if ($bytes.Length -gt 65536) { Fail "单个文件超过 64KB（FS_FILE_MAX）：$f" }
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = $sha.ComputeHash($bytes)
    $nameBytes = New-Object byte[] 32
    $nb = [Text.Encoding]::ASCII.GetBytes((Split-Path -Leaf $f))
    [Array]::Copy($nb, 0, $nameBytes, 0, [Math]::Min($nb.Length, 31))
    $entries += [pscustomobject]@{
        Name = (Split-Path -Leaf $f)
        Bytes = $bytes
        Hash = $hash
        NameBytes = $nameBytes
        Mode = 0x1A4   # 0644
    }
}
# 同一包里不允许重名（安装到同一个目录里会互相覆盖）
$dup = $entries | Group-Object Name | Where-Object Count -gt 1
if ($dup) { Fail ("包内文件名重复：{0}" -f (($dup | ForEach-Object Name) -join ', ')) }

# ---- manifest（key=value，行序固定） ----
$fileList = ($entries | ForEach-Object Name) -join ','
$manifestLines = @(
    "name=$Name",
    "version=$Version",
    "type=$Type",
    "entry=$Entry",
    "requires=$Requires",
    "capabilities=$Capabilities",
    "account=$Account",
    "files=$fileList"
)
$manifestText = ($manifestLines -join "`n") + "`n"
$manifestBytes = [Text.Encoding]::ASCII.GetBytes($manifestText)
if ($manifestBytes.Length -ge 640) { Fail "manifest 太长（<640）：$($manifestBytes.Length)" }

# ---- 布局 ----
function Align16([int]$v) { return [int]([Math]::Ceiling($v / 16.0) * 16) }

$dataLen = 0
foreach ($e in $entries) { $dataLen += $e.Bytes.Length }
$headerSize = 64
$manifestOff = $headerSize
$filetabOff = Align16($manifestOff + $manifestBytes.Length)
$dataOff = Align16($filetabOff + $entries.Count * 76)
$sigOff = $dataOff + $dataLen
$totalLen = $sigOff + 256

$buf = New-Object byte[] $totalLen
function PutU32([byte[]]$b, [int]$off, [uint32]$v) {
    $b[$off] = [byte]($v -band 0xFF)
    $b[$off + 1] = [byte](($v -shr 8) -band 0xFF)
    $b[$off + 2] = [byte](($v -shr 16) -band 0xFF)
    $b[$off + 3] = [byte](($v -shr 24) -band 0xFF)
}

# header
$buf[0] = [byte][char]'A'; $buf[1] = [byte][char]'P'
$buf[2] = [byte][char]'K'; $buf[3] = [byte][char]'G'
PutU32 $buf 4 1
PutU32 $buf 8 $(if ($Type -eq 'service') { 1 } else { 2 })
PutU32 $buf 12 $manifestOff
PutU32 $buf 16 $manifestBytes.Length
PutU32 $buf 20 $entries.Count
PutU32 $buf 24 $filetabOff
PutU32 $buf 28 $dataOff
PutU32 $buf 32 $dataLen
PutU32 $buf 36 $sigOff
PutU32 $buf 40 $totalLen
PutU32 $buf 44 0
$signer = [Text.Encoding]::ASCII.GetBytes('aster-pkg-v1')
[Array]::Copy($signer, 0, $buf, 48, [Math]::Min($signer.Length, 16))

# manifest
[Array]::Copy($manifestBytes, 0, $buf, $manifestOff, $manifestBytes.Length)

# 文件表 + 数据
$dataCursor = 0
for ($i = 0; $i -lt $entries.Count; $i++) {
    $e = $entries[$i]
    $off = $filetabOff + $i * 76
    [Array]::Copy($e.NameBytes, 0, $buf, $off, 32)
    PutU32 $buf ($off + 32) $dataCursor
    PutU32 $buf ($off + 36) $e.Bytes.Length
    PutU32 $buf ($off + 40) $e.Mode
    [Array]::Copy($e.Hash, 0, $buf, $off + 44, 32)
    [Array]::Copy($e.Bytes, 0, $buf, $dataOff + $dataCursor, $e.Bytes.Length)
    $dataCursor += $e.Bytes.Length
}

# ---- 签名（覆盖 [0, sigOff)） ----
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('mkpkg-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    $bodyPath = Join-Path $tmp 'body.bin'
    $sigPath = Join-Path $tmp 'sig.bin'
    $body = New-Object byte[] $sigOff
    [Array]::Copy($buf, 0, $body, 0, $sigOff)
    [IO.File]::WriteAllBytes($bodyPath, $body)

    & openssl dgst -sha256 -sign $Key -out $sigPath $bodyPath
    if ($LASTEXITCODE -ne 0) { Fail 'openssl 签名失败' }
    $sig = [IO.File]::ReadAllBytes($sigPath)
    if ($sig.Length -ne 256) { Fail "签名长度异常：$($sig.Length)（需要 RSA-2048）" }
    [Array]::Copy($sig, 0, $buf, $sigOff, 256)
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# ---- 写 .pkg ----
$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
[IO.File]::WriteAllBytes($Out, $buf)

# ---- C 数组（内核内建包目录） ----
if ($OutC) {
    if (-not $ArrayPrefix) {
        $ArrayPrefix = 'pkg_blob_' + ($Name -replace '[^A-Za-z0-9]', '_') + '_' + ($Version -replace '[^A-Za-z0-9]', '_')
    }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('/* Auto-generated by tools/mkpkg.ps1. 不要手改。 */')
    $lines.Add("const unsigned char ${ArrayPrefix}[] = {")
    for ($i = 0; $i -lt $buf.Length; $i += 16) {
        $n = [Math]::Min(16, $buf.Length - $i)
        $slice = New-Object System.Collections.Generic.List[string]
        for ($j = 0; $j -lt $n; $j++) { $slice.Add(('0x{0:X2}' -f $buf[$i + $j])) }
        $lines.Add('    ' + ($slice -join ', ') + ',')
    }
    $lines.Add('};')
    $lines.Add("const unsigned int ${ArrayPrefix}_len = $($buf.Length);")
    $outCDir = Split-Path -Parent $OutC
    if ($outCDir -and -not (Test-Path $outCDir)) { New-Item -ItemType Directory -Force -Path $outCDir | Out-Null }
    [IO.File]::WriteAllText($OutC, (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
}

# ---- 公钥模数（内核验签用） ----
if ($ModulusC) {
    $modLine = (& openssl rsa -in $Key -noout -modulus) -join "`n"
    $modHex = ($modLine -replace '(?s).*Modulus\s*=\s*', '').Trim()
    if ($modHex.Length -ne 512) { Fail "RSA 模数长度异常：$($modHex.Length)" }
    $modBytes = New-Object byte[] 256
    for ($i = 0; $i -lt 256; $i++) { $modBytes[$i] = [Convert]::ToByte($modHex.Substring($i * 2, 2), 16) }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('/* Auto-generated by tools/mkpkg.ps1 from the build signing key. */')
    $lines.Add('const unsigned char pkg_modulus[256] = {')
    for ($i = 0; $i -lt 256; $i += 12) {
        $n = [Math]::Min(12, 256 - $i)
        $slice = New-Object System.Collections.Generic.List[string]
        for ($j = 0; $j -lt $n; $j++) { $slice.Add(('0x{0:X2}' -f $modBytes[$i + $j])) }
        $lines.Add('    ' + ($slice -join ', ') + ',')
    }
    $lines.Add('};')
    $modDir = Split-Path -Parent $ModulusC
    if ($modDir -and -not (Test-Path $modDir)) { New-Item -ItemType Directory -Force -Path $modDir | Out-Null }
    [IO.File]::WriteAllText($ModulusC, (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
}

Write-Host ("[OK] pkg {0}-{1} ({2} bytes, {3} file(s)) -> {4}" -f $Name, $Version, $buf.Length, $entries.Count, $Out)
exit 0
