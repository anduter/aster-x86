# session-run.ps1 - run build.ps1 with per-session isolation and a test lock
#
# Isolation: -ObjDir/-DistDir/-QmpPort/-SerialLog are derived from -Name, so two
# sessions do not overwrite each other's artifacts.
# Lock: only -Test needs the host echo port 12345. The lock lives in the
# machine/user temp dir (not <repo>/.session) so linked worktrees and parallel
# checkouts serialize too. -Test first waits for port 12345 to be free, then
# acquires the lock (1800 s default), writing {pid,time,session,repo}.
#
# usage:
#   powershell -File tools/session-run.ps1 -Name a -Test
#   powershell -File tools/session-run.ps1 -Name b            # build only
param(
    [string]$Repo = 'D:\OS_Project',
    [string]$Name = 'session',
    [string]$ObjDir = '',
    [string]$DistDir = '',
    [int]$QmpPort = 0,
    [string]$SerialLog = '',
    [string]$LockPath = '',
    [switch]$Test,
    [int]$WaitSec = 1800
)

Set-Location $Repo
if (-not $ObjDir)  { $ObjDir = ".obj-$Name" }
if (-not $DistDir) { $DistDir = ".dist-$Name" }
if ($QmpPort -eq 0) {
    $map = @{ 'a' = 4481; 'b' = 4491; 'c' = 4501 }
    $QmpPort = if ($map.ContainsKey($Name)) { $map[$Name] } else { 4511 }
}
if (-not $SerialLog) { $SerialLog = "serial-$Name.log" }

$sessionDir = Join-Path $Repo '.session'
New-Item -ItemType Directory -Force -Path $sessionDir | Out-Null
# 锁必须跨 worktree/仓库实例互斥：放全机（当前用户）临时目录，而不是
# <repo>/.session（每个隔离 worktree 各有一份，锁不共享）。
if (-not $LockPath) {
    $lockPath = Join-Path ([IO.Path]::GetTempPath()) 'aster-test.lock'
} else {
    $lockPath = $LockPath
}
$lockDir = Split-Path -Parent $lockPath
if ($lockDir -and -not (Test-Path -LiteralPath $lockDir)) {
    New-Item -ItemType Directory -Force -Path $lockDir | Out-Null
}

function Write-History([string]$line) {
    Add-Content -Path (Join-Path $sessionDir 'history.log') -Value $line -Encoding UTF8
}

# 返回 12345 的 LISTEN 持有者 PID（0 = 空闲）。Get-NetTCPConnection 不可用
# 时退回 netstat。
function Get-EchoPortOwner {
    try {
        $c = @(Get-NetTCPConnection -LocalPort 12345 -State Listen -ErrorAction SilentlyContinue |
               Select-Object -First 1)
        if ($c.Count -gt 0 -and $c[0].OwningProcess) { return [int]$c[0].OwningProcess }
    } catch { }
    try {
        $line = (@(netstat -ano | Select-String -Pattern ':12345\s') | Where-Object { $_.Line -match 'LISTENING' } |
                 Select-Object -First 1)
        if ($line -and $line.Line -match 'LISTENING\s+(\d+)') { return [int]$matches[1] }
    } catch { }
    return 0
}

# 抢测试锁之前先等宿主回显端口 12345 空闲：端口是编译期常量，锁外的
# 会话（直接跑 build.ps1 -Test）或残留进程也会占用它。
if ($Test) {
    $portDeadline = (Get-Date).AddSeconds($WaitSec)
    $lastPortDesc = ''
    $lastPortLog = [datetime]::MinValue
    $ownerDesc = ''
    while ((Get-Date) -lt $portDeadline) {
        $ownerPid = Get-EchoPortOwner
        if ($ownerPid -eq 0) { break }
        $ownerDesc = "pid=$ownerPid"
        try {
            $op = Get-Process -Id $ownerPid -ErrorAction Stop
            $ownerDesc = ("session/process={0} pid={1}" -f $op.ProcessName, $ownerPid)
        } catch { }
        $now = Get-Date
        if ($ownerDesc -ne $lastPortDesc -or ($now - $lastPortLog).TotalSeconds -ge 30) {
            $left = [int](($portDeadline - $now).TotalSeconds)
            Write-Host ("[session-run] waiting for echo port 12345: {0}; {1}s left" -f $ownerDesc, $left) -ForegroundColor Yellow
            $lastPortDesc = $ownerDesc
            $lastPortLog = $now
        }
        Start-Sleep -Seconds 2
    }
    if ((Get-EchoPortOwner) -ne 0) {
        throw ("echo port 12345 still busy after {0}s ({1})" -f $WaitSec, $ownerDesc)
    }
}

$lockStream = $null
$lockTime = ''
$lockJson = ''
if ($Test) {
    $deadline = (Get-Date).AddSeconds($WaitSec)
    $lastHolder = ''
    $lastLog = [datetime]::MinValue

    while ((Get-Date) -lt $deadline) {
        # 1) 正常路径：原子地独占写入打开。FileShare.Read 允许等待者读取
        #    {pid,time,session}（日志/陈旧判断需要），但拒绝第二个写者，
        #    所以同一时刻仍只有一个持锁者。文件存在但没人持有时也会成功——
        #    陈旧文件本身不会挡路，只有活着的持锁者才会失败。
        try {
            $lockStream = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'Read')
            break
        } catch {
            # 2) 打不开：以共享读方式取持锁者信息（持锁者用 FileShare.Read，
            #    所以这里可读；若不可读则按活锁处理，不删不抢）。
            $txt = ''
            $readable = $false
            try {
                $rs = [IO.File]::Open($lockPath, 'Open', 'Read', 'ReadWrite')
                $sr = New-Object IO.StreamReader($rs)
                $txt = $sr.ReadToEnd()
                $sr.Dispose()
                $rs.Dispose()
                $readable = $true
            } catch { }

            $holderPid = 0
            $holderSession = '?'
            $holderStart = [datetime]::MinValue
            $parsed = $false
            if ($readable -and $txt.Trim()) {
                try {
                    $info = $txt | ConvertFrom-Json
                    if ($info.pid) { $holderPid = [int]$info.pid }
                    if ($info.session) { $holderSession = [string]$info.session } elseif ($info.name) { $holderSession = [string]$info.name }
                    foreach ($t in @($info.time, $info.start)) {
                        if ($t) {
                            try { $holderStart = [datetime]::Parse([string]$t); break } catch { }
                        }
                    }
                    $parsed = ($holderPid -gt 0)
                } catch { }
            }

            $stale = $false
            $why = ''
            if ($readable) {
                if (-not $parsed) {
                    $stale = $true
                    $why = 'unreadable/empty lock file'
                } else {
                    $alive = Get-Process -Id $holderPid -ErrorAction SilentlyContinue
                    $ageMin = if ($holderStart -gt [datetime]::MinValue) {
                        ((Get-Date) - $holderStart).TotalMinutes
                    } else {
                        ((Get-Date) - (Get-Item -LiteralPath $lockPath).LastWriteTime).TotalMinutes
                    }
                    if (-not $alive) {
                        $stale = $true
                        $why = ("holder pid {0} is gone" -f $holderPid)
                    } elseif ($ageMin -gt 30) {
                        $stale = $true
                        $why = ("lock age {0:N0} min > 30" -f $ageMin)
                    }
                    $holderDesc = ("session={0} pid={1} age={2:N0}m" -f $holderSession, $holderPid, $ageMin)
                }
            } else {
                $holderDesc = '(actively held; holder info unreadable)'
            }

            if ($stale) {
                # 3) 抢占式清理：Move-Item 到唯一临时名。打开中的锁文件因
                #    FileShare.None（无 SHARE_DELETE）无法被移动；两个等待者
                #    同时 Move 也只有一个成功（另一个 source 不存在）。因此
                #    不会误删/重复删活锁。
                $tmp = Join-Path $sessionDir ("test.lock.stale-{0}-{1}" -f $PID, (Get-Random))
                try {
                    Move-Item -LiteralPath $lockPath -Destination $tmp -Force -ErrorAction Stop
                    # 移动后再确认一次内容，避免把刚被替换的新锁当陈旧锁
                    try {
                        $moved = Get-Content -LiteralPath $tmp -Raw -ErrorAction Stop
                        $movedInfo = $moved | ConvertFrom-Json
                        if ($parsed -and $movedInfo.pid -and ([int]$movedInfo.pid -ne $holderPid)) {
                            # 误抢了新锁：尽量还回原位
                            if (-not (Test-Path -LiteralPath $lockPath)) {
                                Move-Item -LiteralPath $tmp -Destination $lockPath -Force -ErrorAction SilentlyContinue
                            }
                            $stale = $false
                            $why = 'lock content changed during takeover; restored'
                        }
                    } catch { }
                    if ($stale) {
                        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
                        Write-Host ("[session-run] removed stale lock ({0})" -f $why) -ForegroundColor Yellow
                        continue
                    }
                } catch {
                    # Move 失败：锁已被别人接手或仍被持有，退避后重试
                }
            }

            $now = Get-Date
            if (-not $stale -and ($holderDesc -ne $lastHolder -or ($now - $lastLog).TotalSeconds -ge 30)) {
                $left = [int](($deadline - $now).TotalSeconds)
                Write-Host ("[session-run] waiting for test lock: {0}; {1}s left" -f $holderDesc, $left) -ForegroundColor Yellow
                $lastHolder = $holderDesc
                $lastLog = $now
            }
            Start-Sleep -Seconds 2
        }
    }
    if (-not $lockStream) { throw "test lock not acquired within $WaitSec s (last holder: $lastHolder)" }
    $lockTime = (Get-Date).ToString('o')
    $lockJson = (@{ pid = $PID; time = $lockTime; session = $Name; name = $Name; repo = $Repo; start = $lockTime } |
                 ConvertTo-Json -Compress)
    $lockStream.SetLength(0)
    $w = New-Object IO.StreamWriter($lockStream)
    $w.Write($lockJson); $w.Flush()
    Write-Host ("[session-run] test lock acquired by {0} (pid {1})" -f $Name, $PID) -ForegroundColor Green
}

try {
    $buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'build.ps1',
              '-ObjDir', $ObjDir, '-DistDir', $DistDir, '-QmpPort', "$QmpPort", '-SerialLog', $SerialLog)
    if ($Test) { $buildArgs += '-Test' }

    Write-Host ("[session-run] {0}: obj={1} dist={2} qmp={3} serial={4} test={5}" -f `
                $Name, $ObjDir, $DistDir, $QmpPort, $SerialLog, $Test.IsPresent) -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $log = Join-Path $Repo ("logs-session-$Name.log")
    & powershell @buildArgs *> $log
    $code = $LASTEXITCODE
    $sw.Stop()

    $verdict = '(build only)'
    if ($Test) {
        $line = (Select-String -Path $log -Pattern '=== TEST (PASS|FAIL)[^=]*===' -ErrorAction SilentlyContinue | Select-Object -Last 1).Line
        if ($line) { $verdict = $line.Trim() } else { $verdict = '(no verdict captured)' }
    }
    Write-History ("{0} name={1} test={2} exit={3} elapsed={4:N1}s {5}" -f `
                   (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Name, $Test.IsPresent, $code, $sw.Elapsed.TotalSeconds, $verdict)
    Write-Host ("[session-run] exit={0} elapsed={1:N1}s {2}" -f $code, $sw.Elapsed.TotalSeconds, $verdict) -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Red' })
    exit $code
}
finally {
    if ($lockStream) {
        $lockStream.Dispose()
        # 只删自己的锁：dispose 后可能有下一个会话已经抢到同一路径；
        # 对比 pid+time，避免把别人的新锁删掉。
        if ($lockJson) {
            try {
                $cur = Get-Content -LiteralPath $lockPath -Raw -ErrorAction Stop
                $curInfo = $cur | ConvertFrom-Json
                if ([int]$curInfo.pid -eq $PID -and [string]$curInfo.time -eq $lockTime) {
                    Remove-Item $lockPath -Force -ErrorAction SilentlyContinue
                }
            } catch { }
        }
    }
}
