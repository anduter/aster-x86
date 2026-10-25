# =============================================================================
# release.ps1 - 发版流水线：bump version.h → changelog → build/签名 → 校验和 → tag
#
# 明确不做的事：
#   - 不 push（远端可能不存在；推送命令由脚本打印，由人确认后执行）
#   - 不伪造 CI：产物只代表"本机构建 + 签名成功"
#
# 前置：
#   - 工作区干净（-AllowDirty 可跳过，但发版不推荐）
#   - 签名私钥就位：keys/shell_private.pem（tools/gen-key.ps1 生成，gitignored）
#   - 交叉工具链 / nasm / openssl 就位（见 README）
#
# 用法：
#   powershell -NoProfile -File tools/release.ps1 -Version 0.72.0
#   powershell -NoProfile -File tools/release.ps1 -Version 0.72.0 -NoBuild       # 只 bump+changelog+commit+tag
#   powershell -NoProfile -File tools/release.ps1 -Version 0.72.0 -NoCommit -NoTag -NoBuild  # 只看文件改动
#   powershell -NoProfile -File tools/release.ps1 -Version 0.72.0 -DryRun        # 打印计划，不落盘
#
# 产物（dist/ 已 gitignore）：
#   dist/release/floppy.img  kernel.bin  user_shell.elf  shell.sig  kernel.sig
#   dist/release/SHA256SUMS  RELEASE.md
#   docs/changelog/v<version>.md（跟踪）
#
# 退出码：0 = 成功，1 = 失败
# =============================================================================
param(
    [Parameter(Mandatory=$true)][string]$Version,
    [switch]$NoCommit,
    [switch]$NoTag,
    [switch]$NoBuild,
    [switch]$AllowDirty,
    [switch]$DryRun,
    [string]$Repo = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

function Step([string]$msg) { Write-Host ''; Write-Host ("--- {0} ---" -f $msg) -ForegroundColor Cyan }
function Info([string]$msg) { Write-Host ("  {0}" -f $msg) -ForegroundColor DarkGray }
function Fail([string]$msg) { throw ("release: {0}" -f $msg) }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path (Join-Path $Repo 'build.ps1'))) { Fail "$Repo 不像仓库根（找不到 build.ps1）" }
$root = (Resolve-Path $Repo).Path
$startDir = (Get-Location).Path
Set-Location $root

if ($Version -notmatch '^\d+\.\d+\.\d+$') { Fail ("-Version 必须是 x.y.z（收到 '{0}'）" -f $Version) }
$tagName = "v$Version"
$versionFile = 'src/kernel/core/version.h'
$changelogFile = "docs/changelog/$tagName.md"

if (-not $DryRun) {
    if (-not (Test-Path $versionFile)) { Fail "找不到 $versionFile" }
    if (-not (Test-Path 'keys/shell_private.pem') -and -not $NoBuild) {
        Fail '缺少签名私钥 keys/shell_private.pem（先跑 tools/gen-key.ps1，或用 CI secret 注入）'
    }
    if ((& git tag -l $tagName) -eq $tagName) { Fail "tag $tagName 已存在（发版不可覆盖；请换版本号）" }
    $dirty = @(& git -c core.quotepath=false status --porcelain)
    if ($dirty.Count -gt 0 -and -not $AllowDirty) {
        $dirty | Select-Object -First 20 | ForEach-Object { Info $_ }
        Fail '工作区不干净：先提交/ stash，或显式 -AllowDirty（发版不推荐）'
    }
}

Write-Host ''
Write-Host ("=== release {0} ===" -f $tagName) -ForegroundColor Cyan
Info ("repo    : {0}" -f $root)
Info ("head    : {0}" -f ((& git log -1 --format='%h %s') -join ' '))
Info ("version : {0}" -f $Version)
if ($DryRun) { Write-Host '  (DryRun：只打印计划，不修改任何文件)' -ForegroundColor Yellow }

# ---- 1. bump version.h ----
Step '1. bump version.h'
$versionText = [IO.File]::ReadAllText((Join-Path $root $versionFile))
$m = [regex]::Match($versionText, '(?m)^(#define\s+OS_VERSION\s+")[^"]*(")')
if (-not $m.Success) { Fail "在 $versionFile 里找不到 #define OS_VERSION" }
$oldVersion = $m.Groups[0].Value
$newDefine = $m.Groups[1].Value + $Version + $m.Groups[2].Value
Info ("{0} -> {1}" -f $oldVersion.Trim(), $newDefine.Trim())
if (-not $DryRun) {
    $newText = $versionText.Substring(0, $m.Index) + $newDefine + $versionText.Substring($m.Index + $m.Length)
    [IO.File]::WriteAllText((Join-Path $root $versionFile), $newText, (New-Object Text.UTF8Encoding($false)))
}

# ---- 2. changelog（git log） ----
Step '2. changelog'
$prevTag = (& git describe --tags --abbrev=0 2>$null)
$range = if ($LASTEXITCODE -eq 0 -and $prevTag) { "$prevTag..HEAD" } else { 'HEAD' }
$logLines = @(& git log --no-decorate --oneline $range 2>$null | Select-Object -First 200)
if ($logLines.Count -eq 0) { $logLines = @('(no commits)') }
$date = Get-Date -Format 'yyyy-MM-dd'
$cl = New-Object System.Collections.Generic.List[string]
$cl.Add("# $tagName ($date)")
$cl.Add('')
if ($prevTag) { $cl.Add("Changes since $prevTag.") } else { $cl.Add('Initial release.') }
$cl.Add('')
foreach ($line in $logLines) { $cl.Add("- $line") }
$cl.Add('')
$cl.Add('## Verification')
$cl.Add('')
$cl.Add('- Built and signed locally with `build.ps1`; regression run with `build.ps1 -Test`.')
$cl.Add('- CI status is tracked separately; this file does not claim a CI run.')
$changelogText = ($cl -join "`n") + "`n"
if (-not $DryRun) {
    $clDir = Split-Path -Parent (Join-Path $root $changelogFile)
    New-Item -ItemType Directory -Force -Path $clDir | Out-Null
    [IO.File]::WriteAllText((Join-Path $root $changelogFile), $changelogText, (New-Object Text.UTF8Encoding($false)))
}
Info ("{0}（{1} 条提交）" -f $changelogFile, $logLines.Count)

# ---- 3. commit（只提交 version.h + changelog） ----
$committed = $false
Step '3. commit'
if ($NoCommit) {
    Write-Host '  -NoCommit：跳过提交（下面的 tag 也会跳过，除非你手动提交）' -ForegroundColor Yellow
} elseif ($DryRun) {
    Info ("git commit --only {0} {1}" -f $versionFile, $changelogFile)
} else {
    & git add -- $versionFile $changelogFile
    if ($LASTEXITCODE -ne 0) { Fail 'git add 失败' }
    # --only：即使别的会话 staged 了文件，也只提交这两个路径
    & git commit --only -m "[release] $tagName" -- $versionFile $changelogFile
    if ($LASTEXITCODE -ne 0) { Fail 'git commit 失败（pre-commit 拦截？见 docs/ci.md）' }
    $committed = $true
    Info ("committed {0}" -f ((& git rev-parse --short HEAD)))
}

# ---- 4. build + sign + checksums ----
$releaseDir = Join-Path $root 'dist/release'
$artifactNames = @('floppy.img', 'kernel.bin', 'user_shell.elf', 'shell.sig', 'kernel.sig')
if ($NoBuild) {
    Step '4. build + sign'
    Write-Host '  -NoBuild：跳过构建/签名/校验和（只做版本与 tag）' -ForegroundColor Yellow
} elseif ($DryRun) {
    Step '4. build + sign'
    Info 'build.ps1 -ObjDir obj/release -DistDir dist/release'
    Info 'sha256 -> dist/release/SHA256SUMS'
} else {
    Step '4. build + sign'
    $buildLog = Join-Path $root 'obj/release-build.log'
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'obj') | Out-Null
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $buildOut = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') `
            -ObjDir 'obj/release' -DistDir 'dist/release' -QmpPort 4695 -SerialLog 'serial-release.log' 2>&1 | Out-String)
        $buildCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
    }
    [IO.File]::WriteAllText($buildLog, $buildOut, (New-Object Text.UTF8Encoding($false)))
    if ($buildCode -ne 0) {
        ($buildOut -split "`r?`n" | Select-Object -Last 25) | ForEach-Object { Info $_ }
        Fail ("build.ps1 失败（exit={0}），日志：{1}" -f $buildCode, $buildLog)
    }
    $km = [regex]::Match($buildOut, '=== ok: .*\(kernel (\d+) bytes\) ===')
    Info ("build ok；kernel {0} bytes" -f $(if ($km.Success) { $km.Groups[1].Value } else { '?' }))

    Step '5. checksums'
    New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
    foreach ($name in @('kernel.bin', 'user_shell.elf', 'shell.sig', 'kernel.sig')) {
        $srcFile = Join-Path $root ('obj/release/' + $name)
        if (Test-Path $srcFile) { Copy-Item -LiteralPath $srcFile -Destination (Join-Path $releaseDir $name) -Force }
    }
    $missing = @($artifactNames | Where-Object { -not (Test-Path (Join-Path $releaseDir $_)) })
    if ($missing.Count -gt 0) { Fail ("发布产物缺失：{0}" -f ($missing -join ', ')) }

    $sums = New-Object System.Collections.Generic.List[string]
    foreach ($name in $artifactNames) {
        $h = (Get-FileHash -LiteralPath (Join-Path $releaseDir $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        $sums.Add(("{0} *{1}" -f $h, $name))
        Info ("{0}  {1}" -f $h.Substring(0, 16), $name)
    }
    [IO.File]::WriteAllText((Join-Path $releaseDir 'SHA256SUMS'), (($sums -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))

    # 公钥模数指纹（发布说明里引用，便于和 keys/shell_public.pem 核对）
    $fingerprint = 'unknown'
    $pub = Join-Path $root 'keys/shell_public.pem'
    if (Test-Path $pub) {
        $modHex = ((& openssl rsa -pubin -in $pub -noout -modulus 2>$null) -join "`n") -replace '(?s).*Modulus\s*=\s*', ''
        $modHex = $modHex.Trim().ToLowerInvariant()
        if ($modHex.Length -eq 512) {
            $sha = [Security.Cryptography.SHA256]::Create()
            $fingerprint = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($modHex))) -replace '-', '').ToLowerInvariant()
            $sha.Dispose()
        }
    }
    $releaseNotes = $changelogText + "`n## Artifacts`n`n" + (($artifactNames | ForEach-Object { "- ``$_``" }) -join "`n") +
        "`n`nSHA-256: see ``SHA256SUMS``.`n`nSigning modulus fingerprint: ``sha256:$fingerprint```n"
    [IO.File]::WriteAllText((Join-Path $releaseDir 'RELEASE.md'), $releaseNotes, (New-Object Text.UTF8Encoding($false)))
    Info ("artifact dir : {0}" -f $releaseDir)
    Info ("modulus fp   : sha256:{0}" -f $fingerprint)
}

# ---- 6. tag ----
Step '6. tag'
if ($NoTag) {
    Write-Host '  -NoTag：跳过打 tag' -ForegroundColor Yellow
} elseif ($DryRun) {
    Info ("git tag -a {0} -m `"release {0}`"" -f $tagName)
} else {
    if (-not $committed) {
        Fail '-NoCommit 时不能自动打 tag（tag 会指向未包含版本号的提交）；请手动提交后再发版，或去掉 -NoCommit'
    }
    $headVersion = (& git show ('HEAD:' + $versionFile) 2>$null) -join "`n"
    if ($headVersion -notmatch [regex]::Escape($Version)) {
        Fail 'HEAD 里的 version.h 不包含本次版本号（提交没包含 bump？）'
    }
    & git tag -a $tagName -m "release $tagName"
    if ($LASTEXITCODE -ne 0) { Fail "git tag $tagName 失败" }
    Info ("tagged {0} -> {1}" -f $tagName, ((& git rev-parse --short HEAD)))
}

Write-Host ''
Write-Host ("=== release {0} OK ===" -f $tagName) -ForegroundColor Green
Write-Host '未 push。确认后手动执行：' -ForegroundColor Yellow
Write-Host '  git push origin main'
Write-Host ("  git push origin {0}" -f $tagName)
Write-Host ''
Write-Host '发布前自查：' -ForegroundColor Cyan
Write-Host '  - dist/release/SHA256SUMS 与 keys/shell_public.pem 的模数指纹一致'
Write-Host '  - build.ps1 -Test 连续 2 次全 PASS（发版必须；本脚本不代替回归）'
Write-Host '  - GitHub CI 是否真的跑过：以 Actions 页面为准，不要凭本地结果声称 CI 通过'

Set-Location $startDir
exit 0
