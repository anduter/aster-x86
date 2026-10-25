# =============================================================================
# gen-key.ps1 - 生成/轮换镜像签名密钥对（RSA-2048，PKCS#1 v1.5 + SHA-256）
#
# 为什么需要它：
#   build.ps1 用 keys/shell_private.pem 给 user_shell.elf 与 kernel.bin 签名，
#   内核只内嵌由该密钥推导出的 256 字节模数（obj/shell_signature.c）。
#   私钥**不能进 git**：开源仓库只保留 keys/README.md 与公钥。
#
# 用法：
#   powershell -NoProfile -File tools/gen-key.ps1            # 生成（已存在则拒绝）
#   powershell -NoProfile -File tools/gen-key.ps1 -Force     # 轮换：覆盖旧密钥对
#
# 轮换后旧签名/旧镜像全部作废：必须重新 build.ps1 并重跑回归（旧公钥不再被信任）。
#
# 输出：
#   keys/shell_private.pem   PKCS#1 私钥（gitignored，本地/CI secret 注入）
#   keys/shell_public.pem    SubjectPublicKeyInfo 公钥（可提交，供审计/发布校验）
#
# 退出码：0 = 成功，1 = 失败
# =============================================================================
param(
    [string]$PrivateKey = 'keys/shell_private.pem',
    [string]$PublicKey  = 'keys/shell_public.pem',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) {
    throw 'gen-key: 找不到 openssl（需要 OpenSSL 1.1+；Windows 可用 Git for Windows 自带的那份）'
}

foreach ($p in @($PrivateKey, $PublicKey)) {
    if ((Test-Path $p) -and -not $Force) {
        throw ("gen-key: {0} 已存在；轮换密钥请显式加 -Force（旧签名会全部作废）" -f $p)
    }
}

$dir = Split-Path -Parent $PrivateKey
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

# 沿用现有格式：`openssl genrsa -traditional` 产出 PKCS#1（传统 RSA 私钥头）。
# sign_image.ps1 读取 -modulus 的方式对 PKCS#1/PKCS#8 都成立，但保持旧格式便于 diff 审计。
& openssl genrsa -traditional -out $PrivateKey 2048
if ($LASTEXITCODE -ne 0) { throw 'gen-key: openssl genrsa 失败' }

& openssl rsa -in $PrivateKey -noout -check
if ($LASTEXITCODE -ne 0) { throw 'gen-key: 私钥自检失败' }

& openssl rsa -in $PrivateKey -pubout -out $PublicKey
if ($LASTEXITCODE -ne 0) { throw 'gen-key: 导出公钥失败' }

# 指纹 = 模数的 SHA-256（与内核验签使用的 256 字节模数同一份数据）
$modHex = ((& openssl rsa -in $PrivateKey -noout -modulus) -join "`n") -replace '(?s).*Modulus\s*=\s*', ''
$modHex = $modHex.Trim()
if ($modHex.Length -ne 512) { throw ("gen-key: 模数长度异常（{0} hex，期望 512）" -f $modHex.Length) }
$sha = [Security.Cryptography.SHA256]::Create()
$fingerprint = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($modHex.ToLowerInvariant()))) -replace '-', '').ToLowerInvariant()
$sha.Dispose()

# 私钥权限尽量收紧（Windows ACL：仅当前用户；非 Windows 忽略）
if ($env:OS -eq 'Windows_NT') {
    try {
        $acl = Get-Acl $PrivateKey
        $acl.SetAccessRuleProtection($true, $false)
        $me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
        $rule = New-Object Security.AccessControl.FileSystemAccessRule($me, 'FullControl', 'Allow')
        $acl.AddAccessRule($rule)
        Set-Acl -Path $PrivateKey -AclObject $acl
    } catch {
        Write-Host ("gen-key: [warn] 收紧私钥 ACL 失败（不影响构建）：{0}" -f $_.Exception.Message) -ForegroundColor Yellow
    }
}

Write-Host ''
Write-Host '[OK] 密钥对已生成' -ForegroundColor Green
Write-Host ("  private : {0}  (gitignored；不要提交/上传)" -f (Resolve-Path $PrivateKey))
Write-Host ("  public  : {0}" -f (Resolve-Path $PublicKey))
Write-Host ("  modulus : sha256:{0}" -f $fingerprint)
Write-Host ''
Write-Host '下一步：' -ForegroundColor Cyan
Write-Host '  powershell -NoProfile -File build.ps1        # 重新签名（公钥模数会写进 obj/shell_signature.c）'
Write-Host '  powershell -NoProfile -File build.ps1 -Test # 回归（旧密钥签的镜像已作废）'
Write-Host ''
Write-Host 'CI/发布：私钥只从 secret（如 GITHUB_ENV 写入的临时文件）注入，或在 job 内用本脚本生成一次性密钥；' -ForegroundColor DarkGray
Write-Host '        开源仓库永远不含私钥。详见 keys/README.md 与 SECURITY.md。' -ForegroundColor DarkGray
