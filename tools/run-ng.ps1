# =============================================================================
# run-ng.ps1 - ng WP3-1: 在 QEMU 里启动 dist-ng/aster-ng.elf，等一组期望行
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
# WP2-7 追加（parity 100 项；-Panic 之外所有模式都要求）：
#   ng: parity 100/100 PASS / ng: WP2-7 selftest PASS
#   逐项 `parity: NNN/100 [域] ... ok`（001 与 100 各有一条正则），
#   以及失败模式 `ng: parity FAIL` / `parity: NNN/100 ... FAIL`。
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
# WP3-1 追加（display server / demo；stdvga 模式）：
#   ng: desktop bin ok (displayd=/bin/displayd demo=/bin/demo)
#   ng: displayd spawn/up / display port ok / display port file ok
#   ng: surface create ok / surface map ok / shm map ok
#   ng: composite ok (px=0x3070c0 at=160,120) / displayd status ok
#   ng: demo exit ok / shm refcount ok / demo hold wait ok
#   ng: displayd kill ok (status=265 live=0) / WP3-1 selftest PASS
#   QMP：看到 `ng: demo input ready` 注入 qcode b；看到 `ng: composite ok`
#   做 screendump，断言 PPM 上 (at+25,+25) == #3070c0（默认模式强制）。
#   -NoQmp 时 input 走 timeout 分支、不做 screendump；
#   -Fallback 时期望 `ng: WP3-1 selftest skipped (no fb)`。
#
# WP3-2 追加（shell/WM，L2b；stdvga 模式）：
#   ng: wm spawn ok (pid=) / wm up / wm panel ok / wm launcher ok / wm tasks ok
#   ng: displayd hotkey ok / wm task switch ok (focus=.. via=key) /
#   ng: wm launch ok (app=/bin/demo via=key) / demo window up /
#   ng: wm kill ok (status=265) / demo survives wm ok / WP3-2 selftest PASS
#   QMP：看到 `ng: wm tasks ok` 注入 Tab（热键 -> WM 任务切换）；看到
#   `ng: wm task switch ok` 注入 '1'（启动器 spawn demo）；看到
#   `ng: wm task switch ok` 再做一次 screendump：
#     (4,4) == #202838（面板背景）且 Demo 窗口探针 (x+25,y+25) == #3070c0。
#   -NoQmp 时 WM 走 auto 分支（via=auto），不做 screendump；
#   -Fallback 时期望 `ng: WP3-2 selftest skipped (no fb)`。
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
        'ng: WP2-5 selftest PASS',
        'ng: parity 100/100 PASS',
        'ng: WP2-7 selftest PASS',
        'ng: proc bin ok (shell=/bin/shell proc=/bin/proc hello=/bin/hello)',
        'ng: proc table ok (max=32 init=pid 1',
        'ng: proc spawn ok (pid=',
        'ng: proc wait ok (status=0 pid=',
        'ng: proc ipc cross ok (pid=',
        'ng: proc as isolated ok (va=0x40010000',
        'ng: proc kill ok (pids=',
        'ng: desktop bin ok (displayd=/bin/displayd demo=/bin/demo wm=/bin/wm sessiond=/bin/sessiond)',
        'ng: WP3-0 selftest PASS',
        'ng: WP3-1 selftest PASS'
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
        'ng: pkg list ok \(n=1\)',
        '(?m)^parity: 001/100 \[vfs\] .* \.\.\. ok',
        '(?m)^parity: 100/100 \[input\] .* \.\.\. ok'
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
    # WP3-1：display server（L2a）+ demo（L3）。stdvga 路径完整跑；-vga none
    # 时内核打印 "skipped (no fb)"（没有 LFB，displayd 无从映射）。
    if ($Fallback) {
        $required += @('ng: WP3-1 selftest skipped (no fb)')
    } else {
        $required += @(
            'ng: displayd spawn ok (pid=',
            'ng: displayd up (fb=0xfd000000 1280x800',
            'ng: display port ok (ticket=',
            'ng: display port file ok (ticket=',
            'ng: demo spawn ok (pid=',
            'ng: surface create ok (id=',
            'ng: surface map ok (id=',
            'ng: shm map ok (va=0x',
            'ng: composite ok (px=0x',
            'ng: input route ok (',
            'ng: demo exit ok (status=0)',
            'ng: shm refcount ok (live=',
            'ng: displayd status ok (surface=1',
            'ng: demo hold spawn ok (pid=',
            'ng: displayd kill ok (status=265',
            'ng: demo hold wait ok (status=0'
        )
        $patterns += @(
            'ng: displayd up \(fb=0x[0-9a-f]+ 1280x800 pitch=[0-9]+ bpp=32\)',
            'ng: surface create ok \(id=[0-9]+ w=400 h=300 shm=0x[0-9a-f]+ pages=118\)',
            'ng: shm map ok \(va=0x[0-9a-f]+ pages=118\)',
            'ng: composite ok \(px=0x3070c0 at=[0-9]+,[0-9]+\)',
            'ng: displayd status ok \(surface=1 commits=[0-9]+ input=[0-9]+ px=0x3070c0\)',
            'ng: demo hold wait ok \(status=0 waiters=0\)'
        )
        if ($NoQmp) {
            # 不注入按键：demo 打印 timeout 分支，仍然证明 INPUT 路径可达。
            $patterns += @('ng: input route ok \(timeout id=[0-9]+\)')
        } else {
            # QMP 注入 'b' -> displayd readkey -> INPUT 消息 -> demo。
            $patterns += @('ng: input route ok \(key=b id=[0-9]+\)')
        }
    }

    # WP3-2：shell/WM（L2b）。stdvga 路径完整跑；没有 LFB 时打印 skipped。
    if ($Fallback) {
        $required += @('ng: WP3-2 selftest skipped (no fb)')
    } else {
        $required += @(
            'ng: wm spawn ok (pid=',
            'ng: wm up (pid=',
            'ng: wm panel ok (px=',
            'ng: wm launcher ok (apps=',
            'ng: wm tasks ok (n=',
            'ng: wm task switch ok (focus=',
            'ng: wm launch ok (app=',
            'ng: demo window up (id=',
            'ng: wm kill ok (status=265',
            'ng: demo survives wm ok (alive=1',
            'ng: WP3-2 selftest PASS'
        )
        $patterns += @(
            'ng: wm up \(pid=[0-9]+ panel=1280x32\)',
            'ng: wm panel ok \(px=0x202838 at=0,0\)',
            'ng: wm launcher ok \(apps=[0-9]+ pkgs=[0-9]+\)',
            'ng: wm app ok \(n=0 path=/bin/demo\)',
            'ng: wm pkg ok \(path=/System/Packages/[a-z]+\)',
            'ng: wm tasks ok \(n=[0-9]+ apps=[0-9]+\)',
            'ng: surface title ok \(id=[0-9]+ title=Demo\)',
            'ng: demo window up \(id=[0-9]+ title=Demo\)',
            'ng: demo survives wm ok \(alive=1 id=[0-9]+\)'
        )
        if ($NoQmp) {
            # 没有 QMP：WM 在 ~3s 后走 auto fallback，仍产出切换/启动证据。
            $patterns += @(
                'ng: wm task switch ok \(focus=[0-9]+ from=[0-9]+ via=auto\)',
                'ng: wm launch ok \(app=/bin/demo pid=[0-9]+ via=auto\)'
            )
        } else {
            # QMP 注入 Tab -> displayd 热键副本 -> WM 任务切换；
            # 注入 '1' -> WM 启动器 spawn /bin/demo。
            $patterns += @(
                'ng: displayd hotkey ok \(key=0x[0-9a-f]+ scancode=0x[0-9a-f]+\)',
                'ng: wm task switch ok \(focus=[0-9]+ from=[0-9]+ via=key\)',
                'ng: wm launch ok \(app=/bin/demo pid=[0-9]+ via=key\)'
            )
        }
    }

    # WP3-3：session manager（L2c）。init 只 spawn sessiond；displayd/wm 由
    # sessiond 拉起，应用经 WM 的 LAUNCH 由 sessiond spawn；kill WM 后应用
    # 存活、WM 按退避重启。没有 LFB 时打印 skipped。
    if ($Fallback) {
        $required += @('ng: WP3-3 selftest skipped (no fb)')
    } else {
        $required += @(
            'ng: sessiond spawn ok (pid=',
            'ng: sessiond up (pid=',
            'ng: session wm ready ok (wm=',
            'ng: session launch seen ok (pid=',
            'ng: session registry seen ok (apps=',
            'ng: role token seen ok (tokens=',
            'ng: session wm killed ok (pid=',
            'ng: session wm restart seen ok (old=',
            'ng: session survives wm ok (app=',
            'ng: session app reap ok (',
            'ng: session cleanup ok (live=0',
            'ng: WP3-3 selftest PASS'
        )
        $patterns += @(
            'ng: sessiond up \(pid=[0-9]+ displayd=[0-9]+ wm=[0-9]+ port=[0-9]+\)',
            'ng: session start ok \(pid=[0-9]+ displayd=[0-9]+ wm=[0-9]+\)',
            'ng: session app launch ok \(pid=[0-9]+ app=/bin/demo token=0x[0-9a-f]+\)',
            'ng: session window ok \(pid=[0-9]+ id=[0-9]+ title=Demo\)',
            'ng: session registry ok \(apps=[0-9]+ windows=[0-9]+ procs=[0-9]+\)',
            'ng: role token ok \(tokens=[0-9]+ bound=[0-9]+ mismatch=0\)',
            'ng: session launch reject ok \(token=0x[0-9a-f]+ role=2 n=[0-9]+\)',
            'ng: focus reject ok \(id=[0-9]+ token=0x[0-9a-f]+ n=[0-9]+\)',
            'ng: session wm restart ok \(old=[0-9]+ new=[0-9]+ restarts=[0-9]+\)',
            'ng: session wm restart ready ok \(wm=[0-9]+ id=[0-9]+\)',
            'ng: session registry seen ok \(apps=[0-9]+ windows=[0-9]+\)',
            'ng: role token seen ok \(tokens=[0-9]+ tokenfail=0\)',
            'ng: session cleanup ok \(live=0 nodes=[0-9]+\)'
        )
        if ($NoQmp) {
            $patterns += @(
                'ng: wm launch req ok \(app=/bin/demo via=auto\)'
            )
        } else {
            $patterns += @(
                'ng: wm launch req ok \(app=/bin/demo via=key\)'
            )
        }
    }

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
            'ng: shell spawn ok',
            'ng: shell wait ok (pid=',
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
        'ng: WP2-6 selftest FAIL',
        'ng: WP2-7 selftest FAIL',
        'ng: proc .* FAIL',
        'ng: WP3-0 selftest FAIL',
        'ng: WP3-1 .* FAIL',
        'ng: displayd .* FAIL',
        'ng: surface .* FAIL',
        'ng: composite .* FAIL',
        'ng: demo .* FAIL',
        'ng: WP3-2 .* FAIL',
        'ng: wm .* FAIL',
        'ng: session .* FAIL',
        'ng: role token .* FAIL',
        'ng: WP3-3 .* FAIL',
        'ng: hotkey .* FAIL',
        'ng: parity FAIL',
        '(?m)^parity: [0-9]+/100 .* \.\.\. FAIL'
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

Write-Host '=== run-ng (WP3-3) ==='
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

# ---- WP3-1：QMP screendump + PPM 像素断言 --------------------------------
# displayd 合成完一帧后会打印 `ng: composite ok (px=0x.... at=X,Y)`；脚本据此
# 用 QMP screendump 抓一张 PPM，再断言 surface 内部探针像素
#   (X + 25, Y + 25) == #3070c0
# 与内核自检（displayd 从 LFB 读回的 px）/demo 的实心矩形颜色一致。
function Send-QmpScreendump($Qmp, [string]$Path) {
    if (-not $Qmp) { return $false }
    $jsonPath = ($Path -replace '\\', '/')
    try {
        $Qmp.Writer.WriteLine('{"execute":"screendump","arguments":{"filename":"' + $jsonPath + '"}}')
        $resp = $Qmp.Reader.ReadLine()
        if ($resp -and ($resp -match '"return"')) { return $true }
        Write-Host ("run-ng: QMP screendump response: {0}" -f $resp)
    } catch {
        return $false
    }
    return $false
}

function Read-Ppm([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $i = 0
    $tokens = @()
    while ($tokens.Count -lt 4) {
        while ($i -lt $bytes.Length -and ($bytes[$i] -eq 32 -or $bytes[$i] -eq 10 -or
                $bytes[$i] -eq 13 -or $bytes[$i] -eq 9)) { $i++ }
        if ($i -ge $bytes.Length) { return $null }
        if ($bytes[$i] -eq 35) {   # '#' 注释到行尾（PPM 允许）
            while ($i -lt $bytes.Length -and $bytes[$i] -ne 10) { $i++ }
            continue
        }
        $start = $i
        while ($i -lt $bytes.Length -and -not ($bytes[$i] -eq 32 -or $bytes[$i] -eq 10 -or
                $bytes[$i] -eq 13 -or $bytes[$i] -eq 9)) { $i++ }
        $tokens += [Text.Encoding]::ASCII.GetString($bytes, $start, $i - $start)
    }
    $i++   # 跳过 maxval 后面的一个空白字节
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
        @{ cmd = 'spawn /bin/proc hello'; expect = 'ng: shell spawn ok'; count = 1 },
        @{ cmd = 'wait'; expect = 'ng: shell wait ok'; count = 1 },
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
$demoInjected = $false
$screenDumped = $false
$screenChecked = $false
$screenBad = $false
$screenMsg = ''
$screenPixel = ''
$wmTabInjected = $false
$wmLaunchInjected = $false
$wmScreenChecked = $false
$wmScreenOk = $false
$wmScreenMsg = ''
$wmScreenPixel = ''
$wp33LaunchInjected = $false
$wp33ScreenChecked = $false
$wp33ScreenOk = $false
$wp33ScreenMsg = ''
$wp33ScreenPixel = ''
$wmTaskX = 192
$wmTaskY = 152
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
            # WP3-1：demo 已就绪 -> 注入 'b'（displayd readkey -> INPUT -> demo）。
            if (-not $demoInjected -and $qmp -and -not $Fallback -and $text.Contains('ng: demo input ready')) {
                $demoInjected = $true
                if (Send-QmpKey $qmp 'b') {
                    Write-Host 'run-ng: QMP sendkey b -> displayd input route'
                } else {
                    Write-Host 'run-ng: WARN QMP sendkey b failed; demo input uses timeout path'
                }
            }
            # WP3-2：WM 已经用 LIST 看到窗口 -> 注入 Tab（全局热键 -> 任务切换）。
            if (-not $wmTabInjected -and $qmp -and -not $Fallback -and $text.Contains('ng: wm tasks ok (')) {
                $wmTabInjected = $true
                if (Send-QmpKey $qmp 'tab') {
                    Write-Host 'run-ng: QMP sendkey tab -> WM hotkey task switch'
                } else {
                    Write-Host 'run-ng: WARN QMP sendkey tab failed; WM uses auto fallback'
                }
            }
            # WP3-2：任务切换完成 -> 注入 '1'（WM 启动器 spawn /bin/demo）。
            if (-not $wmLaunchInjected -and $qmp -and -not $Fallback -and $text.Contains('ng: wm task switch ok (focus=')) {
                $wmLaunchInjected = $true
                if (Send-QmpKey $qmp '1') {
                    Write-Host 'run-ng: QMP sendkey 1 -> WM launcher spawn demo'
                } else {
                    Write-Host 'run-ng: WARN QMP sendkey 1 failed; WM uses auto fallback'
                }
            }
            # WP3-3：sessiond 的 WM 就绪 -> 注入 '1'（WM 发 LAUNCH，由 sessiond spawn）。
            if (-not $wp33LaunchInjected -and $qmp -and -not $Fallback -and $text.Contains('ng: session wm ready ok (wm=')) {
                $wp33LaunchInjected = $true
                if (Send-QmpKey $qmp '1') {
                    Write-Host 'run-ng: QMP sendkey 1 -> session LAUNCH (sessiond spawn demo)'
                } else {
                    Write-Host 'run-ng: WARN QMP sendkey 1 failed; WM uses auto launch'
                }
            }
            # WP3-1：第一帧合成完成 -> screendump + 像素断言（只做一次）。
            if (-not $screenChecked -and $qmp -and -not $Fallback -and $text.Contains('ng: composite ok (px=')) {
                $screenChecked = $true
                $shot = Join-Path $tmpBase 'screen.ppm'
                Remove-Item -LiteralPath $shot -Force -ErrorAction SilentlyContinue
                if (Send-QmpScreendump $qmp $shot) {
                    for ($i = 0; $i -lt 60; $i++) {
                        if ((Test-Path -LiteralPath $shot) -and (Get-Item -LiteralPath $shot).Length -gt 54) { break }
                        Start-Sleep -Milliseconds 50
                    }
                    Start-Sleep -Milliseconds 150
                    $ppm = $null
                    if (Test-Path -LiteralPath $shot) { $ppm = Read-Ppm $shot }
                    $originX = 160
                    $originY = 120
                    $last = [regex]::Matches($text, 'ng: composite ok \(px=0x[0-9a-f]+ at=(\d+),(\d+)\)')
                    if ($last.Count -gt 0) {
                        $originX = [int]$last[$last.Count - 1].Groups[1].Value
                        $originY = [int]$last[$last.Count - 1].Groups[2].Value
                    }
                    $probeX = $originX + 25
                    $probeY = $originY + 25
                    $pixel = Get-PpmPixel $ppm $probeX $probeY
                    if ($null -eq $pixel) {
                        $screenBad = $true
                        $screenMsg = ("no ppm/pixel at {0},{1}" -f $probeX, $probeY)
                    } elseif ($pixel[0] -eq 0x30 -and $pixel[1] -eq 0x70 -and $pixel[2] -eq 0xC0) {
                        $screenDumped = $true
                        $screenPixel = ("({0},{1})=#{2:x2}{3:x2}{4:x2}" -f $probeX, $probeY, $pixel[0], $pixel[1], $pixel[2])
                        Write-Host ("run-ng: screendump pixel ok {0}" -f $screenPixel)
                    } else {
                        $screenBad = $true
                        $screenMsg = ("pixel ({0},{1})=#{2:x2}{3:x2}{4:x2}, want #3070c0" -f $probeX, $probeY, $pixel[0], $pixel[1], $pixel[2])
                    }
                } else {
                    $screenBad = $true
                    $screenMsg = 'QMP screendump failed'
                }
            }
            # WP3-2：任务切换完成 -> screendump 断言面板背景 + Demo 窗口存在。
            if (-not $wmScreenChecked -and $qmp -and -not $Fallback -and -not $NoQmp -and $text.Contains('ng: wm task switch ok (focus=')) {
                $wmScreenChecked = $true
                $shot2 = Join-Path $tmpBase 'screen2.ppm'
                Remove-Item -LiteralPath $shot2 -Force -ErrorAction SilentlyContinue
                $taskMatch = [regex]::Matches($text, 'ng: wm task ok \(id=(\d+) x=(\d+) y=(\d+)')
                if ($taskMatch.Count -gt 0) {
                    $wmTaskX = [int]$taskMatch[$taskMatch.Count - 1].Groups[2].Value
                    $wmTaskY = [int]$taskMatch[$taskMatch.Count - 1].Groups[3].Value
                }
                if (Send-QmpScreendump $qmp $shot2) {
                    for ($i = 0; $i -lt 60; $i++) {
                        if ((Test-Path -LiteralPath $shot2) -and (Get-Item -LiteralPath $shot2).Length -gt 54) { break }
                        Start-Sleep -Milliseconds 50
                    }
                    Start-Sleep -Milliseconds 150
                    $ppm2 = $null
                    if (Test-Path -LiteralPath $shot2) { $ppm2 = Read-Ppm $shot2 }
                    $panelPx = Get-PpmPixel $ppm2 4 4
                    $demoPx = Get-PpmPixel $ppm2 ($wmTaskX + 25) ($wmTaskY + 25)
                    if ($null -eq $panelPx -or $null -eq $demoPx) {
                        $wmScreenMsg = ("no ppm/pixel panel=({0}) demo=({1},{2})" -f $panelPx, ($wmTaskX + 25), ($wmTaskY + 25))
                    } elseif ($panelPx[0] -ne 0x20 -or $panelPx[1] -ne 0x28 -or $panelPx[2] -ne 0x38) {
                        $wmScreenMsg = ("panel (4,4)=#{0:x2}{1:x2}{2:x2}, want #202838" -f $panelPx[0], $panelPx[1], $panelPx[2])
                    } elseif ($demoPx[0] -ne 0x30 -or $demoPx[1] -ne 0x70 -or $demoPx[2] -ne 0xC0) {
                        $wmScreenMsg = ("demo ({0},{1})=#{2:x2}{3:x2}{4:x2}, want #3070c0" -f ($wmTaskX + 25), ($wmTaskY + 25), $demoPx[0], $demoPx[1], $demoPx[2])
                    } else {
                        $wmScreenOk = $true
                        $wmScreenPixel = ("panel(4,4)=#{0:x2}{1:x2}{2:x2} demo({3},{4})=#{5:x2}{6:x2}{7:x2}" -f `
                            $panelPx[0], $panelPx[1], $panelPx[2], ($wmTaskX + 25), ($wmTaskY + 25), `
                            $demoPx[0], $demoPx[1], $demoPx[2])
                        Write-Host ("run-ng: wm screendump pixel ok {0}" -f $wmScreenPixel)
                    }
                } else {
                    $wmScreenMsg = 'QMP screendump (wm) failed'
                }
            }
            # WP3-3：WM 重启后会话回来了 -> screendump 断言面板 + Demo 窗口仍在。
            if (-not $wp33ScreenChecked -and $qmp -and -not $Fallback -and -not $NoQmp -and $text.Contains('ng: session wm restart ready ok (wm=')) {
                $wp33ScreenChecked = $true
                $shot3 = Join-Path $tmpBase 'screen3.ppm'
                Remove-Item -LiteralPath $shot3 -Force -ErrorAction SilentlyContinue
                $task3 = [regex]::Matches($text, 'ng: wm task ok \(id=(\d+) x=(\d+) y=(\d+) title=Demo\)')
                if ($task3.Count -gt 0) {
                    $wmTaskX = [int]$task3[$task3.Count - 1].Groups[2].Value
                    $wmTaskY = [int]$task3[$task3.Count - 1].Groups[3].Value
                }
                if (Send-QmpScreendump $qmp $shot3) {
                    for ($i = 0; $i -lt 60; $i++) {
                        if ((Test-Path -LiteralPath $shot3) -and (Get-Item -LiteralPath $shot3).Length -gt 54) { break }
                        Start-Sleep -Milliseconds 50
                    }
                    Start-Sleep -Milliseconds 150
                    $ppm3 = $null
                    if (Test-Path -LiteralPath $shot3) { $ppm3 = Read-Ppm $shot3 }
                    $panelPx3 = Get-PpmPixel $ppm3 4 4
                    $demoPx3 = Get-PpmPixel $ppm3 ($wmTaskX + 25) ($wmTaskY + 25)
                    if ($null -eq $panelPx3 -or $null -eq $demoPx3) {
                        $wp33ScreenMsg = ("no ppm/pixel panel=({0}) demo=({1},{2})" -f $panelPx3, ($wmTaskX + 25), ($wmTaskY + 25))
                    } elseif ($panelPx3[0] -ne 0x20 -or $panelPx3[1] -ne 0x28 -or $panelPx3[2] -ne 0x38) {
                        $wp33ScreenMsg = ("panel (4,4)=#{0:x2}{1:x2}{2:x2}, want #202838" -f $panelPx3[0], $panelPx3[1], $panelPx3[2])
                    } elseif ($demoPx3[0] -ne 0x30 -or $demoPx3[1] -ne 0x70 -or $demoPx3[2] -ne 0xC0) {
                        $wp33ScreenMsg = ("demo ({0},{1})=#{2:x2}{3:x2}{4:x2}, want #3070c0" -f ($wmTaskX + 25), ($wmTaskY + 25), $demoPx3[0], $demoPx3[1], $demoPx3[2])
                    } else {
                        $wp33ScreenOk = $true
                        $wp33ScreenPixel = ("panel(4,4)=#{0:x2}{1:x2}{2:x2} demo({3},{4})=#{5:x2}{6:x2}{7:x2}" -f `
                            $panelPx3[0], $panelPx3[1], $panelPx3[2], ($wmTaskX + 25), ($wmTaskY + 25), `
                            $demoPx3[0], $demoPx3[1], $demoPx3[2])
                        Write-Host ("run-ng: wp3-3 screendump pixel ok {0}" -f $wp33ScreenPixel)
                    }
                } else {
                    $wp33ScreenMsg = 'QMP screendump (wp3-3) failed'
                }
            }
            # WP2-6：shell 已 ready -> 逐字符注入整条命令序列（一次会话只做一次）。
            if (-not $Smoke -and -not $NoQmp -and $qmp -and -not $shellTyped -and                $text.Contains('ng: shell ready')) {
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
    if (-not $Panic -and -not $Fallback -and -not $NoQmp) {
        if (-not $screenDumped -or $screenBad) {
            Fail ("QMP screendump pixel assertion failed ({0})" -f $screenMsg)
        }
        Write-Host ("run-ng: screendump pixel: {0} (QMP PPM)" -f $screenPixel)
        if (-not $wmScreenChecked -or -not $wmScreenOk) {
            Fail ("WP3-2 screendump pixel assertion failed ({0})" -f $wmScreenMsg)
        }
        Write-Host ("run-ng: wm screendump pixel: {0} (QMP PPM)" -f $wmScreenPixel)
        if (-not $wp33ScreenChecked -or -not $wp33ScreenOk) {
            Fail ("WP3-3 screendump pixel assertion failed ({0})" -f $wp33ScreenMsg)
        }
        Write-Host ("run-ng: wp3-3 screendump pixel: {0} (QMP PPM)" -f $wp33ScreenPixel)
    }
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
