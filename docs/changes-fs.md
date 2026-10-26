# fs 会话改动记录

> 记录本会话对 `D:\OS_Project` 的全部改动，便于与其他并行会话对齐。
> 时间范围：2026-10-24 00:30 ~ 11:50（Asia/Shanghai）。

---

## 1. 已提交（按时间顺序）

| 提交 | 内容 | 验证 |
|---|---|---|
| `8cb1413` | 建立 git 仓库、`.gitignore`（排除 `tools/` 工具链、`dist/`、`obj/`、日志）、初始提交 140 文件、tag `v0.44.0` | `git count-objects` 397 KiB |
| `c3bcc0f` | 文档漂移修正：README「单核」「没有用户态」、security-model「无 TLB shootdown」「无 capability 表」、system-model「不支持 SMP」 | 人工核对 |
| `c13cb84` | 调度器重写：per-CPU runqueue、按负载放置（`sched_place`）、迁移（`sched_balance`）、IPI kick；删除 `ap_safe()` 名字白名单；`PROG_F_PIN_AP` 亲和标志 | `-Test` 67/67 |
| `eab33f9` | 对象表、管道加锁（SMP 安全） | 68/68 |
| `46f5fdc` | ramfs 公开入口统一 `fs_lock`（宏改名 + 尾部包装，避免多 return 漏解锁） | 69/69 |
| `768920b` | `docs/driver-service.md`：用户态驱动服务模型（消息格式、D0–D5 计划）+ e1000 RX 诊断附录 | — |
| `a288d72` | **网络修复**：打开 PCI CMD 的 MEMORY+BUS MASTER（此前 TX 的 `TDH` 会前进但帧不上线）；`net_init` 轮询窗口放大；ARP/ICMP 进回归 | 实测抓到线上帧 + `net: ARP reply` / `ICMP echo reply` |
| `88eed28` | UDP 收发（`net_udp_send`/`net_parse_udp`）+ `tools/net_peer_test.ps1`（socket 后端扮演对端，确定性验证 ARP/ICMP/UDP）；`.gitignore` 放行 `tools/*.ps1` | `NET PEER PASS` |
| `3f66220` | 补提交 `tools/{gensyms,mkfat16,soak}.ps1`（此前被 `/tools/` 全忽略，新克隆无法构建） | — |
| `0aa6566` | `tools/stress.ps1`：文件串口长稳脚本（崩溃/挂死检测、定时注入、进度），`soak.ps1` 增加 `-Smp` | 3 分钟冒烟 |
| `4840f2c` | README 记录 8 小时长稳结果与 `stress.ps1` 用法 | 见 §3 |
| `8458ec0` | **VFS 后端路由**：`fs_*_ex` 统一入口，`syscall.c` 删除 8 处 `FD_KIND_FAT16` 分支；`proc_fd_t.fs_be`；FAT 句柄拒绝 dup/fork 共享（修掉潜在双重关闭）；测试改为等 `ushell: ready` 再注入 | 86/86（当时基线） |
| `8e72a57` | 断开 `fs → proc`：`ramfs.c` 不再 include `proc.h`，uid/gid 由调用方传入（`fs_open_as`/`mkdir_as`/`unlink_as`/`chown_as`） | 83/85（另 2 条为在飞 UDP 断言） |
| `5d13736` | 后端改为 `fs_backend_t` ops 表 + `probe` 路由；ramfs/FAT16 各注册一张表，`vfs.c` 只按表分派 | 启动日志两条 fs 路径均绿 |
| `5336c4e` | `fs/vfs_selftest.c`：后端一致性自检（11 步），`kernel.c` 挂接 | 私有启动：`fs: backend selftest ok` |

## 2. 未提交（本会话的改动，留在工作区）

| 文件 | 改动 | 说明 |
|---|---|---|
| `build.ps1` | ① 启动等待改为轮询 `ushell: ready`（上限 60s）；② 新增断言 `VFS 后端一致性` | 与其它会话的改动同文件，为避免 `git add -A` 卷入他人改动，未提交 |

## 3. 关键验证证据

**8 小时长稳（2026-10-24 01:45–09:45，smp=2）**

```
stress: done ran=480min injected=478 hb=28548
KERNEL PANIC=0  CPU/user exception=0  vmm: WARN=0  OOM=0  CRASH.txt=未生成
```

**网络（PCI bus master 修复后）**

```
e1000: PCI CMD=107 (memory=1 busmaster=1)
net: ARP reply gw=52:55:0a:00:02:02
net: ICMP echo reply seq=1
net: UDP echo reply
```

**VFS 后端一致性（私有 QEMU，独立串口）**

```
fs: backend selftest ok
```

## 4. 已知问题 / 未完成

1. **`build.ps1 -Test` 在有并发会话时会出现大面积假失败**：`-serial file:` 带缓冲，且 QMP 固定端口 4444、日志固定 `serial.log`；两个会话同时跑时，quit 可能连到别人的 QEMU，我这份被强杀导致尾部丢失。必须按 `docs/parallel-sessions.md` §3 串行。本轮最后一次全量跑报了 66 红，但事后 `serial.log` 里这些"失败项"全部有输出（含 `fs: backend selftest ok`、`sectest: udp ok`、`chroot ok`）。
2. **TCP 探测（`net_tcp_echo_test`）**：guest 的 SYN/数据能到宿主（实测宿主收到 `tcp` 并回显），但 guest 收不到回程；同时 3×300 万 + 600 万次 `rx_poll` 让启动多花几十秒。属 `net.c`，未改。
3. **`dev_ops` 未做**：需要改 `device.c` / `e1000.c` / `net.c`，后两者当前归 UDP 子代理。
4. 最后一次手工验证中 QEMU 在注入 `echo hi` 时自行退出（`-no-reboot` 下通常意味着三击/复位），日志停在 heartbeat 12，未定位；下一次串行窗口需要复现确认。

## 5. 与其他会话的边界

- 本会话只写 `src/kernel/fs/*`、`docs/*`（新增文档）、`tools/*.ps1`；
- 共享文件（`kernel.c`、`build.ps1`、`syscall.c`、`process.c`、`proc.h`）每次改动都在会话消息里声明，且只提交明确路径；
- 自 11:16 起遵循 `docs/parallel-sessions.md`：`[fs]` 前缀、禁止 `git add -A`、构建前确认无 QEMU 且对方不在构建。

---

## 6. 更新（12:05）：回归基线恢复

`build.ps1` 的收尾逻辑改成**按串口静默判完成**，全量回归回到全绿：

```
=== TEST PASS (89/89) ===
```

改动（提交 `b9df248 [test]`）：

1. 注入命令后不再等固定秒数/固定标志，而是每 300ms 看 `serial.log` 长度，
   **连续 3 秒不增长**即认为命令执行完（上限 90 秒）；
2. `QMP quit` 之后**等进程真正退出（最多 5 秒）**再读日志，避免
   `-serial file:` 缓冲尾部丢失导致的“假失败”。

该提交只动 `build.ps1`，但文件里同时含有并行会话的未提交改动
（`-Cpu qemu32,+nx,+smep,+smap`、UDP/TCP echo job），已在会话中声明。

---

## 7. 更新（21:20）：rename / move（`SYS_RENAME = 58`）

本批把 FS 从"能演示"往"能用"推进的第一步：**同卷内改名/移动**。
`link`/`symlink` 和 append 打开模式本批未做（见 §7.5）。

### 7.1 语义（`fs_rename_as` / `fs_rename`）

| 场景 | 结果 |
|---|---|
| 同卷改名/移动（ramfs） | 改节点 `name` + `parent`，文件内容/大小不变 |
| 跨卷（`system:` → `data:` / `fat:`） | `E_INVAL`（VFS 路由到不同后端即拒） |
| 源不存在 / 父目录不存在 | `E_NOENT` |
| 源或目标父目录缺 `w+x` | `E_ACCES`；`uid == 0` 绕过 |
| 目标是文件、源也是文件（ramfs） | 允许替换；目标仍被打开则 `E_BUSY` |
| 目标是空目录、源也是目录（ramfs） | 允许替换 |
| 目标是非空目录 | `E_EXIST` |
| 源/目标类型不一致（文件顶目录或反过来） | `E_EXIST` |
| 目录移动到自己的子树 | `E_INVAL`（防止父链成环） |
| 源文件被打开时改名 | 允许（ramfs 句柄指向节点，不依赖名字） |
| FAT16 根目录 8.3 改名 | 读目录项 → 改 11 字节短名 → 回写扇区 |
| FAT16 跨目录 / 长名 / LFN | `E_INVAL` |
| FAT16 目标已存在 | `E_EXIST`（不做替换：避免释放目标簇链的多步回滚） |
| svcfs / blkfs | 只读后端，`E_INVAL` |

约定与限制：

1. `new_path` 必须是**完整目标路径**：不实现 `mv a.txt dir/` 的"移入目录"语义
   （shell 的 `mv` 只是 rename 的薄包装，`mv <old> <new>`）。
2. 文件 `mtime` 不变（内容没改）；源/目标父目录的 `mtime` 更新为当前 RTC。
3. 权限检查在 VFS 层做（父目录 `w+x`，root 绕过），ramfs 后端再按节点判一次
   （防御性重复）；FAT16 没有 `mode/uid/gid`，跳过判权。
4. FAT16 目录项位置 `(dir_lba, dir_idx)` 不变，所以**打开中的 FAT16 文件改名是
   安全的**：close 仍按原槽位回写簇号/大小，不依赖名字。
5. 系统调用层把失败统一压成 `-1`（和 open/unlink 一致）；内核自检直接看 `E_*`。

### 7.2 改动文件

| 文件 | 内容 |
|---|---|
| `src/kernel/fs/fs.h` | `fs_backend_t.rename` op；`fs_rename()` / `fs_rename_as()` 声明 |
| `src/kernel/fs/vfs.c` | 双路径路由、同后端/跨卷校验、父目录 `w+x` 判权、`fs_rename*` 实现 |
| `src/kernel/fs/ramfs.c` | `ramfs_rename_impl()`（覆盖策略/防环/权限）+ `ramfs_rename_locked()`（持 `fs_lock`）+ 适配器 |
| `src/kernel/fs/fat16.c/.h` | `fat16_rename()`：根目录 8.3 改名；适配器 |
| `src/kernel/fs/svcfs.c`、`blkfs.c` | 只读后端 `rename` stub 返回 `E_INVAL` |
| `src/kernel/proc/trap/syscall.h` | `SYS_RENAME 58`（55 已是 `SYS_CHROOT`，56/57 已用，**没有**占用 55） |
| `src/kernel/fs/fs_syscall.c/.h` | `sys_rename_user()`：两条路径都 `make_abs_path`（相对路径按 cwd） |
| `src/user/ulib.h` | `SYS_RENAME` + `fs_rename()` 包装 |
| `src/user/user_shell.c` | `mv <old> <new>`（含 help） |
| `src/kernel/fs/fs_rename_selftest.c` | 新自检；由 `fs_backend_selftest()` 末尾调用 |

### 7.3 验证证据

手工（QEMU 串口注入；`fat.img` 由 `tools/mkfat16.ps1` 生成 `HOST.TXT`）：

```
ushell> write system:/a.txt hello
ok
ushell> mv system:/a.txt system:/b.txt
ok
ushell> ls system:
  b.txt
ushell> cat system:/b.txt
hello
ushell> mv fat:/host.txt fat:/host2.txt
ok
ushell> ls fat:
  HOST2.TXT
ushell> cat fat:/host2.txt
hello from host
```

宿主侧再用 `tools/mkfat16.ps1 -Extract HOST2.TXT` 从 `fat.img` 读回
`hello from host`，`HOST.TXT` 已不存在 —— 说明改名真正落到了盘上，不只是内存缓存。

内核自检（每次启动都会跑）：

```
fs: error-path selftest ok
selftest: rename ok
fs: backend selftest ok
```

回归：见 §7.4。

### 7.4 回归

代码提交 `ae20490` 后，连续 2 次（同一份 `build.ps1`、串行拿 test lock）：

```
[session-run] fsfeat: obj=.obj-fsfeat dist=.dist-fsfeat qmp=4511 serial=serial-fsfeat.log test=True
[session-run] exit=0 elapsed=14.7s === TEST PASS (97/97) ===
[session-run] fsfeat: ... test=True
[session-run] exit=0 elapsed=14.4s === TEST PASS (97/97) ===
```

两次串口里都有：

```
fs: error-path selftest ok
selftest: rename ok
fs: backend selftest ok
```

注意：跑回归时工作区里还有其它会话未提交的 `build.ps1`/`proc`/`net` 改动；
97 项断言数与当时基线一致（`-Test` 的检查表由 `build.ps1` 定义，我没改它，
后续由 FS 会话负责人把 `selftest: rename ok` 加成第 98 项）。

### 7.5 本批未做

- `link` / `symlink`：需要节点引用计数/路径解析，改动面大于 rename，留到下一批；
- append 打开模式（`FS_OPEN_APPEND`）：`fs_open` 目前总是 offset=0，FAT16 的
  `FS_OPEN_WRITE|CREAT` 还会截断；加标志要同时改 ramfs/FAT 与 `sys_open_user`，
  本批未动；
- 跨卷 rename（"copy + unlink" 语义）和 FAT16 目标替换：刻意不做，先保证语义清晰。

## 8. 更新（阶段 4a）：单文件 64KB + XFS1 v5

- `FS_FILE_MAX`：40960 → **65536**（任务卡写的 32768 是更早的基线，仓库当时已是
  40960；这次直接到 64KB）。`FS_FILE_MAX_LEGACY = 40960` 留给旧镜像。
- `FS_DISK_VERSION = 5`：
  - super 增加 `max_file` / `data_bytes`，并用 `pad[340]` 撑满 512 字节
    （之前 `blk_write(...,&super)` 只写了 164 字节结构体，块层却读 512 字节，
    属于越界读；现在是完整扇区）；
  - 挂载接受 **v4 与 v5**：v4 没有新字段，按 40960 限流并打印
    `fs: XFS1 v4 legacy image mounted ...`，下一次 `fs_sync` 自动写 v5（数据区
    布局未变，不需要迁移数据）；v5 的 `max_file` 必须在 [40960, 65536]；
    其它版本直接拒绝；
  - 挂载增加 inode 越界校验：`size > 当前限额`、`data_lba + 扇区数 > 设备容量`
    都会让整次挂载失败（之前会按 size 直接 memcpy 进 64KB 的 `n->data`，损坏
    镜像可以打穿节点）。
- ramfs 节点数据 64KB → `nodes[64]` 的 `.bss` 从 2.6MB 涨到 4MB，`_kernel_end`
  从 0x83ac64 抬到 0x9e1f04；`heap_init` 的 "arena 抬到镜像之后" 机制自动把它
  挪到 0x9e2000（仍剩 14.4MB 可用），不需要改 `HEAP_BASE`。
- 内核 shell 的 `file write/read` 缓冲从栈上移到 `static`（64KB×2 放栈上会踩栈）。
- 回归：`fs: XFS1 v5 mounted, file limit 65536 bytes` 与
  `fs: 64KB file limit ok (wrote 65536 bytes, one-past rejected)`，
  `build.ps1 -Test` 158/158。
- 本批未做：FAT16 单文件上限（仍是簇链限制）、append 模式、跨卷 rename。
