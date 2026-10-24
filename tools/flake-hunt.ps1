# =============================================================================
# flake-hunt.ps1 - repeat the end-to-end regression and quantify flaky failures
#
# Each iteration runs the isolated session wrapper:
#   powershell -File tools/session-run.ps1 -Name <Name> -Test -QmpPort <port>
# which gives us .obj-<Name>/.dist-<Name>/serial-<Name>.log and serializes -Test
# through .session/test.lock (host echo port 12345 is a compile-time constant,
# so only one -Test may run at a time).
#
# Per run we record:
#   * PASS/FAIL, exit code, host/build elapsed, compile counters
#   * failed check names (parsed from the === checks === section)
#   * a copy of the serial log and the build log (serial-<Name>-NN.log ...)
#   * on failure, a tail-of-serial snapshot (last 40 lines)
#   * flaky-mode indicators: 0x4000415c exception, user exceptions, serial
#     interleaving/truncation, ASLR uniqueness, heartbeat/AP markers
#   * git HEAD + working-tree fingerprint, so a moving tree is visible
#
# Host-side robustness:
#   * before every run: kill a leftover flake QEMU (matched by dist dir / serial
#     log / QMP port), and when the test lock is free, clear a stale 12345
#     listener left by a crashed previous run
#   * after every run: wait for 12345 to be released; if the lock is free and a
#     listener remains, kill the stale owner and record it
#
# usage:
#   powershell -File tools/flake-hunt.ps1 -Runs 20
#   powershell -File tools/flake-hunt.ps1 -Runs 20 -KeepLogs
#
# Outputs under .session/flake/ (ignored by git):
#   results.csv, summary.md, run-NN build/serial logs, failure tail snapshots.
# =============================================================================
param(
    [int]$Runs = 20,
    [string]$Name = 'flake',
    [string]$Repo = 'D:\OS_Project',
    [int]$QmpPort = 0,
    [switch]$KeepLogs,
    [int]$WaitSec = 300,          # how long session-run waits for the test lock
    [int]$RunTimeoutSec = 300     # hard cap for one iteration incl. boot/teardown
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
Set-Location $Repo

if ($Runs -lt 1) { throw "-Runs must be >= 1" }
if ($QmpPort -eq 0) { $QmpPort = 4521 }   # flake session uses 4521 (a=4481 b=4491 c=4501)

$objDir    = ".obj-$Name"
$distDir   = ".dist-$Name"
$serialRel = "serial-$Name.log"
$buildLog  = "logs-session-$Name.log"
$serialAbs = Join-Path $Repo $serialRel
$buildAbs  = Join-Path $Repo $buildLog
$distAbs   = Join-Path $Repo $distDir
$outDir    = Join-Path $Repo '.session\flake'
$lockPath  = Join-Path $Repo '.session\test.lock'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$script:hostIssues = @()

function Write-Note([string]$msg) {
    Write-Host ("[flake-hunt] {0}" -f $msg) -ForegroundColor DarkYellow
    $script:hostIssues += $msg
}

# ---- text decoding: build logs are UTF-16LE (PowerShell *> redirect), serial is raw ----
function Read-TextAuto([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        return [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
    }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) {
        return [Text.Encoding]::BigEndianUnicode.GetString($bytes, 2, $bytes.Length - 2)
    }
    return [Text.Encoding]::UTF8.GetString($bytes)
}

function Test-PortFree {
    param([int]$Port, [string]$Proto = 'tcp', [string]$Addr = '127.0.0.1')
    try {
        if ($Proto -eq 'udp') {
            $u = New-Object System.Net.Sockets.UdpClient($Port)
            $u.Close()
        } else {
            $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse($Addr), $Port)
            $l.Start(); $l.Stop()
        }
        return $true
    } catch {
        return $false
    }
}

function Test-LockHeld {
    if (-not (Test-Path $lockPath)) { return $false }
    try {
        $fs = [IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'None')
        $fs.Dispose()
        return $false
    } catch {
        return $true
    }
}

function Get-FlakeQemu {
    try {
        Get-CimInstance Win32_Process -Filter "Name='qemu-system-i386.exe'" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.CommandLine -and (
                    $_.CommandLine -match [regex]::Escape($distDir) -or
                    $_.CommandLine -match [regex]::Escape($serialRel) -or
                    $_.CommandLine -match ('-qmp tcp:127\.0\.0\.1:' + $QmpPort)
                )
            }
    } catch { @() }
}

function Stop-FlakeQemu {
    $procs = @(Get-FlakeQemu)
    foreach ($p in $procs) {
        Write-Note ("killing leftover flake qemu pid={0}" -f $p.ProcessId)
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
    if ($procs.Count -gt 0) { Start-Sleep -Milliseconds 300 }
    return $procs.Count
}

function Get-PortOwners([int]$Port) {
    $owners = @()
    try {
        $owners += @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
                     Select-Object -ExpandProperty OwningProcess)
    } catch { }
    try {
        $owners += @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue |
                     Select-Object -ExpandProperty OwningProcess)
    } catch { }
    return @($owners | Where-Object { $_ -and $_ -ne 0 } | Select-Object -Unique)
}

# 12345 may only be held while a -Test owns the lock. If the lock is free, any
# listener is a leftover from a crashed run and safe to kill.
function Clear-StaleEcho {
    if (Test-LockHeld) { return $false }
    $owners = @(Get-PortOwners 12345)
    if ($owners.Count -eq 0) { return $false }
    foreach ($owner in $owners) {
        $cl = ''
        try { $cl = (Get-CimInstance Win32_Process -Filter ("ProcessId=$owner") -ErrorAction SilentlyContinue).CommandLine } catch { }
        Write-Note ("clearing stale 12345 listener pid={0} cmd={1}" -f $owner, $cl)
        Stop-Process -Id $owner -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
    return $true
}

function Wait-PortFree([int]$Port, [int]$TimeoutMs, [string]$Proto = 'tcp') {
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        if (Test-PortFree -Port $Port -Proto $Proto -Addr '0.0.0.0') { return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Get-TreeFingerprint {
    $head = (& git rev-parse HEAD 2>$null)
    $status = @(& git status --porcelain 2>$null)
    $sha = [Security.Cryptography.SHA256]::Create()
    $raw = [Text.Encoding]::UTF8.GetBytes(($status -join "`n"))
    $fp = [BitConverter]::ToString($sha.ComputeHash($raw)).Replace('-', '').Substring(0, 12)
    return @{ Head = $head; Dirty = $status.Count; Fingerprint = $fp; Status = $status }
}

function Get-InterleaveSuspects([string]$text) {
    $bad = New-Object System.Collections.Generic.List[string]
    foreach ($ln in ($text -split "`r?`n")) {
        if ($ln -match 'thread: heartbeat' -and $ln -notmatch '^thread: heartbeat \d+ pid=\d+ cpu=\d+\s*$') {
            $bad.Add("heartbeat-line: $ln")
        }
        if ($ln -match 'uptime:' -and $ln -notmatch '^uptime: \d+ s \(ticks \d+\)\s*$') {
            $bad.Add("uptime-line: $ln")
        }
        if ($ln -match 'ushell> echo hi' -and $ln -notmatch '^ushell> echo hi\s*$') {
            $bad.Add("prompt-line: $ln")
        }
        # a log line that got another log line spliced into its middle
        if ($ln -match '^(thread: heartbeat|uptime:|sched:|proc:|ushell>|user: )' -and
            $ln -match '(thread: heartbeat \d|uptime: \d+ s|sched: pid \d|proc: pid \d|ushell: ready)') {
            $first = $ln.Substring(0, [Math]::Min(28, $ln.Length))
            if ($ln.IndexOf("thread: heartbeat") -gt 0 -or
                $ln.IndexOf("uptime: ") -gt 0 -or
                $ln.IndexOf("sched: pid") -gt 0 -or
                $ln.IndexOf("proc: pid") -gt 0) {
                $bad.Add("spliced-line: $ln")
            }
        }
    }
    return @($bad | Select-Object -Unique)
}

function Get-SerialMetrics([string]$text) {
    $lineCount = 0
    if ($text.Length -gt 0) { $lineCount = ($text -split "`n").Count }
    $exc = @([regex]::Matches($text, 'user: exception \d+ at eip=0x[0-9a-f]+'))
    $a415c = @([regex]::Matches($text, 'user: exception \d+ at eip=0x4000415c'))
    # 0x15 是 NX 取指的预期 fault（P|U|I/D），sectest 的 stack/rodata NX 用例
    # 每次正常运行都会各打印一行。只有"超出预期条数"的 NX fault 才是异常。
    $err15 = @([regex]::Matches($text, 'user: exception \d+ at eip=0x[0-9a-f]+ err=15'))
    $cr2eq = @([regex]::Matches($text, 'user: exception \d+ at eip=0x([0-9a-f]+) err=[0-9a-fx]+ cr2=0x\1'))
    $sectestFaults = @([regex]::Matches($text, 'sectest: (code write|guard write|stack exec|rodata exec) pid=\d+ status=142'))
    $rodata = @([regex]::Matches($text, 'sectest: rodata exec pid=\d+ status=142'))
    $expectedNx = @([regex]::Matches($text, 'sectest: (stack exec|rodata exec) pid=\d+ status=142'))
    $ready = @([regex]::Matches($text, 'ushell: ready'))
    $hbCpu1 = @([regex]::Matches($text, 'thread: heartbeat [1-9]\d* pid=\d+ cpu=1'))
    $restart = @([regex]::Matches($text, 'svcwatch: restart echo -> pid \d+'))
    $nativeRestart = @([regex]::Matches($text, 'native: restart ok'))
    $nativeChain = @([regex]::Matches($text, 'native: process_create/wait (ok|FAILED)'))
    $interleave = @(Get-InterleaveSuspects $text)
    $truncated = ($text.Length -gt 0 -and -not $text.EndsWith("`n"))
    $aslrStack = @([regex]::Matches($text, 'stack_init=0x([0-9a-f]+)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    $aslrHeap = @([regex]::Matches($text, 'heap_base=0x([0-9a-f]+)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    $aslrTramp = @([regex]::Matches($text, 'tramp=0x([0-9a-f]+)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    return [pscustomobject]@{
        Lines = $lineCount
        Exceptions = $exc.Count
        Exceptions415c = $a415c.Count
        SectestRodata = $rodata.Count
        SectestFaults = $sectestFaults.Count
        ExpectedNx = $expectedNx.Count
        UnexpectedNx = [Math]::Max(0, $err15.Count - $expectedNx.Count)
        UnexpectedCr2Eip = [Math]::Max(0, $cr2eq.Count - $expectedNx.Count)
        Unmatched415c = [Math]::Max(0, $a415c.Count - $rodata.Count)
        UnmatchedExceptions = [Math]::Max(0, $exc.Count - $sectestFaults.Count)
        ReadyCount = $ready.Count
        HeartbeatCpu1 = $hbCpu1.Count
        RestartLine = $restart.Count
        NativeRestart = $nativeRestart.Count
        NativeChain = $nativeChain.Count
        Interleave = $interleave
        Truncated = $truncated
        AslrStackUnique = $aslrStack.Count
        AslrHeapUnique = $aslrHeap.Count
        AslrTrampUnique = $aslrTramp.Count
    }
}

# build.ps1 以 UTF-8 BOM 保存；session-run 用 *> 捕获子 powershell 的 UTF-8 输出时
# 会按宿主代码页(936)解码，日志里的中文检查名因此是乱码。检查顺序是固定的，
# 所以从 build.ps1 源码里按顺序取规范名，再把日志里的 [PASS]/[FAIL] token 按序对上。
function Get-CheckNamesFromSource([string]$repo) {
    $p = Join-Path $repo 'build.ps1'
    if (-not (Test-Path $p)) { return @() }
    $text = [IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)
    $names = @()
    foreach ($m in [regex]::Matches($text, "Name\s*=\s*'([^']+)'")) {
        $names += $m.Groups[1].Value
    }
    return $names
}

function Get-CheckResults([string]$buildText, [string[]]$names) {
    $tokens = @([regex]::Matches($buildText, '\[(PASS|FAIL)\]') | ForEach-Object { $_.Groups[1].Value })
    $fails = @()
    for ($i = 0; $i -lt $tokens.Count; $i++) {
        if ($tokens[$i] -ne 'FAIL') { continue }
        $name = if ($i -lt $names.Count) { $names[$i] } else { "(check#$($i + 1))" }
        $fails += $name
    }
    return [pscustomobject]@{
        Tokens = $tokens.Count
        Fails = @($fails | Select-Object -Unique)
        Aligned = ($tokens.Count -eq $names.Count)
    }
}

function Get-CountFromVerdict([string]$verdict) {
    if ($verdict -match '\((\d+)/(\d+)\)') { return @{ Failed = 0; Total = [int]$matches[2] } }
    if ($verdict -match '\((\d+) of (\d+) checks failed\)') { return @{ Failed = [int]$matches[1]; Total = [int]$matches[2] } }
    return @{ Failed = -1; Total = -1 }
}

# Timing/order-sensitive checks by name (used only for a *heuristic* bucket; the
# detailed logs stay the source of truth for the final classification).
$timingChecks = @(
    'svcwatch 周期重启 echo', 'restart 后服务可用', 'AP 进入 idle',
    '时钟中断在推进', '内核线程被调度', '负载放置把线程送上 AP',
    'exec 地址空间切换', '注入的命令被执行', 'ASLR 栈/堆/蹦床',
    '命令行参数传递可用', 'COW fork 可用',
    'TCP echo 可用', 'UDP echo 可用', 'ICMP echo 可用', 'ARP 应答可用',
    'e1000 网卡可用', 'FAT16 能读宿主文件'
)

Write-Host ("=== flake-hunt: {0} runs, session '{1}', qmp {2} ===" -f $Runs, $Name, $QmpPort) -ForegroundColor Cyan
$startedAt = Get-Date
$sourceCheckNames = @(Get-CheckNamesFromSource $Repo)
Write-Host ("canonical check names from build.ps1: {0}" -f $sourceCheckNames.Count) -ForegroundColor DarkCyan
$tree0 = Get-TreeFingerprint
Write-Host ("repo HEAD {0}, dirty files {1}" -f $tree0.Head, $tree0.Dirty) -ForegroundColor DarkCyan

$rows = New-Object System.Collections.Generic.List[object]
$prevFingerprint = $tree0.Fingerprint

for ($i = 1; $i -le $Runs; $i++) {
    $tag = '{0:D2}' -f $i
    $script:hostIssues = @()
    $runStart = Get-Date
    $sw = [Diagnostics.Stopwatch]::StartNew()

    # ---------- pre-run host cleanup ----------
    [void](Stop-FlakeQemu)
    if (-not (Test-LockHeld)) {
        [void](Clear-StaleEcho)
        if (-not (Wait-PortFree -Port 12345 -TimeoutMs 15000 -Proto 'udp')) {
            Write-Note "host 12345/udp still busy after 15s"
        }
        if (-not (Wait-PortFree -Port 12345 -TimeoutMs 5000 -Proto 'tcp')) {
            Write-Note "host 12345/tcp still busy after 5s"
        }
    }
    if (-not (Test-PortFree -Port $QmpPort)) {
        Write-Note ("qmp port {0} busy before run (not our qemu?)" -f $QmpPort)
    }

    Remove-Item $serialAbs -Force -ErrorAction SilentlyContinue
    Remove-Item $buildAbs -Force -ErrorAction SilentlyContinue

    $hostOut = Join-Path $outDir ("host-$tag.out.txt")
    $hostErr = Join-Path $outDir ("host-$tag.err.txt")
    Remove-Item $hostOut, $hostErr -Force -ErrorAction SilentlyContinue

    # ---------- run one isolated regression ----------
    $srunArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools\session-run.ps1',
                  '-Name', $Name, '-Test', '-QmpPort', "$QmpPort", '-WaitSec', "$WaitSec")
    $proc = Start-Process -FilePath 'powershell' -ArgumentList $srunArgs -WorkingDirectory $Repo `
                          -PassThru -WindowStyle Hidden -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr
    $timedOut = $false
    if (-not $proc.WaitForExit($RunTimeoutSec * 1000)) {
        $timedOut = $true
        Write-Note ("run {0} timed out after {1}s; killing process tree" -f $i, $RunTimeoutSec)
        & taskkill /PID $proc.Id /T /F 2>$null | Out-Null
        Start-Sleep -Milliseconds 500
    }
    $exitCode = if ($timedOut) { -1 } else { $proc.ExitCode }
    $sw.Stop()

    # ---------- post-run host cleanup ----------
    [void](Stop-FlakeQemu)
    $portReleased = $true
    if (-not (Wait-PortFree -Port 12345 -TimeoutMs 10000 -Proto 'udp')) { $portReleased = $false }
    if (-not (Wait-PortFree -Port 12345 -TimeoutMs 3000 -Proto 'tcp')) { $portReleased = $false }
    if (-not $portReleased) {
        if (-not (Test-LockHeld)) {
            Write-Note "12345 not released after run; clearing stale listener"
            [void](Clear-StaleEcho)
            Start-Sleep -Milliseconds 500
            $portReleased = (Test-PortFree -Port 12345 -Proto 'udp') -and (Test-PortFree -Port 12345 -Proto 'tcp' -Addr '0.0.0.0')
        } else {
            Write-Note "12345 still held by another session (lock held); not touching it"
        }
    }

    # ---------- collect evidence ----------
    $buildText = Read-TextAuto $buildAbs
    $serialText = Read-TextAuto $serialAbs
    $metrics = Get-SerialMetrics $serialText
    $checkResults = Get-CheckResults $buildText $sourceCheckNames
    $fails = @($checkResults.Fails)
    $verdict = ''
    $vm = [regex]::Match($buildText, '(?m)^=== TEST (PASS|FAIL)[^=]*===\s*$')
    if ($vm.Success) { $verdict = $vm.Value.Trim() }
    $counts = Get-CountFromVerdict $verdict
    $compiled = 0; $skipped = 0
    $cm = [regex]::Match($buildText, '=== compile: (\d+) compiled, (\d+) skipped ===')
    if ($cm.Success) { $compiled = [int]$cm.Groups[1].Value; $skipped = [int]$cm.Groups[2].Value }
    $buildElapsed = ''
    $em = [regex]::Match($buildText, '=== elapsed: ([\d.]+)s ===')
    if ($em.Success) { $buildElapsed = [double]$em.Groups[1].Value }

    $passed = $verdict -match '^=== TEST PASS'
    $failed = -not $passed
    # A run that never reached the checks (compile/link error, port/lock conflict,
    # timeout) is a host/build problem, not a test-verdict failure.
    $hostFailure = $timedOut -or (-not $verdict)

    $serialCopy = Join-Path $outDir ("serial-{0}-{1}.log" -f $Name, $tag)
    if (Test-Path $serialAbs) { Copy-Item $serialAbs $serialCopy -Force }
    $buildCopy = Join-Path $outDir ("build-{0}-{1}.log" -f $Name, $tag)
    if (Test-Path $buildAbs) { Copy-Item $buildAbs $buildCopy -Force }

    $tailCopy = ''
    if ($failed -and $serialText.Length -gt 0) {
        $tailCopy = Join-Path $outDir ("serial-{0}-{1}.tail40.txt" -f $Name, $tag)
        $tail = @($serialText -split "`r?`n" | Where-Object { $_ -ne '' } | Select-Object -Last 40)
        [IO.File]::WriteAllText($tailCopy, ($tail -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    }

    $tree = Get-TreeFingerprint
    $treeChanged = ($tree.Fingerprint -ne $prevFingerprint)
    $prevFingerprint = $tree.Fingerprint

    $failNames = ($fails -join '; ')
    $row = [pscustomobject]@{
        Run = $i
        Timestamp = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
        Head = $tree.Head
        DirtyFiles = $tree.Dirty
        TreeFingerprint = $tree.Fingerprint
        TreeChanged = $treeChanged
        Compiled = $compiled
        Skipped = $skipped
        HostElapsedSec = [Math]::Round($sw.Elapsed.TotalSeconds, 2)
        BuildElapsedSec = $buildElapsed
        ExitCode = $exitCode
        Verdict = $verdict
        Passed = [bool]$passed
        FailCount = if ($counts.Failed -ge 0) { $counts.Failed } else { $fails.Count }
        FailNames = $failNames
        CheckTokens = $checkResults.Tokens
        ChecksAligned = $checkResults.Aligned
        SerialLines = $metrics.Lines
        UserExceptions = $metrics.Exceptions
        Exceptions415c = $metrics.Exceptions415c
        SectestRodata = $metrics.SectestRodata
        SectestFaults = $metrics.SectestFaults
        ExpectedNx = $metrics.ExpectedNx
        UnexpectedNx = $metrics.UnexpectedNx
        UnexpectedCr2Eip = $metrics.UnexpectedCr2Eip
        Unmatched415c = $metrics.Unmatched415c
        UnmatchedExceptions = $metrics.UnmatchedExceptions
        ReadyCount = $metrics.ReadyCount
        HeartbeatCpu1 = $metrics.HeartbeatCpu1
        RestartLine = $metrics.RestartLine
        NativeRestart = $metrics.NativeRestart
        NativeChain = $metrics.NativeChain
        InterleaveSuspects = $metrics.Interleave.Count
        InterleaveSample = (($metrics.Interleave | Select-Object -First 3) -join ' | ')
        SerialTruncated = $metrics.Truncated
        AslrUnique = ("{0}/{1}/{2}" -f $metrics.AslrStackUnique, $metrics.AslrHeapUnique, $metrics.AslrTrampUnique)
        PortReleasedAfter = $portReleased
        TimedOut = $timedOut
        HostIssues = ($script:hostIssues -join '; ')
        SerialCopy = $serialCopy
        BuildCopy = $buildCopy
        TailSnapshot = $tailCopy
    }
    $rows.Add($row)

    $color = if ($passed) { 'Green' } elseif ($hostFailure) { 'Magenta' } else { 'Red' }
    $status = if ($passed) { 'PASS' } elseif ($hostFailure) { 'HOST/BUILD' } else { 'FAIL' }
    $detail = if ($fails.Count -gt 0) { ("fails={0}: {1}" -f $fails.Count, ($fails -join ', ')) } else { '' }
    Write-Host ("[run {0}/{1}] {2} {3:N1}s {4} {5}" -f $i, $Runs, $status, $sw.Elapsed.TotalSeconds, $verdict, $detail) -ForegroundColor $color
    if ($timedOut) { Write-Host ("           timed out; see {0}" -f $hostOut) -ForegroundColor Magenta }
    if ($metrics.Interleave.Count -gt 0) {
        Write-Host ("           interleave suspects: {0}" -f (($metrics.Interleave | Select-Object -First 2) -join ' | ')) -ForegroundColor Yellow
    }
    if ($metrics.UnexpectedNx -gt 0 -or $metrics.UnexpectedCr2Eip -gt 0) {
        Write-Host ("           A-class unexpected NX faults: nx={0} cr2==eip={1} (expected nx lines={2}), ready count: {3}" -f $metrics.UnexpectedNx, $metrics.UnexpectedCr2Eip, $metrics.ExpectedNx, $metrics.ReadyCount) -ForegroundColor Yellow
    }

    if (-not $KeepLogs -and $passed) {
        Remove-Item $serialCopy, $buildCopy -Force -ErrorAction SilentlyContinue
    }
}

# ---------- summary ----------
$csvPath = Join-Path $outDir 'results.csv'
$rows | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8

$passCount = @($rows | Where-Object { $_.Passed }).Count
$failRows = @($rows | Where-Object { -not $_.Passed -and $_.Verdict })
$hostRows = @($rows | Where-Object { -not $_.Passed -and -not $_.Verdict })
$verdictRuns = $passCount + $failRows.Count

$failNameCounts = @{}
foreach ($r in $failRows) {
    if (-not $r.FailNames) { continue }
    foreach ($n in ($r.FailNames -split '; ')) {
        if (-not $n) { continue }
        if (-not $failNameCounts.ContainsKey($n)) { $failNameCounts[$n] = 0 }
        $failNameCounts[$n]++
    }
}

# heuristic buckets (final classification is done from the logs)
$aRuns = @($rows | Where-Object { $_.UnexpectedNx -gt 0 -or $_.UnexpectedCr2Eip -gt 0 })
$bRuns = @($failRows | Where-Object { $_.InterleaveSuspects -gt 0 -and $_.UnexpectedNx -eq 0 })
$hostRuns = $hostRows
$cRuns = @($failRows | Where-Object {
    $_.UnexpectedNx -eq 0 -and $_.InterleaveSuspects -eq 0 -and $_.FailNames -and
    (@(($_.FailNames -split '; ') | Where-Object { $timingChecks -notcontains $_ }).Count -eq 0)
})
$uRuns = @($failRows | Where-Object { $cRuns -notcontains $_ -and $bRuns -notcontains $_ -and $aRuns -notcontains $_ })

$summary = New-Object System.Collections.Generic.List[string]
$summary.Add('# flake-hunt summary')
$summary.Add('')
$summary.Add(('- started: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')))
$summary.Add(('- repo HEAD: {0}' -f $tree0.Head))
$summary.Add(('- runs: {0}, PASS: {1}, FAIL: {2}, HOST/BUILD: {3}' -f $Runs, $passCount, $failRows.Count, $hostRows.Count))
if ($verdictRuns -gt 0) {
    $summary.Add(('- failure rate (test verdicts): {0}/{1} = {2:P1}' -f $failRows.Count, $verdictRuns, ($failRows.Count / $verdictRuns)))
}
$summary.Add(('- failure rate (all runs): {0}/{1} = {2:P1}' -f ($failRows.Count + $hostRows.Count), $Runs, (($failRows.Count + $hostRows.Count) / $Runs)))
$summary.Add('')
$summary.Add('## by HEAD (shared tree moves while other sessions commit)')
$summary.Add('| HEAD | runs | pass | fail | host/build | fail names |')
$summary.Add('|------|------|------|------|------------|-----------|')
$headGroups = @($rows | Group-Object Head | Sort-Object Name)
foreach ($g in $headGroups) {
    $gPass = @($g.Group | Where-Object { $_.Passed }).Count
    $gFail = @($g.Group | Where-Object { -not $_.Passed -and $_.Verdict }).Count
    $gHost = @($g.Group | Where-Object { -not $_.Passed -and -not $_.Verdict }).Count
    $gNames = @($g.Group | Where-Object { $_.FailNames } | ForEach-Object { $_.FailNames }) -join '; '
    $summary.Add(('| {0} | {1} | {2} | {3} | {4} | {5} |' -f
        $g.Name.Substring(0, [Math]::Min(7, $g.Name.Length)), $g.Count, $gPass, $gFail, $gHost, ($gNames -replace '\|', '/')))
}
$summary.Add('')
$summary.Add('## heuristic buckets (verify against per-run logs)')
$summary.Add(('- A unexpected NX (err=15 / cr2==eip beyond sectest): {0} run(s): {1}' -f $aRuns.Count, (($aRuns | ForEach-Object { $_.Run }) -join ',')))
$summary.Add(('- B serial interleave/truncation: {0} run(s): {1}' -f $bRuns.Count, (($bRuns | ForEach-Object { $_.Run }) -join ',')))
$summary.Add(('- C timing/order-sensitive only: {0} run(s): {1}' -f $cRuns.Count, (($cRuns | ForEach-Object { $_.Run }) -join ',')))
$summary.Add(('- D host/build: {0} run(s): {1}' -f $hostRows.Count, (($hostRows | ForEach-Object { $_.Run }) -join ',')))
$summary.Add(('- U unclassified: {0} run(s): {1}' -f $uRuns.Count, (($uRuns | ForEach-Object { $_.Run }) -join ',')))
$summary.Add(('- expected NX lines (allowed, not flake): total {0}' -f (($rows | Measure-Object -Property ExpectedNx -Sum).Sum)))
$summary.Add('')
$summary.Add('## failed check counts')
if ($failNameCounts.Count -eq 0) { $summary.Add('- (none)') }
foreach ($k in ($failNameCounts.Keys | Sort-Object { -$failNameCounts[$_] })) {
    $summary.Add(('- {0}: {1}' -f $k, $failNameCounts[$k]))
}
$summary.Add('')
$summary.Add('## runs')
$summary.Add('| run | HEAD | result | sec | fails | NX exp/unexp | interleave | ready | port | notes |')
$summary.Add('|-----|------|--------|-----|-------|--------------|------------|-------|------|-------|')
foreach ($r in $rows) {
    $result = if ($r.Passed) { 'PASS' } elseif ($r.Verdict) { 'FAIL' } else { 'HOST' }
    $names = if ($r.FailNames) { $r.FailNames } else { '' }
    $summary.Add(('| {0} | {1} | {2} | {3} | {4} | {5}/{6} | {7} | {8} | {9} | {10} |' -f
        $r.Run, $r.Head.Substring(0, [Math]::Min(7, $r.Head.Length)), $result, $r.HostElapsedSec, $names,
        $r.ExpectedNx, $r.UnexpectedNx, $r.InterleaveSuspects,
        $r.ReadyCount, $r.PortReleasedAfter, ($r.HostIssues -replace '\|', '/')))
}
$summary.Add('')
$summary.Add('## artifacts')
$summary.Add(('- results.csv (all columns)'))
$summary.Add(('- serial-<name>-NN.log / build-<name>-NN.log (kept for failures; all with -KeepLogs)'))
$summary.Add(('- serial-<name>-NN.tail40.txt (failure snapshots)'))
$summaryPath = Join-Path $outDir 'summary.md'
[IO.File]::WriteAllText($summaryPath, ($summary -join "`r`n"), (New-Object Text.UTF8Encoding($true)))

Write-Host ''
Write-Host '=== flake-hunt summary ===' -ForegroundColor Cyan
Write-Host ("runs={0} PASS={1} FAIL={2} HOST/BUILD={3}" -f $Runs, $passCount, $failRows.Count, $hostRows.Count)
if ($verdictRuns -gt 0) {
    Write-Host ("failure rate (verdicts): {0}/{1} = {2:P1}" -f $failRows.Count, $verdictRuns, ($failRows.Count / $verdictRuns))
}
if ($failNameCounts.Count -gt 0) {
    Write-Host 'failed checks:'
    foreach ($k in ($failNameCounts.Keys | Sort-Object { -$failNameCounts[$_] })) {
        Write-Host ("  {0} x{1}" -f $k, $failNameCounts[$k])
    }
}
Write-Host ("A(unexpected NX)={0} B(interleave)={1} C(timing-only)={2} D(host)={3} U={4}" -f
            $aRuns.Count, $bRuns.Count, $cRuns.Count, $hostRows.Count, $uRuns.Count)
Write-Host ("summary: {0}" -f $summaryPath) -ForegroundColor DarkCyan
Write-Host ("csv:     {0}" -f $csvPath) -ForegroundColor DarkCyan
