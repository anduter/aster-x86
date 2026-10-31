# =============================================================================
# sdk-demo.ps1 - Aster SDK 示例 GUI app 在 QEMU 里真跑（WP3-4 出口）
#
# 做什么：
#   1. tools/build-ng.ps1 -SdkDemo -DistDir .dist-sdk -ObjDir .obj-sdk
#      —— 只把 /bin/demo 的源文件换成 sdk/examples/hello_gui/main.c（blob 符号
#      ng_demo_elf 不变），所以 init -> sessiond -> WM 的启动路径、WM 启动器
#      的 LAUNCH、display v3 的 CREATE/LIST/FOCUS 全是产品代码，SDK 示例是
#      被 sessiond/WM 真正拉起的一个 GUI 应用。
#   2. 启动 QEMU（stdvga + QMP），经 QMP 注入按键驱动 WP3-1/3-2/3-3 自检；
#      等 SDK 示例的会话/窗口/令牌证据行，以及 WM 任务表里的
#      `title=Aster SDK`。
#   3. QMP `stop` 冻结画面后 screendump，断言：
#        - 面板 (4,4) = #202838（WM 面板）
#        - SDK 窗口内 (x+40,y+80) = #3070c0（示例画的强调色块）
#      坐标 (x,y) 取自 WM 自己的任务行，窗口放置由 displayd 决定。
#
# 断言（任一缺失 -> exit 1）：
#   ng: sdk session ok (session=.. token=0x..)
#   ng: sdk window ok (id=.. w=320 h=200)
#   ng: sdk token ok (token=0x.. id=..)          <- sessiond -> argv -> displayd
#   ng: sdk demo ok (pid=.. id=.. mode=window token=0x..)
#   ng: session app launch ok (pid=.. app=/bin/demo token=0x..)
#   ng: session window ok (pid=.. id=.. title=Aster SDK)
#   ng: wm task ok (id=.. x=.. y=.. title=Aster SDK)   （WP3-3 启动之后的）
#   screendump 像素断言 panel(4,4) + window(x+40,y+80)
#
# 成功输出：`sdk-demo: PASS (...)`，退出码 0。
#
# usage:
#   powershell -ExecutionPolicy Bypass -File tools/sdk-demo.ps1
#   powershell ... -NoBuild          # 复用 .dist-sdk/.obj-sdk
#   powershell ... -Cpu qemu64       # 其它 CPU 模型
#   powershell ... -KeepLog          # 保留临时串口日志/截图
# =============================================================================
param(
    [string]$Repo = '',
    [string]$Cpu = 'max',
    [int]$TimeoutSec = 90,
    [switch]$NoBuild,
    [switch]$KeepLog
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $Repo)) {
    Write-Host ("sdk-demo: FAIL (repo not found: {0})" -f $Repo) -ForegroundColor Red
    exit 1
}
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$OnWindows = ($env:OS -eq 'Windows_NT')

function Fail([string]$msg) {
    Write-Host ("sdk-demo: FAIL ({0})" -f $msg) -ForegroundColor Red
    exit 1
}

# clang/lld 在 MSYS2（WP0-4 工具链）。
$mingw = 'D:\msys64\mingw64\bin'
if ((Test-Path (Join-Path $mingw 'clang.exe')) -and
    ($env:PATH -notlike ('*' + $mingw + '*'))) {
    $env:PATH = $mingw + ';' + $env:PATH
}

$distRel = '.dist-sdk'
$objRel = '.obj-sdk'
$elf = Join-Path (Join-Path $Repo $distRel) 'aster-ng.elf'
$disk = Join-Path (Join-Path $Repo $distRel) 'disk.img'

# ---- 1. 构建 SdkDemo 内核 --------------------------------------------------
if (-not $NoBuild) {
    Write-Host '=== sdk-demo build (WP3-4: -SdkDemo) ==='
    $psHost = $null
    try { $psHost = (Get-Process -Id $PID).Path } catch { }
    if (-not $psHost) { $psHost = 'powershell' }
    $buildArgs = @('-NoProfile')
    if ($OnWindows) { $buildArgs += @('-ExecutionPolicy', 'Bypass') }
    $buildArgs += @('-File', (Join-Path $PSScriptRoot 'build-ng.ps1'),
        '-Repo', $Repo, '-DistDir', $distRel, '-ObjDir', $objRel, '-SdkDemo')
    & $psHost @buildArgs
    if ($LASTEXITCODE -ne 0) { Fail 'build-ng.ps1 -SdkDemo failed' }
} else {
    Write-Host 'sdk-demo: reuse existing .dist-sdk/.obj-sdk (-NoBuild)'
}
if (-not (Test-Path -LiteralPath $elf)) { Fail ("ELF not found: {0}" -f $elf) }
if (-not (Test-Path -LiteralPath $disk)) { Fail ("disk image not found: {0}" -f $disk) }

# ---- 2. QEMU ---------------------------------------------------------------
$qemu = $null
$qcmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if ($qcmd) {
    $qemu = $qcmd.Source
} elseif ($OnWindows -and (Test-Path 'D:\Program Files\qemu\qemu-system-x86_64.exe')) {
    $qemu = 'D:\Program Files\qemu\qemu-system-x86_64.exe'
}
if (-not $qemu) { Fail 'qemu-system-x86_64 not found (PATH or D:\Program Files\qemu)' }

$tmpBase = Join-Path ([IO.Path]::GetTempPath()) ("aster-sdk-demo-{0}" -f $PID)
New-Item -ItemType Directory -Force -Path $tmpBase | Out-Null
$serialLog = Join-Path $tmpBase 'serial.log'
$errLog = Join-Path $tmpBase 'qemu-err.log'
$outLog = Join-Path $tmpBase 'qemu-out.log'
$shot = Join-Path $tmpBase 'sdk-screen.ppm'
Remove-Item -LiteralPath $serialLog, $errLog, $outLog, $shot -Force -ErrorAction SilentlyContinue

$qmpPort = 0
try {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $qmpPort = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
} catch {
    Fail 'cannot reserve a QMP port'
}

Write-Host ("sdk-demo: elf  = {0} ({1} bytes)" -f $elf, (Get-Item -LiteralPath $elf).Length)
Write-Host ("sdk-demo: disk = {0}" -f $disk)
Write-Host ("sdk-demo: qemu = {0}" -f $qemu)

$qemuArgs = @(
    '-kernel', ('"' + $elf + '"'),
    '-serial', ('"file:' + $serialLog + '"'),
    '-drive', ('"file=' + $disk + ',format=raw,if=ide"'),
    '-display', 'none',
    '-no-reboot',
    '-m', '256',
    '-cpu', $Cpu,
    '-vga', 'std',
    '-qmp', ("tcp:127.0.0.1:{0},server=on,wait=off" -f $qmpPort)
)
$spArgs = @{
    FilePath = $qemu
    ArgumentList = $qemuArgs
    PassThru = $true
    RedirectStandardError = $errLog
    RedirectStandardOutput = $outLog
}
if ($OnWindows) { $spArgs['WindowStyle'] = 'Hidden' }
$p = Start-Process @spArgs

# ---- QMP 小工具 ------------------------------------------------------------
function Connect-Qmp([int]$Port) {
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $client = [Net.Sockets.TcpClient]::new()
            $client.Connect('127.0.0.1', $Port)
            $stream = $client.GetStream()
            $stream.ReadTimeout = 3000
            $reader = New-Object IO.StreamReader($stream)
            $writer = New-Object IO.StreamWriter($stream)
            $writer.AutoFlush = $true
            $null = $reader.ReadLine()   # greeting
            $writer.WriteLine('{"execute":"qmp_capabilities"}')
            $null = $reader.ReadLine()
            return @{ Client = $client; Reader = $reader; Writer = $writer }
        } catch {
            Start-Sleep -Milliseconds 50
        }
    }
    return $null
}

function Send-QmpJson($Qmp, [string]$Json) {
    if (-not $Qmp) { return $false }
    try {
        $Qmp.Writer.WriteLine($Json)
        # stop/screendump 期间 QEMU 可能先吐异步 event（例如 {"event":"STOP"}），
        # 必须继续读到本次命令的 return/error，不能只看第一行。
        for ($i = 0; $i -lt 8; $i++) {
            $resp = $Qmp.Reader.ReadLine()
            if (-not $resp) { return $false }
            if ($resp -match '"return"') { return $true }
            if ($resp -match '"error"') {
                Write-Host ("sdk-demo: QMP error: {0}" -f $resp)
                return $false
            }
            Write-Host ("sdk-demo: QMP event: {0}" -f $resp)
        }
    } catch {
        return $false
    }
    return $false
}

function Send-QmpKey($Qmp, [string]$Key) {
    if (-not $Qmp) { return $false }
    $events = ('{{"execute":"input-send-event","arguments":{{"events":[' +
        '{{"type":"key","data":{{"down":true,"key":{{"type":"qcode","data":"{0}"}}}}}},' +
        '{{"type":"key","data":{{"down":false,"key":{{"type":"qcode","data":"{0}"}}}}}}' +
        ']}}}}') -f $Key
    if (Send-QmpJson $Qmp $events) { return $true }
    return (Send-QmpJson $Qmp ('{{"execute":"human-monitor-command","arguments":{{"command-line":"sendkey {0}"}}}}' -f $Key))
}

function Send-QmpStop($Qmp) {
    return (Send-QmpJson $Qmp '{"execute":"stop"}')
}

function Send-QmpScreendump($Qmp, [string]$Path) {
    $jsonPath = ($Path -replace '\\', '/')
    return (Send-QmpJson $Qmp ('{"execute":"screendump","arguments":{"filename":"' + $jsonPath + '"}}'))
}

function Read-Ppm([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $i = 0
    $tokens = @()
    while ($tokens.Count -lt 4) {
        while ($i -lt $bytes.Length -and ($bytes[$i] -eq 32 -or $bytes[$i] -eq 10 -or
                $bytes[$i] -eq 13 -or $bytes[$i] -eq 9)) { $i++ }
        if ($i -ge $bytes.Length) { return $null }
        if ($bytes[$i] -eq 35) {
            while ($i -lt $bytes.Length -and $bytes[$i] -ne 10) { $i++ }
            continue
        }
        $start = $i
        while ($i -lt $bytes.Length -and -not ($bytes[$i] -eq 32 -or $bytes[$i] -eq 10 -or
                $bytes[$i] -eq 13 -or $bytes[$i] -eq 9)) { $i++ }
        $tokens += [Text.Encoding]::ASCII.GetString($bytes, $start, $i - $start)
    }
    $i++
    if ($tokens[0] -ne 'P6') { return $null }
    return @{ W = [int]$tokens[1]; H = [int]$tokens[2]; Offset = $i; Bytes = $bytes }
}

function Get-PpmPixel($Ppm, [int]$X, [int]$Y) {
    if ($null -eq $Ppm -or $X -lt 0 -or $Y -lt 0 -or $X -ge $Ppm.W -or $Y -ge $Ppm.H) {
        return $null
    }
    $o = $Ppm.Offset + ($Y * $Ppm.W + $X) * 3
    if ($o + 2 -ge $Ppm.Bytes.Length) { return $null }
    return @($Ppm.Bytes[$o], $Ppm.Bytes[$o + 1], $Ppm.Bytes[$o + 2])
}

function Format-Pixel($Px) {
    if ($null -eq $Px) { return '(none)' }
    return ('#{0:x2}{1:x2}{2:x2}' -f $Px[0], $Px[1], $Px[2])
}

# ---- 3. 跑 QEMU，注入按键，等证据 ------------------------------------------
$qmp = Connect-Qmp -Port $qmpPort
if (-not $qmp) {
    if (-not $p.HasExited) {
        try { $p.Kill() } catch { }
        $p.WaitForExit(3000) | Out-Null
    }
    Fail 'QMP connect failed (needed for key injection + screendump)'
}

$interactiveQuit = 0
$tabInjected = $false
$launchInjected = $false
$sessionLaunchInjected = $false
$pass = $false
$screenOk = $false
$wp33Pass = $false
$stopped = $false
$failWhy = ''
$windowX = 0
$windowY = 0
$sdkSession = 0
$sdkToken = 0
$serialText = ''
$sw = [Diagnostics.Stopwatch]::StartNew()

while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    if ($p.HasExited) { break }
    if (Test-Path -LiteralPath $serialLog) {
        $text = Get-Content -LiteralPath $serialLog -Raw -ErrorAction SilentlyContinue
        if ($text) {
            $serialText = $text
            if ($text -match 'ng: sdk demo FAIL') { $failWhy = 'SDK app reported FAIL'; break }
            if ($text -match 'ng: WP3-[123] selftest FAIL') { $failWhy = 'kernel WP3 selftest FAIL'; break }
            if ($text -match 'PANIC:') { $failWhy = 'kernel PANIC'; break }

            # 自检里的交互式 demo/hold 实例：注入 q 让它们尽快走干净退出路径。
            $interactiveCount = ([regex]::Matches($text,
                'ng: sdk demo ok \(pid=\d+ id=\d+ mode=interactive')).Count
            while ($interactiveQuit -lt $interactiveCount -and $qmp) {
                $interactiveQuit++
                $null = Send-QmpKey $qmp 'q'
                Write-Host ("sdk-demo: QMP q -> interactive instance {0}" -f $interactiveQuit)
            }
            # WP3-2：WM 看到窗口 -> Tab；切换完成 -> '1' 启动。
            if (-not $tabInjected -and $qmp -and $text.Contains('ng: wm tasks ok (')) {
                $tabInjected = $true
                $null = Send-QmpKey $qmp 'tab'
                Write-Host 'sdk-demo: QMP tab -> WM task switch'
            }
            if (-not $launchInjected -and $qmp -and $text.Contains('ng: wm task switch ok (focus=')) {
                $launchInjected = $true
                $null = Send-QmpKey $qmp '1'
                Write-Host 'sdk-demo: QMP 1 -> WM launcher'
            }
            # WP3-3：sessiond 的 WM 就绪 -> '1'（WM 发 LAUNCH，sessiond spawn 应用）。
            if (-not $sessionLaunchInjected -and $qmp -and $text.Contains('ng: session wm ready ok (wm=')) {
                $sessionLaunchInjected = $true
                $null = Send-QmpKey $qmp '1'
                Write-Host 'sdk-demo: QMP 1 -> session LAUNCH (SDK app)'
            }

            $hasSession = $text.Contains('ng: sdk session ok (')
            $hasWindow = $text.Contains('ng: sdk window ok (id=')
            $hasToken = $text.Contains('ng: sdk token ok (')
            $hasDemo = $text.Contains('ng: sdk demo ok (')

            # WP3-3 的 LAUNCH 之后必须看到 SDK 应用被会话登记、并被 WM 任务表列出。
            $launchMatches = [regex]::Matches($text,
                'ng: session app launch ok \(pid=\d+ app=/bin/demo token=0x[0-9a-f]+\)')
            $hasSessionWindow = $false
            $hasTask = $false
            if ($launchMatches.Count -gt 0) {
                $launchIdx = $launchMatches[$launchMatches.Count - 1].Index
                $tail = $text.Substring($launchIdx)
                $hasSessionWindow = ($tail -match 'ng: session window ok \(pid=\d+ id=\d+ title=Aster SDK\)')
                $taskMatches = [regex]::Matches($tail,
                    'ng: wm task ok \(id=\d+ x=(\d+) y=(\d+) title=Aster SDK\)')
                if ($taskMatches.Count -gt 0) {
                    $hasTask = $true
                    $m = $taskMatches[$taskMatches.Count - 1]
                    $windowX = [int]$m.Groups[1].Value
                    $windowY = [int]$m.Groups[2].Value
                }
            }

            if ($hasSession -and $hasWindow -and $hasToken -and $hasDemo -and
                $hasSessionWindow -and $hasTask) {
                # 冻结画面 -> 截图（避免内核自检随后 kill 应用/重启 WM 的画面竞态）。
                $stopped = Send-QmpStop $qmp
                Remove-Item -LiteralPath $shot -Force -ErrorAction SilentlyContinue
                $dumped = Send-QmpScreendump $qmp $shot
                for ($i = 0; $i -lt 40; $i++) {
                    if ((Test-Path -LiteralPath $shot) -and (Get-Item -LiteralPath $shot).Length -gt 54) { break }
                    Start-Sleep -Milliseconds 50
                }
                $ppm = $null
                if (Test-Path -LiteralPath $shot) { $ppm = Read-Ppm $shot }
                $panelPx = Get-PpmPixel $ppm 4 4
                $winPx = Get-PpmPixel $ppm ($windowX + 40) ($windowY + 80)
                if (-not $dumped -or $null -eq $ppm) {
                    $failWhy = 'screendump/PPM failed'
                } elseif ($panelPx[0] -ne 0x20 -or $panelPx[1] -ne 0x28 -or $panelPx[2] -ne 0x38) {
                    $failWhy = ("panel(4,4)=" + (Format-Pixel $panelPx) + ', want #202838')
                } elseif ($winPx[0] -ne 0x30 -or $winPx[1] -ne 0x70 -or $winPx[2] -ne 0xC0) {
                    $failWhy = ("window({0},{1})={2}, want #3070c0" -f ($windowX + 40), ($windowY + 80), (Format-Pixel $winPx))
                } else {
                    $screenOk = $true
                    $mSess = [regex]::Match($text,
                        'ng: sdk session ok \(session=(\d+) token=0x([0-9a-f]+)\)')
                    if ($mSess.Success) {
                        $sdkSession = [UInt32]$mSess.Groups[1].Value
                        $sdkToken = [Convert]::ToUInt32($mSess.Groups[2].Value, 16)
                    }
                }
                break
            }
        }
    }
    Start-Sleep -Milliseconds 40
}
$sw.Stop()

# ---- 3b. 恢复 VM，等内核自检在 SDK 应用上走完（WP3-3 PASS） ----------------
# 截图是在 QMP stop 的冻结画面上做的；继续跑完再断言 WP3-3，证明这个 SDK
# 应用不只是"窗口出现过"，而是真的走完了会话的注册/心跳/回收/清理路径。
if ($screenOk) {
    if (-not (Send-QmpJson $qmp '{"execute":"cont"}')) {
        $failWhy = 'QMP cont failed after screendump'
        $screenOk = $false
    }
    if ($screenOk) {
        $waitSw = [Diagnostics.Stopwatch]::StartNew()
        while ($waitSw.Elapsed.TotalSeconds -lt 15) {
            if (Test-Path -LiteralPath $serialLog) {
                $t2 = Get-Content -LiteralPath $serialLog -Raw -ErrorAction SilentlyContinue
                if ($t2 -match 'ng: WP3-3 selftest PASS') { $wp33Pass = $true; break }
                if ($t2 -match 'ng: WP3-3 selftest FAIL') {
                    $failWhy = 'WP3-3 selftest FAIL with SDK app'
                    break
                }
            }
            Start-Sleep -Milliseconds 80
        }
        if ($wp33Pass) {
            $pass = $true
        } elseif (-not $failWhy) {
            $failWhy = 'WP3-3 selftest did not PASS after SDK app demo'
        }
    }
}

# ---- 4. 结束 QEMU ----------------------------------------------------------
if (-not $p.HasExited) {
    try { $p.Kill() } catch { }
    $p.WaitForExit(3000) | Out-Null
}
if ($qmp) {
    try { $qmp.Client.Close() } catch { }
    $qmp = $null
}

$serialText = ''
if (Test-Path -LiteralPath $serialLog) {
    $serialText = (Get-Content -LiteralPath $serialLog -Raw -ErrorAction SilentlyContinue)
}

if (-not $pass) {
    if (-not $failWhy) {
        $missing = @()
        if ($serialText -notmatch 'ng: sdk session ok \(') { $missing += 'sdk session ok' }
        if ($serialText -notmatch 'ng: sdk token ok \(') { $missing += 'sdk token ok' }
        if ($serialText -notmatch 'ng: sdk demo ok \(') { $missing += 'sdk demo ok' }
        if ($serialText -notmatch 'ng: session window ok \(.*title=Aster SDK\)') { $missing += 'session window Aster SDK' }
        if ($serialText -notmatch 'ng: wm task ok \(.*title=Aster SDK\)') { $missing += 'WM task Aster SDK' }
        if ($missing.Count -gt 0) { $failWhy = 'missing: ' + ($missing -join ', ') }
        else { $failWhy = 'timeout before screendump assertions' }
    }
    Write-Host 'sdk-demo: serial output (tail):'
    Write-Host ('-' * 48)
    if ($serialText) {
        $lines = $serialText.TrimEnd() -split "`r?`n"
        $from = [Math]::Max(0, $lines.Count - 80)
        for ($i = $from; $i -lt $lines.Count; $i++) { Write-Host $lines[$i] }
    } else {
        Write-Host '(empty)'
    }
    Write-Host ('-' * 48)
    if (Test-Path -LiteralPath $errLog) {
        $qe = Get-Content -LiteralPath $errLog -Raw -ErrorAction SilentlyContinue
        if ($qe -and $qe.Trim()) { Write-Host ('qemu stderr: ' + $qe.Trim()) }
    }
    if (-not $KeepLog) { Remove-Item -Recurse -Force $tmpBase -ErrorAction SilentlyContinue }
    Fail ("missing SDK demo evidence ({0})" -f $failWhy)
}

Write-Host ('sdk-demo: session ok (session={0} token=0x{1:x8}) window=({2},{3})' -f $sdkSession, $sdkToken, $windowX, $windowY)
Write-Host ('sdk-demo: matched SDK session/window/token lines + WM task "Aster SDK" + session registry')
Write-Host ('sdk-demo: screendump pixel ok panel(4,4)=#202838 window({0},{1})=#3070c0' -f ($windowX + 40), ($windowY + 80))
Write-Host 'sdk-demo: WP3-3 selftest PASS after resume (SDK app registered/heartbeat/reaped)'
Write-Host ('sdk-demo: PASS (session channel + display v3 token + WM task list + WP3-3, {0:N1}s, cpu {1})' -f $sw.Elapsed.TotalSeconds, $Cpu)
if ($KeepLog) {
    Write-Host ("sdk-demo: logs kept in {0}" -f $tmpBase)
} else {
    Remove-Item -Recurse -Force $tmpBase -ErrorAction SilentlyContinue
}
exit 0
