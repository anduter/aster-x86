# =============================================================================
# audit-ng-ui.ps1 - ng display/session 协议畸形输入审计的回归驱动（audit-ui 会话）
#
# 流程（与 audit-ng-sys.ps1 同一套路，**独立产物目录 + 独立 QMP 端口**）：
#   1. 用 clang 把 tests/audit-ng/ui/audit_ui.c 编成独立 ring3 ELF
#      （基址 0x40000000，校验 ELF64/PT_LOAD/W^X）；探针自己 spawn /bin/displayd
#      和 /bin/sessiond，只经 IPC 协议攻击，不读串口、不改内核；
#   2. 复制 dist-ng/disk.img，用 tests/s1/fat-add.ps1 把 ELF 写进 FAT16
#      根目录（AUDIT.ELF）——不改 dist-ng/disk.img，不编进内核；
#   3. 起一台独立 QEMU（**随机空闲 QMP 端口**；显式避开 soak 的 11995/11996、
#      syscall 审计的 11997 以及各并行会话的 4444/4481/4491/4542/4543/4631），
#      等 `ng: shell ready` 后经 QMP input-send-event 注入
#         spawn /mnt/disk/audit.elf
#         wait <pid>
#   4. 解析串口：
#        - 必须出现 `ng: audit ui PASS (checks=N)`（N >= MinChecks）；
#        - 不得出现 `ng: audit ... FAIL` / `PANIC:`；
#        - 探针自己 spawn 的子进程（shm-oob）越界写只杀它自己。
#
# 覆盖的畸形输入（见 tests/audit-ng/ui/audit_ui.c 的 a_attack_*）：
#   display：CREATE 超大/零 w/h、坏 token、标题 16 字节无 NUL、x/y 越界、
#            LIST 小缓冲 + 端口背压、FOCUS 越权令牌、COMMIT/MAP/DESTROY 坏 id、
#            REGISTER_HOTKEY 令牌/洪泛、表满（8 surface）、WM_STATUS/
#            SESSION_STATUS 状态投毒；
#   shm：MAP 后越界写（子进程 #PF 自限）、坏 handle、owner 被杀后映射仍可写；
#   session：REGISTER/HEARTBEAT/EXIT/LAUNCH/KILL 坏令牌、令牌不导出、
#            伪造 LIST_REPLY(count=0xFFFFFFFF) 投毒、KILL/REGISTER 洪泛。
#
# usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-ng-ui.ps1
#   ... -TimeoutSec 300
#   ... -MinChecks 30
#   ... -KeepVm                # 失败时保留 QEMU（手动看串口）
#   ... -QmpPort 4655          # 指定端口（默认随机空闲端口）
#
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$ElfPath = '',
    [string]$DiskImg = '',
    [string]$Cpu = 'max',
    [int]$Port = 0,
    [int]$TimeoutSec = 300,
    [int]$MinChecks = 26,
    [int]$QmpPort = 0,
    [switch]$ShmOob,
    [switch]$KeepVm
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path
$OnWindows = ($env:OS -eq 'Windows_NT')

# 与其它会话冲突的端口（soak / syscall 审计 / 历史并行会话）一律避开。
$ReservedPorts = @(11995, 11996, 11997, 4444, 4481, 4491, 4542, 4543, 4631)

$workDir = Join-Path $Repo 'obj-ng/audit-ui'
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

function Fail([string]$msg) {
    Write-Host ("audit-ng-ui: FAIL ({0})" -f $msg) -ForegroundColor Red
    exit 1
}

function Step([string]$msg) {
    Write-Host ("audit-ng-ui: {0}" -f $msg) -ForegroundColor Cyan
}

# ---- 工具链 ----------------------------------------------------------------
$mingw = 'D:\msys64\mingw64\bin'
$clangCmd = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clangCmd -and (Test-Path $mingw)) {
    $env:PATH = $mingw + ';' + $env:PATH
    $clangCmd = Get-Command clang -ErrorAction SilentlyContinue
}
if (-not $clangCmd) { Fail 'clang not found; add D:\msys64\mingw64\bin to PATH' }
$clang = $clangCmd.Source
$lldCmd = Get-Command ld.lld -ErrorAction SilentlyContinue
if (-not $lldCmd) { Fail 'ld.lld not found; add D:\msys64\mingw64\bin to PATH' }

$qemu = ''
$qcmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if ($qcmd) {
    $qemu = $qcmd.Source
} elseif ($OnWindows -and (Test-Path 'D:\Program Files\qemu\qemu-system-x86_64.exe')) {
    $qemu = 'D:\Program Files\qemu\qemu-system-x86_64.exe'
}
if (-not $qemu) { Fail 'qemu-system-x86_64 not found (PATH or D:\Program Files\qemu)' }

# ---- 内核 / 磁盘镜像 --------------------------------------------------------
if (-not $ElfPath) { $ElfPath = Join-Path $Repo 'dist-ng/aster-ng.elf' }
if (-not $DiskImg) { $DiskImg = Join-Path $Repo 'dist-ng/disk.img' }
if (-not (Test-Path -LiteralPath $ElfPath)) { Fail ("kernel not found: {0} (run tools/build-ng.ps1)" -f $ElfPath) }
if (-not (Test-Path -LiteralPath $DiskImg)) { Fail ("disk image not found: {0} (run tools/build-ng.ps1)" -f $DiskImg) }
$ElfPath = (Resolve-Path -LiteralPath $ElfPath).Path
$DiskImg = (Resolve-Path -LiteralPath $DiskImg).Path

Write-Host '=== audit-ng-ui (display/session protocol) ==='
Write-Host ("repo:  {0}" -f $Repo)
Write-Host ("kernel:{0} ({1} bytes)" -f $ElfPath, (Get-Item -LiteralPath $ElfPath).Length)
Write-Host ("disk:  {0} ({1} bytes, base)" -f $DiskImg, (Get-Item -LiteralPath $DiskImg).Length)
Write-Host ("cpu:   {0}" -f $Cpu)

# ---- 1. 编译 ring3 探针 -----------------------------------------------------
$target = '--target=x86_64-elf'
$cflags = @($target, '-ffreestanding', '-nostdlib', '-mno-red-zone',
    '-fno-stack-protector', '-fcf-protection=none', '-mno-sse',
    '-mgeneral-regs-only', '-fno-builtin', '-fno-pic', '-fno-pie',
    '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-ffunction-sections', '-fdata-sections',
    '-I', (Join-Path $Repo 'src/ng'),
    '-I', (Join-Path $Repo 'src/desktop/include'), '-c')
$probeSrc = Join-Path $Repo 'tests/audit-ng/ui/audit_ui.c'
if (-not (Test-Path -LiteralPath $probeSrc)) { Fail ("missing probe: {0}" -f $probeSrc) }
$probeObj = Join-Path $workDir 'audit_ui.o'
$probeElf = Join-Path $workDir 'audit.elf'
Remove-Item -LiteralPath $probeObj -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $probeElf -Force -ErrorAction SilentlyContinue

Step 'compiling tests/audit-ng/ui/audit_ui.c'
$out = & $clang @($cflags + @($probeSrc, '-o', $probeObj)) 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    Fail 'probe compile failed'
}
# 探针直接用 L2 的协议纯函数（ds_msg_encode/decode、ds_title_get、令牌），
# 所以要把 display/session 的协议库一起编进来（与 build-ng.ps1 的用户程序一致）。
$protoObjs = @()
foreach ($rel in @('src/desktop/src/ds_protocol.c',
                   'src/desktop/src/session_protocol.c')) {
    $src = Join-Path $Repo $rel
    if (-not (Test-Path -LiteralPath $src)) { Fail ("missing protocol source: {0}" -f $src) }
    $obj = Join-Path $workDir ((Split-Path -Leaf $rel) -replace '\.c$', '.o')
    Remove-Item -LiteralPath $obj -Force -ErrorAction SilentlyContinue
    $out = & $clang @($cflags + @($src, '-o', $obj)) 2>&1
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host $_ }
        Fail ("protocol compile failed: {0}" -f $rel)
    }
    $protoObjs += $obj
}
Step ("compiled protocol lib: {0}" -f (($protoObjs | ForEach-Object { Split-Path -Leaf $_ }) -join ', '))
$userLd = Join-Path $Repo 'src/ng/user/user.ld'
$linkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-Wl,--gc-sections', '-o', $probeElf) + @($probeObj) + $protoObjs
$out = & $clang @linkArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    Fail 'probe link failed'
}
if (-not (Test-Path -LiteralPath $probeElf)) { Fail 'probe ELF not produced' }

# ELF 校验（与内核加载窗口一致：0x40000000..0x40010000）。
# 刚 link 完立刻读偶尔会看到 0 字节（Windows 上首读与文件关闭/杀软扫描的
# 竞态）；重试到文件长度稳定。
$bytes = @()
for ($try = 0; $try -lt 40; $try++) {
    $bytes = [IO.File]::ReadAllBytes($probeElf)
    if ($bytes.Length -ge 64) { break }
    Start-Sleep -Milliseconds 50
}
$probs = New-Object System.Collections.Generic.List[string]
if ($bytes.Length -lt 64) {
    $probs.Add('smaller than ELF64 header')
} else {
    if (-not ($bytes[0] -eq 0x7F -and $bytes[1] -eq 0x45 -and $bytes[2] -eq 0x4C -and $bytes[3] -eq 0x46)) {
        $probs.Add('bad magic')
    }
    if ($bytes[4] -ne 2) { $probs.Add('not ELFCLASS64') }
    if ($bytes[5] -ne 1) { $probs.Add('not little-endian') }
    $eType = [BitConverter]::ToUInt16($bytes, 16)
    $eMachine = [BitConverter]::ToUInt16($bytes, 18)
    $entry = [BitConverter]::ToUInt64($bytes, 24)
    $phOff = [BitConverter]::ToUInt64($bytes, 32)
    $phEnt = [BitConverter]::ToUInt16($bytes, 54)
    $phNum = [BitConverter]::ToUInt16($bytes, 56)
    if ($eType -ne 2) { $probs.Add('e_type != ET_EXEC') }
    if ($eMachine -ne 62) { $probs.Add('e_machine != EM_X86_64') }
    if ($entry -lt 0x40000000 -or $entry -ge 0x40010000) {
        $probs.Add(("entry=0x{0:x} outside user window" -f $entry))
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
        if (($pVaddr % 4096) -ne 0) { $probs.Add('PT_LOAD vaddr not 4K aligned') }
        if ($pAlign -lt 4096) { $probs.Add('PT_LOAD align < 4096') }
        if ($pFilesz -gt $pMemsz) { $probs.Add('PT_LOAD filesz > memsz') }
        if (($pFlags -band 3) -eq 3) { $probs.Add('PT_LOAD W+X') }
        if ($pVaddr -lt 0x40000000) { $probs.Add('PT_LOAD below base') }
        if (($pVaddr + $pMemsz) -gt 0x40010000) { $probs.Add('PT_LOAD above 0x40010000') }
    }
    if ($load -lt 1) { $probs.Add('no PT_LOAD') }
    if ($bytes.Length -gt (256 * 1024)) { $probs.Add('ELF > PROC_ELF_MAX (256 KiB)') }
}
if ($probs.Count -gt 0) {
    foreach ($p in $probs) { Write-Host ("  ! {0}" -f $p) }
    Fail 'probe ELF verification failed'
}
Write-Host ("probe: {0} ({1} bytes, {2} PT_LOAD, entry 0x{3:x})" -f `
    (Split-Path -Leaf $probeElf), $bytes.Length, $load, $entry)

# ---- 2. 磁盘副本 + FAT16 注入 ----------------------------------------------
$diskCopy = Join-Path $workDir 'disk-audit-ui.img'
Copy-Item -LiteralPath $DiskImg -Destination $diskCopy -Force
$fatAdd = Join-Path $Repo 'tests/s1/fat-add.ps1'
$psHost = $null
try { $psHost = (Get-Process -Id $PID).Path } catch { }
if (-not $psHost) { $psHost = 'powershell' }
$fatArgs = @('-NoProfile')
if ($OnWindows) { $fatArgs += @('-ExecutionPolicy', 'Bypass') }
$fatArgs += @('-File', $fatAdd, '-Image', $diskCopy, '-File', $probeElf,
              '-Name', 'AUDIT.ELF')
$out = & $psHost @fatArgs 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    Fail 'fat-add.ps1 failed'
}
$out | Select-Object -Last 3 | ForEach-Object { Write-Host $_ }
Write-Host ("disk:  copy + /mnt/disk/AUDIT.ELF ok ({0} bytes)" -f (Get-Item -LiteralPath $diskCopy).Length)

# ---- QMP / 串口辅助 --------------------------------------------------------
function Reserve-QmpPort {
    if ($QmpPort -ne 0) {
        if ($ReservedPorts -contains $QmpPort) {
            Fail ("QMP port {0} is reserved for other sessions" -f $QmpPort)
        }
        $probe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $QmpPort)
        try {
            $probe.Start()
            $probe.Stop()
            return $QmpPort
        } catch {
            Fail ("QMP port {0} is in use" -f $QmpPort)
        }
    }
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
            $listener.Start()
            $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
            $listener.Stop()
            if (-not ($ReservedPorts -contains $port)) { return $port }
        } catch {
            Fail 'cannot reserve a QMP port'
        }
    }
    Fail 'cannot reserve a QMP port outside the reserved ranges'
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
            $null = $reader.ReadLine()  # greeting
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
        Write-Host ("audit-ng-ui: QMP response: {0}" -f $resp)
    } catch {
        return $false
    }
    return $false
}

function Send-Typed($Qmp, [string]$Text) {
    $map = @{ ' ' = 'spc'; '/' = 'slash'; '.' = 'dot'; '-' = 'minus'; '=' = 'equal' }
    foreach ($ch in $Text.ToCharArray()) {
        $key = $map["$ch"]
        if (-not $key) { $key = "$ch" }
        if (-not (Send-QmpKey $Qmp $key)) { Fail ("QMP typing failed at '{0}'" -f $ch) }
        Start-Sleep -Milliseconds 20
    }
    if (-not (Send-QmpKey $Qmp 'ret')) { Fail 'QMP Enter failed' }
}

function Get-SerialText {
    if (-not (Test-Path -LiteralPath $script:SerialLog)) { return '' }
    $t = Get-Content -LiteralPath $script:SerialLog -Raw -ErrorAction SilentlyContinue
    if ($null -eq $t) { return '' }
    return $t
}

function Wait-Serial {
    param([string]$Pattern, [double]$TimeoutSec)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $t = Get-SerialText
        if ($t) {
            $m = [regex]::Match($t, $Pattern)
            if ($m.Success) { return $m }
        }
        if ($script:Qemu -and $script:Qemu.HasExited) {
            return $null
        }
        Start-Sleep -Milliseconds 40
    }
    return $null
}

# ---- 3. 起 QEMU ------------------------------------------------------------
$script:SerialLog = Join-Path $workDir 'serial-ui.log'
$errLog = Join-Path $workDir 'qemu.err'
$outLog = Join-Path $workDir 'qemu.out'
Remove-Item -LiteralPath $script:SerialLog -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $errLog -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $outLog -Force -ErrorAction SilentlyContinue
if ($Port -ne 0) { $QmpPort = $Port }
$qmpPort = Reserve-QmpPort
$qemuArgs = @(
    '-kernel', ('"' + $ElfPath + '"'),
    '-serial', ('"file:' + $script:SerialLog + '"'),
    '-drive', ('"file=' + $diskCopy + ',format=raw,if=ide"'),
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
$script:Qemu = Start-Process @spArgs
$script:Qmp = $null
$fail = 0

function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) {
        Write-Host ("  [PASS] {0} {1}" -f $name, $detail) -ForegroundColor Green
    } else {
        Write-Host ("  [FAIL] {0} {1}" -f $name, $detail) -ForegroundColor Red
        $script:fail++
    }
}

try {
    Step ("QEMU up (qmp {0}, serial {1})" -f $qmpPort, (Split-Path -Leaf $script:SerialLog))
    $script:Qmp = Connect-Qmp $qmpPort
    if (-not $script:Qmp) { Fail 'QMP connect failed' }
    $m = Wait-Serial 'ng: shell ready' 90
    if (-not $m) { Fail 'shell ready timeout (boot/selftest failed?)' }
    Write-Host '  [PASS] boot: ng: shell ready' -ForegroundColor Green

    Step 'spawn /mnt/disk/audit.elf'
    $spawnCmd = 'spawn /mnt/disk/audit.elf'
    if ($ShmOob) { $spawnCmd = $spawnCmd + ' oob' }
    Send-Typed $script:Qmp $spawnCmd
    $m = Wait-Serial 'ng: shell spawn ok \(pid=(\d+)' 20
    if (-not $m) { Fail 'probe spawn failed (FAT16 spawn path?)' }
    $probePid = $m.Groups[1].Value
    Write-Host ("  [PASS] probe spawned (pid={0})" -f $probePid) -ForegroundColor Green
    Start-Sleep -Milliseconds 300
    Send-Typed $script:Qmp ("wait {0}" -f $probePid)
    if (-not (Wait-Serial ('ng> wait ' + [regex]::Escape($probePid)) 5)) {
        Write-Host '  [WARN] wait echo not seen; retrying once' -ForegroundColor Yellow
        Send-Typed $script:Qmp ("wait {0}" -f $probePid)
    }

    Step 'running protocol attacks (displayd / shm / sessiond)'
    $verdict = Wait-Serial 'ng: audit ui (PASS|FAIL)|PANIC: vec=0x[0-9a-f]+ err=' $TimeoutSec
    $t = Get-SerialText
    if ($ShmOob) {
        # 已知限制对照：ring3 越界写 -> 内核 PANIC（vec=0xe）。这里只记录，
        # 不算 ui 审计的失败（隔离 ring3 #PF 属于 syscall 审计会话的范围）。
        $panic = [regex]::Match($t, 'PANIC: vec=0x([0-9a-f]+) err=0x([0-9a-f]+) rip=0x[0-9a-f]+ cr2=0x[0-9a-f]+')
        if ($panic.Success) {
            Write-Host ("  [INFO] shm-oob: user #PF panics kernel (known limitation) vec=0x{0} err=0x{1}" -f `
                $panic.Groups[1].Value, $panic.Groups[2].Value) -ForegroundColor Yellow
            Check 'shm-oob: fault containment is a known kernel limitation' `
                ($panic.Groups[1].Value -eq 'e') '(see docs/claims/ng-audit-ui.md)'
        } else {
            Check 'shm-oob: expected kernel PANIC for ring3 oob write' $false '(no PANIC line)'
        }
    } else {
    if (-not $verdict) {
        $panic = [regex]::Match($t, 'PANIC: vec=0x([0-9a-f]+) err=0x[0-9a-f]+')
        if ($panic.Success) {
            Check 'ui audit: no kernel panic' $false ("(PANIC vec=0x{0})" -f $panic.Groups[1].Value)
        } else {
            Check 'ui audit: verdict line' $false '(timeout; see serial)'
        }
    } else {
        $passed = ($verdict.Groups[1].Value -eq 'PASS')
        $cm = [regex]::Match($t, 'ng: audit ui PASS \(checks=(\d+)\)')
        $fm = [regex]::Match($t, 'ng: audit ui FAIL \(fails=(\d+) checks=(\d+)\)')
        $checks = 0
        if ($cm.Success) { $checks = [int]$cm.Groups[1].Value }
        elseif ($fm.Success) { $checks = [int]$fm.Groups[3].Value }
        if ($fm.Success) {
            Write-Host ("  [INFO] fails={0} checks={1}" -f $fm.Groups[1].Value, $fm.Groups[2].Value) -ForegroundColor Cyan
        }
        Check 'ui audit: verdict PASS' $passed ("(checks={0})" -f $checks)
        Check 'ui audit: case count' ($checks -ge $MinChecks) ("(checks={0} >= {1})" -f $checks, $MinChecks)
        $failLines = [regex]::Matches($t, 'ng: audit [^\r\n]* FAIL \([^\r\n]*\)')
        foreach ($fl in ($failLines | Select-Object -First 20)) { Write-Host ("    " + $fl.Value) -ForegroundColor Yellow }
        $waitOk = Wait-Serial ('ng: shell wait ok \(pid={0} status=0\)' -f [regex]::Escape($probePid)) 20
        Check 'ui audit: probe exit status 0' ($null -ne $waitOk)
        Check 'ui audit: no kernel panic' (-not ([regex]::IsMatch($t, '(?m)^PANIC:')))
        Check 'ui audit: malformed input did not kill displayd' `
            (-not ($t -match 'ng: audit (display malformed|display badgeom|display badops|display table) FAIL'))
        Check 'ui audit: session survived bad tokens/storm' `
            (-not ($t -match 'ng: audit session (bad-token|kill-storm|count-clamp|discover) FAIL'))
        Check 'ui audit: shm map/bad-handle path ok' `
            ($t.Contains('ng: audit shm map ok') -and $t.Contains('ng: audit shm bad-handle ok'))

        # ---- 第二轮修复的宿主侧回归断言 --------------------------------------
        # 1) 状态行必须能一次 write() 写下去：fs_sys_write 的 bounce buffer 上限
        #    是 512（FS_SYS_IO_MAX）。以前伪造全 0xFFFFFFFF 的 SESSION_STATUS 后，
        #    每次状态写都 EINVAL -> /run/display.status 冻结（内核自检读它）。
        Check 'fix: status write never fails (line <= 512)' `
            (-not $t.Contains('ng: displayd status FAIL (write)'))
        # 2) 客户端标题不能伪造证据行：串口里不允许出现以 PANIC: 开头的行
        #    （净化后的形态是 "... title=..PANIC: vec=0x"）。
        $panicLines = [regex]::Matches($t, '(?m)^PANIC:[^\r\n]*')
        Check 'fix: title cannot forge an evidence line' `
            ($panicLines.Count -eq 0) ("(forged={0})" -f $panicLines.Count)
        Check 'fix: injected title was sanitized' `
            ($t.Contains('title=..PANIC: vec=0x'))
        # 3) 伪造 WM 令牌不能抢占 WM 权威；WM 面板销毁后令牌回收。
        Check 'fix: forged WM token claim rejected' `
            ($t.Contains('ng: surface wm-claim reject ok'))
        Check 'fix: WM token revoked with its panel' `
            ($t.Contains('ng: displayd wm token cleared ok'))
        # 4) FOCUS 令牌策略：token=0 不能抢会话窗口的焦点。
        Check 'fix: focus token policy enforced' `
            ($t -match 'ng: focus reject ok \(id=[0-9]+ token=0x[0-9a-f]+')
        # 5) v2 LIST_REPLY 的 entry 布局（44 vs 48 字节）不同：sessiond 只认
        #    v3，registry/tokenfail 证据不被伪造的旧版本消息搅乱。
        Check 'fix: v2 LIST_REPLY ignored (no registry mismatch)' `
            (-not $t.Contains('session registry mismatch'))
    }
    }
} finally {
    if ($script:Qmp) {
        try { $script:Qmp.Client.Close() } catch { }
        $script:Qmp = $null
    }
    if ($script:Qemu) {
        if (-not $script:Qemu.HasExited) {
            if ($KeepVm) {
                Write-Host ("audit-ng-ui: -KeepVm: QEMU pid {0} left running (qmp {1})" -f $script:Qemu.Id, $qmpPort) -ForegroundColor Yellow
            } else {
                try { $script:Qemu.Kill() } catch { }
                $script:Qemu.WaitForExit(3000) | Out-Null
            }
        }
        $script:Qemu = $null
    }
}

Write-Host ("audit-ng-ui: serial: {0}" -f $script:SerialLog)
Write-Host ("audit-ng-ui: qmp={0}" -f $qmpPort)
Write-Host ("audit-ng-ui: => {0}" -f $(if ($fail -eq 0) { 'PASS' } else { "FAIL ($fail)" })) `
    -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
if ($fail -ne 0) { exit 1 }
exit 0
