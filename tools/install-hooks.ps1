# =============================================================================
# install-hooks.ps1 - 安装提交守卫（core.hooksPath），幂等
#
#   powershell -NoProfile -File tools/install-hooks.ps1
#   powershell -NoProfile -File tools/install-hooks.ps1 -Uninstall
#
# 做的事：git config core.hooksPath tools/githooks
# 效果：git commit 时自动跑 tools/githooks/pre-commit → tools/pre-commit.ps1
#      （.ps1 BOM / 禁用文件 / src/kernel 模块边界；见 docs/ci.md）
#
# 这是**本地**配置（.git/config），不会被提交；每个克隆/每个新机器跑一次即可。
# =============================================================================
param(
    [string]$Repo = '',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path (Join-Path $Repo 'build.ps1'))) { throw "install-hooks: $Repo 不像仓库根（找不到 build.ps1）" }
$root = (Resolve-Path $Repo).Path
Set-Location $root

$hookRel = 'tools/githooks'
$hookAbs = Join-Path $root 'tools/githooks/pre-commit'

$gitDir = (& git rev-parse --git-dir 2>$null)
if ($LASTEXITCODE -ne 0) { throw "install-hooks: $root 不是 git 工作区" }

if ($Uninstall) {
    $cur = (& git config --local --get core.hooksPath 2>$null)
    if ($cur) {
        & git config --local --unset core.hooksPath
        Write-Host ("install-hooks: 已移除 core.hooksPath（原来是 {0}）" -f $cur) -ForegroundColor Yellow
    } else {
        Write-Host 'install-hooks: core.hooksPath 本来就没设置，无需卸载' -ForegroundColor DarkGray
    }
    exit 0
}

# ---- 前置检查：hook 入口必须存在 ----
if (-not (Test-Path -LiteralPath $hookAbs)) {
    Write-Host ("install-hooks: 缺少 {0}" -f $hookAbs) -ForegroundColor Red
    exit 1
}

# ---- 设置（幂等） ----
$before = (& git config --local --get core.hooksPath 2>$null)
if ($before -eq $hookRel) {
    Write-Host ("install-hooks: core.hooksPath 已经是 {0}，无需改动" -f $hookRel) -ForegroundColor DarkGray
} else {
    & git config --local core.hooksPath $hookRel
    if ($LASTEXITCODE -ne 0) { Write-Host 'install-hooks: git config 写入失败' -ForegroundColor Red; exit 1 }
    if ($before) {
        Write-Host ("install-hooks: core.hooksPath {0} -> {1}" -f $before, $hookRel) -ForegroundColor Green
    } else {
        Write-Host ("install-hooks: core.hooksPath 已设为 {0}" -f $hookRel) -ForegroundColor Green
    }
}

# ---- 验证 ----
$after = (& git config --get core.hooksPath)
Write-Host ("install-hooks: 验证 git config --get core.hooksPath -> [{0}]" -f $after) -ForegroundColor Cyan

$bytes = [IO.File]::ReadAllBytes($hookAbs)
$hasCr = $false
foreach ($b in $bytes) { if ($b -eq 0x0D) { $hasCr = $true; break } }
$shebang = [Text.Encoding]::ASCII.GetString($bytes[0..([Math]::Min(9, $bytes.Length - 1))])
if (-not $shebang.StartsWith('#!/bin/sh')) {
    Write-Host 'install-hooks: 警告：pre-commit 首行不是 #!/bin/sh，Git Bash 可能无法执行' -ForegroundColor Yellow
}
if ($hasCr) {
    Write-Host 'install-hooks: 警告：pre-commit 含 CRLF 行尾（建议保持 LF，否则克隆到 Linux 会跑不起来）' -ForegroundColor Yellow
}

# ---- 冒烟测试：暂存区为空时直接跑一次 hook（应当 0 通过） ----
$staged = @(& git diff --cached --name-only)
if ($staged.Count -eq 0) {
    $psExe = if (Get-Command powershell -ErrorAction SilentlyContinue) { 'powershell' } else { 'pwsh' }
    & $psExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/pre-commit.ps1')
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Host 'install-hooks: 冒烟测试通过（暂存区为空 → ok）' -ForegroundColor Green
    } else {
        Write-Host ("install-hooks: 冒烟测试失败（exit={0}），先修 tools/pre-commit.ps1 再提交" -f $code) -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host ("install-hooks: 暂存区有 {0} 个文件，跳过冒烟测试（提交时 hook 会跑）" -f $staged.Count) -ForegroundColor DarkGray
}

# ---- 下一步提示 ----
Write-Host ''
Write-Host '已生效。提交时钩子会自动检查（.ps1 BOM / 禁用文件 / src/kernel 模块边界）。' -ForegroundColor Cyan
Write-Host '  试一下：git commit --allow-empty -m "hook smoke test"   # 应通过' -ForegroundColor DarkGray
Write-Host '  提交前全量自检：powershell -NoProfile -File tools/ci.ps1 -Quick' -ForegroundColor DarkGray
Write-Host '  按会话限定路径提交：powershell -NoProfile -File tools/session-commit.ps1 -Name <会话名> -Message "..." -Paths p1,p2' -ForegroundColor DarkGray
exit 0
