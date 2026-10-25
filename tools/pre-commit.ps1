# =============================================================================
# pre-commit.ps1 - 提交守卫（机制，不是约定）
#
# 由 tools/githooks/pre-commit 调用（git config core.hooksPath tools/githooks）：
#   git commit → tools/githooks/pre-commit → powershell tools/pre-commit.ps1
#
# 只检查**暂存区**（git diff --cached），拦三类真实发生过的事故：
#   1. .ps1/.psm1 丢 UTF-8 BOM → PS5.1 按 ANSI 解码，解析失败（已发生 3 次，build.ps1 常中招）
#   2. 构建产物/日志被 add 进来：
#      .obj-* .dist-* *.img *.o *.bin *.elf *.map serial-*.log logs-*.log .session/
#   3. 破坏模块边界：暂存区含 src/kernel/** 时跑 tools/check-deps.ps1（>0 违规即拒绝）
#
# 期望结果：通过 exit 0；拦截时打印 "pre-commit: blocked: ..." 与修复命令并 exit 1。
# 手工跑：powershell -NoProfile -File tools/pre-commit.ps1
# 紧急绕过：git commit --no-verify（仅限救急；之后必须补跑 tools/ci.ps1 -Quick 并修好）
# =============================================================================

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

$gitExe = (Get-Command git -ErrorAction SilentlyContinue)
if (-not $gitExe) {
    Write-Host 'pre-commit: git not found, skipping guards' -ForegroundColor Yellow
    exit 0
}
$gitExe = $gitExe.Source

$root = (& git rev-parse --show-toplevel 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $root) {
    Write-Host 'pre-commit: not inside a git worktree, skipping guards' -ForegroundColor Yellow
    exit 0
}
$root = ($root -join '').Trim()
Push-Location $root

$staged = @(& git -c core.quotepath=false diff --cached --name-only --diff-filter=ACMR)
$staged = @($staged | ForEach-Object { ($_ -replace '\\', '/').Trim() } | Where-Object { $_ })

if ($staged.Count -eq 0) {
    Write-Host 'pre-commit: ok (nothing staged)' -ForegroundColor DarkGray
    Pop-Location
    exit 0
}

$problems = New-Object System.Collections.Generic.List[string]
$hints = New-Object System.Collections.Generic.List[string]

# ---- 规则 1：禁用路径 ----
$forbidden = @(
    @{ Re = '(^|/)\.obj-';             Label = '.obj-*（构建产物，应被 .gitignore 覆盖）' },
    @{ Re = '(^|/)\.dist-';            Label = '.dist-*（构建产物）' },
    @{ Re = '\.img$';                  Label = '*.img（磁盘/软盘镜像）' },
    @{ Re = '\.o$';                    Label = '*.o（目标文件）' },
    @{ Re = '\.bin$';                  Label = '*.bin（裸二进制）' },
    @{ Re = '\.elf$';                  Label = '*.elf（ELF）' },
    @{ Re = '\.map$';                  Label = '*.map（链接 map）' },
    @{ Re = '(^|/)serial-[^/]*\.log$'; Label = 'serial-*.log（串口日志）' },
    @{ Re = '(^|/)logs-[^/]*\.log$';   Label = 'logs-*.log（构建日志）' },
    @{ Re = '(^|/)\.session/';         Label = '.session/（会话协调产物）' }
)

foreach ($rel in $staged) {
    foreach ($f in $forbidden) {
        if ($rel -match $f.Re) {
            $problems.Add(("{0}: 禁用文件（{1}）" -f $rel, $f.Label))
            $hints.Add(("git rm --cached -- '{0}'   # 保留工作区文件，只把它从提交里去掉" -f $rel))
            break
        }
    }
}

# ---- 规则 2：.ps1/.psm1 必须带 UTF-8 BOM，且能解析 ----
# 读的是 **blob**（暂存区内容），不是工作区文件：两者可能不一致。
$tmpDir = Join-Path ([IO.Path]::GetTempPath()) ('precommit-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

function Get-StagedBlobBytes([string]$rel) {
    $out = Join-Path $tmpDir ([guid]::NewGuid().ToString('N') + '.blob')
    $err = Join-Path $tmpDir ([guid]::NewGuid().ToString('N') + '.err')
    # 用 Start-Process 抓 stdout：PowerShell 管道会把字节流解码成字符串，BOM 会丢
    $argStr = 'cat-file blob ":' + $rel + '"'
    try {
        $p = Start-Process -FilePath $gitExe -ArgumentList $argStr -NoNewWindow -Wait -PassThru `
                -RedirectStandardOutput $out -RedirectStandardError $err
    } catch {
        return $null
    }
    if ($p.ExitCode -ne 0) { return $null }
    if (-not (Test-Path -LiteralPath $out)) { return $null }
    return [IO.File]::ReadAllBytes($out)
}

$psFiles = @($staged | Where-Object { $_ -match '\.(ps1|psm1)$' })
foreach ($rel in $psFiles) {
    $worktree = Join-Path $root $rel
    $bytes = Get-StagedBlobBytes $rel
    if ($null -eq $bytes) {
        # 读不到 blob 就退回工作区文件（至少不要静默放过）
        if (Test-Path -LiteralPath $worktree) {
            $bytes = [IO.File]::ReadAllBytes($worktree)
        } else {
            $problems.Add(("{0}: 暂存区内容读取失败（blob 为空）" -f $rel))
            continue
        }
    }

    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    if (-not $hasBom) {
        $problems.Add(("{0}: 缺少 UTF-8 BOM（前 3 字节不是 EF BB BF）" -f $rel))
        $hints.Add("powershell -NoProfile -File tools/ci.ps1 -FixBom   # 给被跟踪的脚本补 BOM，然后 git add 再次提交")
        $hints.Add(("或手工：`$b=[IO.File]::ReadAllBytes('{0}'); [IO.File]::WriteAllBytes('{0}', [byte[]](0xEF,0xBB,0xBF)+`$b)" -f $rel))
    }

    # 无 BOM 时按 ANSI 解码会误报/漏报，先补一个临时 BOM 再解析，拿到的才是真实语法状态
    $parsePath = $worktree
    if (-not $hasBom) {
        $buf = New-Object byte[] ($bytes.Length + 3)
        $buf[0] = 0xEF; $buf[1] = 0xBB; $buf[2] = 0xBF
        [Array]::Copy($bytes, 0, $buf, 3, $bytes.Length)
        $parsePath = Join-Path $tmpDir ([guid]::NewGuid().ToString('N') + '.ps1')
        [IO.File]::WriteAllBytes($parsePath, $buf)
    } else {
        # 有 BOM 也要用 blob 内容解析（工作区可能与暂存区不同）
        $parsePath = Join-Path $tmpDir ([guid]::NewGuid().ToString('N') + '.ps1')
        [IO.File]::WriteAllBytes($parsePath, $bytes)
    }

    $tokens = $null
    $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($parsePath, [ref]$tokens, [ref]$errors)
    $errors = @($errors)
    if ($errors.Count -gt 0) {
        $first = $errors[0]
        $problems.Add(("{0}: 语法错误 {1} 处，首个在第 {2} 行：{3}" -f $rel, $errors.Count, $first.Extent.StartLineNumber, $first.Message))
        foreach ($e in ($errors | Select-Object -Skip 1 -First 2)) {
            $problems.Add(("    {0}:{1}: {2}" -f $rel, $e.Extent.StartLineNumber, $e.Message))
        }
    }
}

# ---- 规则 3：动到 src/kernel/** 就跑模块边界检查 ----
$kernelTouched = @($staged | Where-Object { $_ -match '^src/kernel/' })
if ($kernelTouched.Count -gt 0) {
    $deps = Join-Path $root 'tools/check-deps.ps1'
    $log = Join-Path $tmpDir 'check-deps.log'
    $depCode = 0
    if (Test-Path -LiteralPath $deps) {
        $savedEap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & powershell -NoProfile -ExecutionPolicy Bypass -File $deps -Repo $root *> $log
            $depCode = $LASTEXITCODE
        } catch {
            $depCode = 1
            "check-deps 运行失败: $_" | Set-Content -Path $log -Encoding UTF8
        } finally {
            $ErrorActionPreference = $savedEap
        }
    } else {
        $depCode = 0
    }

    if ($depCode -ne 0) {
        $problems.Add(("src/kernel/** 有改动，但 tools/check-deps.ps1 报违规（exit={0}）" -f $depCode))
        if (Test-Path -LiteralPath $log) {
            $lines = @(Get-Content $log | Where-Object { $_.Trim() })
            foreach ($l in ($lines | Select-Object -Last 20)) { $problems.Add(("    {0}" -f $l)) }
        }
        $hints.Add('powershell -NoProfile -File tools/check-deps.ps1    # 看完整违规列表')
        $hints.Add('跨模块调用请走接口头 include/<mod>.h；规则见 docs/modules.md')
    }
}

Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
Pop-Location

if ($problems.Count -gt 0) {
    Write-Host ''
    Write-Host ("pre-commit: blocked: {0} issue(s)" -f $problems.Count) -ForegroundColor Red
    foreach ($p in $problems) { Write-Host ("  - {0}" -f $p) -ForegroundColor Red }
    if ($hints.Count -gt 0) {
        Write-Host ''
        Write-Host 'fix:' -ForegroundColor Yellow
        foreach ($h in ($hints | Select-Object -Unique)) { Write-Host ("  $ {0}" -f $h) -ForegroundColor Yellow }
    }
    Write-Host ''
    Write-Host '紧急绕过（仅限救急，之后必须修复并补跑 tools/ci.ps1 -Quick）：' -ForegroundColor DarkGray
    Write-Host '  git commit --no-verify ...' -ForegroundColor DarkGray
    exit 1
}

Write-Host ("pre-commit: ok ({0} staged file(s))" -f $staged.Count) -ForegroundColor DarkGray
exit 0
