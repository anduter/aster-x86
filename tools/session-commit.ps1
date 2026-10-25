# =============================================================================
# session-commit.ps1 - 按会话限定路径提交（防止把别的会话 staged 的文件一起提交）
#
# 事故：git commit 把别的会话 staged 的文件一起提交（已发生 3 次，如 ed33a6a
# 扫入 ipc→svc rename）。本脚本把"只提交自己的文件"变成机制。
#
# 用法：
#   powershell -NoProfile -File tools/session-commit.ps1 `
#       -Name ci -Message "add ci.ps1 + pre-commit guard" `
#       -Paths tools/ci.ps1,tools/pre-commit.ps1,docs/ci.md
#
# 行为（任一步失败即 exit 1，不做任何提交）：
#   1. -Name / -Message / -Paths 都不能为空
#   2. 暂存区里已有**不在 -Paths 中**的文件 → 直接失败并列出（这才是扫入事故的根源）
#   3. 只 git add -- <Paths>，然后确认暂存区集合 ⊆ -Paths（不可能多出别的文件）
#   4. 跑 tools/pre-commit.ps1（BOM / 禁用文件 / 模块边界）
#   5. git commit -m "[<Name>] <Message>"，打印哈希
#
# 注意：-Paths 可以给目录（如 docs），该目录下所有改动都算你的。
# 相关：docs/ci.md、docs/parallel-sessions.md 的"提交卫生"小节
# =============================================================================
param(
    [AllowEmptyString()][string]$Name = '',
    [AllowEmptyString()][string]$Message = '',
    [AllowEmptyString()][AllowEmptyCollection()][string[]]$Paths = @(),
    [string]$Repo = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

$script:pushed = $false
function Fail([string]$msg) {
    Write-Host ''
    Write-Host ("session-commit: aborted: {0}" -f $msg) -ForegroundColor Red
    if ($script:pushed) { Pop-Location -ErrorAction SilentlyContinue }
    exit 1
}

# ---- 1. 参数校验（不用 Mandatory：脚本化调用时缺参会弹交互提示，会挂住） ----
$tag = ($Name -replace '^\s*\[|\]\s*$', '').Trim()
if (-not $tag) { Fail '-Name 不能为空（例：-Name ci）' }
if (-not $Message -or -not $Message.Trim()) { Fail '-Message 不能为空' }
# `powershell -File ... -Paths a,b,c` 会把 "a,b,c" 当成**一个**字符串传进来
# （-File 不解析逗号），所以这里统一按 , 和 ; 再拆一次。
$Paths = @($Paths | ForEach-Object { $_ -split '[,;]' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Paths.Count -eq 0) {
    Fail '-Paths 不能为空（例：-Paths tools/ci.ps1,docs/ci.md）；本脚本不允许"提交全部"'
}

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path (Join-Path $Repo 'build.ps1'))) { Fail ("{0} 不像仓库根（找不到 build.ps1）" -f $Repo) }
$root = (Resolve-Path $Repo).Path
Set-Location $root
Push-Location $root
$script:pushed = $true

$gitDir = (& git rev-parse --git-dir 2>$null)
if ($LASTEXITCODE -ne 0) { Fail ("{0} 不是 git 工作区" -f $root) }

# ---- 2. 暂存区里已有别人的文件？ ----
function Normalize ([string]$p) {
    $x = $p -replace '\\', '/'
    $x = $x -replace '^\./', ''
    $x = $x.TrimEnd('/')
    return $x
}
$want = @($Paths | ForEach-Object { Normalize $_ })

$staged = @(& git -c core.quotepath=false diff --cached --name-only)
$staged = @($staged | ForEach-Object { Normalize $_ } | Where-Object { $_ })

function Test-MatchPath([string]$file) {
    foreach ($w in $want) {
        if ($file -eq $w) { return $true }
        if ($file.StartsWith($w + '/')) { return $true }   # -Paths 给了目录
    }
    return $false
}

$foreign = @($staged | Where-Object { -not (Test-MatchPath $_) })
if ($foreign.Count -gt 0) {
    Write-Host ''
    Write-Host ("session-commit: 暂存区里已有 {0} 个不属于本次提交的文件：" -f $foreign.Count) -ForegroundColor Red
    foreach ($f in $foreign) { Write-Host ("  - {0}" -f $f) -ForegroundColor Red }
    Write-Host ''
    Write-Host '这通常意味着另一个会话先 git add 过。请先决定归属，再重试：' -ForegroundColor Yellow
    Write-Host '  git restore --staged -- <file>     # 从暂存区撤出（工作区不动）' -ForegroundColor Yellow
    Write-Host '  （或把属于你的路径加进 -Paths）' -ForegroundColor Yellow
    Fail '暂存区有其它来源的文件（拒绝扫入）'
}

# ---- 3. 只 add 指定路径 ----
& git add -- @Paths
if ($LASTEXITCODE -ne 0) { Fail ("git add 失败（路径是否存在？）：{0}" -f ($Paths -join ', ')) }

$after = @(& git -c core.quotepath=false diff --cached --name-only)
$after = @($after | ForEach-Object { Normalize $_ } | Where-Object { $_ })
if ($after.Count -eq 0) { Fail ("git add 之后暂存区仍为空，检查 -Paths：{0}" -f ($Paths -join ', ')) }

$extra = @($after | Where-Object { -not (Test-MatchPath $_) })
if ($extra.Count -gt 0) {
    Write-Host ("session-commit: git add 之后出现 -Paths 之外的文件：") -ForegroundColor Red
    foreach ($f in $extra) { Write-Host ("  - {0}" -f $f) -ForegroundColor Red }
    Fail '暂存区集合 != -Paths（拒绝提交）'
}

foreach ($w in $want) {
    $hit = @($after | Where-Object { $_ -eq $w -or $_.StartsWith($w + '/') })
    if ($hit.Count -eq 0) {
        Write-Host ("session-commit: 提示：-Paths 里的 {0} 没有产生任何暂存改动（已提交？）" -f $w) -ForegroundColor DarkYellow
    }
}

Write-Host ("session-commit: {0} 个文件将提交" -f $after.Count) -ForegroundColor Cyan
foreach ($f in $after) { Write-Host ("  + {0}" -f $f) -ForegroundColor DarkGray }

# ---- 4. 提交守卫（复用 hook 的同一份检查） ----
$psExe = if (Get-Command powershell -ErrorAction SilentlyContinue) { 'powershell' } else { 'pwsh' }
& $psExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/pre-commit.ps1')
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Host 'session-commit: pre-commit 检查未通过，未提交' -ForegroundColor Red
    Pop-Location -ErrorAction SilentlyContinue
    exit 1
}

# ---- 5. 提交 ----
$msg = $Message.Trim()
if ($msg -match ('^\[{0}\]' -f [regex]::Escape($tag))) {
    $fullMsg = $msg                       # 已经带前缀，避免 [[ci]] 双前缀
} else {
    $fullMsg = "[{0}] {1}" -f $tag, $msg
}

Write-Host ''
Write-Host ("session-commit: git commit -m `"{0}`"" -f $fullMsg) -ForegroundColor Cyan
& git commit -m $fullMsg
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host ''
    Write-Host ("session-commit: git commit 失败（exit={0}）" -f $code) -ForegroundColor Red
    Pop-Location -ErrorAction SilentlyContinue
    exit 1
}

$hash = (& git rev-parse --short HEAD)
$oneline = (& git log -1 --oneline)
Write-Host ''
Write-Host ("session-commit: committed {0}" -f $hash) -ForegroundColor Green
Write-Host ("  {0}" -f $oneline) -ForegroundColor Green
Pop-Location
exit 0
