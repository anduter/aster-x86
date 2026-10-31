# =============================================================================
# audit-ng-sys.ps1 - ng 用户态 syscall 边界审计的回归驱动（audit-sys 会话）
#
# 流程：
#   1. 用 clang 把 tests/audit-ng/sys/audit_sys.c 编成独立 ring3 ELF
#      （基址 0x40000000，检查 ELF64/PT_LOAD/W^X）；
#   2. 复制 dist-ng/disk.img，用 tests/s1/fat-add.ps1 把 ELF 写进 FAT16
#      根目录（AUDIT.ELF）——不改 dist-ng/disk.img，不编进内核；
#   3. 起一台独立 QEMU（随机空闲 QMP 端口；不用 soak 的 11997），等
#      `ng: shell ready` 后经 QMP input-send-event 注入
#         spawn /mnt/disk/audit.elf [crash]
#         wait <pid>
#   4. 解析串口：boundary 模式要求 AUDIT: summary fail=0 且无 PANIC；
#      crash 模式是"已知限制"对照（ring3 空指针 -> 内核 panic），
#      期望看到 PANIC: vec=0xe。
#
# usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-ng-sys.ps1
#   ... -Scenario crash
#   ... -Scenario all
#   ... -Cpu qemu64            # SMEP/SMAP off 的降级 CPU
#   ... -KeepVm                # 失败时保留 QEMU（手动看串口）
#
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [ValidateSet('boundary', 'crash', 'all')]
    [string]$Scenario = 'boundary',
    [string]$Repo = '',
    [string]$ElfPath = '',
    [string]$DiskImg = '',
    [string]$Cpu = 'max',
    [int]$TimeoutSec = 120,
    [switch]$KeepVm
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path
$OnWindows = ($env:OS -eq 'Windows_NT')

$workDir = Join-Path $Repo 'obj-ng/audit-sys'
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

function Fail([string]$msg) {
    Write-Host ("audit-ng-sys: FAIL ({0})" -f $msg) -ForegroundColor Red
    exit 1
}

function Step([string]$msg) {
    Write-Host ("audit-ng-sys: {0}" -f $msg) -ForegroundColor Cyan
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

Write-Host '=== audit-ng-sys (user->kernel boundary) ==='
Write-Host ("repo:  {0}" -f $Repo)
Write-Host ("kernel:{0} ({1} bytes)" -f $ElfPath, (Get-Item -LiteralPath $ElfPath).Length)
Write-Host ("disk:  {0} ({1} bytes, base)" -f $DiskImg, (Get-Item -LiteralPath $DiskImg).Length)
Write-Host ("cpu:   {0}   scenario: {1}" -f $Cpu, $Scenario)

# ---- 1. 编译 ring3 探针 -----------------------------------------------------
$target = '--target=x86_64-elf'
$cflags = @($target, '-ffreestanding', '-nostdlib', '-mno-red-zone',
    '-fno-stack-protector', '-fcf-protection=none', '-mno-sse',
    '-mgeneral-regs-only', '-fno-builtin', '-fno-pic', '-fno-pie',
    '-std=c11', '-Os', '-Wall', '-Wextra', '-Werror',
    '-ffunction-sections', '-fdata-sections',
    '-I', (Join-Path $Repo 'src/ng'), '-c')
$probeSrc = Join-Path $Repo 'tests/audit-ng/sys/audit_sys.c'
if (-not (Test-Path -LiteralPath $probeSrc)) { Fail ("missing probe: {0}" -f $probeSrc) }
$probeObj = Join-Path $workDir 'audit_sys.o'
$probeElf = Join-Path $workDir 'audit.elf'
Remove-Item -LiteralPath $probeObj -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $probeElf -Force -ErrorAction SilentlyContinue

Step 'compiling tests/audit-ng/sys/audit_sys.c'
$out = & $clang @($cflags + @($probeSrc, '-o', $probeObj)) 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | ForEach-Object { Write-Host $_ }
    Fail 'probe compile failed'
}
$userLd = Join-Path $Repo 'src/ng/user/user.ld'
$linkArgs = @($target, '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-Wl,--gc-sections', '-o', $probeElf, $probeObj)
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
$diskCopy = Join-Path $workDir 'disk-audit.img'
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
    try {
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $listener.Start()
        $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
        $listener.Stop()
        return $port
    } catch {
        Fail 'cannot reserve a QMP port'
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
        Write-Host ("audit-ng-sys: QMP response: {0}" -f $resp)
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
$script:SerialLog = Join-Path $workDir ("serial-{0}.log" -f $Scenario)
$errLog = Join-Path $workDir 'qemu.err'
$outLog = Join-Path $workDir 'qemu.out'
Remove-Item -LiteralPath $script:SerialLog -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $errLog -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $outLog -Force -ErrorAction SilentlyContinue
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
    $m = Wait-Serial 'ng: shell ready' 60
    if (-not $m) { Fail 'shell ready timeout (boot/selftest failed?)' }
    Write-Host '  [PASS] boot: ng: shell ready' -ForegroundColor Green

    if ($Scenario -eq 'crash' -or $Scenario -eq 'all') {
        Step 'scenario crash: ring3 NULL deref (known limitation -> PANIC)'
        Send-Typed $script:Qmp 'spawn /mnt/disk/audit.elf crash'
        $m = Wait-Serial 'ng: shell spawn ok \(pid=(\d+)' 15
        Check 'crash: probe spawned' ($null -ne $m)
        if ($m) {
            # shell 的 readkey 循环会让出 CPU 的前提是它阻塞；发 wait 让
            # crash 子进程真正被调度（否则 shell 自旋、子进程永远不跑）。
            Start-Sleep -Milliseconds 300
            Send-Typed $script:Qmp ("wait {0}" -f $m.Groups[1].Value)
            if (-not (Wait-Serial ('ng> wait ' + [regex]::Escape($m.Groups[1].Value)) 5)) {
                Write-Host '  [WARN] crash: wait command echo not seen; retrying once' -ForegroundColor Yellow
                Send-Typed $script:Qmp ("wait {0}" -f $m.Groups[1].Value)
            }
        }
        $panic = Wait-Serial 'PANIC: vec=0x([0-9a-f]+) err=0x[0-9a-f]+ rip=0x[0-9a-f]+ cr2=0x[0-9a-f]+' 30
        $t = Get-SerialText
        $survived = $t.Contains('AUDIT: crash mode survived')
        if ($panic) {
            $vec = $panic.Groups[1].Value
            Check 'crash: user #PF panics kernel (known limitation)' ($vec -eq 'e') ("(vec=0x{0})" -f $vec)
        } elseif ($survived) {
            Write-Host '  [INFO] crash: ring3 fault was contained (limitation no longer!)' -ForegroundColor Yellow
            Check 'crash: ring3 fault contained' $true
        } else {
            Check 'crash: expected PANIC or containment' $false
        }
        if ($Scenario -eq 'all') {
            # crash 会停住内核，all 模式下 crash 场景结束即收尾，不再跑 boundary。
            Write-Host '  [INFO] -Scenario all: crash 场景会停机，boundary 请单独跑（默认）' -ForegroundColor Yellow
        }
    } else {
        Step 'scenario boundary: spawn audit probe + wait'
        Send-Typed $script:Qmp 'spawn /mnt/disk/audit.elf'
        $m = Wait-Serial 'ng: shell spawn ok \(pid=(\d+)' 15
        if (-not $m) { Fail 'probe spawn failed (FAT16 spawn path?)' }
        $probePid = $m.Groups[1].Value
        Write-Host ("  [PASS] probe spawned (pid={0})" -f $probePid) -ForegroundColor Green
        Send-Typed $script:Qmp ("wait {0}" -f $probePid)

        $summary = Wait-Serial 'AUDIT: summary pass=(\d+) fail=(\d+) total=(\d+)' $TimeoutSec
        $t = Get-SerialText
        if (-not $summary) {
            $panic = [regex]::Match($t, 'PANIC: vec=0x([0-9a-f]+)')
            if ($panic.Success) {
                Check 'boundary: no kernel panic' $false ("(PANIC vec=0x{0})" -f $panic.Groups[1].Value)
            } else {
                Check 'boundary: summary line' $false '(timeout; see serial)'
            }
        } else {
            $pass = [int]$summary.Groups[1].Value
            $fails = [int]$summary.Groups[2].Value
            $total = [int]$summary.Groups[3].Value
            Write-Host ("  [INFO] summary pass={0} fail={1} total={2}" -f $pass, $fails, $total) -ForegroundColor Cyan
            Check 'boundary: all cases PASS' ($fails -eq 0) ("(fail={0})" -f $fails)
            Check 'boundary: case count >= 540' ($total -ge 540 -and $pass -ge 540) ("(total={0})" -f $total)
            $failLines = [regex]::Matches($t, 'AUDIT: FAIL [^\r\n]+')
            foreach ($fl in ($failLines | Select-Object -First 12)) { Write-Host ("    " + $fl.Value) }
            $waitOk = Wait-Serial ('ng: shell wait ok \(pid={0} status=0\)' -f [regex]::Escape($probePid)) 20
            Check 'boundary: probe exit status 0' ($null -ne $waitOk)
            $t = Get-SerialText
            Check 'boundary: no kernel panic' (-not $t.Contains('PANIC:'))
            Check 'boundary: no child from bad-&pid spawn (id=7777)' (-not $t.Contains('id=7777'))
            Check 'boundary: storm block children reached recv' `
                (([regex]::Matches($t, 'AUDIT: child up id=10 pid=\d+ block')).Count -ge 2)
            Check 'boundary: child mapped parent shm and wrote it' `
                ($t.Contains('AUDIT: child shm id=30 va=0x5a5a5a5a'))
            Check 'boundary: child mapped LFB before kill' `
                ($t.Contains('AUDIT: child fb id=31 mapped'))
            # Round 2 additions: cross-process IPC / self-kill / FAT16 / SVC /
            # resource limits / exit-time close race. Keep new comments ASCII:
            # Windows PowerShell 5.1 reads .ps1 as ANSI (GBK here), and a
            # multibyte Chinese comment can swallow the following newline.
            Check 'boundary: cross-process recv wake' `
                ($t.Contains('AUDIT: info xproc recv wake ok'))
            Check 'boundary: blocked senders handoff' `
                ($t.Contains('AUDIT: info xproc send handoff ok'))
            Check 'boundary: kill blocked sender' `
                ($t.Contains('AUDIT: info kill blocked sender ok'))
            Check 'boundary: close wakes blocked sender' `
                ($t.Contains('AUDIT: info close wakes blocked sender ok'))
            Check 'boundary: cross-process recv/close EPERM' `
                ($t.Contains('AUDIT: info cross-process recv/close EPERM ok'))
            Check 'boundary: FAT16 read/write/listdir' `
                ($t.Contains('AUDIT: info fat16 read/write/listdir ok'))
            Check 'boundary: SVC 30..35 boundary' `
                ($t.Contains('AUDIT: info svc 30..35 boundary ok'))
            Check 'boundary: self-kill path' `
                ($t.Contains('AUDIT: info selfkill path ok'))
            Check 'boundary: proc table / thread pool limits' `
                ($t.Contains('AUDIT: info proc table/thread pool limits ok'))
            Check 'boundary: kill blocked-in-wait' `
                ($t.Contains('AUDIT: info kill blocked-in-wait ok'))
            Check 'boundary: normal-exit shm/fb release' `
                ($t.Contains('AUDIT: info normal-exit shm/fb release ok'))
            Check 'boundary: self-kill with shm/fb' `
                ($t.Contains('AUDIT: info self-kill with shm/fb ok'))
            Check 'boundary: ipc deep queue recycle' `
                ($t.Contains('AUDIT: info ipc deep queue recycle ok'))
            Check 'boundary: ipc waiter pool saturation' `
                ($t.Contains('AUDIT: info ipc waiter pool ok'))
            Check 'boundary: vfs path fuzz' `
                ($t.Contains('AUDIT: info vfs path fuzz ok'))
            Check 'boundary: ipc msg fuzz' `
                ($t.Contains('AUDIT: info ipc msg fuzz ok'))
            Check 'boundary: user-written ELF spawn' `
                ($t.Contains('AUDIT: info user-written ELF spawn ok'))
            $headroom = [regex]::Match($t, 'AUDIT: info ramfs headroom=(\d+)')
            Check 'boundary: ramfs node headroom >= 8' `
                ($headroom.Success -and [int]$headroom.Groups[1].Value -ge 8) `
                ("(headroom={0})" -f $(if ($headroom.Success) { $headroom.Groups[1].Value } else { '?' }))
            Check 'boundary: exit cleanup armed' `
                ($t.Contains('AUDIT: info exit cleanup armed'))
            # Probe exit closes its port and must wake the blocked sender; the
            # child then prints ECANCELED (rc=-125) and exits 4, no panic.
            $exitWoke = Wait-Serial 'AUDIT: child sender fail id=200 rc=-125' 10
            Check 'boundary: exit cleanup woke blocked sender' ($null -ne $exitWoke)
            $t = Get-SerialText
            Check 'boundary: no kernel panic after probe exit' (-not $t.Contains('PANIC:'))
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
                Write-Host ("audit-ng-sys: -KeepVm: QEMU pid {0} left running (qmp {1})" -f $script:Qemu.Id, $qmpPort) -ForegroundColor Yellow
            } else {
                try { $script:Qemu.Kill() } catch { }
                $script:Qemu.WaitForExit(3000) | Out-Null
            }
        }
        $script:Qemu = $null
    }
}

Write-Host ("audit-ng-sys: serial: {0}" -f $script:SerialLog)
Write-Host ("audit-ng-sys: scenario={0} => {1}" -f $Scenario, $(if ($fail -eq 0) { 'PASS' } else { "FAIL ($fail)" })) `
    -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
if ($fail -ne 0) { exit 1 }
exit 0
