# =============================================================================
# release-ng.ps1 - Aster D0 本地发布包（E4 / WP4-3）
#
# 一条命令：
#   1. tools/abi-check.ps1（ABI 1.0 冻结门禁）
#   2. tools/build-ng.ps1（内核 + 7 个用户 ELF + disk.img）
#   3. 生成发布目录：ELF、disk.img、SDK 头、ABI 1.0、文档、
#      RELEASE-NOTES.md、SHA256SUMS
#   4. 重新计算 SHA-256 并逐文件校验
#
# 明确不做：
#   - 不 push、不创建远端 release、不打 tag；
#   - 不做签名（D0 是本地开发包；签名更新属 B 域/D1）；
#   - 不跑 QEMU/浸泡（那是 WP4-4 的证据）。
#
# 产物目录默认 dist-ng/release/d0（在 .gitignore 保护下，只本地保留）。
#
# usage:
#   powershell -File tools/release-ng.ps1
#   powershell -File tools/release-ng.ps1 -SkipBuild          # 复用已有 ELF/disk.img
#   powershell -File tools/release-ng.ps1 -AllowDirty         # 允许 tracked 改动（不推荐）
#   powershell -File tools/release-ng.ps1 -OutDir dist-ng/release/d0-test
#
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$DistDir = 'dist-ng',
    [string]$ObjDir = 'obj-ng',
    [string]$OutDir = 'dist-ng/release/d0',
    [string]$Version = '1.0.0-d0',
    [switch]$SkipBuild,
    [switch]$SkipAbiCheck,
    [switch]$AllowDirty
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

function Step([string]$msg) {
    Write-Host ''
    Write-Host ("--- {0} ---" -f $msg) -ForegroundColor Cyan
}
function Info([string]$msg) { Write-Host ("  {0}" -f $msg) }
function Fail([string]$msg) {
    Write-Host ("release-ng: FAIL ({0})" -f $msg) -ForegroundColor Red
    exit 1
}

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $Repo)) { Fail ("repo not found: {0}" -f $Repo) }
$root = (Resolve-Path -LiteralPath $Repo).Path

$buildScript = Join-Path $root 'tools/build-ng.ps1'
$abiScript = Join-Path $root 'tools/abi-check.ps1'
$abiHeader = Join-Path $root 'docs/abi/abi-1.0.h'
$abiDoc = Join-Path $root 'docs/abi/abi-1.0.md'
$d0Doc = Join-Path $root 'docs/release/d0.md'
$sdkDir = Join-Path $root 'sdk/include/aster'
foreach ($p in @($buildScript, $abiScript, $abiHeader, $abiDoc, $d0Doc, $sdkDir)) {
    if (-not (Test-Path -LiteralPath $p)) { Fail ("missing required path: {0}" -f $p) }
}

# ---- 输出目录安全 -----------------------------------------------------------
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $root 'dist-ng/release')).TrimEnd('\', '/')
if ([IO.Path]::IsPathRooted($OutDir)) {
    $outFull = [IO.Path]::GetFullPath($OutDir)
} else {
    $outFull = [IO.Path]::GetFullPath((Join-Path $root $OutDir))
}
$outFull = $outFull.TrimEnd('\', '/')
$releasePrefix = $releaseRoot + [IO.Path]::DirectorySeparatorChar
if (-not $outFull.StartsWith($releasePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    Fail ("-OutDir must be under dist-ng/release (got {0})" -f $outFull)
}
if ($outFull -eq $releaseRoot) {
    Fail '-OutDir must be a child of dist-ng/release, not the root itself'
}

# ---- git 信息 ---------------------------------------------------------------
$gitBranch = ''
$gitCommit = ''
$gitFull = ''
$gitDescribe = ''
$trackedDirty = @()
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$gitBranch = (@(& git -C $root rev-parse --abbrev-ref HEAD 2>$null) -join '').Trim()
$gitCommit = (@(& git -C $root rev-parse --short HEAD 2>$null) -join '').Trim()
$gitFull = (@(& git -C $root rev-parse HEAD 2>$null) -join '').Trim()
$gitDescribe = (@(& git -C $root describe --tags --always 2>$null) -join '').Trim()
$trackedDirty = @(& git -C $root status --porcelain --untracked-files=no 2>$null)
$ErrorActionPreference = $prevEap
if ($gitCommit -eq '') { Fail 'git rev-parse failed (repo is not a git checkout?)' }
if ($gitBranch -ne 'ng') {
    Write-Host ("release-ng: warning: branch is '{0}', expected 'ng'" -f $gitBranch) -ForegroundColor Yellow
}
if ($trackedDirty.Count -gt 0) {
    if (-not $AllowDirty) {
        $trackedDirty | Select-Object -First 10 | ForEach-Object { Info $_ }
        Fail 'tracked working tree is dirty; commit first or pass -AllowDirty'
    }
    Write-Host ("release-ng: warning: {0} tracked changes (AllowDirty)" -f $trackedDirty.Count) -ForegroundColor Yellow
}

$buildDate = Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'

Write-Host ''
Write-Host '=== release-ng (D0 local release) ==='
Info ("repo    : {0}" -f $root)
Info ("branch  : {0}" -f $gitBranch)
Info ("commit  : {0} ({1})" -f $gitCommit, $gitDescribe)
Info ("version : {0}" -f $Version)
Info ("out     : {0}" -f $outFull)
Info ("dirty   : {0}" -f $(if ($trackedDirty.Count -gt 0) { 'yes (-AllowDirty)' } else { 'no' }))

$shell = Get-Command powershell -ErrorAction SilentlyContinue
if (-not $shell) { $shell = Get-Command pwsh -ErrorAction SilentlyContinue }
if (-not $shell) { Fail 'neither powershell nor pwsh found for child builds' }

function Invoke-Child([string]$script, [string[]]$extra) {
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script) + $extra
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $lines = @(& $shell.Source @argv 2>&1 | ForEach-Object { [string]$_ })
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    return [pscustomobject]@{ Code = $code; Lines = $lines }
}

# ---- 1. ABI 冻结门禁 --------------------------------------------------------
if (-not $SkipAbiCheck) {
    Step '1. abi-check (ABI 1.0 freeze)'
    $abi = Invoke-Child $abiScript @('-Repo', $root)
    $abi.Lines | ForEach-Object { Write-Host $_ }
    if ($abi.Code -ne 0) { Fail ("abi-check failed (exit {0})" -f $abi.Code) }
} else {
    Write-Host ''
    Write-Host '--- 1. abi-check skipped (-SkipAbiCheck) ---' -ForegroundColor Yellow
}

# ---- 2. build-ng ------------------------------------------------------------
$elf = Join-Path $root (Join-Path $DistDir 'aster-ng.elf')
$disk = Join-Path $root (Join-Path $DistDir 'disk.img')
if (-not $SkipBuild) {
    Step '2. build-ng'
    $build = Invoke-Child $buildScript @('-Repo', $root, '-DistDir', $DistDir, '-ObjDir', $ObjDir)
    # 完整构建日志太长：保留关键行 + 末尾 12 行，其余折叠。
    $keep = @($build.Lines | Where-Object {
        $_ -match '^(=== |mode:|clang:|lld:|ELF64:|build-ng:|disk image:)'
    })
    $keep | ForEach-Object { Write-Host $_ }
    if ($build.Code -ne 0) {
        Write-Host '--- build output (tail) ---' -ForegroundColor Red
        $build.Lines | Select-Object -Last 20 | ForEach-Object { Write-Host $_ }
        Fail ("build-ng failed (exit {0})" -f $build.Code)
    }
    Info ("build: {0} bytes (ELF), {1} bytes (disk)" -f `
        (Get-Item -LiteralPath $elf).Length, (Get-Item -LiteralPath $disk).Length)
} else {
    Write-Host ''
    Write-Host '--- 2. build-ng skipped (-SkipBuild) ---' -ForegroundColor Yellow
}
if (-not (Test-Path -LiteralPath $elf)) { Fail ("missing kernel ELF: {0}" -f $elf) }
if (-not (Test-Path -LiteralPath $disk)) { Fail ("missing disk image: {0}" -f $disk) }
$elfSize = (Get-Item -LiteralPath $elf).Length
$diskSize = (Get-Item -LiteralPath $disk).Length
if ($elfSize -le 0 -or $diskSize -le 0) { Fail 'empty build artifact' }

# ---- 3. 组装发布目录 --------------------------------------------------------
Step '3. assemble release tree'
if (Test-Path -LiteralPath $outFull) {
    Info ("clean: {0}" -f $outFull)
    Remove-Item -LiteralPath $outFull -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $outFull | Out-Null

function Copy-Into([string]$src, [string]$rel) {
    if (-not (Test-Path -LiteralPath $src)) { Fail ("missing source: {0}" -f $src) }
    $dst = Join-Path $outFull $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Info ("copy: {0}" -f $rel)
}

# 本地保护：发布目录整体不进入 git（根 .gitignore 只覆盖 dist/，不覆盖 dist-ng/release）。
[IO.File]::WriteAllText((Join-Path $outFull '.gitignore'),
    "# generated D0 release artifacts (local only, never commit)`n*`n",
    (New-Object Text.UTF8Encoding($false)))

Copy-Into $elf 'aster-ng.elf'
Copy-Into $disk 'disk.img'
Copy-Into $abiHeader 'abi/abi-1.0.h'
Copy-Into $abiDoc 'abi/abi-1.0.md'
Copy-Into $d0Doc 'docs/d0.md'
Copy-Into (Join-Path $root 'docs/sdk/getting-started.md') 'docs/sdk-getting-started.md'
Copy-Into (Join-Path $root 'docs/design/architecture.md') 'docs/architecture.md'
Copy-Into (Join-Path $root 'docs/design/evolution.md') 'docs/evolution.md'
Copy-Into (Join-Path $root 'docs/design/README.md') 'docs/design-readme.md'
Copy-Into (Join-Path $root 'LICENSE') 'LICENSE'
Copy-Into (Join-Path $root 'sdk/README.md') 'sdk/README.md'

$sdkHeaders = @(Get-ChildItem -LiteralPath $sdkDir -Filter '*.h' -File | Sort-Object Name)
if ($sdkHeaders.Count -lt 8) {
    Fail ("SDK header set too small ({0}); expected at least 8" -f $sdkHeaders.Count)
}
foreach ($h in $sdkHeaders) {
    Copy-Into $h.FullName ('sdk/include/aster/' + $h.Name)
}

# ---- 4. RELEASE-NOTES.md ----------------------------------------------------
Step '4. RELEASE-NOTES.md'
$notesTemplate = @'
# Aster D0 — Release Notes

- 版本：`@@VERSION@@`（native ABI **1.0.0**，冻结）
- 构建时间：@@DATE@@
- 分支：`@@BRANCH@@`
- 提交：`@@COMMIT@@`（`@@FULL@@`，describe: `@@DESCRIBE@@`）
- 构建机：本机本地构建；**未推送、未签名**（D0 开发包）
- 内核 ELF：`@@ELFSIZE@@` 字节
- 磁盘镜像：`@@DISKSIZE@@` 字节（8 MB FAT16，IDE 主盘）

## 内容

| 路径 | 内容 |
|---|---|
| `aster-ng.elf` | 64 位内核（含 ring3 shell / displayd / wm / sessiond / demo / proc 等 ELF） |
| `disk.img` | 8 MB FAT16 数据盘（`/mnt/disk`） |
| `abi/abi-1.0.h` | ABI 1.0 机器可读冻结头 |
| `abi/abi-1.0.md` | ABI 1.0 冻结规范（调用约定/四类命名空间/错误码/上限/版本策略） |
| `sdk/include/aster/*.h` | SDK 头（8 个；ABI 1.0 消费端） |
| `sdk/README.md` | SDK 概览 |
| `docs/d0.md` | D0 范围/出口/已知限制/安装运行/校验 |
| `docs/architecture.md`、`docs/evolution.md`、`docs/design-readme.md` | 设计基线快照 |
| `docs/sdk-getting-started.md` | SDK 上手（v3 display + session 通道） |
| `LICENSE` | 许可证 |
| `SHA256SUMS` | 上述文件（除本文件、`.gitignore`、`SHA256SUMS`）的 SHA-256 |

## 运行（QEMU）

```text
qemu-system-x86_64 -kernel aster-ng.elf -m 256 -cpu max -vga std ^
  -drive file=disk.img,format=raw,if=ide -serial stdio -display gtk
```

无 GUI 的串口冒烟：

```text
qemu-system-x86_64 -kernel aster-ng.elf -m 256 -cpu max -vga std ^
  -drive file=disk.img,format=raw,if=ide -display none ^
  -serial file:serial.log -no-reboot
```

预期串口包含 `ng: ring3 hello`、`ng: shell ready` 与各 `WP* selftest PASS`；
具体证据见 `docs/d0.md` 和仓库 `docs/claims/`。

## 校验

```powershell
# 逐文件 SHA-256 校验（在发布目录内）
Get-Content SHA256SUMS | ForEach-Object {
    $h, $p = $_ -split '  ', 2
    $a = (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLowerInvariant()
    if ($a -ne $h) { throw "SHA256 mismatch: $p" }
}
"OK"
```

Linux/WSL 可用 `sha256sum -c SHA256SUMS`。

## 已知限制（D0）

1. **WM 死循环**：协作式调度无抢占，WM 自旋会冻结整机；S1 的“崩溃”三场景
   PASS，“卡死”记为 KNOWN（`tools/s1-fault.ps1`）。
2. **用户态 #PF/#GP 仍 panic**：syscall 的坏指针返回 `EFAULT`，但应用直接解引用
   坏地址/执行特权指令会 panic（ABI 1.0 不承诺用户 fault 隔离）。
3. **sessiond 无接管**：杀 sessiond 后应用/displayd/WM 成孤儿继续跑，注册表/
   重启策略消失（architecture 开放决策 2）。
4. **权限模型是 root-only**：所有进程 uid=0；ticket 是 bearer token，无 cap/revoke。
5. **无安装器/包启动**：SDK 应用进镜像仍靠构建期 `.incbin`；`-SdkDemo` 是 demo
   harness，不是产品部署机制。
6. **静态 ELF64 / 无 POSIX**：无动态链接、无 mmap/dup/pipe/socket/sleep；
   POSIX Profile 属 D1。
7. **QEMU 矩阵有限**：stdvga 1280×800×32 + PIIX3 IDE + PS/2 键盘；鼠标/IME/
   真机不在 D0。
8. **ABI 1.0 的延后项**见 `abi/abi-1.0.md` §10（未分配号 ENOSYS、cap、每进程
   uid/fd、用户 fault、版本查询等）。

完整说明见 `docs/d0.md`。
'@

$notes = $notesTemplate
$notes = $notes.Replace('@@VERSION@@', $Version)
$notes = $notes.Replace('@@DATE@@', $buildDate)
$notes = $notes.Replace('@@BRANCH@@', $gitBranch)
$notes = $notes.Replace('@@COMMIT@@', $gitCommit)
$notes = $notes.Replace('@@FULL@@', $gitFull)
$notes = $notes.Replace('@@DESCRIBE@@', $gitDescribe)
$notes = $notes.Replace('@@ELFSIZE@@', [string]$elfSize)
$notes = $notes.Replace('@@DISKSIZE@@', [string]$diskSize)
[IO.File]::WriteAllText((Join-Path $outFull 'RELEASE-NOTES.md'), $notes,
    (New-Object Text.UTF8Encoding($false)))
Info 'write: RELEASE-NOTES.md'

# ---- 5. SHA256SUMS ----------------------------------------------------------
Step '5. SHA256SUMS'
$sumPath = Join-Path $outFull 'SHA256SUMS'
$payload = @(Get-ChildItem -LiteralPath $outFull -Recurse -File |
    Where-Object { $_.Name -ne 'SHA256SUMS' -and $_.Name -ne '.gitignore' } |
    Sort-Object FullName)
if ($payload.Count -lt 15) { Fail ("payload too small ({0} files)" -f $payload.Count) }

$sumLines = New-Object System.Collections.Generic.List[string]
foreach ($f in $payload) {
    $rel = $f.FullName.Substring($outFull.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $sumLines.Add(($hash + '  ' + $rel))
}
[IO.File]::WriteAllText($sumPath, (($sumLines -join "`n") + "`n"),
    (New-Object Text.UTF8Encoding($false)))
Info ("write: SHA256SUMS ({0} files)" -f $sumLines.Count)

# ---- 6. 自校验 --------------------------------------------------------------
Step '6. verify SHA256SUMS'
$verifyLines = [IO.File]::ReadAllLines($sumPath)
if ($verifyLines.Count -ne $sumLines.Count) {
    Fail ("SHA256SUMS line count {0}, expected {1}" -f $verifyLines.Count, $sumLines.Count)
}
$seen = @{}
$verified = 0
foreach ($line in $verifyLines) {
    if ($line.Trim().Length -eq 0) { continue }
    $parts = $line -split '  ', 2
    if ($parts.Count -ne 2) { Fail ("bad SHA256SUMS line: {0}" -f $line) }
    $want = $parts[0]
    $rel = $parts[1]
    if ($seen.ContainsKey($rel)) { Fail ("duplicate path in SHA256SUMS: {0}" -f $rel) }
    $seen[$rel] = $true
    $p = Join-Path $outFull ($rel.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $p)) { Fail ("missing file from SHA256SUMS: {0}" -f $rel) }
    $got = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($got -ne $want) { Fail ("SHA256 mismatch: {0}" -f $rel) }
    $verified++
}
if ($verified -ne $payload.Count) {
    Fail ("verified {0} files, payload has {1}" -f $verified, $payload.Count)
}
Write-Host ("[ok] {0} files verified" -f $verified) -ForegroundColor DarkGreen

# ---- 7. 摘要 ----------------------------------------------------------------
Step '7. summary'
$tree = @(Get-ChildItem -LiteralPath $outFull -Recurse -File |
    Where-Object { $_.Name -ne '.gitignore' } |
    ForEach-Object { $_.FullName.Substring($outFull.Length + 1).Replace('\', '/') } |
    Sort-Object)
$tree | ForEach-Object { Info $_ }
Write-Host ''
Write-Host ("release-ng: PASS -> {0}" -f $outFull) -ForegroundColor Green
Write-Host ("release-ng: {0} files, SHA256SUMS verified; local only, nothing pushed" -f $verified) -ForegroundColor Green
exit 0
