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
