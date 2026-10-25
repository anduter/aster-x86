# =============================================================================
# ci.ps1 - 统一检查入口：提交卫生（静态）+ 构建/回归（完整）
#
# 分两层，因为依赖完全不同：
#   -Quick  静态检查，< 10s，只需要 git + PowerShell
#           → GitHub 托管 runner（ubuntu-latest）可跑
#   -Full   静态 + build.ps1 + N 次 build.ps1 -Test（默认 2 次）
#           → 需要 tools/i686-elf 与 QEMU，只能本机 / 自托管 runner
#           见 docs/ci.md
#
# 静态检查（失败即 exit 1）：
#   1. 工作区干净（git status --porcelain 为空）；-AllowDirty 降级为警告
#   2. 所有被跟踪的 *.ps1/*.psm1 以 EF BB BF 开头，且 Parser::ParseFile 0 错误
#      历史无 BOM 的纯 ASCII 脚本列在 $BomLegacy 里豁免（见 docs/ci.md）；
#      被 pre-commit 改动过的文件必须补 BOM，-StrictBom 可全局强制，-FixBom 批量补
#   3. 被跟踪文件里没有构建产物/日志：
#      .obj-* .dist-* *.img *.o *.bin *.elf *.map serial-*.log logs-*.log
#   4. tools/check-deps.ps1 0 违规
#
# 完整检查（追加）：
#   5. build.ps1 成功
#   6. 连续 2 次 build.ps1 -Test 全 PASS 且检查项数一致（默认 98/98）
#      隔离产物：-ObjDir .obj-ci -DistDir .dist-ci -QmpPort 4691 -SerialLog serial-ci.log
#      结束时清理（-KeepArtifacts 保留现场）
#   7. -FlakeRuns N：追加 tools/flake-hunt.ps1 -Runs N（只报告，不作门禁）
#
# 用法：
#   powershell -NoProfile -File tools/ci.ps1 -Quick
#   powershell -NoProfile -File tools/ci.ps1 -Full
#   pwsh -NoProfile -File tools/ci.ps1 -Quick -AllowDirty     # CI（setup 后工作区可能不干净）
#   powershell -NoProfile -File tools/ci.ps1 -Full -TestRepeat 3 -ExpectChecks 98
#   powershell -NoProfile -File tools/ci.ps1 -Quick -StrictBom
#   powershell -NoProfile -File tools/ci.ps1 -Quick -FixBom   # 给历史文件补 BOM（单独提交）
#
# 退出码：0 = PASS，1 = FAIL
# =============================================================================
param(
    [switch]$Quick,
    [switch]$Full,
    [int]$FlakeRuns = 0,
    [switch]$AllowDirty,
    [switch]$StrictBom,
    [switch]$FixBom,
    [switch]$KeepArtifacts,
    [int]$TestRepeat = 2,
    [int]$ExpectChecks = 0,       # 0 = 用第一次 -Test 的项数自动对齐
    [int]$MinChecks = 90,
    [string]$PowerShellExe = 'powershell',
    [string]$ObjDir = '.obj-ci',
    [string]$DistDir = '.dist-ci',
    [int]$QmpPort = 4691,
    [string]$SerialLog = 'serial-ci.log',
    [string]$Repo = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Quick -and -not $Full) {
    Write-Host 'ci: 未指定 -Quick/-Full，默认执行 -Quick' -ForegroundColor DarkYellow
    $Quick = $true
}
$Mode = if ($Full) { 'full' } else { 'quick' }

# ---- 仓库根：默认取本脚本的上级目录（跨平台，不写死 D:\OS_Project） ----
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path (Join-Path $Repo 'build.ps1'))) { throw "ci: $Repo 不像仓库根（找不到 build.ps1）" }
$root = (Resolve-Path $Repo).Path
$startDir = (Get-Location).Path
Set-Location $root

$OnWindows = ($env:OS -eq 'Windows_NT')
# 用当前 PowerShell 宿主跑子脚本：CI（pwsh/Linux）与本地（PS5.1）都不写死解释器名
$psHost = (Get-Process -Id $PID).Path
if (-not $psHost) { $psHost = $PowerShellExe }

$failures = New-Object System.Collections.Generic.List[string]
$artifactsCreated = $false
$swAll = [Diagnostics.Stopwatch]::StartNew()
$tmpRoot = Join-Path ([IO.Path]::GetTempPath()) ('ci-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null

function Report([string]$label, [bool]$ok, [string]$detail) {
    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    $color = if ($ok) { 'Green' } else { 'Red' }
    Write-Host ("  [{0}] {1}" -f $tag, $label) -ForegroundColor $color -NoNewline
    if ($detail) { Write-Host ("  ({0})" -f $detail) -ForegroundColor DarkGray } else { Write-Host '' }
    if (-not $ok) { $failures.Add($label) }
}

function Note([string]$msg) { Write-Host ("  [warn] {0}" -f $msg) -ForegroundColor Yellow }
function Sub([string]$msg) { Write-Host ("         {0}" -f $msg) -ForegroundColor DarkGray }

function Show-Tail([string]$text, [int]$lines = 25) {
    if (-not $text) { return }
    Sub '---- tail ----'
    foreach ($l in (@($text -split "`r?`n") | Select-Object -Last $lines)) { Sub $l }
    Sub '--------------'
}
function Phase([string]$msg) { Write-Host ''; Write-Host ("--- {0} ---" -f $msg) -ForegroundColor Cyan }

function Invoke-Script([string]$exe, [string[]]$argList, [string]$logPath) {
    # *> 会把子进程 stderr 变成 ErrorRecord；EAP=Stop 时会在 redirect 期间抛，
    # 所以这里临时降级，退出码仍然由 $LASTEXITCODE 说话。
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @argList *> $logPath
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
    }
    return $code
}

function Test-TcpFree([int]$Port) {
    try {
        $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Parse('127.0.0.1'), $Port)
        $l.Start(); $l.Stop()
        return $true
    } catch { return $false }
}

function Test-UdpFree([int]$Port) {
    try {
        $u = New-Object Net.Sockets.UdpClient($Port)
        $u.Close()
        return $true
    } catch { return $false }
}

function Remove-CiArtifacts {
    if ($KeepArtifacts -or -not $artifactsCreated) { return }
    foreach ($i in 1..3) {
        $left = @()
        foreach ($p in @((Join-Path $root $ObjDir), (Join-Path $root $DistDir), (Join-Path $root $SerialLog))) {
            if (Test-Path -LiteralPath $p) {
                try { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction Stop }
                catch { $left += $p }
            }
        }
        if ($left.Count -eq 0) { break }
        Start-Sleep -Milliseconds 400
    }
    if ($left.Count -gt 0) {
        Note ("清理未完成（可能 QEMU 仍占用，稍后手工删除）：{0}" -f ($left -join ', '))
    } else {
        Sub ("已清理 {0} {1} {2}" -f $ObjDir, $DistDir, $SerialLog)
    }
}

# 历史上就没有 BOM 的纯 ASCII 脚本（PS5.1 仍能解析）。新增/改动的文件不在豁免之列：
# pre-commit 对暂存文件是严格模式，所以任何被 touch 的文件都会自动补上 BOM。
# 全部补齐后删掉这张表，并去掉 docs/ci.md 里的"历史豁免"说明。
$BomLegacy = @(
    'sign_image.ps1',
    'tools/arch-move.ps1',
    'tools/auto-accept-soak.ps1',
    'tools/converge-includes.ps1',
    'tools/e1000-probe.ps1',
    'tools/error-path-check.ps1',
    'tools/fix-includes.ps1',
    'tools/gensyms.ps1',
    'tools/mkfat16.ps1',
    'tools/net_peer_test.ps1',
    'tools/session-run.ps1',
    'tools/session-status.ps1',
    'tools/soak-file.ps1',
    'tools/soak.ps1',
    'tools/stress.ps1',
    'tools/watch-changes.ps1'
)

$Forbidden = @(
    @{ Re = '(^|/)\.obj-';                        Label = '.obj-* (构建产物)' },
    @{ Re = '(^|/)\.dist-';                       Label = '.dist-* (构建产物)' },
    @{ Re = '\.img$';                             Label = '*.img (磁盘镜像)' },
    @{ Re = '\.o$';                               Label = '*.o (目标文件)' },
    @{ Re = '\.bin$';                             Label = '*.bin (裸二进制)' },
    @{ Re = '\.elf$';                             Label = '*.elf (ELF)' },
    @{ Re = '\.map$';                             Label = '*.map (链接 map)' },
    @{ Re = '(^|/)serial-[^/]*\.log$';            Label = 'serial-*.log (串口日志)' },
    @{ Re = '(^|/)logs-[^/]*\.log$';              Label = 'logs-*.log (构建日志)' }
)

function Get-ForbiddenHit([string]$rel) {
    foreach ($f in $Forbidden) { if ($rel -match $f.Re) { return $f.Label } }
    return $null
}

# 无 BOM 的文件在 PS5.1 里按 ANSI 解码，中文注释会被解成乱码；
# 为了拿到可信的 parse 结果，检查时先补一个临时 BOM 再 ParseFile。
function Test-Ps1Parse([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $parsePath = $path
    if (-not $hasBom) {
        $buf = New-Object byte[] ($bytes.Length + 3)
        $buf[0] = 0xEF; $buf[1] = 0xBB; $buf[2] = 0xBF
        [Array]::Copy($bytes, 0, $buf, 3, $bytes.Length)
        $parsePath = Join-Path $tmpRoot ([guid]::NewGuid().ToString('N') + '.ps1')
        [IO.File]::WriteAllBytes($parsePath, $buf)
    }
    $tokens = $null
    $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($parsePath, [ref]$tokens, [ref]$errors)
    return @{ HasBom = $hasBom; Errors = @($errors) }
}

Write-Host ''
Write-Host ("=== CI ({0}) ===" -f $Mode) -ForegroundColor Cyan
Write-Host ("  repo  : {0}" -f $root) -ForegroundColor DarkGray
Write-Host ("  head  : {0}" -f ((& git log -1 --format='%h %s') -join ' ')) -ForegroundColor DarkGray
Write-Host ("  host  : {0} / PowerShell {1} / {2}" -f $psHost, $PSVersionTable.PSVersion, $env:OS) -ForegroundColor DarkGray

$exitCode = 1
try {
    # =========================================================================
    # 静态检查 1/4：工作区干净
    # =========================================================================
    Phase 'static 1/4: worktree'
    $dirty = @(& git -c core.quotepath=false status --porcelain)
    $dirtyOk = ($dirty.Count -eq 0)
    if (-not $dirtyOk -and $AllowDirty) {
        Note ("-AllowDirty：工作区有 {0} 项未提交改动，跳过干净检查" -f $dirty.Count)
        foreach ($d in ($dirty | Select-Object -First 10)) { Sub $d }
        if ($dirty.Count -gt 10) { Sub ("… ({0} more)" -f ($dirty.Count - 10)) }
        $dirtyOk = $true
    }
    Report 'worktree clean' $dirtyOk ("{0} 项改动" -f $dirty.Count)
    if (-not $dirtyOk -and $dirty.Count -gt 0) {
        foreach ($d in ($dirty | Select-Object -First 20)) { Sub $d }
        if ($dirty.Count -gt 20) { Sub ("… ({0} more)" -f ($dirty.Count - 20)) }
        Sub '先提交或 stash；只想跑检查时用 -AllowDirty'
    }

    # =========================================================================
    # 静态检查 2/4：*.ps1/*.psm1 必须有 UTF-8 BOM 且能被解析
    # =========================================================================
    Phase 'static 2/4: BOM + parse'
    $trackedPs = @(& git -c core.quotepath=false ls-files '*.ps1' '*.psm1')

    if ($FixBom) {
        $fixed = 0
        foreach ($rel in $trackedPs) {
            $ps1Path = Join-Path $root $rel
            if (-not (Test-Path -LiteralPath $ps1Path)) { continue }
            $bytes = [IO.File]::ReadAllBytes($ps1Path)
            if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { continue }
            $buf = New-Object byte[] ($bytes.Length + 3)
            $buf[0] = 0xEF; $buf[1] = 0xBB; $buf[2] = 0xBF
            [Array]::Copy($bytes, 0, $buf, 3, $bytes.Length)
            [IO.File]::WriteAllBytes($ps1Path, $buf)
            Sub ("BOM added: {0}" -f $rel)
            $fixed++
        }
        Write-Host ("  [fix] 给 {0} 个文件补了 UTF-8 BOM，请单独提交（[chore] BOM），不要混进功能提交" -f $fixed) -ForegroundColor Yellow
    }

    $noBom = @()
    $parseBad = @()
    $legacy = @()
    $legacyNonAscii = @()
    foreach ($rel in $trackedPs) {
        $ps1Path = Join-Path $root $rel
        if (-not (Test-Path -LiteralPath $ps1Path)) { continue }
        $res = Test-Ps1Parse $ps1Path
        if (-not $res.HasBom) {
            if (($BomLegacy -contains $rel) -and -not $StrictBom) {
                $legacy += $rel
                $bytes = [IO.File]::ReadAllBytes($ps1Path)
                foreach ($b in $bytes) { if ($b -gt 0x7F) { $legacyNonAscii += $rel; break } }
            } else {
                $noBom += $rel
            }
        }
        if ($res.Errors.Count -gt 0) {
            $e = $res.Errors[0]
            $parseBad += ("{0}:{1}: {2}" -f $rel, $e.Extent.StartLineNumber, $e.Message)
        }
    }

    $bomOk = ($noBom.Count -eq 0 -and $parseBad.Count -eq 0)
    Report ("BOM + parse ({0} files)" -f $trackedPs.Count) $bomOk ("missing BOM: {0}, parse errors: {1}" -f $noBom.Count, $parseBad.Count)
    foreach ($f in $noBom) { Sub ("no BOM: {0}" -f $f) }
    foreach ($f in $parseBad) { Sub ("parse : {0}" -f $f) }
    if ($noBom.Count -gt 0) {
        Sub '修复：powershell -NoProfile -File tools/ci.ps1 -FixBom  然后重新 git add'
        Sub '原因：PS5.1 对无 BOM 的 UTF-8 按 ANSI 解码，中文注释/字符串会解析成乱码甚至语法错误'
    }
    if ($legacy.Count -gt 0) {
        Note ("{0} 个历史脚本无 BOM（纯 ASCII，可解析；名单见本脚本 `$BomLegacy 与 docs/ci.md 的'历史豁免'）" -f $legacy.Count)
        if ($legacyNonAscii.Count -gt 0) {
            Note ("其中含非 ASCII 字节（有乱码风险，应尽快补 BOM）：{0}" -f ($legacyNonAscii -join ', '))
        }
        Sub '全局强制：-StrictBom；批量补齐：-FixBom（单独提交）'
    }

    # =========================================================================
    # 静态检查 3/4：禁用文件（构建产物 / 日志）
    # =========================================================================
    Phase 'static 3/4: forbidden files'
    $trackedAll = @(& git -c core.quotepath=false ls-files)
    $badFiles = @()
    foreach ($rel in $trackedAll) {
        $hit = Get-ForbiddenHit $rel
        if ($hit) { $badFiles += ("{0}  <- {1}" -f $rel, $hit) }
    }
    Report ("no build artifacts ({0} tracked)" -f $trackedAll.Count) ($badFiles.Count -eq 0) ("bad: {0}" -f $badFiles.Count)
    foreach ($f in $badFiles) { Sub $f }
    if ($badFiles.Count -gt 0) { Sub '修复：git rm --cached <path>（并在 .gitignore 里补规则）' }

    # =========================================================================
    # 静态检查 4/4：模块边界
    # =========================================================================
    Phase 'static 4/4: module boundaries (check-deps)'
    $depScript = Join-Path $root 'tools/check-deps.ps1'
    $depLog = Join-Path $tmpRoot 'check-deps.log'
    $depCode = Invoke-Script $psHost (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $depScript, '-Repo', $root)) $depLog
    $depText = if (Test-Path $depLog) { [IO.File]::ReadAllText($depLog) } else { '' }
    $depViol = '?'
    $m = [regex]::Match($depText, 'module include violations:\s*(\d+)')
    if ($m.Success) { $depViol = $m.Groups[1].Value }
    $depOk = ($depCode -eq 0 -and $depViol -eq '0')
    Report 'check-deps 0 violations' $depOk ("exit={0}, violations={1}" -f $depCode, $depViol)
    if (-not $depOk) {
        foreach ($line in ($depText -split "`r?`n")) { if ($line.Trim()) { Sub $line } }
    } else {
        $cm = [regex]::Match($depText, '(?m)^checked:.*$')
        if ($cm.Success) { Sub $cm.Value }
    }

    # =========================================================================
    # 完整检查：构建 + N 次回归
    # =========================================================================
    if ($Full -and $failures.Count -eq 0) {
        Phase 'full 1/3: preflight'
        $pfOk = $true
        if (-not $OnWindows) {
            Report 'platform is Windows' $false ("{0} 上无法构建（缺 QEMU / i686-elf），-Full 只能在 Windows 本机或自托管 runner 跑" -f $env:OS)
            $pfOk = $false
        } else {
            $tcPath = Join-Path $root 'tools/i686-elf/bin'
            $haveTc = ((Test-Path (Join-Path $tcPath 'i686-elf-gcc.exe')) -or ((Get-Command i686-elf-gcc -ErrorAction SilentlyContinue) -ne $null))
            Report 'i686-elf toolchain' $haveTc ("tools/i686-elf/bin 或 PATH")
            if (-not $haveTc) { $pfOk = $false; Sub '见 README / docs/ci.md：自托管 runner 需要 tools/i686-elf（约 3.5GB）' }
            $haveNasm = ((Get-Command nasm -ErrorAction SilentlyContinue) -ne $null)
            Report 'nasm' $haveNasm ''
            if (-not $haveNasm) { $pfOk = $false }
            $haveQemu = ((Get-Command qemu-system-i386 -ErrorAction SilentlyContinue) -ne $null)
            Report 'qemu-system-i386' $haveQemu ''
            if (-not $haveQemu) { $pfOk = $false }
            $echoFree = ((Test-UdpFree 12345) -and (Test-TcpFree 12345))
            Report 'echo port 12345 free' $echoFree '同一时刻只能跑一个 -Test'
            if (-not $echoFree) { $pfOk = $false; Sub '另一个会话的 -Test 在跑？等它结束再试' }
            $qmpFree = Test-TcpFree $QmpPort
            Report ("qmp port {0} free" -f $QmpPort) $qmpFree ''
            if (-not $qmpFree) { $pfOk = $false }
        }
        if (-not $pfOk) { throw 'full: preflight 未通过' }

        $buildLogDir = Join-Path $root '.session/ci'
        New-Item -ItemType Directory -Force -Path $buildLogDir | Out-Null
        $commonArgs = @('-ObjDir', $ObjDir, '-DistDir', $DistDir, '-QmpPort', "$QmpPort", '-SerialLog', $SerialLog)

        # ---- full 2/3: build ----
        Phase 'full 2/3: build'
        $artifactsCreated = $true
        $log = Join-Path $buildLogDir 'build.log'
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-Script $PowerShellExe (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'build.ps1')) + $commonArgs) $log
        $sw.Stop()
        $out = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
        $built = ($code -eq 0 -and ($out -match '=== ok: '))
        Report 'build.ps1' $built ("exit={0}, {1:N1}s, log={2}" -f $code, $sw.Elapsed.TotalSeconds, $log)
        if (-not $built) { Show-Tail $out; throw 'full: 构建失败' }

        # ---- full 3/3: 2x -Test ----
        Phase ("full 3/3: {0}x build.ps1 -Test" -f $TestRepeat)
        $expect = $ExpectChecks
        $counts = @()
        for ($i = 1; $i -le $TestRepeat; $i++) {
            $log = Join-Path $buildLogDir ("test-{0}.log" -f $i)
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $code = Invoke-Script $PowerShellExe (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'build.ps1'), '-Test') + $commonArgs) $log
            $sw.Stop()
            $out = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
            $vm = [regex]::Match($out, '=== TEST PASS \((\d+)/\1\) ===')
            $n = if ($vm.Success) { [int]$vm.Groups[1].Value } else { 0 }
            $ok = ($code -eq 0 -and $vm.Success -and $n -ge $MinChecks)
            if ($i -eq 1 -and $ExpectChecks -eq 0 -and $vm.Success) { $expect = $n }
            if ($ok -and $expect -gt 0 -and $n -ne $expect) {
                # 项数漂移：说明有人加了检查（或这轮少跑了），必须显式确认，不能悄悄放过
                $ok = $false
                Sub ("检查项数 {0} != 期望 {1}（新增检查请同步 CI 的 -ExpectChecks）" -f $n, $expect)
            }
            $detail = if ($vm.Success) { "{0}/{0}" -f $n } elseif ($code -ne 0) { "exit={0}" -f $code } else { 'no verdict' }
            Report ("-Test run {0}/{1}" -f $i, $TestRepeat) $ok ("{0}, {1:N1}s, log={2}" -f $detail, $sw.Elapsed.TotalSeconds, $log)
            $counts += $(if ($vm.Success) { $n } else { 0 })
            if (-not $ok) { Show-Tail $out }
            if (-not $ok) { throw ("full: -Test run {0} 失败" -f $i) }
        }
        $summary = ($counts | ForEach-Object { "{0}/{0}" -f $_ }) -join ' , '
        Write-Host ("  [ok] 回归：{0} 次连续 PASS（{1}）" -f $TestRepeat, $summary) -ForegroundColor Green

        # ---- 可选：flake-hunt（只报告） ----
        if ($FlakeRuns -gt 0) {
            Phase ("optional: flake-hunt -Runs {0}" -f $FlakeRuns)
            $log = Join-Path $buildLogDir 'flake-hunt.log'
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $code = Invoke-Script $PowerShellExe (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools/flake-hunt.ps1'), '-Runs', "$FlakeRuns", '-Repo', $root)) $log
            $sw.Stop()
            $out = if (Test-Path $log) { [IO.File]::ReadAllText($log) } else { '' }
            $fm = [regex]::Match($out, 'runs=(\d+) PASS=(\d+) FAIL=(\d+) HOST/BUILD=(\d+)')
            if ($fm.Success) {
                Note ("flake-hunt: runs={0} PASS={1} FAIL={2} HOST={3}（仅报告，不作门禁；已知偶发 #PF 见 docs/parallel-sessions.md）" -f `
                        $fm.Groups[1].Value, $fm.Groups[2].Value, $fm.Groups[3].Value, $fm.Groups[4].Value)
                $sm = [regex]::Match($out, '(?m)^summary: (.+)$')
                if ($sm.Success) { Sub ("summary: {0}" -f $sm.Groups[1].Value.Trim()) }
            } else {
                Note ("flake-hunt 未产出汇总（exit={0}），日志：{1}" -f $code, $log)
            }
        }

        # ---- 收尾：Full 跑完工作区仍应干净（防止产物漏进被跟踪路径） ----
        $dirtyAfter = @(& git -c core.quotepath=false status --porcelain)
        Report 'worktree still clean after -Full' ($dirtyAfter.Count -eq 0) ("{0} 项" -f $dirtyAfter.Count)
        if ($dirtyAfter.Count -gt 0) { foreach ($d in $dirtyAfter) { Sub $d } }
    }

    $exitCode = if ($failures.Count -eq 0) { 0 } else { 1 }
}
catch {
    $msg = $_.Exception.Message
    if ($_.InvocationInfo -and $_.InvocationInfo.ScriptLineNumber) {
        Sub ("at line {0}: {1}" -f $_.InvocationInfo.ScriptLineNumber, $msg)
    } else {
        Sub $msg
    }
    if ($_.ScriptStackTrace) {
        foreach ($sline in ($_.ScriptStackTrace -split "`r?`n")) { if ($sline.Trim()) { Sub ("  stack: {0}" -f $sline.Trim()) } }
    }
    $failures.Add("fatal: $msg")
    $exitCode = 1
}
finally {
    Remove-CiArtifacts
    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
    Set-Location $startDir
    $swAll.Stop()
    Write-Host ''
    if ($exitCode -eq 0) {
        Write-Host ("elapsed: {0:N1}s" -f $swAll.Elapsed.TotalSeconds) -ForegroundColor DarkGray
        Write-Host '=== CI PASS ===' -ForegroundColor Green
    } else {
        Write-Host '--- failures ---' -ForegroundColor Red
        foreach ($f in $failures) { Write-Host ("  * {0}" -f $f) -ForegroundColor Red }
        Write-Host ("elapsed: {0:N1}s" -f $swAll.Elapsed.TotalSeconds) -ForegroundColor DarkGray
        Write-Host ("=== CI FAIL ({0}) ===" -f $failures.Count) -ForegroundColor Red
    }
}
exit $exitCode
