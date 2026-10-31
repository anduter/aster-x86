# 演进计划（ng 分支）

> 配套 architecture.md。执行会话按工作包（WP）推进；每个 WP 独立提交、独立门禁。
> 原则：**重写底层，移植中层，新写顶层**；不整体重写。

## 0. 总原则

1. main 冻结在 69ab006；打 tag v0.75-32bit-final。
2. 新分支 ng；旧树只做参考和 parity oracle，不再加功能。
3. 一个 WP 一个会话；按路径所有权，不跨域改文件。
4. 每个 WP 必须有出口测试和 claims；没有出口不算完成。
5. D0 之前不加“顺手功能”；范围变更先改 architecture.md / decisions.md。
6. 每天至少一个可演示产物（截图/串口/命令）。

## 1. 阶段与时间盒

| 阶段 | 内容 | 时间盒 | 出口 |
|---|---|---|---|
| E0 准备 | tag/branch/docs/CI/域检查/工具链 | 1 周 | ng 可构建；check-domains 生效 |
| E1 64 位核心 | boot、long mode、页表、trap、syscall、PMM/heap、调度、IPC、ELF64 | **6 周（硬上限）** | ring3 hello + host 单测 |
| E2 服务/驱动移植 | VFS/ramfs、FAT16、fb/input/blk、auth/log/pkg、shell | 8 周 | 64 位 shell + 100 项 parity |
| E3 桌面会话 S1 | display server、shell/WM、session manager、SDK alpha、demo | 8 周 | ✅ 杀 shell 应用不死（tag ng-e3；WM 死循环为已知限制） |
| E4 D0 收口 | 故障注入、外部开发者测试、ABI 冻结、文档/发布 | 4 周 | D0 出口（见 architecture.md） |
| E5 D1 | POSIX Profile、TLS/HTTP、浏览器 B0、真实硬件、8h 浸泡 | 按 D1 计划 | D1 出口 |

E1 是硬时间盒：6 周内没跑到 ring3，停下来重新评估（增加移植、缩范围或调整架构），不要无限期调页表。

## 2. 工作包（WP）

### E0

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP0-1 | main 打 tag、开 ng、冻结旧树 | git | tag 存在；ng 可构建 |
| WP0-2 | 域目录 + 文档 + 所有权 | docs/design、src/* | architecture/evolution/decisions 入库 |
| WP0-3 | check-domains + CI 骨架 | tools/ci | 向上依赖会失败 |
| WP0-4 | 64 位工具链探针 | tools/ | x86_64 编译器可用；hello 能链 |

### E1

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP1-1 | boot 入口：multiboot2/UEFI stub、long mode、串口 | src/kernel/boot | 串口打印 64 位 hello |
| WP1-2 | 4 级页表、W^X/NX、SMEP/SMAP | src/kernel/mm | 页表单测 + 保护生效 |
| WP1-3 | trap/syscall：IDT、syscall 指令、ABI 1.0 草案 | src/kernel/proc | 用户态可进内核再返回 |
| WP1-4 | PMM/heap 迁移 | src/kernel/mm | host 单测通过 |
| WP1-5 | 调度/线程/上下文切换 | src/kernel/proc | 两个线程可切换 |
| WP1-6 | IPC/cap：端口、句柄、cap、close | src/kernel/svc | ping/reply 单测 |
| WP1-7 | ELF64 加载器 + ring3 hello | src/kernel/user、src/apps | ring3 打印并退出 |
| WP1-8 | host 单测 64 位化 + ng CI 门禁 | tests/、tools/ci-ng.ps1 | 单测全 PASS；一条命令门禁 |

E1 出口状态（2026-10-31）：**E1 完成，待打 tag `ng-e1`**。WP1-1..WP1-8 全部
完成：

- `tools/unit-ng.ps1` host 单测 **1085 checks, 0 failures**（WP1-7 为 901；
  WP1-8 补 uaccess 页边界 55、syscall 分发 81、handle/ipc 极端用例 48）；
- `tools/run-ng.ps1` 39 条期望行 + 11 条正则全绿：ring3 由 ELF64 加载器加载
  `hello` 并打印 `ng: ring3 hello`、正常退出（见 `docs/claims/ng-WP1-7.md`）；
- `tools/ci-ng.ps1` 一条命令跑 check-domains / check-deps / unit-ng / build-ng /
  run-ng / 旧 32 位 `build.ps1 -Test`，全绿输出 `=== NG CI PASS ===`
  （`-Quick` 跳 QEMU/legacy）；`.github/workflows/ng.yml` 在 ubuntu-latest 上
  跑 `-Quick` + 非阻塞 QEMU 冒烟；
- 旧 32 位 **305/305**；check-domains / check-deps 0 违规
  （见 `docs/claims/ng-WP1-8.md`）。

### E2

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP2-1 | VFS/ramfs 移植 | src/services/fs | 文件读写/链接/权限测试 |
| WP2-2 | FAT16 移植 | src/services/fs | 宿主交换文件 |
| WP2-3 | fb/virtio-gpu 2D + 键鼠 | src/drivers | 显示字符/图形；输入事件 |
| WP2-4 | 块设备（ATA/virtio-blk） | src/drivers | 读扇区/文件系统挂载 |
| WP2-5 | auth/log/pkg 移植 | src/services | 登录/日志/装包 |
| WP2-6 | shell 移植 | src/apps | 64 位 shell 可交互 |
| WP2-7 | parity 套件 100 项 | tests/ | 100 项通过 |

E2 出口状态（2026-10-31）：**E2 完成，待打 tag `ng-e2`**。WP2-1..WP2-7 全部
完成，出口证据：

- 内核 parity **100/100 PASS**（`ng: parity 100/100 PASS`；run-ng 108/108
  期望行 + 42 条正则）；host parity **95/95**（显示/输入 5 项需 QEMU）；
- `tools/unit-ng.ps1` host 单测 **3203 checks, 0 failures**；
- `tools/ci-ng.ps1` 一条命令 7/7 PASS（check-domains / check-deps / unit-ng /
  build-ng / run-ng / parity-ng / 旧 32 位 `build.ps1 -Test`）；
- 旧 32 位 **305/305**；`-Fallback` / `-NoQmp` / `-Smoke` 三种模式 parity
  均为 100/100；
- 证据与 100 项逐条结果见 `docs/claims/ng-WP2-7.md`。

E2 进度（2026-10-31）：**WP2-1、WP2-2、WP2-3、WP2-4、WP2-5、WP2-6、WP2-7
全部完成；E2 出口达成，待打 tag `ng-e2`**（tag 由负责人打）。`src/services/fs`
成为第一个真正落地的 L1 服务域；
`src/drivers` 成为第一个 L1D 设备域；`src/services/{auth,log,pkg}` 补齐 L1
认证/日志/包最小集；`src/apps/shell` 成为第一个独立编译的 L3 应用：

- VFS 核心（挂载/节点/路径/fd/权限，负 errno）+ ramfs（节点/目录项/页链，
  1 MiB/文件）+ syscall 20..27（用户指针经 uaccess）+ ring3 demo
  （open→write→read→stat→close→listdir）；
- FAT16 作为第二个 VFS 后端（`src/services/fs/fat16.c`）：L1 `blk_env`
  块设备抽象（内核绑 PMM RAM 盘，host 绑 fake 镜像）、BPB 几何校验、
  FAT 表单扇区缓存、簇链循环/截断/坏簇/越界检测、空闲簇分配/释放、
  8.3 目录项（卷标/LFN 跳过）、根目录+子目录、mount/read/write/create/
  unlink/rename/truncate/readdir；1.44MB 镜像由 `tools/mkfat-ng.ps1` 生成，
  以 `.fat16` PT_LOAD 嵌在 2 MiB（PMM 保留）并在启动时复制到 RAM 盘，
  挂到 `/mnt/fat`；
- WP2-3 显示/输入（`src/drivers/**`，L1D）：PCI mechanism #1 定位 QEMU
  stdvga `1234:1111`、BAR0 探测/映射；Bochs VBE 1280×800×32（回退
  1024×768×32）；LFB 以 PCD/PWT/NX 恒等映射进内核 PML4；8×16 内嵌字模的
  帧缓冲控制台 + 棋盘/文字/像素读回自检；`-vga none` 回退 VGA 文本 0xB8000；
  PS/2 set 1 键盘 + 128 项事件环 + 非阻塞 syscall 30 `readkey`；ring3 demo
  经 run-ng QMP `input-send-event` 注入按键，打印 `ng: ring3 input ok (key=a)`；
- WP2-4 块设备（`src/drivers/block/ata.c`，L1D）：legacy 0x1F0/0x3F6 的 ATA
  PIO（IDENTIFY / LBA28 READ / WRITE / FLUSH），PCI 确认 QEMU PIIX3 IDE
  （8086:7010）；所有等待都是有界轮询（BSY/DRQ/ERR/DF + 超时），错误返回
  EIO、超时返回 ETIMEDOUT、不 panic；`src/ng/ata_blk.c` 把驱动包装成 L1
  `blk_env`（L1D 不向上依赖 L1）；`tools/mkfat-ng.ps1 -SizeMB 8` 生成
  `dist-ng/disk.img`（8MB FAT16、8143 簇），run-ng 以
  `-drive file=...,format=raw,if=ide` 挂成 IDE 主盘，FAT16 挂 `/mnt/disk`；
  ring3 demo 读 `HELLO.TXT`、写/读回 `DISK.TXT`、stat；
- 内核自检 `ng: vfs mount ok` / `ng: ramfs write/read ok` / `ng: fd bad
  rejected` / `ng: vfs path edge ok` / `ng: WP2-1 selftest PASS`，`ng: fat16
  mount ok (clusters=2841)` / `read ok` / `write ok` / `chain edge ok` /
  `bad bpb rejected` / `ng: WP2-2 selftest PASS`，以及 WP2-3 的
  `ng: pci vga found` / `ng: vbe mode 1280x800x32 ok` /
  `ng: fb draw ok (readback)` / `ng: kbd init ok` / `ng: input event ok` /
  `ng: WP2-3 selftest PASS`，以及 WP2-4 的
  `ng: ata identify ok (model=QEMU HARDDISK sectors=16384)` / `ata read ok` /
  `ata write ok` / `ata timeout rejected` / `disk fat16 mount ok (clusters=8143)` /
  `disk file rw ok` / `ng: WP2-4 selftest PASS`；run-ng **80 条期望行 + 34 条正则**
  （`-Fallback` 为 79/28）；
- WP2-5 auth/log/pkg（`src/services/auth`、`log`、`pkg`，L1）：
  - auth：root(0)/alice(1000)，16 字节 salt + SHA-256 **4096 轮**迭代（自实现，
    不依赖内核/OpenSSL），`auth_init` 种入 `/etc/passwd`
    （`name:uid:salt_hex:hash_hex:iterations`），`auth_login` 返回 uid /
    -ENOENT / -EACCES，连续 5 次失败锁定；
  - log：64 KiB 按行环形缓冲（满时丢最旧整行），`log_write(tag,msg)` /
    `log_read(buf,max)`；内核启动/每个 WP 汇总/panic 写入 log；
  - pkg：ramfs `/System/Packages/hello/1.0.0/{manifest,bin/hello}` 种子，
    严格 key=value 清单（name/version/type/entry/sha256），`pkg_list` /
    `pkg_verify`（重算文件 SHA-256），不做签名/RSA（D1）；
  - syscall **31 login / 32 dmesg / 33 log_write / 34 pkg_list / 35 pkg_verify**
    （用户指针全部走 uaccess；内核自检用临时 U=1 页跑成功路径，ring3 demo
    再走一遍），证据行 `ng: auth users ok (2)` / `ng: auth login root ok
    (uid=0)` / `ng: auth login alice ok (uid=1000)` / `ng: auth bad password
    rejected` / `ng: log ring ok (bytes=...)` / `ng: dmesg syscall ok` /
    `ng: pkg list ok (n=1)` / `ng: pkg verify ok` / `ng: WP2-5 selftest PASS`；
- **WP2-6 用户态 shell（`src/apps/shell/shell.c`，L3，独立 ELF64）**：`ng> `
  提示符 + `readkey` 行编辑（可打印/Backspace/Enter/Ctrl-L 清屏/Ctrl-C 丢弃/
  Ctrl-U 删行/Ctrl-D 退出）；命令 `help`、`ls [path]`、`cat <file>`、
  `echo <text> [> path]`、`mkdir`、`rm`、`cd`、`pwd`、`stat`、`dmesg [-n N]`、
  `log <tag> <msg>`、`pkg list|verify`、`login`、`clear`、`exit [code]`；
  未知命令/缺参/文件不存在打印带 errno 文本的友好错误并把状态置 1（`exit`
  无参时返回该状态），不 panic；文件操作全部走 syscall 20..27（先把 shell 自己
  的 cwd 解析成绝对路径）、auth 31、log 32/33、pkg 34/35、输入 30；
  `tools/build-ng.ps1` 把 shell.c 编译成 22760 字节 ELF64（3 PT_LOAD，base
  0x40000000）嵌入内核，启动在 ring3 hello 之后拉起交互 shell
  （`-Smoke` 跳过 shell 只跑 hello）；`tools/run-ng.ps1` 经 QMP
  `input-send-event` 逐字符注入
  `help -> ls / -> cat /etc/passwd -> echo hi > /tmp/s.txt -> cat /tmp/s.txt ->
  dmesg -n 3 -> pkg list -> exit` 并校验
  `ng: shell ready` / `ls|cat|write|dmesg|pkg|exit ok` /
  `ng: shell exit (code=0 ...)` / `ng: shell session file ok (s.txt=3)` /
  `ng: WP2-6 selftest PASS`；顺带修正 `isr.S` ring3 返回未恢复 callee-saved
  寄存器（shell 的 rbx 指向用户栈导致 `*out` 写 #PF）的 WP1-7 潜在缺陷；
- host 单测 1085 → 1438（WP2-1）→ 1824（WP2-2）→ 2290（WP2-3，+466）→
  2522（WP2-4 +232）→ 2791（WP2-5 +269）→ 3108（WP2-6 +317）→ **3203
  checks**（WP2-7 +95：parity 100 项里可 host 化的 VFS/ramfs/FAT16/ATA/
  auth/log/pkg/shell）；旧 32 位 305/305；
- **WP2-7 parity 100 项（E2 出口）**：`tests/parity/parity_cases.h` 是唯一的
  100 项 case 表（编号/域/描述），`tests/parity/parity_body.inc` 是 kernel 与
  host **共用**的断言体（90 项共享 + ATA 5 + 显示/输入 5）。内核 harness
  `src/ng/parity_selftest.c` 逐项打印 `parity: NNN/100 [域] ... ok`，汇总
  `ng: parity 100/100 PASS`，覆盖 vfs 20、ramfs 15、fat16 20、ata 5、auth 10、
  log 5、pkg 10、shell tokenizer/cwd 10、显示 3 + 输入 2；`tools/parity-ng.ps1`
  跑（或复用）run-ng 日志并断言“恰好 100 条、编号连续、无 FAIL”，
  `tools/ci-ng.ps1` 第 6 步接入（复用第 5 步日志，不重复起 QEMU）。
  **没有新增功能，也没有发现需要修复的产品 bug**；证据见
  `docs/claims/ng-WP2-7.md`（含 100 项逐条表）；
- 设计/限制见 `docs/design/vfs-ng.md`、`fat16-ng.md`、`display-input-ng.md`、
  `block-ng.md`、`auth-log-pkg-ng.md`、`shell-ng.md`，证据见
  `docs/claims/ng-WP2-1.md` … `ng-WP2-7.md`。**E2 完成**：64 位 shell +
  100 项 parity + 全绿门禁；下一步 E3（display server / shell-WM / session
  manager / SDK alpha）；fb/输入不做 IRQ/鼠标（E3/C 之后），shell 的
  TTY/屏幕输出留给 E3 display server。

### E3

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP3-0 | **进程**（前置）：proc_t/进程表、spawn/wait/kill、per-process 句柄、跨进程 ticket IPC | src/ng/proc*.c、src/apps/proc | 3 进程链式 ping + shell 子进程 |
| WP3-1 | display server：surface/buffer/焦点/扫描输出 | src/desktop/display | 一个窗口显示 |
| WP3-2 | shell/WM：面板、启动器、任务切换 | src/desktop/shell | 可启动/切换应用 |
| WP3-3 | session manager：应用生命周期/窗口表/角色令牌 | src/desktop/session | 应用不归 shell 管 |
| WP3-4 | SDK alpha + demo app | sdk/、src/apps | 外部可编译 GUI app |
| WP3-5 | S1 故障注入：杀 shell/输入 | tests/ | 应用存活、面板回来 |

E3 进度（2026-10-31）：**WP3-0 完成**（多进程用户态 + 跨进程 IPC 前置）。
`src/ng/proc.h` / `proc_table.c` / `proc.c` / `proc_selftest.c` 落地进程模型：
每进程 1 AS + 1 主线程 + 1 张 64 槽句柄表；进程表 32 槽、pid 单调；
syscall **40 spawn / 41 wait / 42 getpid / 43 kill / 44 getppid**；`spawn` 从
VFS（ramfs `/bin/*`）读 ELF64 → `elf64_load` 新 AS → 新内核线程 → ring3；
跨进程只经 **ticket**（端口全局身份）传递；`wait` 用 `thread_sleep` 阻塞、
子进程退出 `thread_wake` 唤醒；`kill` 摘线程/摘 IPC 等待者/关端口/销毁 AS。
`src/apps/proc/proc.c` 是一个 ELF 多角色（hello/relay/server/hold），
自检跑出 **3 进程链式 ping**（hello→relay→server，ticket 经 argv + 消息 from
两跳）与 AS 隔离/阻塞态 kill 证据；shell 新增 `spawn` / `wait` / `pid` 命令，
run-ng 注入 `spawn /bin/proc hello` + `wait` 验证 shell 作为父进程。
证据见 `docs/claims/ng-WP3-0.md`，设计见 `docs/design/proc-ng.md`。

**WP3-1 完成**（display server，L2a）。内核只加机制：syscall **45 SHM_ALLOC /
46 SHM_MAP / 47 FB_MAP**（共享物理页 + refcount、owner/root 校验、进程退出/
被 kill 先摘映射再销毁 AS；LFB 以 PCD/PWT/NX 映射进用户 AS，root-only）；L2
新增 `src/desktop/{include,src,display}`：`/bin/displayd`（ring3）用
`FB_MAP` 拿扫描输出、`PORT_CREATE` 建显示端口并把 ticket 写进
`/run/display.port`，surface 表/z-order/裁剪/ARGB source-over 合成/基础光标/
焦点/PS2 输入转发全部在 ring3；surface 像素走**共享内存**（控制面仍是 ≤4 KiB
IPC 消息，CREATE 返回 `shm_handle`）。`/bin/demo`（L3）读发现文件 → 建
400x300 surface → 画渐变/矩形/文字 → COMMIT → 等 INPUT。run-ng 用 QMP
`input-send-event` 注入 `b`，并 `screendump` 抓 PPM 断言 `(185,145)=#3070c0`；
kill displayd 后内核继续跑、客户端自己超时退出（崩溃隔离）。证据见
`docs/claims/ng-WP3-1.md`，设计见 `docs/design/display-server-ng.md`。

**WP3-2 完成**（shell/WM，L2b + display 协议 v2）。L2 新增
`src/desktop/shell/wm.c`：ring3 `/bin/wm` 作为 displayd 的普通客户端，
`CREATE(1280x32, title="Aster Panel", POS (0,0))` 拿共享内存画面板（标题/
时钟占位/启动器按钮/任务按钮/通知区），`REGISTER_HOTKEY(key=ANY)` 让 displayd
把每个按键先给 WM 一份，`LIST/LIST_REPLY` 做任务表，`Tab`/`Alt+Tab` → `FOCUS`
切换（下一次 LIST 确认后计数），`1..9` → 启动器 `spawn`。协议扩到 **v2**：
CREATE 加 `x,y,owner_pid,title`（v1 的 16 字节 body 仍可解码，空标题+级联）、
LIST/LIST_REPLY（44 字节/条：id/owner_pid/坐标/尺寸/focused/title）、
REGISTER_HOTKEY、WM_STATUS（计数由 displayd 汇总进已有的
`/run/display.status`，不新建 ramfs 节点）。`/bin/demo` 的 CREATE 带
`title=Demo` 并新增常驻 `demo window` 角色（不抢焦点）。run-ng 用 QMP 注入
`tab`/`1`，并做第二次 `screendump` 断言 `(4,4)=#202838`（面板）与
`(217,177)=#3070c0`（Demo 窗口）；kill WM 后 Demo 仍活着、displayd 继续服务
（`ng: demo survives wm ok`）。证据见 `docs/claims/ng-WP3-2.md`。

**WP3-3 完成**（session manager，L2c + display 协议 v3）。L2 新增
`src/desktop/session/sessiond.c`：ring3 `/bin/sessiond` 是**应用生命周期的
唯一入口** —— init 只 spawn sessiond，sessiond 先 spawn displayd（先
`unlink /run/display.port` 丢掉上一轮的死 ticket，再用 `LIST/LIST_REPLY`
握手确认）再 spawn wm（`session=<ticket>`、`token=<WM role_token>` 经 argv
传递，不新建 ramfs 发现文件）。WM 的启动器不再 spawn：`1..9` 发
`LAUNCH(token,path)`，sessiond 校验角色后 `spawn` 应用并签发 32 位
role_token（`seq|role|nonce`），登记 `{pid,token,role,surface,title}`；
应用 `REGISTER` 窗口后由 sessiond 发 FOCUS（新窗口获得焦点），每 200ms 与
displayd 的 `LIST` 同步注册表（id/owner_pid/token 三字段）。EXIT/KILL/
心跳超时（800ms）回收窗口与进程；kill WM 后应用保活，sessiond 按
`restart_limit=3` + 指数退避（20/40/80ms，TSC tick）重启 WM 并让新面板
回来。display 协议升到 **v3**（只追加）：CREATE 加 `token`（44 B）、
LIST_REPLY entry 加 `token`（48 B）、FOCUS 的 reserved 变成 `token`
（displayd 校验 surface 令牌或 WM 令牌）、新增 SESSION_STATUS（会话状态汇总
进已有的 `/run/display.status` 的 `s*` 字段，**不新建 ramfs 节点**）。
内核镜像布局调整：7 个用户 ELF 的 `.incbin` 从 `.rodata` 挪到 2 MiB 之后的
`.bigdata` 只读段（PD[1] 恒等大页 + PMM 保留），`__kernel_end=0x1E3000`
（2 MiB 内留 ~118 KB）。自检：`ng: sessiond up` / `ng: session app launch ok`
/ `ng: session registry ok` / `ng: role token ok` /
`ng: session survives wm ok (app=.. alive=1 ...)` /
`ng: session wm restart seen ok` / `ng: WP3-3 selftest PASS`，第三张 QMP
截图断言 WM 重启后 `(4,4)=#202838` 与 `(217,177)=#3070c0`；host 单测
4684 → **5025**（+341）；`ci-ng` 7/7、parity 100/100、旧 32 位 305/305。
证据见 `docs/claims/ng-WP3-3.md`，设计见 `docs/design/session-ng.md`。
下一步：WP3-4 SDK alpha + demo app（外部可编译 GUI app）、WP3-5 S1 故障
注入（杀 shell/输入、应用存活、面板回来）。

**WP3-4 完成**（SDK v3 + 会话通道 + GUI demo，2026-10-31）。`sdk/` 从
alpha 骨架升到 **display 协议 v3 + session 通道**：

- `sdk/include/aster/display.h`：发送 version 3（CREATE body 44 带
  `token`、FOCUS 第二字=token、LIST_REPLY entry 48）；解码器接受 v1/v2/v3
  （v1 CREATE 16、v2 CREATE 40/LIST_REPLY 360，token=0）；`CREATE_REPLY`
  之外的 v3 `SESSION_STATUS`（68 B）也进 SDK；`aster_display_focus_ex()`；
- `sdk/include/aster/session.h`（新）：镜像会话协议 v1（REGISTER/
  HEARTBEAT/LAUNCH/EXIT/KILL/LOOKUP/ENTRY/LIST/LIST_REPLY/STATUS），
  `aster_session_connect(&s, argc, argv)` 解析 `session=<ticket>` /
  `token=0x..`（无会话返回 `-ENOENT`），**单独建会话回复端口**；
  `session_register/heartbeat/exit/launch/kill/lookup/list`、role_token
  助手、严格溢出检查的 argv 解析；
- `sdk/include/aster/app.h`：`cfg.token` / `app.token`、`aster_app_focus()`
  （v3 令牌 FOCUS）；
- `sdk/examples/hello_gui`：v3 CREATE/FOCUS + 会话 REGISTER/HEARTBEAT/EXIT、
  用 display `LIST` 回读 token（闭合 sessiond→argv→displayd 链路），打印
  `ng: sdk session ok` / `ng: sdk window ok` / `ng: sdk token ok` /
  `ng: sdk demo ok`；保留 `window`（常驻）与 `hold`（无 surface）两个
  demo harness 角色，配合内核自检；
- `tools/build-ng.ps1 -SdkDemo`：用 SDK 示例替换 `/bin/demo` 的源文件
  （符号 `ng_demo_elf` 不变，不新增 `/bin` 节点、不动 proc.c），默认构建
  完全不变；`tools/sdk-demo.ps1` 一条命令构建 + QEMU + QMP 注入，由
  **init→sessiond→WM LAUNCH→sessiond spawn** 真正拉起 SDK 应用，断言会话/
  窗口/令牌行与 `title=Aster SDK` 的 WM 任务行，并 QMP `stop` 后
  screendump 断言面板 `(4,4)=#202838` + 窗口 `(x+40,y+80)=#3070c0`；
- `tools/sdk-check.ps1`：语法门禁之外，用宿主 clang 编译运行
  `tests/unit-ng/test_sdk.c`（237 checks：v3/v1/v2 编解码、token、会话协议、
  argv 解析、拒绝路径），要求 ≥30 checks；
- 证据：`sdk-demo` PASS（4.4s，session=1048576 token=0x……，窗口 (192,152)，
  像素 #3070c0）、`sdk-check` PASS（9 files + 237 checks）、host 单测
  5034/0、parity 100/100（host 95/95）、run-ng 159+75、旧 32 位 305/305。
  证据与遗留见 `docs/claims/ng-WP3-4.md`，文档见
  `docs/sdk/getting-started.md`；SDK 会话客户端边界补在
  `docs/design/session-ng.md` 第 11 节。

下一步：WP3-5 S1 故障注入（杀 shell/输入、应用存活、面板回来）。

### E4

| ID | 内容 | 主要路径 | 出口 |
|---|---|---|---|
| WP4-1 | ABI 1.0 冻结文档 | docs/abi | 版本/句柄/错误码定稿 |
| WP4-2 | 外部开发者测试 | docs/、sdk/ | 一天内写出 GUI app |
| WP4-3 | D0 文档 + 发布 | docs/、dist/ | D0 可交付 |
| WP4-4 | 1h smoke（D1 再 8h） | tools/ | 无 panic/停滞 |

E4 进度（2026-10-31）：**WP4-1 与 WP4-3 完成；E4 未整体关闭**。

- **WP4-1 完成（ABI 1.0 冻结，v1.0.0）**：`docs/abi/abi-1.0.h` 机器可读头
  （29 个调用号、28 个错误码、64 个关键常量、4 个结构体大小）+
  `docs/abi/abi-1.0.md` 定稿（调用约定、四类命名空间、错误码、上限、版本
  策略）；草案 §10 的 13 项加冻结审计 2 项全部给出“保留/延后/已解决”结论。
  `tools/abi-check.ps1` 对 ABI 头、`sdk/include/aster/syscall.h`、
  `src/ng/syscall.h` 逐号比对，并核对错误码/内核常量/结构体布局；PASS。
- **WP4-3 完成（D0 本地发布包）**：`tools/release-ng.ps1` 一条命令跑
  `abi-check` + `build-ng`，生成 `dist-ng/release/d0/`（`aster-ng.elf`、
  `disk.img`、8 个 SDK 头、ABI 1.0、文档、`RELEASE-NOTES.md`、
  `SHA256SUMS`）并逐文件 SHA-256 自检；只本地，不 push/签名/打 tag。
  说明见 `docs/release/d0.md`，证据见 `docs/claims/ng-WP4-3.md`。
- **未完成**：WP4-2（外部开发者测试，一天内写出 GUI app）与 WP4-4（1 小时
  smoke，无 panic/停滞）不在 WP4-1/WP4-3 范围，尚无证据；D0 总出口按
  `architecture.md` §10 仍待这两项收口。E4 状态：**进行中（2/4 WP 完成）**。
- 冻结后仍保留的已知缺口（不阻塞 D0，D1 前处理）：未分配号 `ENOSYS`、
  IPC 时间单位、cap/revoke、每进程 uid/fd、用户态 fault 隔离、运行时 ABI
  版本查询、`SHM_FREE`、结构体对齐校验；详见 `docs/abi/abi-1.0.md` §10。

## 3. 第一周 checklist

1. git tag v0.75-32bit-final 69ab006；main 只修致命 bug。
2. git switch -c ng。
3. 建目录：src/kernel、src/services、src/drivers、src/desktop、src/apps、sdk、tools/ci。
4. 写 docs/design/domains/*.md（每域一页：职责/接口/失败/manifest）。
5. check-domains 骨架 + CI 接入。
6. 64 位工具链探针（x86_64-elf-gcc 或等价）。
7. boot spike：multiboot2 + long mode + 串口 hello。
8. 每天一个 demo；周五评审 E1 进度。

## 4. 门禁

- 每个 WP：build + 相关 host 单测 + claims；没有 claims 不算完成。
- ng 门禁（E1 起）：`tools/ci-ng.ps1` = check-domains + check-deps + unit-ng +
  build-ng + run-ng + 旧 32 位 `build.ps1 -Test`（`-Quick` 跳 QEMU/legacy）；
  CI 侧 `.github/workflows/ng.yml`。
- check-domains：向上依赖必须失败。
- parity：E2 结束到 **100 项**（`tools/parity-ng.ps1`；内核自检 + host 同表），
  E3 起随 display server/WM/session 增加新项，100 是 E2 基线而非上限。
- 门禁顺序：`ci-ng` 的 parity 步骤复用 run-ng 的串口日志，不重复启动 QEMU
  （`tools/parity-ng.ps1 -LogFile <run-ng 日志>`）。
- 故障注入：E3 起杀 shell/compositor/inputd，按 architecture.md 第 3 节断言。
- 8h 浸泡：E4 之前 1h smoke；D1 出口 8h。
- 文档：每个 WP 必须更新对应 domain 文档或 decisions.md。

## 5. 分支与提交

- main：冻结；只接受致命 bugfix。
- ng：唯一开发分支；每阶段打 tag（ng-e1、ng-e2…）。
- 提交按路径限定，不用 git add -A。
- claims 放 docs/claims/ng-<WP>.md，写清期望的串口/测试行。

## 6. 风险与回退

| 风险 | 应对 |
|---|---|
| E1 超时（页表/启动调不通） | 6 周硬停；评估改用现成 boot 协议/减少范围 |
| FS/驱动移植踩 32 位假设 | parity 测试先行；保留旧树 oracle |
| L2 范围膨胀（S2/S3 提前） | D0 只做 S1；S2/S3 进 D1 |
| SDK/POSIX 分叉 | ABI 1.0 在 64 位后冻结；POSIX 用户态 |
| 多会话冲突 | 路径所有权；一个 WP 一个会话 |
| 动力/演示缺失 | 每天 demo；阶段 tag 可回退 |

## 7. 与旧树的关系

- 旧树（69ab006）是**行为 oracle**：305 项检查、FS/网络/驱动语义都可对照。
- 旧树不删、不更新功能；只修致命 bug。
- 新树每移植一个模块，从旧测试里挑对应用例进 parity 套件。
