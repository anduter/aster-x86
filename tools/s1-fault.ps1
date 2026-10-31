# =============================================================================
# s1-fault.ps1 - ng WP3-5: S1 故障注入（shell/WM 崩溃或卡死时 L3 保活）
#
# 口径（docs/design/architecture.md §3）：S1 的 L3 存活 SLA —— shell/WM 崩溃
# 或卡死时，L3 进程、文档状态、输入不丢；允许视觉中断、焦点/IME 重置、重绘。
# display server 崩溃保活属 S2、GPU 恢复属 S3，本工具不测。
#
# 场景（每个场景独立起一个 QEMU，互不影响）：
#   a) 杀 WM：Demo 进程存活、sessiond 按 restart_limit=3 重启 WM（新 pid）、
#      面板回来（QMP screendump 像素）、按键仍能触发 LAUNCH（输入可用）。
#   b) 杀输入/焦点归属的应用：sessiond 回收窗口（reaps+1）、WM 不跟着重启、
#      按键仍能启动新应用（输入恢复）。
#   c) 杀 sessiond：session 域崩溃（D0 没有 sessiond 接管，architecture
#      开放决策 2），记录行为：WM/应用/输入仍活、面板仍在、没有重启；
#      不要求恢复。
#   d) WM 死循环：协作式调度没有抢占，WM 自旋后 sessiond/displayd 都拿不到
#      CPU，整个 VM 停住（已知限制，S1 SLA 不满足）；只断言"确实卡死且
#      最后一帧面板/窗口仍在"，记录为 KNOWN。
#
# 做法：不改 build-ng / run-ng / kernel。客户机代理 tests/s1/s1fault.c 编成
# 一个 ring3 ELF，由 tests/s1/fat-add.ps1 写进 FAT16 磁盘镜像的副本；ring3
# shell 用 `spawn /mnt/disk/s1fault.elf <mode>` 拉起它（proc_spawn 支持任意
# VFS 路径），代理再 `spawn /bin/sessiond` 走真实会话启动路径，然后按
# tests/s1/s1_logic.c 的步骤表读 /run/display.status、kill 目标、打印证据。
# host 侧本脚本负责：QEMU/QMP、按键注入、串口轮询、screendump 像素断言、
# 每场景的 PASS/FAIL/KNOWN 与退出码。
#
# QEMU 参数复用 run-ng：-kernel dist-ng/aster-ng.elf、-drive <disk>,format=raw,
# if=ide、-display none、-serial file:<log>、-no-reboot、-m 256、-cpu max、
# -vga std；QMP 是独立随机 TCP 端口，不和并行会话冲突。
#
# 证据（每个场景的串口日志 + PPM 留在 -WorkDir，默认 obj-ng/s1/last）：
#   <server> ng: s1 agent up (pid=.. mode=..)
#   <agent>  ng: s1 session up (sessiond=.. displayd=.. wm=.. app=.. ...)
#   <agent>  ng: s1 wm killed ok / wm restart ok / demo alive ok / input ok
#   <agent>  ng: s1 app killed ok / app reaped ok / input recovery ok
#   <agent>  ng: s1 sessiond killed ok / sessiond dead ok
#   <agent>  ng: s1 spin armed (wm=.. scancode=0x58)
#   <agent>  ng: s1 scenario a PASS (step=4)   （d 没有 PASS，host 判 KNOWN）
#   QMP:     screendump ok panel(4,4)=#202838 demo(217,177)=#3070c0
#
# usage:
#   powershell -File tools/s1-fault.ps1                  # 全部 4 个场景
#   powershell -File tools/s1-fault.ps1 -Scenario a      # 只跑杀 WM
#   powershell -File tools/s1-fault.ps1 -Cpu qemu64
#   powershell -File tools/s1-fault.ps1 -WorkDir D:\tmp\s1
# 退出码：0 = 所有场景 PASS 或 KNOWN（d 的已知限制）；1 = 有 FAIL。
#
# 不进 ci-ng：4 个 QEMU 启动约 1 分钟，比 run-ng 重；claims 里给了复跑命令。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$ElfPath = '',
    [string]$DiskImg = '',
    [ValidateSet('all', 'a', 'b', 'c', 'd')][string]$Scenario = 'all',
    [string]$Cpu = 'max',
    [string]$WorkDir = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path
$OnWindows = ($env:OS -eq 'Windows_NT')
$mingw = 'D:\msys64\mingw64\bin'

function Fail([string]$msg) {
    Write-Host ("s1-fault: FAIL ({0})" -f $msg)
    exit 1
}

# 场景内部的失败：throw 给 Run-Scenario 捕获，保证还能停 QEMU/打印串口。
function Abort-Scenario([string]$msg) {
    throw $msg
}

# ---- 工具链 ---------------------------------------------------------------
$clangCmd = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clangCmd -and (Test-Path $mingw)) {
    $env:PATH = $mingw + ';' + $env:PATH
    $clangCmd = Get-Command clang -ErrorAction SilentlyContinue
}
if (-not $clangCmd) { Fail 'clang not found; add D:\msys64\mingw64\bin to PATH' }
$script:Clang = $clangCmd.Source

$lldCmd = Get-Command ld.lld -ErrorAction SilentlyContinue
if (-not $lldCmd -and (Test-Path $mingw)) {
    $env:PATH = $mingw + ';' + $env:PATH
    $lldCmd = Get-Command ld.lld -ErrorAction SilentlyContinue
}
if (-not $lldCmd) { Fail 'ld.lld not found; install lld or add it to PATH' }

# ---- QEMU -----------------------------------------------------------------
$qemu = $null
$qcmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if ($qcmd) {
    $qemu = $qcmd.Source
} elseif ($OnWindows -and (Test-Path 'D:\Program Files\qemu\qemu-system-x86_64.exe')) {
    $qemu = 'D:\Program Files\qemu\qemu-system-x86_64.exe'
}
if (-not $qemu) { Fail 'qemu-system-x86_64 not found (PATH or D:\Program Files\qemu)' }
$script:QemuExe = $qemu
$qemuVer = ''
$verOut = & $qemu --version 2>&1
if ($LASTEXITCODE -eq 0 -and $verOut) { $qemuVer = (@($verOut)[0]).ToString() }

# ---- 内核 ELF / 基础磁盘镜像 ----------------------------------------------
if ($ElfPath) {
    if (-not (Test-Path -LiteralPath $ElfPath)) { Fail ("ELF not found: {0}" -f $ElfPath) }
    $script:Elf = (Resolve-Path -LiteralPath $ElfPath).Path
} else {
    $script:Elf = Join-Path (Join-Path $Repo 'dist-ng') 'aster-ng.elf'
}

$baseDisk = ''
if ($DiskImg) {
    if (-not (Test-Path -LiteralPath $DiskImg)) { Fail ("disk image not found: {0}" -f $DiskImg) }
    $baseDisk = (Resolve-Path -LiteralPath $DiskImg).Path
} else {
    $baseDisk = Join-Path (Join-Path $Repo 'dist-ng') 'disk.img'
}

if (-not (Test-Path -LiteralPath $script:Elf) -or -not (Test-Path -LiteralPath $baseDisk)) {
    Write-Host 's1-fault: kernel/disk image not built yet, running build-ng.ps1 first'
    $psHost = $null
    try { $psHost = (Get-Process -Id $PID).Path } catch { }
    if (-not $psHost) { $psHost = 'powershell' }
    $buildArgs = @('-NoProfile')
    if ($OnWindows) { $buildArgs += @('-ExecutionPolicy', 'Bypass') }
    $buildArgs += @('-File', (Join-Path $PSScriptRoot 'build-ng.ps1'), '-Repo', $Repo)
    & $psHost @buildArgs
    if ($LASTEXITCODE -ne 0) { Fail 'build-ng.ps1 failed' }
    if (-not (Test-Path -LiteralPath $script:Elf)) { Fail 'build-ng.ps1 did not produce the ELF' }
}

# ---- 工作目录（串口日志 / PPM / 代理 ELF / 磁盘副本）----------------------
if (-not $WorkDir) { $WorkDir = Join-Path $Repo 'obj-ng/s1/last' }
if (-not [IO.Path]::IsPathRooted($WorkDir)) { $WorkDir = Join-Path $Repo $WorkDir }
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$WorkDir = (Resolve-Path -LiteralPath $WorkDir).Path

Write-Host '=== s1-fault (WP3-5) ==='
Write-Host ("repo:  {0}" -f $Repo)
Write-Host ("elf:   {0} ({1} bytes)" -f $script:Elf, (Get-Item -LiteralPath $script:Elf).Length)
Write-Host ("disk:  {0} ({1} bytes, base)" -f $baseDisk, (Get-Item -LiteralPath $baseDisk).Length)
Write-Host ("work:  {0}" -f $WorkDir)
Write-Host ("qemu:  {0}" -f $qemu)
if ($qemuVer) { Write-Host ("       {0}" -f $qemuVer) }
Write-Host ("cpu:   {0}   scenarios: {1}" -f $Cpu, $Scenario)

# ---- 内部 powershell 调用（mkfat-ng / fat-add 都以 exit 结束，必须子进程）--
function Invoke-PsChild {
    param([string]$ScriptPath, [string[]]$ChildArgs)
    $psHost = $null
    try { $psHost = (Get-Process -Id $PID).Path } catch { }
    if (-not $psHost) { $psHost = 'powershell' }
    $a = @('-NoProfile')
    if ($OnWindows) { $a += @('-ExecutionPolicy', 'Bypass') }
    $a += @('-File', $ScriptPath) + $ChildArgs
    $out = & $psHost @a 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { if ("$_".Trim()) { Write-Host $_ } }
    return $code
}

# ---- 编译 / 校验客户机代理 ELF --------------------------------------------
function Build-Agent([string]$OutElf) {
    $target = '--target=x86_64-elf'
    $cflags = @($target, '-ffreestanding', '-nostdlib', '-mno-red-zone',
        '-fno-stack-protector', '-fcf-protection=none', '-mno-sse',
        '-mgeneral-regs-only', '-fno-builtin', '-fno-pic', '-fno-pie',
        '-std=c11', '-Os', '-Wall', '-Wextra', '-Werror',
        '-ffunction-sections', '-fdata-sections',
        '-I', (Join-Path $Repo 'src/ng'),
        '-I', (Join-Path $Repo 'tests/s1'), '-c')
    $faultSrc = Join-Path $Repo 'tests/s1/s1fault.c'
    $logicSrc = Join-Path $Repo 'tests/s1/s1_logic.c'
    if (-not (Test-Path -LiteralPath $faultSrc)) { Abort-Scenario ("missing {0}" -f $faultSrc) }
    if (-not (Test-Path -LiteralPath $logicSrc)) { Abort-Scenario ("missing {0}" -f $logicSrc) }
    $faultObj = Join-Path $WorkDir 's1fault.o'
    $logicObj = Join-Path $WorkDir 's1_logic.o'
    Write-Host 's1-fault: building guest agent (tests/s1/s1fault.c)'
    $out = & $script:Clang @($cflags + @($logicSrc, '-o', $logicObj)) 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; Abort-Scenario 'agent s1_logic.c compile failed' }
    $out = & $script:Clang @($cflags + @($faultSrc, '-o', $faultObj)) 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; Abort-Scenario 'agent s1fault.c compile failed' }
    $userLd = Join-Path $Repo 'src/ng/user/user.ld'
    $linkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
        ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
        '-Wl,--gc-sections', '-o', $OutElf, $faultObj, $logicObj)
    $out = & $script:Clang @linkArgs 2>&1
    if ($LASTEXITCODE -ne 0) { $out | ForEach-Object { Write-Host $_ }; Abort-Scenario 'agent link failed' }
    if (-not (Test-Path -LiteralPath $OutElf)) { Abort-Scenario 'agent ELF not produced' }
}

function Test-AgentElf([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $probs = New-Object System.Collections.Generic.List[string]
    if ($bytes.Length -lt 64) { $probs.Add('smaller than ELF64 header') } else {
        if (-not ($bytes[0] -eq 0x7F -and $bytes[1] -eq 0x45 -and $bytes[2] -eq 0x4C -and $bytes[3] -eq 0x46)) {
            $probs.Add('bad magic')
        }
        if ($bytes[4] -ne 2) { $probs.Add(("class={0}, want 2" -f $bytes[4])) }
        if ($bytes[5] -ne 1) { $probs.Add(("data={0}, want 1" -f $bytes[5])) }
        $eType = [BitConverter]::ToUInt16($bytes, 16)
        $eMachine = [BitConverter]::ToUInt16($bytes, 18)
        $entry = [BitConverter]::ToUInt64($bytes, 24)
        $phOff = [BitConverter]::ToUInt64($bytes, 32)
        $phEnt = [BitConverter]::ToUInt16($bytes, 54)
        $phNum = [BitConverter]::ToUInt16($bytes, 56)
        if ($eType -ne 2) { $probs.Add(("e_type={0}, want 2" -f $eType)) }
        if ($eMachine -ne 62) { $probs.Add(("e_machine={0}, want 62" -f $eMachine)) }
        if ($entry -lt 0x40000000 -or $entry -ge 0x40010000) {
            $probs.Add(("e_entry=0x{0:x} outside 0x40000000..0x40010000" -f $entry))
        }
        $load = 0
        for ($i = 0; $i -lt $phNum; $i++) {
            $ph = [int]$phOff + ($i * $phEnt)
            if ($ph + 56 -gt $bytes.Length) { $probs.Add('phdr outside file'); break }
            if ([BitConverter]::ToUInt32($bytes, $ph) -ne 1) { continue }
            $load++
            $pFlags = [BitConverter]::ToUInt32($bytes, $ph + 4)
            $pVaddr = [BitConverter]::ToUInt64($bytes, $ph + 16)
            $pFilesz = [BitConverter]::ToUInt64($bytes, $ph + 32)
            $pMemsz = [BitConverter]::ToUInt64($bytes, $ph + 40)
            $pAlign = [BitConverter]::ToUInt64($bytes, $ph + 48)
            if (($pVaddr % 4096) -ne 0) { $probs.Add(("PT_LOAD vaddr=0x{0:x} not 4 KiB aligned" -f $pVaddr)) }
            if ($pAlign -lt 4096) { $probs.Add('PT_LOAD align < 4096') }
            if ($pFilesz -gt $pMemsz) { $probs.Add('PT_LOAD filesz > memsz') }
            if (($pFlags -band 3) -eq 3) { $probs.Add(("PT_LOAD 0x{0:x} is W+X" -f $pVaddr)) }
            if ($pVaddr -lt 0x40000000) { $probs.Add('PT_LOAD below base') }
            if (($pVaddr + $pMemsz) -gt 0x40010000) { $probs.Add('PT_LOAD above 0x40010000') }
        }
        if ($load -lt 1) { $probs.Add('no PT_LOAD') }
    }
    if ($probs.Count -gt 0) {
        foreach ($p in $probs) { Write-Host ("  ! {0}" -f $p) }
        Abort-Scenario 'agent ELF verification failed'
    }
}

$agentElf = Join-Path $WorkDir 's1fault.elf'
Build-Agent $agentElf
Test-AgentElf $agentElf
$agentInfo = Get-Item -LiteralPath $agentElf
Write-Host ("s1-fault: guest agent ok ({0} bytes)" -f $agentInfo.Length)

# ---- 磁盘镜像：mkfat-ng 生成 + 注入代理 ELF ------------------------------
$script:DiskS1 = Join-Path $WorkDir 'disk-s1.img'
if ($DiskImg) {
    Copy-Item -LiteralPath $baseDisk -Destination $script:DiskS1 -Force
    Write-Host ("s1-fault: disk copy from base ({0})" -f (Split-Path -Leaf $baseDisk))
} else {
    $mkfat = Join-Path $PSScriptRoot 'mkfat-ng.ps1'
    $rc = Invoke-PsChild $mkfat @('-Repo', $Repo, '-Out', $script:DiskS1, '-SizeMB', '8')
    if ($rc -ne 0) { Fail ("mkfat-ng.ps1 failed (exit {0})" -f $rc) }
}
$fatAdd = Join-Path $Repo 'tests/s1/fat-add.ps1'
$rc = Invoke-PsChild $fatAdd @('-Image', $script:DiskS1, '-File', $agentElf, '-Name', 'S1FAULT.ELF')
if ($rc -ne 0) { Fail ("fat-add.ps1 failed (exit {0})" -f $rc) }
Write-Host ("s1-fault: disk image ok ({0} bytes, /mnt/disk/S1FAULT.ELF)" -f (Get-Item -LiteralPath $script:DiskS1).Length)

# ---- QMP ------------------------------------------------------------------
function Reserve-QmpPort {
    try {
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $listener.Start()
        $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
        $listener.Stop()
        return $port
    } catch {
        Abort-Scenario 'cannot reserve a QMP port'
    }
}

function Connect-Qmp([int]$Port) {
    for ($i = 0; $i -lt 80; $i++) {
        try {
            $client = [Net.Sockets.TcpClient]::new()
            $client.Connect('127.0.0.1', $Port)
            $stream = $client.GetStream()
            $stream.ReadTimeout = 3000
            $reader = New-Object IO.StreamReader($stream)
            $writer = New-Object IO.StreamWriter($stream)
            $writer.AutoFlush = $true
            $null = $reader.ReadLine()  # QMP greeting
            $writer.WriteLine('{"execute":"qmp_capabilities"}')
            $null = $reader.ReadLine()
            return @{ Client = $client; Reader = $reader; Writer = $writer }
        } catch {
            Start-Sleep -Milliseconds 50
        }
    }
    return $null
}

function Send-QmpKey($Qmp, [string]$Key) {
    if (-not $Qmp) { return $false }
    $events = ('{{"execute":"input-send-event","arguments":{{"events":[' +
        '{{"type":"key","data":{{"down":true,"key":{{"type":"qcode","data":"{0}"}}}}}},' +
        '{{"type":"key","data":{{"down":false,"key":{{"type":"qcode","data":"{0}"}}}}}}' +
        ']}}}}') -f $Key
    try {
        $Qmp.Writer.WriteLine($events)
        $resp = $Qmp.Reader.ReadLine()
        if ($resp -and ($resp -match '"return"')) { return $true }
        Write-Host ("s1-fault: QMP input-send-event response: {0}" -f $resp)
    } catch {
        return $false
    }
    return $false
}

function Send-QmpScreendump($Qmp, [string]$Path) {
    if (-not $Qmp) { return $false }
    $jsonPath = ($Path -replace '\\', '/')
    try {
        $Qmp.Writer.WriteLine('{"execute":"screendump","arguments":{"filename":"' + $jsonPath + '"}}')
        $resp = $Qmp.Reader.ReadLine()
        if ($resp -and ($resp -match '"return"')) { return $true }
        Write-Host ("s1-fault: QMP screendump response: {0}" -f $resp)
    } catch {
        return $false
    }
    return $false
}

# ---- 串口轮询 -------------------------------------------------------------
function Get-SerialText {
    if (-not $script:SerialLog) { return '' }
    if (-not (Test-Path -LiteralPath $script:SerialLog)) { return '' }
    $t = Get-Content -LiteralPath $script:SerialLog -Raw -ErrorAction SilentlyContinue
    if ($null -eq $t) { return '' }
    return $t
}

function Wait-Serial {
    param(
        [string]$Pattern,
        [double]$TimeoutSec,
        [string]$After = ''
    )
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $t = Get-SerialText
        if ($t) {
            $hay = $t
            if ($After) {
                $idx = $t.LastIndexOf($After)
                if ($idx -lt 0) {
                    Start-Sleep -Milliseconds 40
                    continue
                }
                $hay = $t.Substring($idx + $After.Length)
            }
            $m = [regex]::Match($hay, $Pattern)
            if ($m.Success) { return $m }
        }
        if ($script:Qemu -and $script:Qemu.HasExited) {
            Abort-Scenario 'QEMU exited before the expected serial line'
        }
        Start-Sleep -Milliseconds 40
    }
    return $null
}

function Wait-SerialCount([string]$Pattern, [int]$Min, [double]$TimeoutSec) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $t = Get-SerialText
        if ($t) {
            $n = ([regex]::Matches($t, $Pattern)).Count
            if ($n -ge $Min) { return $n }
        }
        if ($script:Qemu -and $script:Qemu.HasExited) {
            Abort-Scenario 'QEMU exited while waiting for serial line count'
        }
        Start-Sleep -Milliseconds 40
    }
    return -1
}

function Get-SerialCount([string]$Pattern) {
    $t = Get-SerialText
    if (-not $t) { return 0 }
    return ([regex]::Matches($t, $Pattern)).Count
}

# ---- 键盘注入 -------------------------------------------------------------
function Send-Typed([string]$Text) {
    $map = @{ ' ' = 'spc'; '/' = 'slash'; '.' = 'dot'; '-' = 'minus'; '=' = 'equal' }
    if (-not $script:Qmp) { Abort-Scenario 'no QMP for typing' }
    foreach ($ch in $Text.ToCharArray()) {
        $key = $map["$ch"]
        if (-not $key) { $key = "$ch" }
        if (-not (Send-QmpKey $script:Qmp $key)) {
            Abort-Scenario ("QMP typing failed at '{0}'" -f $ch)
        }
        Start-Sleep -Milliseconds 20
    }
    if (-not (Send-QmpKey $script:Qmp 'ret')) { Abort-Scenario 'QMP Enter failed' }
}

# ---- PPM 像素断言 ---------------------------------------------------------
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

function Test-ShotPixels {
    param([string]$Path, [switch]$NoDemo)
    if (-not (Test-Path -LiteralPath $Path)) { Abort-Scenario ("no screendump at {0}" -f $Path) }
    $ppm = Read-Ppm $Path
    if ($null -eq $ppm) { Abort-Scenario ("bad PPM {0}" -f $Path) }
    if ($ppm.W -ne 1280 -or $ppm.H -ne 800) {
        Abort-Scenario ("unexpected framebuffer {0}x{1}" -f $ppm.W, $ppm.H)
    }
    $p = Get-PpmPixel $ppm 4 4
    if ($null -eq $p) { Abort-Scenario 'no panel pixel' }
    if ($p[0] -ne 0x20 -or $p[1] -ne 0x28 -or $p[2] -ne 0x38) {
        Abort-Scenario ("panel (4,4)=#{0:x2}{1:x2}{2:x2}, want #202838" -f $p[0], $p[1], $p[2])
    }
    $msg = "panel(4,4)=#202838"
    if (-not $NoDemo) {
        $d = Get-PpmPixel $ppm 217 177
        if ($null -eq $d) { Abort-Scenario 'no demo pixel' }
        if ($d[0] -ne 0x30 -or $d[1] -ne 0x70 -or $d[2] -ne 0xC0) {
            Abort-Scenario ("demo (217,177)=#{0:x2}{1:x2}{2:x2}, want #3070c0" -f $d[0], $d[1], $d[2])
        }
        $msg += " demo(217,177)=#3070c0"
    }
    return $msg
}

function Take-Shot([string]$Name) {
    if (-not $script:Qmp) { Abort-Scenario 'no QMP for screendump' }
    $path = Join-Path $WorkDir ("screen-{0}.ppm" -f $Name)
    Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if (-not (Send-QmpScreendump $script:Qmp $path)) {
        Abort-Scenario 'QMP screendump failed'
    }
    for ($i = 0; $i -lt 80; $i++) {
        if ((Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -gt 54) { break }
        Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 150
    return $path
}

# 截图 + 像素断言；第一次可能撞上合成还没落 LFB，失败重试最多 3 次。
function Take-CheckedShot {
    param([string]$Name, [switch]$NoDemo)
    $lastErr = ''
    for ($try = 1; $try -le 3; $try++) {
        $path = Take-Shot ("{0}-{1}" -f $Name, $try)
        try {
            $pix = Test-ShotPixels $path -NoDemo:$NoDemo
            Write-Host ("s1-fault: screendump ok ({0}) {1}" -f (Split-Path -Leaf $path), $pix)
            return $pix
        } catch {
            $lastErr = $_.Exception.Message
            Start-Sleep -Milliseconds 300
        }
    }
    Abort-Scenario ("screendump pixel assertion failed ({0})" -f $lastErr)
}

# ---- 会话启动（shell -> agent -> sessiond）--------------------------------
function Start-Share([string]$Mode) {
    $script:SerialLog = Join-Path $WorkDir ("serial-{0}.log" -f $Mode)
    $errLog = Join-Path $WorkDir ("qemu-{0}.err" -f $Mode)
    $outLog = Join-Path $WorkDir ("qemu-{0}.out" -f $Mode)
    Remove-Item -LiteralPath $script:SerialLog -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $errLog -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $outLog -Force -ErrorAction SilentlyContinue
    $port = Reserve-QmpPort
    $qemuArgs = @(
        '-kernel', ('"' + $script:Elf + '"'),
        '-serial', ('"file:' + $script:SerialLog + '"'),
        '-drive', ('"file=' + $script:DiskS1 + ',format=raw,if=ide"'),
        '-display', 'none',
        '-no-reboot',
        '-m', '256',
        '-cpu', $Cpu,
        '-vga', 'std',
        '-qmp', ("tcp:127.0.0.1:{0},server=on,wait=off" -f $port)
    )
    $spArgs = @{
        FilePath = $script:QemuExe
        ArgumentList = $qemuArgs
        PassThru = $true
        RedirectStandardError = $errLog
        RedirectStandardOutput = $outLog
    }
    if ($OnWindows) { $spArgs['WindowStyle'] = 'Hidden' }
    $script:Qemu = Start-Process @spArgs
    $script:Qmp = Connect-Qmp $port
    if (-not $script:Qmp) { Abort-Scenario 'QMP connect failed' }
    Write-Host ("s1-fault: QEMU up (scenario {0}, qmp {1}, serial {2})" -f `
        $Mode, $port, (Split-Path -Leaf $script:SerialLog))

    if (-not (Wait-Serial 'ng: shell ready' 60)) { Abort-Scenario 'shell ready timeout' }
    Send-Typed ("spawn /mnt/disk/s1fault.elf {0}" -f $Mode)
    $m = Wait-Serial 'ng: shell spawn ok \(pid=(\d+)' 15
    if (-not $m) {
        Abort-Scenario 'guest agent spawn failed (FAT16 disk / spawn path)'
    }
    $agentPid = $m.Groups[1].Value
    Write-Host ("s1-fault: guest agent spawned (pid={0})" -f $agentPid)
    Send-Typed ("wait {0}" -f $agentPid)
    if (-not (Wait-Serial 'ng: s1 agent up \(pid=' 15)) {
        Abort-Scenario 'guest agent did not start (shell wait)'
    }
}

function Stop-Vm {
    if ($script:Qmp) {
        try { $script:Qmp.Client.Close() } catch { }
        $script:Qmp = $null
    }
    if ($script:Qemu) {
        if (-not $script:Qemu.HasExited) {
            try { $script:Qemu.Kill() } catch { }
            $script:Qemu.WaitForExit(3000) | Out-Null
        }
        $script:Qemu = $null
    }
}

# ---- 场景 a：杀 WM --------------------------------------------------------
function Invoke-ScenarioA {
    if (-not (Wait-Serial 'ng: s1 session up \(' 25)) { Abort-Scenario 'scenario a: session up timeout' }
    # 基线计数用增量：boot 的 WP3-3 自检也打印过 launcher/restart-ready/launch。
    $readyBefore = Get-SerialCount 'ng: session wm restart ready ok'
    $keyLaunchBefore = Get-SerialCount 'ng: wm launch req ok \(app=/bin/demo via=key\)'
    $appLaunchBefore = Get-SerialCount 'ng: session app launch ok'
    if (-not (Wait-Serial 'ng: s1 wm killed ok \(pid=(\d+)' 5)) { Abort-Scenario 'scenario a: wm kill evidence missing' }
    $m = Wait-Serial 'ng: s1 wm restart ok \(old=(\d+) new=(\d+) restarts=(\d+)' 25
    if (-not $m) { Abort-Scenario 'scenario a: WM restart timeout' }
    Write-Host ("s1-fault: [a] WM restarted ({0} -> {1}, restarts={2})" -f `
        $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value)
    # 新 WM 的 REGISTER_HOTKEY 在 wm_launcher_scan 之前；sessiond 的
    # `restart ready` 在 REGISTER 之后，用它做"新 WM 已经能收热键"的确认。
    # 不能用 `-After 'ng: s1 wm restart ok'`：WM 的 launcher 行可能比 agent
    # 的 restart 证据行更早（两个进程），会等到下一轮 WM 重启（^ 8s）。
    if ((Wait-SerialCount 'ng: session wm restart ready ok' ($readyBefore + 1) 12) -lt 0) {
        Abort-Scenario 'scenario a: new WM did not register'
    }
    $null = Take-CheckedShot 'a-wm-restart'
    if (-not (Wait-Serial 'ng: s1 input req \(scenario=a\)' 6)) {
        Abort-Scenario 'scenario a: input request missing'
    }
    # 连按两次 '1'：新 WM 的自检 fallback 可能先 auto-launch 一次，agent 只有
    # 看到 launches 至少 +2 才结束；host 另外单独断言 at least one via=key。
    if (-not (Send-QmpKey $script:Qmp '1')) { Abort-Scenario 'scenario a: QMP key 1 failed' }
    Start-Sleep -Milliseconds 400
    if (-not (Send-QmpKey $script:Qmp '1')) { Abort-Scenario 'scenario a: QMP key 1 failed' }
    Write-Host 's1-fault: [a] QMP sendkey 1 x2 -> new WM launcher -> sessiond LAUNCH'
    if ((Wait-SerialCount 'ng: wm launch req ok \(app=/bin/demo via=key\)' ($keyLaunchBefore + 1) 12) -lt 0) {
        Abort-Scenario 'scenario a: key did not reach the new WM launcher'
    }
    if ((Wait-SerialCount 'ng: session app launch ok' ($appLaunchBefore + 2) 12) -lt 0) {
        Abort-Scenario 'scenario a: key -> sessiond LAUNCH timeout'
    }
    if (-not (Wait-Serial 'ng: s1 input ok \(launches=' 10)) {
        Abort-Scenario 'scenario a: agent did not see the key launches'
    }
    if (-not (Wait-Serial 'ng: s1 scenario a PASS' 5)) {
        Abort-Scenario 'scenario a: agent did not report PASS'
    }
    $null = Take-CheckedShot 'a-final'
    return 'WM restarted with new pid; Demo survived; panel back; 2x key -> via=key LAUNCH ok'
}

# ---- 场景 b：杀输入/焦点应用 ---------------------------------------------
function Invoke-ScenarioB {
    if (-not (Wait-Serial 'ng: s1 session up \(' 25)) { Abort-Scenario 'scenario b: session up timeout' }
    if (-not (Wait-Serial 'ng: s1 app killed ok \(pid=(\d+)' 5)) { Abort-Scenario 'scenario b: app kill evidence missing' }
    $m = Wait-Serial 'ng: s1 app reaped ok \(pid=(\d+) reaps=(\d+)' 25
    if (-not $m) { Abort-Scenario 'scenario b: app reap timeout' }
    Write-Host ("s1-fault: [b] focused app reaped (pid={0}, reaps={1})" -f `
        $m.Groups[1].Value, $m.Groups[2].Value)
    $null = Take-CheckedShot 'b-app-reaped' -NoDemo
    if (-not (Wait-Serial 'ng: s1 input req \(scenario=b\)' 6)) {
        Abort-Scenario 'scenario b: input request missing'
    }
    if (-not (Send-QmpKey $script:Qmp '1')) { Abort-Scenario 'scenario b: QMP key 1 failed' }
    Write-Host "s1-fault: [b] QMP sendkey 1 -> WM launcher -> sessiond LAUNCH (input recovered)"
    if (-not (Wait-Serial 'ng: s1 input recovery ok \(launches=' 20)) {
        Abort-Scenario 'scenario b: relaunch timeout'
    }
    if (-not (Wait-Serial 'ng: s1 scenario b PASS' 5)) {
        Abort-Scenario 'scenario b: agent did not report PASS'
    }
    $null = Take-CheckedShot 'b-final'
    return 'focused app killed/reaped; WM stayed up; key launched a new app (input recovery)'
}

# ---- 场景 c：杀 sessiond --------------------------------------------------
function Invoke-ScenarioC {
    if (-not (Wait-Serial 'ng: s1 session up \(' 25)) { Abort-Scenario 'scenario c: session up timeout' }
    $m = Wait-Serial 'ng: s1 sessiond killed ok \(pid=(\d+)' 5
    if (-not $m) { Abort-Scenario 'scenario c: sessiond kill evidence missing' }
    $sessPid = $m.Groups[1].Value
    if (-not (Wait-Serial 'ng: s1 sessiond dead ok \(pid=' 10)) {
        Abort-Scenario 'scenario c: sessiond death not confirmed'
    }
    Write-Host ("s1-fault: [c] sessiond killed (pid={0}), no session takeover expected (D0)" -f $sessPid)
    $launchBefore = Get-SerialCount 'ng: wm launch req ok \('
    $null = Take-CheckedShot 'c-after-kill'
    if (-not (Wait-Serial 'ng: s1 input req \(scenario=c\)' 6)) {
        Abort-Scenario 'scenario c: input request missing'
    }
    if (-not (Send-QmpKey $script:Qmp '1')) { Abort-Scenario 'scenario c: QMP key 1 failed' }
    $n = Wait-SerialCount 'ng: wm launch req ok \(' ($launchBefore + 1) 8
    if ($n -lt 0) { Abort-Scenario 'scenario c: WM did not react to key after sessiond death' }
    Write-Host "s1-fault: [c] QMP sendkey 1 -> WM launcher still alive (input path intact)"
    if (-not (Wait-Serial 'ng: s1 scenario c PASS' 5)) {
        Abort-Scenario 'scenario c: agent did not report PASS'
    }
    $text = Get-SerialText
    $idx = $text.IndexOf('ng: s1 sessiond killed ok')
    if ($idx -ge 0 -and ($text.Substring($idx) -match 'ng: session wm restart (ok|sched)')) {
        Abort-Scenario 'scenario c: unexpected session recovery after sessiond death'
    }
    $null = Take-CheckedShot 'c-final'
    return 'sessiond died (no takeover, D0 known limitation); WM/Demo/input/panel survived'
}

# ---- 场景 d：WM 死循环（已知限制）----------------------------------------
function Invoke-ScenarioD {
    if (-not (Wait-Serial 'ng: s1 session up \(' 25)) { Abort-Scenario 'scenario d: session up timeout' }
    if (-not (Wait-Serial 'ng: s1 spin armed \(wm=(\d+)' 6)) {
        Abort-Scenario 'scenario d: spin arm missing'
    }
    if (-not (Wait-Serial 'ng: s1 input req \(scenario=d\)' 6)) {
        Abort-Scenario 'scenario d: input request missing'
    }
    # 自旋前先抓一帧：WM 卡死以后协作式调度会连 QEMU 的 main loop 一起饿死，
    # 冻结后的 screendump 不再返回（这一点本身就是已知限制的证据）。
    $null = Take-CheckedShot 'd-before-spin'
    $deadBefore = Get-SerialCount 'ng: session wm dead ok'
    $restartBefore = Get-SerialCount 'ng: session wm restart (ok|sched)'
    if (-not (Send-QmpKey $script:Qmp 'f12')) { Abort-Scenario 'scenario d: QMP f12 failed' }
    Write-Host 's1-fault: [d] QMP sendkey f12 -> WM fault spin (fault=spin only)'
    if (-not (Wait-Serial 'ng: wm fault spin \(scancode=0x58\)' 8)) {
        Abort-Scenario 'scenario d: WM did not enter the fault spin'
    }
    $t1 = Get-SerialText
    Start-Sleep -Milliseconds 1500
    $t2 = Get-SerialText
    if ($t2.Length -ne $t1.Length) {
        $extra = if ($t2.Length -gt $t1.Length) { $t2.Substring($t1.Length) } else { '(serial shrank?)' }
        Abort-Scenario ("scenario d: system kept making progress after WM spin ({0})" -f $extra.Trim())
    }
    $idx = $t2.LastIndexOf('ng: wm fault spin')
    if ($idx -ge 0) {
        $tail = $t2.Substring($idx)
        if ($tail -match 'ng: (session wm (dead|restart)|wm exit|s1 scenario)') {
            Abort-Scenario 'scenario d: session/WM made progress after spin (preemption exists?)'
        }
    }
    if ((Get-SerialCount 'ng: session wm dead ok') -ne $deadBefore -or
        (Get-SerialCount 'ng: session wm restart (ok|sched)') -ne $restartBefore) {
        Abort-Scenario 'scenario d: session manager reacted to the spin (preemption exists?)'
    }
    if ($script:Qemu.HasExited) { Abort-Scenario 'scenario d: QEMU exited during spin' }
    # 冻结后再试一次 screendump：单线程 TCG 下 vCPU 在 ring3 自旋会饿死
    # main loop，QMP 3s 内没有响应/文件 —— 这属于 d 的已知限制证据，不判 FAIL。
    $frozenShot = Join-Path $WorkDir 'screen-d-after-spin.ppm'
    Remove-Item -LiteralPath $frozenShot -Force -ErrorAction SilentlyContinue
    $shotOk = Send-QmpScreendump $script:Qmp $frozenShot
    if ($shotOk) {
        for ($i = 0; $i -lt 25; $i++) {
            if ((Test-Path -LiteralPath $frozenShot) -and (Get-Item -LiteralPath $frozenShot).Length -gt 54) { break }
            Start-Sleep -Milliseconds 40
        }
    }
    $freezeEvidence = ''
    if ((Test-Path -LiteralPath $frozenShot) -and (Get-Item -LiteralPath $frozenShot).Length -gt 54) {
        $pix = Test-ShotPixels $frozenShot
        Write-Host ("s1-fault: [d] frozen-frame screendump ok {0}" -f $pix)
        $freezeEvidence = 'frozen frame still has panel + window'
    } else {
        Write-Host 's1-fault: [d] QMP screendump after spin: no response within 3s (QEMU main loop starved) - freeze confirmed'
        $freezeEvidence = 'QMP screendump starved after spin (main loop starved)'
    }
    return @{
        Status = 'KNOWN'
        Detail = ('WM spin froze the whole VM (cooperative scheduler, no preemption): ' +
            'sessiond cannot detect/restart; ' + $freezeEvidence + ' - S1 SLA not met')
    }
}

# ---- 场景运行器 -----------------------------------------------------------
function Run-Scenario([string]$Mode) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $status = 'FAIL'
    $detail = ''
    $script:SerialLog = $null
    try {
        Start-Share $Mode
        switch ($Mode) {
            'a' { $detail = Invoke-ScenarioA }
            'b' { $detail = Invoke-ScenarioB }
            'c' { $detail = Invoke-ScenarioC }
            'd' {
                $r = Invoke-ScenarioD
                $status = $r.Status
                $detail = $r.Detail
                $sw.Stop()
                return [pscustomobject]@{ Mode = $Mode; Status = $status; Detail = $detail; Sec = [math]::Round($sw.Elapsed.TotalSeconds, 1) }
            }
        }
        $status = 'PASS'
    } catch {
        $status = 'FAIL'
        $detail = $_.Exception.Message
        Write-Host ("s1-fault: scenario {0} FAIL ({1})" -f $Mode, $detail)
        $text = Get-SerialText
        if ($text) {
            Write-Host 's1-fault: serial tail:'
            Write-Host ('-' * 56)
            $lines = $text.TrimEnd() -split "`r?`n"
            $start = [Math]::Max(0, $lines.Count - 40)
            for ($i = $start; $i -lt $lines.Count; $i++) { Write-Host $lines[$i] }
            Write-Host ('-' * 56)
        }
        $errLog = Join-Path $WorkDir ("qemu-{0}.err" -f $Mode)
        if (Test-Path -LiteralPath $errLog) {
            $e = Get-Content -LiteralPath $errLog -Raw -ErrorAction SilentlyContinue
            if ($e -and $e.Trim()) {
                Write-Host 's1-fault: qemu stderr:'
                Write-Host $e.TrimEnd()
            }
        }
    } finally {
        Stop-Vm
    }
    $sw.Stop()
    return [pscustomobject]@{ Mode = $Mode; Status = $status; Detail = $detail; Sec = [math]::Round($sw.Elapsed.TotalSeconds, 1) }
}

# ---- 跑（默认 4 个场景，一个场景一个 QEMU）--------------------------------
$modes = if ($Scenario -eq 'all') { @('a', 'b', 'c', 'd') } else { @($Scenario) }
$results = @()
$allSw = [Diagnostics.Stopwatch]::StartNew()
foreach ($m in $modes) {
    Write-Host ''
    Write-Host ("s1-fault: === scenario {0} ===" -f $m)
    $results += Run-Scenario $m
}
$allSw.Stop()

$pass = @($results | Where-Object { $_.Status -eq 'PASS' }).Count
$known = @($results | Where-Object { $_.Status -eq 'KNOWN' }).Count
$failed = @($results | Where-Object { $_.Status -eq 'FAIL' }).Count

Write-Host ''
Write-Host '=== s1-fault summary ==='
foreach ($r in $results) {
    $tag = switch ($r.Status) {
        'PASS' { 'PASS ' }
        'KNOWN' { 'KNOWN' }
        default { 'FAIL ' }
    }
    Write-Host ("s1-fault: scenario {0} {1} ({2:N1}s) {3}" -f $r.Mode, $tag, $r.Sec, $r.Detail)
}
Write-Host ("s1-fault: {0} pass, {1} known, {2} fail in {3:N1}s" -f $pass, $known, $failed, $allSw.Elapsed.TotalSeconds)
Write-Host ("s1-fault: artifacts in {0}" -f $WorkDir)
if ($failed -eq 0) {
    Write-Host 's1-fault: PASS'
    exit 0
}
Write-Host 's1-fault: FAIL'
exit 1
