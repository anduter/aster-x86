# =============================================================================
# pkg-verify.ps1 - 宿主侧校验一个 .pkg（格式 + RSA 签名 + 每个文件的 SHA-256）
#
# 与内核里的 pkg_verify() 是同一套规则；用来在构建/发布时先发现坏包，
# 而不是等 QEMU 里安装失败。
#
# 用法：
#   powershell -NoProfile -File tools/pkg-verify.ps1 -Package obj/pkg/echosvc-1.0.0.pkg
#   powershell -NoProfile -File tools/pkg-verify.ps1 -Package x.pkg -PublicKey keys/shell_public.pem -Quiet
#
# 退出码：0 = 通过，1 = 失败（打印原因）
# =============================================================================
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [string]$PublicKey = 'keys/shell_public.pem',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

function Fail([string]$msg) {
    if (-not $Quiet) { Write-Host ("pkg-verify: FAIL: {0}" -f $msg) -ForegroundColor Red }
    exit 1
}

if (-not (Test-Path -LiteralPath $Package)) { Fail "包不存在：$Package" }
if (-not (Test-Path -LiteralPath $PublicKey)) { Fail "公钥不存在：$PublicKey" }
if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) { Fail '找不到 openssl' }

$buf = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Package).Path)
if ($buf.Length -lt 64 + 256) { Fail "文件太小：$($buf.Length)" }

function GetU32([byte[]]$b, [int]$off) {
    return ([uint32]$b[$off] -bor ([uint32]$b[$off + 1] -shl 8) -bor
            ([uint32]$b[$off + 2] -shl 16) -bor ([uint32]$b[$off + 3] -shl 24))
}

if ($buf[0] -ne [byte][char]'A' -or $buf[1] -ne [byte][char]'P' -or
    $buf[2] -ne [byte][char]'K' -or $buf[3] -ne [byte][char]'G') { Fail 'magic 不是 APKG' }

$version     = GetU32 $buf 4
$type        = GetU32 $buf 8
$manifestOff = GetU32 $buf 12
$manifestLen = GetU32 $buf 16
$fileCount   = GetU32 $buf 20
$filetabOff  = GetU32 $buf 24
$dataOff     = GetU32 $buf 28
$dataLen     = GetU32 $buf 32
$sigOff      = GetU32 $buf 36
$totalLen    = GetU32 $buf 40

if ($version -ne 1) { Fail "版本不支持：$version" }
if ($type -ne 1 -and $type -ne 2) { Fail "类型非法：$type" }
if ($totalLen -ne $buf.Length) { Fail "total_len=$totalLen 与实际 $($buf.Length) 不符" }
if ($manifestOff -lt 64 -or ($manifestOff + $manifestLen) -gt $buf.Length) { Fail 'manifest 越界' }
if ($fileCount -lt 1 -or $fileCount -gt 16) { Fail "file_count 非法：$fileCount" }
if ($filetabOff -lt 64 -or ($filetabOff + $fileCount * 76) -gt $buf.Length) { Fail '文件表越界' }
if ($dataOff -lt $filetabOff -or ($dataOff + $dataLen) -gt $buf.Length) { Fail '数据区越界' }
if ($sigOff -ne $dataOff + $dataLen) { Fail '签名偏移不连续' }
if ($sigOff + 256 -ne $totalLen) { Fail '签名长度不是 256' }

# ---- 签名 ----
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('pkgverify-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    $bodyPath = Join-Path $tmp 'body.bin'
    $sigPath = Join-Path $tmp 'sig.bin'
    $body = New-Object byte[] $sigOff
    [Array]::Copy($buf, 0, $body, 0, $sigOff)
    $sig = New-Object byte[] 256
    [Array]::Copy($buf, $sigOff, $sig, 0, 256)
    [IO.File]::WriteAllBytes($bodyPath, $body)
    [IO.File]::WriteAllBytes($sigPath, $sig)

    $out = (& openssl dgst -sha256 -verify $PublicKey -signature $sigPath $bodyPath 2>&1) -join ' '
    if ($LASTEXITCODE -ne 0 -or $out -notmatch 'Verified OK') { Fail ("签名校验失败：{0}" -f $out.Trim()) }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# ---- manifest ----
$manifestText = [Text.Encoding]::ASCII.GetString($buf, $manifestOff, $manifestLen)
$man = @{}
foreach ($line in ($manifestText -split "`n")) {
    $line = $line.Trim()
    if (-not $line) { continue }
    $i = $line.IndexOf('=')
    if ($i -le 0) { Fail "manifest 行格式错误：$line" }
    $man[$line.Substring(0, $i)] = $line.Substring($i + 1)
}
foreach ($req in @('name', 'version', 'type', 'entry', 'requires', 'capabilities', 'account', 'files')) {
    if (-not $man.ContainsKey($req)) { Fail "manifest 缺字段：$req" }
}

# ---- 文件表 + 哈希 ----
$fileNames = @($man['files'] -split ',' | Where-Object { $_ })
if ($fileNames.Count -ne $fileCount) { Fail "manifest files=$($fileNames.Count) 与 file_count=$fileCount 不符" }

$sha = [Security.Cryptography.SHA256]::Create()
$cursorExpected = 0
for ($i = 0; $i -lt $fileCount; $i++) {
    $e = $filetabOff + $i * 76
    $rawName = [Text.Encoding]::ASCII.GetString($buf, $e, 32)
    $nul = $rawName.IndexOf([char]0)
    if ($nul -ge 0) { $rawName = $rawName.Substring(0, $nul) }
    $off  = GetU32 $buf ($e + 32)
    $size = GetU32 $buf ($e + 36)
    if ($rawName -ne $fileNames[$i]) { Fail "文件表顺序/名字不符：$rawName != $($fileNames[$i])" }
    if ($off -ne $cursorExpected) { Fail "文件数据不连续：$rawName off=$off expected=$cursorExpected" }
    if ($off + $size -gt $dataLen) { Fail "文件数据越界：$rawName" }
    $chunk = New-Object byte[] $size
    [Array]::Copy($buf, $dataOff + $off, $chunk, 0, $size)
    $actual = $sha.ComputeHash($chunk)
    $stored = New-Object byte[] 32
    [Array]::Copy($buf, $e + 44, $stored, 0, 32)
    # PS5.1 的 .NET 没有 Convert.ToHexString（.NET 5+），用 BitConverter 保持兼容
    if (([BitConverter]::ToString($actual) -replace '-', '') -ne
        ([BitConverter]::ToString($stored) -replace '-', '')) { Fail "文件哈希不匹配：$rawName" }
    $cursorExpected += $size
}
if ($cursorExpected -ne $dataLen) { Fail "数据区有 $($dataLen - $cursorExpected) 字节未声明" }

if (-not $Quiet) {
    Write-Host ("pkg-verify: OK {0}" -f (Split-Path -Leaf $Package)) -ForegroundColor Green
    Write-Host ("  name={0} version={1} type={2} entry={3}" -f $man['name'], $man['version'], $man['type'], $man['entry'])
    Write-Host ("  requires={0}" -f $man['requires'])
    Write-Host ("  capabilities={0} account={1}" -f $man['capabilities'], $man['account'])
    Write-Host ("  files={0} data={1}B total={2}B" -f $fileCount, $dataLen, $buf.Length)
}
exit 0
