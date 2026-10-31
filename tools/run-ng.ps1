# =============================================================================
# run-ng.ps1 - ng WP2-6: 在 QEMU 里启动 dist-ng/aster-ng.elf，等一组期望行
#
# 启动命令（WP1-1/WP1-4 出口）：
#   qemu-system-x86_64 -kernel dist-ng/aster-ng.elf -serial file:<log> \
#                      -display none -no-reboot -m 256 -cpu max -vga std
#
# WP2-6 追加（用户态 shell）：
#   - 内核在 WP1-7 ring3 hello 之后加载并运行 src/apps/shell/shell.c；
#   - 看到用户态 `ng: shell ready` 后，经 QMP `input-send-event` 逐字符注入
#     `help` -> `ls /` -> `cat /etc/passwd` -> `echo hi > /tmp/s.txt` ->
#     `cat /tmp/s.txt` -> `dmesg -n 3` -> `pkg list` -> `exit`，每个字符 25ms，
#     每步等到对应 `ng: shell <cmd> ok` 再发下一条（'>' 用 shift+dot）；
#   - 期望 shell 证据行 + 内核会话证据（/tmp/s.txt 内容、pkg list 计数）；
#   - `-Smoke`（配合 tools/build-ng.ps1 -Smoke）：内核跳过 shell，只期望
#     `ng: shell skipped (smoke mode)` 与 ring3 hello 路径；
#   - `-NoQmp`：不注入，shell 停在 readkey；不要求 WP2-6 的交互行。
#
# WP2-4 追加：
#   - `-drive file=dist-ng/disk.img,format=raw,if=ide`：8MB FAT16 磁盘镜像挂到
#     QEMU PIIX3 IDE 主盘（legacy 0x1F0/0x3F6）；内核 ATA PIO 读它并挂 /mnt/disk。
#     镜像由 tools/build-ng.ps1 -> tools/mkfat-ng.ps1 -SizeMB 8 生成；ELF 或镜像
#     缺失时本脚本先跑一次 build-ng.ps1。
#
# WP2-3 追加：
#   - `-vga std`（PCI 1234:1111 + Bochs VBE 1280x800x32）；
#   - QMP（TCP 随机端口）注入 `sendkey a`：脚本看到用户态
#     `ng: ring3 input ready` 后发送，验证 syscall 30 真拿到硬件键；
#     QMP 不可用时用户程序走 timeout 分支，仍然 PASS（-NoQmp 强制跳过）；
#   - `-Fallback`：QEMU `-vga none`，期望 VGA 文本 0xB8000 降级行，用于验证
#     “显示不可用但系统可用”的路径。
#
# 用 file: 串口而不是 stdio：脚本要在 QEMU 不退出（内核 hlt 循环）的情况下
# 轮询串口内容，命中所有期望行后主动结束 QEMU。
#
# -cpu 默认 max：默认的 qemu64 不报 SMEP/SMAP，WP1-2 的 CR4 置位路径就测不到；
# 想看"CPU 不支持时打印 off"的检测路径，用 -Cpu qemu64（见 claims）。
#
# 期望行（全部命中才 PASS）：
#   ng: hello 64-bit
#   ng: paging 4-level ok / wx layout ok / nx on / WP1-2 selftest PASS
#   ng: idt 256 entries, 32 exception stubs
#   ng: #pf ro write handled ok
#   ng: #ud handled ok
#   ng: #gp handled ok
#   ng: syscall ring0 ok
#   ng: ring3 syscall ok
#   ng: WP1-3 selftest PASS
#   ng: pmm alloc/free ok / pmm double-free rejected
#   ng: heap kmalloc ok / heap coalesce ok / pgtbl from pmm ok
#   ng: WP1-4 selftest PASS
# WP1-5 追加（全部 ASCII 字面量，内核自检自己断言顺序/计数）：
#   ng: sched create ok (3) / ng: sched order ok (ABCABCABC)
#   ng: sched switch ok (rounds>=6) / ng: sched exit reap ok
#   ng: sched sleep/wake ok / ng: WP1-5 selftest PASS
# WP1-6 追加（IPC/句柄/端口）：
#   ng: ipc ping/pong ok (n=20) / ng: ipc ticket ok
#   ng: ipc queue full ok / ng: ipc timeout ok
#   ng: ipc bad handle rejected / ng: ipc close wakes waiter ok
#   ng: ipc user ptr ok / ng: ipc handle table ok (slots=64 ports=16)
#   ng: WP1-6 selftest PASS
# WP1-7 追加（ELF64 加载器 + ring3 hello）：
#   ng: elf64 header ok / ng: elf64 map ok (segs=3)
#   ng: ring3 exec ok / <用户态> ng: ring3 hello / ng: ring3 exit ok
#   ng: WP1-7 selftest PASS
# WP2-1 追加（VFS/ramfs + ring3 fs demo）：
#   ng: vfs mount ok / ng: ramfs write/read ok / ng: fd bad rejected
#   ng: vfs path edge ok / ng: fs syscall user ptr EFAULT ok
#   <用户态> ng: fs open ok / write ok / read ok / stat ok / close ok /
#            listdir ok / ng: fs user demo ok
#   ng: WP2-1 selftest PASS
# WP2-2 追加（FAT16 / RAM 盘镜像 + ring3 FAT demo）：
#   ng: fat16 mount ok (clusters=...) / ng: fat16 read ok /
#   ng: fat16 write ok / ng: fat16 chain edge ok (n=...) /
#   ng: fat16 bad bpb rejected (n=...) / ng: WP2-2 selftest PASS
#   <用户态> ng: fs fat read ok / write ok / stat ok / unlink ok
# WP2-4 追加（ATA PIO + 真实磁盘 FAT16 + ring3 disk demo）：
#   ng: ata identify ok (model=..., sectors=...) / ng: ata read ok /
#   ng: ata write ok / ng: ata timeout rejected (n=...) /
#   ng: disk fat16 mount ok (clusters=...) / ng: disk file rw ok /
#   ng: WP2-4 selftest PASS
#   <用户态> ng: fs disk read ok / write ok / stat ok
# WP2-5 追加（auth/log/pkg + ring3 svc demo）：
#   ng: auth users ok (2) / ng: auth login root ok (uid=0) /
#   ng: auth login alice ok (uid=1000) / ng: auth bad password rejected /
#   ng: log ring ok (bytes=...) / ng: dmesg syscall ok /
#   ng: pkg list ok (n=1) / ng: pkg verify ok / ng: WP2-5 selftest PASS
#   <用户态> ng: ring3 auth ok / ng: ring3 pkg ok / ng: ring3 svc ok
# WP2-6 追加（用户态 shell，QMP 注入；-NoQmp 时跳过本节）：
#   ng: shell elf ok / ng: shell load ok / ng: shell launch ok
#   <用户态> ng: shell ready
#   ng: shell ls ok / cat ok / write ok / dmesg ok / pkg ok / exit ok
#   ng: shell exit (code=0 syscalls=... writes=... fs=...)
#   ng: shell session ok (exit=0 rsp=ok) /
#   ng: shell session file ok (s.txt=3) / ng: shell session pkg ok (lists=+1)
#   ng: WP2-6 selftest PASS
#   -Smoke 模式：ng: shell skipped (smoke mode) 替代以上全部。
# 以及正则：smep/smap、tss/ist、syscall MSR、memmap usable 页数、线程交错、
# ipc 统计行、ELF 段规划/per-thread 栈/uaccess、VFS 挂载/ramfs 计数/fd 拒绝数、
# FAT16 簇数/链边界/坏 BPB 计数、ATA 型号/扇区数、磁盘 FAT16 簇数。
# WP2-3 追加（std 模式）：
#   ng: pci vga found / ng: vbe mode 1280x800x32 ok /
#   ng: fb draw ok (readback) / ng: kbd init ok / ng: input event ok /
#   ng: WP2-3 selftest PASS；用户态 ng: ring3 input ready ->
#   ng: ring3 input ok (key=a|key=other|timeout)
# Fallback 模式（-Fallback，QEMU -vga none）：
#   ng: pci vga missing / ng: vga text fallback ok (0xB8000) 替代 VBE/fb 行。
#
# -Panic：负向模式。配合 tools/build-ng.ps1 -PanicTest 构建的内核，期望
# `PANIC: vec=... err=... rip=... cr2=...`，命中即 PASS（内核随后 hlt）。
#
# 若 ELF 不存在会先调用 tools/build-ng.ps1（干净检出也能一条命令 PASS）。
#
# usage: powershell -File tools/run-ng.ps1 [-Repo <root>] [-ElfPath <elf>]
#                                           [-DiskImg <disk.img>]
#                                           [-TimeoutSec 20] [-SerialLog <path>]
#                                           [-Cpu max] [-Panic] [-Fallback]
#                                           [-NoQmp] [-Smoke] [-Vga std]
# 退出码：0 = PASS；1 = FAIL（缺 QEMU / 启动失败 / 超时 / 期望行缺失）。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$ElfPath = '',
    [string]$DiskImg = '',
    [int]$TimeoutSec = 20,
    [string]$SerialLog = '',
    [string]$Cpu = 'max',
    [switch]$Panic,
    [switch]$Fallback,
    [switch]$NoQmp,
    [switch]$Smoke,
    [string]$Vga = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path

# Windows / Linux 都能跑：-ExecutionPolicy、-WindowStyle 与非 Windows 的 QEMU
# 默认安装路径只在 Windows 上使用（CI 的 ubuntu runner 走 -Quick 时不会到 QEMU）。
$OnWindows = ($env:OS -eq 'Windows_NT')

# 显示设备：-Fallback 用 -vga none（验证 VGA 文本降级），否则用 stdvga。
$useVga = $Vga
if (-not $useVga) { $useVga = $(if ($Fallback) { 'none' } else { 'std' }) }

# ---- 期望的串口行 ----------------------------------------------------------
if ($Panic) {
    $required = @(
        'ng: panic selftest: triggering unrecoverable #UD'
    )
    $patterns = @(
        'PANIC: vec=0x6 err=0x0 rip=0x[0-9a-f]+ cr2=0x[0-9a-f]+'
    )
    $failPatterns = @()
} else {
    $required = @(
        'ng: hello 64-bit',
        'ng: paging 4-level ok',
        'ng: wx layout ok (text rx, rodata ro, data rw-nx)',
        'ng: nx on',
        'ng: WP1-2 selftest PASS',
        'ng: idt 256 entries, 32 exception stubs',
        'ng: #pf ro write handled ok',
        'ng: #ud handled ok',
        'ng: #gp handled ok',
        'ng: syscall ring0 ok',
        'ng: ring3 syscall ok',
        'ng: WP1-3 selftest PASS',
        'ng: pmm alloc/free ok',
        'ng: pmm double-free rejected',
        'ng: heap kmalloc ok',
        'ng: heap coalesce ok',
        'ng: pgtbl from pmm ok',
        'ng: WP1-4 selftest PASS',
        'ng: sched create ok (3)',
        'ng: sched order ok (ABCABCABC)',
        'ng: sched switch ok (rounds>=6)',
        'ng: sched exit reap ok',
        'ng: sched sleep/wake ok',
        'ng: WP1-5 selftest PASS',
        'ng: ipc ping/pong ok (n=20)',
        'ng: ipc ticket ok',
        'ng: ipc queue full ok',
        'ng: ipc timeout ok',
        'ng: ipc bad handle rejected',
        'ng: ipc close wakes waiter ok',
        'ng: ipc user ptr ok',
        'ng: ipc handle table ok (slots=64 ports=16)',
        'ng: WP1-6 selftest PASS',
        'ng: elf64 header ok',
        'ng: elf64 map ok (segs=3)',
        'ng: ring3 exec ok',
        'ng: ring3 hello',
        'ng: fs open ok',
        'ng: fs write ok',
        'ng: fs read ok',
        'ng: fs stat ok',
        'ng: fs close ok',
        'ng: fs listdir ok',
        'ng: fs user demo ok',
        'ng: fs fat read ok',
        'ng: fs fat write ok',
        'ng: fs fat stat ok',
        'ng: fs fat unlink ok',
        'ng: ring3 exit ok',
        'ng: WP1-7 selftest PASS',
        'ng: vfs mount ok',
        'ng: ramfs write/read ok',
        'ng: fd bad rejected',
        'ng: vfs path edge ok',
        'ng: fs syscall user ptr EFAULT ok',
        'ng: WP2-1 selftest PASS',
        'ng: fat16 mount ok',
        'ng: fat16 read ok',
        'ng: fat16 write ok',
        'ng: fat16 chain edge ok',
        'ng: fat16 bad bpb rejected',
        'ng: WP2-2 selftest PASS',
        'ng: ata identify ok',
        'ng: ata read ok',
        'ng: ata write ok',
        'ng: ata timeout rejected',
        'ng: disk fat16 mount ok',
        'ng: disk file rw ok',
        'ng: fs disk read ok',
        'ng: fs disk write ok',
        'ng: fs disk stat ok',
        'ng: WP2-4 selftest PASS',
        'ng: auth users ok (2)',
        'ng: auth login root ok (uid=0)',
        'ng: auth login alice ok (uid=1000)',
        'ng: auth bad password rejected',
        'ng: log ring ok',
        'ng: dmesg syscall ok',
        'ng: pkg list ok',
        'ng: pkg verify ok',
        'ng: ring3 auth ok',
        'ng: ring3 pkg ok',
        'ng: ring3 svc ok',
        'ng: WP2-5 selftest PASS'
    )
    $patterns = @(
        'ng: smep=(on|off) smap=(on|off)',
        'ng: tss rsp0=0x[0-9a-f]+ ist1=0x[0-9a-f]+ idt8\.ist=1',
        'ng: syscall msr star=0x[0-9a-f]+ lstar=0x[0-9a-f]+ fmask=0x[0-9a-f]+',
        'ng: memmap ok \(usable=[0-9]+ pages\)',
        '(?s)ng: thread A 0.*?ng: thread B 0.*?ng: thread C 0',
        'ng: ipc stat sends=[0-9]+ recvs=[0-9]+ replies=20 timeouts=[0-9]+ eagains=[0-9]+ bad=[0-9]+ live=0',
        'ng: elf64 header entry=0x[0-9a-f]+ phnum=[0-9]+ type=2 machine=62',
        'ng: elf64 bad image rejected \(n=[0-9]+\)',
        'ng: syscall stack per-thread ok \(probe=0x[0-9a-f]+ rsp0=0x[0-9a-f]+ tss\.rsp0=0x[0-9a-f]+ boot=0x[0-9a-f]+\)',
        'ng: uaccess ok \(bad ptr EFAULT\)',
        'ng: elf64 data/bss ok',
        'ng: vfs mount ok \(root=ramfs mounts=2\)',
        'ng: ramfs write/read ok \(bytes=[0-9]+ limit=1MiB\)',
        'ng: fd bad rejected \(n=[0-9]+\)',
        'ng: vfs path edge ok \(n=[0-9]+\)',
        'ng: fs stat ok \(size=5\)',
        'ng: fs listdir ok \(n=1\)',
        'ng: fat16 mount ok \(clusters=[0-9]+\)',
        'ng: fat16 chain edge ok \(n=[0-9]+\)',
        'ng: fat16 bad bpb rejected \(n=[0-9]+\)',
        'ng: ata identify ok \(model=[^)]*sectors=[0-9]+ lba28=1 lba48=[01]\)',
        'ng: ata read ok \(lba=0 sig=0x[0-9a-f]+\)',
        'ng: ata write ok \(lba=[0-9]+ verify=1 restored=1\)',
        'ng: ata timeout rejected \(n=[0-9]+\)',
        'ng: disk fat16 mount ok \(clusters=[0-9]+\)',
        'ng: disk file rw ok \(bytes=600\)',
        'ng: ata stat reads=[0-9]+ writes=[0-9]+ timeouts=[0-9]+ errors=[0-9]+',
        'ng: log ring ok \(bytes=[0-9]+\)',
        'ng: pkg list ok \(n=1\)'
    )

    # WP2-3：显示/输入证据行。stdvga 与 -vga none 降级路径各取一套。
    if ($Fallback) {
        $required += @(
            'ng: pci vga missing',
            'ng: vga text fallback ok',
            'ng: kbd init ok',
            'ng: input event ok',
            'ng: ring3 input ready',
            'ng: ring3 input ok',
            'ng: WP2-3 selftest PASS'
        )
        $patterns += @(
            'ng: vbe unavailable .*vga text fallback'
        )
    } else {
        $required += @(
            'ng: pci vga found',
            'ng: vbe mode 1280x800x32 ok',
            'ng: fb draw ok (readback)',
            'ng: kbd init ok',
            'ng: input event ok',
            'ng: ring3 input ready',
            'ng: ring3 input ok',
            'ng: WP2-3 selftest PASS'
        )
        $patterns += @(
            'ng: pci vga 1234:1111 cmd=0x[0-9a-f]+ bar0=0x[0-9a-f]+ size=[0-9]+',
            'ng: vbe id=0x[0-9a-f]+ mem=[0-9]+',
            'ng: vbe pitch=[0-9]+ virt=1280x800',
            'ng: lfb map ok \(base=0x[0-9a-f]+ pages=[0-9]+\)',
            'ng: lfb pte=0x[0-9a-f]+ \(pcd/pwt\)',
            'ng: fb console cols=[0-9]+ rows=[0-9]+',
            'ng: ring3 input ok \((key=a|key=other|timeout)\)'
        )
    }

    # WP2-6：用户态 shell。默认内核在自检后拉起 shell，run-ng 经 QMP 逐字符
    # 注入命令序列；-Smoke 内核跳过 shell（只跑 ring3 hello）；-NoQmp 不注入，
    # 因此也不要求 WP2-6 的交互行（shell 会停在 readkey 等键，脚本命中前面的
    # 期望行后主动结束 QEMU）。
    if ($Smoke) {
        $required += @('ng: shell skipped (smoke mode)')
    } elseif (-not $NoQmp) {
        $required += @(
            'ng: shell elf ok',
            'ng: shell load ok',
            'ng: shell launch ok',
            'ng: shell ready',
            'ng: shell ls ok',
            'ng: shell cat ok',
            'ng: shell write ok',
            'ng: shell dmesg ok',
            'ng: shell pkg ok',
            'ng: shell exit ok',
            'ng: shell session ok (exit=0 rsp=ok)',
            'ng: shell session file ok (s.txt=3)',
            'ng: shell session pkg ok',
            'ng: WP2-6 selftest PASS'
        )
        $patterns += @(
            'ng: shell elf ok \(size=[0-9]+ entry=0x[0-9a-f]+ segs=[0-9]+\)',
            'ng: shell load ok \(base=0x[0-9a-f]+ end=0x[0-9a-f]+ pages=[0-9]+\)',
            'ng: shell exit \(code=0 syscalls=[0-9]+ writes=[0-9]+ fs=[0-9]+\)',
            'ng: shell session pkg ok \(lists=\+[0-9]+\)'
        )
    }

    $failPatterns = @(
        'ng: WP1-2 selftest FAIL',
        'ng: WP1-3 selftest FAIL',
        'ng: memmap FAIL',
        'ng: pmm .* FAIL',
        'ng: heap .* FAIL',
        'ng: WP1-4 selftest FAIL',
        'ng: sched .* FAIL',
        'ng: WP1-5 selftest FAIL',
        'ng: ipc .* FAIL',
        'ng: WP1-6 selftest FAIL',
        'ng: elf64 .* FAIL',
        'ng: ring3 .* FAIL',
        'ng: WP1-7 selftest FAIL',
        'ng: vfs .* FAIL',
        'ng: ramfs .* FAIL',
        'ng: fd .* FAIL',
        'ng: fs .* FAIL',
        'ng: WP2-1 selftest FAIL',
        'ng: fat16 .* FAIL',
        'ng: WP2-2 selftest FAIL',
        'ng: ata .* FAIL',
        'ng: disk .* FAIL',
        'ng: WP2-4 selftest FAIL',
        'ng: auth .* FAIL',
        'ng: log .* FAIL',
        'ng: pkg .* FAIL',
        'ng: WP2-5 selftest FAIL',
        'ng: WP2-3 selftest FAIL',
        'ng: pci vga .* FAIL',
        'ng: vbe .* FAIL',
        'ng: lfb .* FAIL',
        'ng: fb .* FAIL',
        'ng: kbd .* FAIL',
        'ng: input .* FAIL',
        'ng: vga text fallback FAIL',
        'ng: shell .* FAIL',
        'ng: WP2-6 selftest FAIL'
    )
}

function Test-Serial([string]$text) {
    if (-not $text) { return $false }
    foreach ($line in $required) {
        if (-not $text.Contains($line)) { return $false }
    }
    foreach ($p in $patterns) {
        if ($text -notmatch $p) { return $false }
    }
    return $true
}

function Fail([string]$msg) {
    Write-Host ("run-ng: FAIL ({0})" -f $msg)
    exit 1
}

# ---- 内核 ELF + WP2-4 磁盘镜像 ---------------------------------------------
if ($ElfPath) {
    if (-not (Test-Path -LiteralPath $ElfPath)) { Fail ("ELF not found: {0}" -f $ElfPath) }
    $elf = (Resolve-Path -LiteralPath $ElfPath).Path
} else {
    $elf = Join-Path (Join-Path $Repo 'dist-ng') 'aster-ng.elf'
}

if ($DiskImg) {
    if (-not (Test-Path -LiteralPath $DiskImg)) { Fail ("disk image not found: {0}" -f $DiskImg) }
    $disk = (Resolve-Path -LiteralPath $DiskImg).Path
} else {
    $disk = Join-Path (Join-Path $Repo 'dist-ng') 'disk.img'
}

if (-not (Test-Path -LiteralPath $elf) -or -not (Test-Path -LiteralPath $disk)) {
    Write-Host 'run-ng: kernel/disk image not built yet, running build-ng.ps1 first'
    $build = Join-Path $PSScriptRoot 'build-ng.ps1'
    $psHost = $null
    try { $psHost = (Get-Process -Id $PID).Path } catch { }
    if (-not $psHost) { $psHost = 'powershell' }
    $buildArgs = @('-NoProfile')
    if ($OnWindows) { $buildArgs += @('-ExecutionPolicy', 'Bypass') }
    $buildArgs += @('-File', $build, '-Repo', $Repo)
    & $psHost @buildArgs
    if ($LASTEXITCODE -ne 0) { Fail 'build-ng.ps1 failed' }
    if (-not (Test-Path -LiteralPath $elf)) { Fail 'build-ng.ps1 did not produce the ELF' }
    if (-not (Test-Path -LiteralPath $disk)) { Fail 'build-ng.ps1 did not produce dist-ng/disk.img' }
}

# ---- QEMU ------------------------------------------------------------------
$qemu = $null
$qcmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if ($qcmd) {
    $qemu = $qcmd.Source
} elseif ($OnWindows -and (Test-Path 'D:\Program Files\qemu\qemu-system-x86_64.exe')) {
    $qemu = 'D:\Program Files\qemu\qemu-system-x86_64.exe'
}
if (-not $qemu) { Fail 'qemu-system-x86_64 not found (PATH or D:\Program Files\qemu)' }

$qemuVer = ''
$verOut = & $qemu --version 2>&1
if ($LASTEXITCODE -eq 0 -and $verOut) { $qemuVer = (@($verOut)[0]).ToString() }

# ---- 串口日志（默认放临时目录，避免污染仓库）------------------------------
$tmpBase = Join-Path ([IO.Path]::GetTempPath()) ('aster-ng-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmpBase | Out-Null
$keepLog = [bool]$SerialLog
if (-not $SerialLog) { $SerialLog = Join-Path $tmpBase 'serial.log' }
$errLog = Join-Path $tmpBase 'qemu.err'
$outLog = Join-Path $tmpBase 'qemu.out'
Remove-Item -LiteralPath $SerialLog -Force -ErrorAction SilentlyContinue

Write-Host '=== run-ng (WP2-6) ==='
Write-Host ("elf:  {0} ({1} bytes)" -f $elf, (Get-Item -LiteralPath $elf).Length)
Write-Host ("disk: {0} ({1} bytes, ide)" -f $disk, (Get-Item -LiteralPath $disk).Length)
Write-Host ("shell: {0}" -f $(if ($Smoke) { 'skipped (-Smoke)' } elseif ($NoQmp) { 'no QMP injection (-NoQmp)' } else { 'QMP command script' }))
Write-Host ("qemu: {0}" -f $qemu)
if ($qemuVer) { Write-Host ("      {0}" -f $qemuVer) }

# ---- QMP：随机 TCP 端口，用于 sendkey 注入 --------------------------------
$qmpPort = 0
if (-not $NoQmp) {
    try {
        $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
        $listener.Start()
        $qmpPort = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
        $listener.Stop()
    } catch {
        Write-Host 'run-ng: WARN cannot reserve a QMP port; keyboard uses timeout path'
        $qmpPort = 0
    }
}

function Connect-Qmp([int]$Port) {
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $client = [Net.Sockets.TcpClient]::new()
            $client.Connect('127.0.0.1', $Port)
            $stream = $client.GetStream()
            $stream.ReadTimeout = 2000
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
    # QEMU 11 的 QMP 已经没有 sendkey（CommandNotFound）；用 input-send-event
    # 直接发 qcode 的按下/释放，失败再退到 HMP 的 sendkey。
    $events = ('{{"execute":"input-send-event","arguments":{{"events":[' +
        '{{"type":"key","data":{{"down":true,"key":{{"type":"qcode","data":"{0}"}}}}}},' +
        '{{"type":"key","data":{{"down":false,"key":{{"type":"qcode","data":"{0}"}}}}}}' +
        ']}}}}') -f $Key
    try {
        $Qmp.Writer.WriteLine($events)
        $resp = $Qmp.Reader.ReadLine()
        if ($resp -and ($resp -match '"return"')) {
            return $true
        }
        Write-Host ("run-ng: QMP input-send-event response: {0}" -f $resp)
    } catch {
        return $false
    }
    try {
        $Qmp.Writer.WriteLine(('{{"execute":"human-monitor-command","arguments":{{"command-line":"sendkey {0}"}}}}' -f $Key))
        $resp = $Qmp.Reader.ReadLine()
        if ($resp -and ($resp -match '"return"')) {
            return $true
        }
        Write-Host ("run-ng: QMP hmp sendkey response: {0}" -f $resp)
    } catch {
        return $false
    }
    return $false
}

# ---- WP2-6：QMP 注入 shell 命令序列 ----------------------------------------
# QMP input-send-event 一次可以带多个事件；普通字符是 down+up，'>' 是
# shift down + '.' down/up + shift up（PS/2 驱动按 shift 状态给出 '>'）。
$ShellSpecialKeys = @{
    ' ' = 'spc'; '/' = 'slash'; '.' = 'dot'; '-' = 'minus'; '=' = 'equal'
}

function Send-QmpKeys($Qmp, $Specs) {
    if (-not $Qmp) { return $false }
    $parts = @()
    foreach ($s in $Specs) {
        $down = $(if ($s.down) { 'true' } else { 'false' })
        $parts += ('{"type":"key","data":{"down":' + $down +
            ',"key":{"type":"qcode","data":"' + $s.key + '"}}}')
    }
    $cmd = '{"execute":"input-send-event","arguments":{"events":[' + ($parts -join ',') + ']}}'
    try {
        $Qmp.Writer.WriteLine($cmd)
        $resp = $Qmp.Reader.ReadLine()
        if ($resp -and ($resp -match '"return"')) { return $true }
        Write-Host ("run-ng: QMP input-send-event response: {0}" -f $resp)
    } catch {
        return $false
    }
    return $false
}

function Send-ShellCommand($Qmp, [string]$Cmd) {
    if (-not $Qmp) { return $false }
    foreach ($ch in $Cmd.ToCharArray()) {
        $ok = $false
        if ($ch -eq '>') {
            $ok = Send-QmpKeys $Qmp @(
                @{ key = 'shift'; down = $true }, @{ key = 'dot'; down = $true },
                @{ key = 'dot'; down = $false }, @{ key = 'shift'; down = $false })
        } else {
            $key = $ShellSpecialKeys["$ch"]
            if (-not $key) { $key = "$ch" }
            $ok = Send-QmpKeys $Qmp @(
                @{ key = $key; down = $true }, @{ key = $key; down = $false })
        }
        if (-not $ok) { return $false }
        Start-Sleep -Milliseconds 25
    }
    return (Send-QmpKeys $Qmp @(@{ key = 'ret'; down = $true }, @{ key = 'ret'; down = $false }))
}

function Wait-SerialCount([string]$Needle, [int]$Want, [int]$TimeoutMs) {
    $w = [Diagnostics.Stopwatch]::StartNew()
    while ($w.Elapsed.TotalMilliseconds -lt $TimeoutMs) {
        if (Test-Path -LiteralPath $SerialLog) {
            $t = Get-Content -LiteralPath $SerialLog -Raw -ErrorAction SilentlyContinue
            if ($t) {
                $n = ([regex]::Matches($t, [regex]::Escape($Needle))).Count
                if ($n -ge $Want) { return $true }
            }
        }
        Start-Sleep -Milliseconds 40
    }
    return $false
}

# 命令序列与 WP2-6 出口一致：help / ls / cat / echo 重定向 / cat / dmesg /
# pkg list / exit；每步等到对应证据行再发下一条，避免按键在环里堆太多。
function Send-ShellScript($Qmp) {
    $steps = @(
        @{ cmd = 'help';                 expect = '';                     count = 0 },
        @{ cmd = 'ls /';                 expect = 'ng: shell ls ok';      count = 1 },
        @{ cmd = 'cat /etc/passwd';      expect = 'ng: shell cat ok';     count = 1 },
        @{ cmd = 'echo hi > /tmp/s.txt'; expect = 'ng: shell write ok';   count = 1 },
        @{ cmd = 'cat /tmp/s.txt';       expect = 'ng: shell cat ok';     count = 2 },
        @{ cmd = 'dmesg -n 3';           expect = 'ng: shell dmesg ok';   count = 1 },
        @{ cmd = 'pkg list';             expect = 'ng: shell pkg ok';     count = 1 },
        @{ cmd = 'exit';                 expect = 'ng: shell exit ok';    count = 1 }
    )
    foreach ($step in $steps) {
        if (-not (Send-ShellCommand $Qmp $step.cmd)) {
            Write-Host ("run-ng: WARN QMP typing failed at '{0}'" -f $step.cmd)
            return $false
        }
        if ($step.expect) {
            if (-not (Wait-SerialCount $step.expect $step.count 3000)) {
                Write-Host ("run-ng: WARN '{0}' produced no '{1}'" -f $step.cmd, $step.expect)
            }
        } else {
            Start-Sleep -Milliseconds 250
        }
    }
    return $true
}
# -kernel 之后是 ELF；-serial file: 让脚本可轮询；headless、不自动重启、256 MiB；
# -drive if=ide 把 8MB FAT16 镜像挂到 PIIX3 IDE 主盘（legacy 0x1F0/0x3F6）。
$qemuArgs = @(
    '-kernel', ('"' + $elf + '"'),
    '-serial', ('"file:' + $SerialLog + '"'),
    '-drive', ('"file=' + $disk + ',format=raw,if=ide"'),
    '-display', 'none',
    '-no-reboot',
    '-m', '256',
    '-cpu', $Cpu,
    '-vga', $useVga
)
if ($qmpPort -gt 0) {
    $qemuArgs += @('-qmp', ("tcp:127.0.0.1:{0},server=on,wait=off" -f $qmpPort))
}
Write-Host ("args: -kernel <elf> -serial file:<log> -drive file=<disk.img>,format=raw,if=ide -display none -no-reboot -m 256 -cpu {0} -vga {1} (timeout {2}s, qmp {3})" -f $Cpu, $useVga, $TimeoutSec, $(if ($qmpPort) { $qmpPort } else { 'off' }))

$sw = [Diagnostics.Stopwatch]::StartNew()
$spArgs = @{
    FilePath = $qemu
    ArgumentList = $qemuArgs
    PassThru = $true
    RedirectStandardError = $errLog
    RedirectStandardOutput = $outLog
}
if ($OnWindows) { $spArgs['WindowStyle'] = 'Hidden' }
$p = Start-Process @spArgs

$qmp = $null
$injected = $false
$shellTyped = $false
if ($qmpPort -gt 0) {
    $qmp = Connect-Qmp -Port $qmpPort
    if ($qmp) {
        Write-Host 'run-ng: QMP connected (sendkey injection armed)'
    } else {
        Write-Host 'run-ng: WARN QMP connect failed; readkey uses timeout path'
    }
}

$matched = $false
$failedEarly = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    if ($p.HasExited) { break }
    if (Test-Path -LiteralPath $SerialLog) {
        $text = Get-Content -LiteralPath $SerialLog -Raw -ErrorAction SilentlyContinue
        if ($text) {
            foreach ($fp in $failPatterns) {
                if ($text -match $fp) { $failedEarly = $true; break }
            }
            if ($failedEarly) { break }
            # 用户程序已开始轮询 readkey：注入 'a'（只发一次）。
            if (-not $injected -and $qmp -and $text.Contains('ng: ring3 input ready')) {
                if (Send-QmpKey $qmp 'a') {
                    $injected = $true
                    Write-Host 'run-ng: QMP sendkey a -> ring3 readkey'
                } else {
                    Write-Host 'run-ng: WARN QMP sendkey failed; readkey uses timeout path'
                    $injected = $true
                }
            }
            # WP2-6：shell 已 ready -> 逐字符注入整条命令序列（一次会话只做一次）。
            if (-not $Smoke -and -not $NoQmp -and $qmp -and -not $shellTyped -and
                $text.Contains('ng: shell ready')) {
                $shellTyped = $true
                Write-Host 'run-ng: QMP typing WP2-6 shell commands'
                $null = Send-ShellScript $qmp
            }
            if (Test-Serial $text) { $matched = $true; break }
        }
    }
    Start-Sleep -Milliseconds 100
}
$sw.Stop()

# 内核是 hlt 循环，检测完/超时后必须主动结束 QEMU
$exitedEarly = $p.HasExited
if (-not $exitedEarly) {
    try { $p.Kill() } catch { }
    $p.WaitForExit(3000) | Out-Null
}
if ($qmp) {
    try { $qmp.Client.Close() } catch { }
    $qmp = $null
}

$serialText = ''
if (Test-Path -LiteralPath $SerialLog) {
    $serialText = (Get-Content -LiteralPath $SerialLog -Raw -ErrorAction SilentlyContinue)
}
$qemuErr = ''
if (Test-Path -LiteralPath $errLog) { $qemuErr = (Get-Content -LiteralPath $errLog -Raw -ErrorAction SilentlyContinue) }

function Remove-TmpBase {
    if (-not $keepLog) { Remove-Item -Recurse -Force $tmpBase -ErrorAction SilentlyContinue }
}

if ($matched) {
    Write-Host 'run-ng: serial output:'
    Write-Host ('-' * 48)
    Write-Host ($serialText.TrimEnd())
    Write-Host ('-' * 48)
    $missingCount = @($required | Where-Object { -not $serialText.Contains($_) }).Count
    Write-Host ("run-ng: matched {0}/{1} expected lines + {2} patterns" -f ($required.Count - $missingCount), $required.Count, $patterns.Count)
    if ($Panic) {
        Write-Host ("run-ng: PASS (panic path observed in {0:N1}s, cpu {1})" -f $sw.Elapsed.TotalSeconds, $Cpu)
    } else {
        Write-Host ("run-ng: PASS (all expected lines in {0:N1}s, cpu {1})" -f $sw.Elapsed.TotalSeconds, $Cpu)
    }
    Remove-TmpBase
    exit 0
}

# 失败：把串口/QEMU 输出都打出来，便于定位页表/长模式/far jump 的坑
Write-Host 'run-ng: serial output:'
Write-Host ('-' * 48)
if ($serialText) { Write-Host ($serialText.TrimEnd()) } else { Write-Host '(empty)' }
Write-Host ('-' * 48)
$missing = @($required | Where-Object { -not $serialText -or -not $serialText.Contains($_) })
if ($missing.Count -gt 0) {
    foreach ($m in $missing) { Write-Host ("run-ng: missing line: {0}" -f $m) }
}
if ($serialText) {
    foreach ($p in $patterns) {
        if ($serialText -notmatch $p) { Write-Host ("run-ng: missing pattern: {0}" -f $p) }
    }
}
if ($qemuErr -and $qemuErr.Trim()) {
    Write-Host 'run-ng: qemu stderr:'
    Write-Host ($qemuErr.TrimEnd())
}
if ($failedEarly) { Fail 'kernel reported ng: selftest FAIL' }
if ($exitedEarly) {
    # Start-Process 返回的进程对象有时读不到 ExitCode；读不到就不打，stderr 已足够定位
    $code = $null
    try { $p.WaitForExit(); $code = $p.ExitCode } catch { }
    $suffix = ''
    if ("$code" -ne '') { $suffix = " (code $code)" }
    Fail ("QEMU exited before all expected lines{0}" -f $suffix)
}
Fail ("timeout {0}s, expected lines not all found" -f $TimeoutSec)
