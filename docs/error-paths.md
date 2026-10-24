# 错误路径审计（error paths / 失败路径完整性）

> 会话：errap（独立会话，与 `a58f7dd5` 保留位缺页、`a97aa5f8` syscall 拆分、`06b111e0` 偶发失败并行）
> 基线：`main` @ `94df011`，版本 0.71.0，回归 97 项
> 范围：`fs/{ramfs,fat16,pipe,vfs,blkfs,svcfs}.*`、`mm/{pmm,heap,object,memtest}.c`、
> `dev/device.c`、`dev/block/{block,ata}.c`、`svc/{service,channel,supervisor}.*`、
> `ipc/port.c`、`proc/process.c`（只读审计，文件被并行会话占用）
> 方法：逐函数阅读 + 全仓 70 个 `.c` 的调用点扫描（函数名匹配 91 处命中），
> 重点看“返回 `E_*`/int 但调用点丢弃”“失败后不回滚”“0 既是失败又是合法值”。

## 0. 统计

| 项 | 数值 |
|---|---|
| 精读文件 | 31 个（≈ 6300 行） |
| 调用点扫描 | 70 个 `.c`，91 处命中（`fs_*` / `blk_*` / `pmm_*` / `malloc` / `service_*` / `port_*` / `chan_*` / `pipe_*`） |
| 发现问题 | 见 §1/§2 表格，共 **66** 条 |
| 已修复 | **22** 条（P0 2 条、P1 17 条、P2 3 条） |
| 未修复（给出建议） | 44 条（多数需要改跨模块语义/被并行会话占用的文件） |

分布（已修复 / 合计）：

| 模块 | 已修 | 合计 | 主要风险 |
|---|---|---|---|
| fs/ramfs.c | 6 | 12 | 截断写、删打开中的文件、半挂载、几何未校验 |
| fs/fat16.c | 8 | 13 | 挂载失败污染状态、簇泄漏、close 泄漏槽位、错误当“不存在” |
| fs/pipe.c | 1 | 2 | 槽复用后读写串台；无读者时写端永久阻塞 |
| fs/vfs.c | 0 | 2 | 未知后端 id 默认落到 ramfs；错误原因丢失 |
| fs/blkfs.c + svcfs.c | 2 | 5 | close 丢错误且泄漏槽位；>63 字节静默截断 |
| mm/pmm.c | 1 | 4 | 重复初始化残留保留页数；账面空闲数偏大 |
| mm/heap.c | 2 | 4 | `size+3` 回绕写穿堆；野指针 free 破坏无关内存 |
| mm/object.c | 0 | 2 | `obj_deref` 返回裸指针不持引用（并发 close → UAF） |
| dev/device.c | 0 | 2 | `dev_init` 忽略注册失败；`dev_open` 的 0 语义混用 |
| dev/block/block.c | 2 | 4 | 边界检查缺失、`blk_register` 半注册、缓存无锁 |
| dev/block/ata.c | 0 | 3 | 写命令不查 ERR 位；初始化无回滚 |
| svc/service.c | 0 | 4 | 服务重启后同名注册永久 `E_EXIST`；按 port 误删多条 |
| svc/supervisor.c | 0 | 2 | 注册/回复失败被丢弃 |
| ipc/port.c | 0 | 2 | pending channel 引用计数依赖调用方；accept 无超时 |
| proc/process.c | 0 | 3 | fork 失败路径泄漏 fs 引用/管道写端计数（**未修：文件归并行会话**） |
| proc/trap/syscall_util.c | 0 | 2 | 路径截断不报错；错误码无区分 |

优先级定义：**P0** 会造成数据损坏/越界/跨对象串数据；**P1** 会造成资源泄漏、半初始化、
静默失败；**P2** 是健壮性/可诊断性问题，正常路径无影响；**P3** 是注释/一致性。

---

## 1. 已修复（本次改动）

所有修复都保持正常路径行为不变，只补失败路径。验证见 §4。

### fs/ramfs.c

| ID | 风险点 | 后果 | 修复 |
|---|---|---|---|
| R1 (P0) | `fs_write` 超过 `FS_FILE_MAX` 时把 `len` 截断后照写，文件满时返回 **0** | 文件停在半截；`0` 与“成功写 0 字节”无法区分；调用点普遍用 `返回值 == len` 判成功，于是静默丢数据 | 先算 `space`，`len > space` 直接 `E_NOSPC`，**一个字节都不写**（原内容完整保留） |
| R2 (P0) | `fs_unlink` 不检查是否有打开的句柄，直接 `memset(node)` | 句柄仍指向已清空槽位；`node_alloc()` 把同一槽位复用给新文件后，旧句柄读写到**别人的数据**（UAF 类） | 新增 `node_is_open()`，打开中返回 `E_BUSY`（新错误码，`core/error.h`） |
| R3 (P1) | `fs_read` 用 `size - offset` 计算可读长度，无 `offset > size` 校验 | 节点被截断/回收后无符号下溢成巨大值，`memcpy` 越界读 | 先判 `offset > size` 返回 `E_INVAL` |
| R4 (P1) | `fs_mount_disk` 先清空内存树再逐条建节点，中途失败直接 return | 留下“半个文件树”（卷根有效、子节点缺失、by_index 悬空父指针）；之后 open/stat 当作正常卷用 | 新增 `mount_disk_fail()`：任何失败都把 nodes/volumes/handles 清回**自洽空状态**；`node_alloc` 失败仍返回 `E_FULL` |
| R5 (P1) | 未校验超级块几何字段（`inode_lba`/`data_lba`）就按它读盘 | 损坏/伪造的镜像会把挂载过程带进越界读（块层拒绝后已清空内存树） | 在“还没动内存状态”时校验：设备容量、`inode_lba == DISK_INODE_LBA`、inode 表不越界、`DISK_DATA_LBA <= data_lba < sector_count` |
| R6 (P2) | `fs_format_disk`/`fs_sync` 不检查设备是否放得下 inode 表 | 小设备上格式写到一半失败，错误码还是 `E_INVAL` | 入口检查 `sector_count < DISK_DATA_LBA + 1` → `E_NOSPC` |

### fs/fat16.c

| ID | 风险点 | 后果 | 修复 |
|---|---|---|---|
| F1 (P1) | `fat16_mount` 先把 BPB 写进全局 `fat.*` 再校验；失败只清 `fat.dev`（有的分支连这个都不清） | 挂载失败后 `mounted` 仍是旧值、几何字段是新盘/半解析值；**重新挂载失败会把原本可用的挂载点弄坏**，后续读扇区算错 LBA | 改为局部变量解析 → 全部校验（含 `total_sectors <= dev->sector_count`）→ 通过后才提交；失败完全不改全局状态 |
| F2 (P1) | `cluster_alloc` 先占 FAT 项再清零簇，清零写失败直接 `return 0` | FAT 里留下“已占用、无人引用”的永久簇泄漏 | 失败时 `fat_set(c, 0)` 回滚 |
| F3 (P1) | `fat16_write` 挂链 `fat_set` 失败泄漏刚分配的簇；磁盘满且 0 字节写出时返回 **0** | 簇泄漏；`0` 被当成“成功写了 0 字节” | 失败回滚新簇；`done == 0` 时返回 `-1`，已写部分返回短计数（POSIX 语义） |
| F4 (P1) | `fat16_close` 回写目录项失败时直接 `return -1`，不清句柄槽 | 该 fd 槽永久 `used`，反复失败会耗尽 8 个 FAT16 句柄 | 无论持久化成败都 `memset(fh)` 释放槽位，返回值仍报错 |
| F5 (P1) | `fat16_unlink` 先释放簇链再写目录项 | 写目录项失败 → 目录项仍有效但簇已释放，原文件读出来是垃圾（半删除） | 先让目录项 `0xE5` 落盘，再回收簇链 |
| F6 (P1) | `fat16_open` 把 `find_root_entry` 的读盘错误（-1）当成“文件不存在”，走新建分支 | 瞬时 I/O 错误会**重复创建目录项**（或把已有文件当新文件） | 区分 `0`（存在）/`E_NOENT`（不存在）/其它（I/O 错，关闭句柄返回 -1） |
| F7 (P1) | `fat16_unlink` 不检查打开中的句柄就回收簇链 | 读句柄仍按旧 `first_cluster` 读，读到已被别的文件复用的簇（数据串台） | 匹配 `dir_lba/dir_idx` 的打开句柄存在时返回 `E_BUSY` |
| F8 (P2) | `fat16_listdir` 读盘失败返回 `E_NOENT` | 调用方无法区分“目录没有第 N 项”和“盘读失败” | 改返回 `E_INVAL` |

### 其它已修

| ID | 文件 | 风险点 | 后果 | 修复 |
|---|---|---|---|---|
| P1 (P1) | fs/pipe.c | `pipe_read`/`pipe_write` 只校验下标范围，不校验 `used` | 管道槽被回收并复用后，旧 fd 会读到/写到**另一条管道** | 在锁内检查 `pp->used`，失效返回 -1 |
| B1 (P1) | fs/blkfs.c, fs/svcfs.c | `close` 用 `(void)chan_send/chan_recv_block` 丢弃结果 | 命令只发一半或服务端没确认，close 仍返回 0，脏数据静默丢弃 | 校验 send 长度与 recv 结果，失败向上报错 |
| B2 (P1) | fs/blkfs.c, fs/svcfs.c | `close` 在服务连接失败时直接 `return -1`，不释放 `handles[].used` | 失败一次永久少一个 `blk:`/`svc:` 句柄（各只有 8 个） | 统一从清理出口返回，槽位一定释放 |
| H1 (P1) | mm/heap.c | `malloc` 的 `(size + 3) & ~3` 在 `size` 接近 `UINT_MAX` 时回绕成小值 | 后续按小 size 分裂块，堆被写穿 | 先拒绝 `size > HEAP_SIZE - HEADER_SIZE` |
| H2 (P1) | mm/heap.c | `free` 不校验指针范围就先读 `block->magic` | 野指针命中可读内存里的伪魔数时，把无关数据当块头改坏 | 先校验 `[HEAP_BASE, HEAP_BASE+HEAP_SIZE)` 与 4 字节对齐，再解引用 |
| K1 (P1) | dev/block/block.c | `blk_read`/`blk_write` 注释称“带边界检查”，实际只判空指针 | 是否越界完全依赖各驱动自觉；新驱动漏检就 OOB | 入口校验 `sector_size == BLK_SECTOR` 且 `lba < sector_count` |
| K2 (P1) | dev/block/block.c | `blk_register` 先写块表、忽略 `dev_register` 返回值 | 设备表满时块层能读写但 `dev_open`/枚举看不到（半注册） | 先 `dev_register`（`E_EXIST` 放行），失败不占块槽位 |
| M1 (P2) | mm/pmm.c | `pmm_init` 早退分支不清 `pmm_bitmap_pages` | 重复初始化时旧保留页数被带进 `page_is_used()` | 早退时置 0 |

---

## 2. 未修复 / 建议（按优先级）

### P1

| ID | 位置 | 问题 | 建议 |
|---|---|---|---|
| C1 | `proc/process.c:293` `proc_fork` | 克隆用户页前已对子进程 fd 做 `fs_ref_ex` / `pipe_ref_write`；克隆失败只 `memset(child)`，**不加释放** | 失败分支逆序回滚：对已 ref 的 fd 调 `fs_close_ex`、对 `FD_KIND_PIPE_W` 调 `pipe_unref_write`。⚠ 文件被 `a97aa5f8`/其它会话占用，本会话未改，建议由其所有者合并 |
| P2 | `fs/pipe.c` | 写端在**所有读端已关闭**时永久阻塞（没有 readers 计数，`pipe_unref_read` 也不会唤醒写端） | 增 `readers` 计数：`pipe_create` 置 1，新增 `pipe_ref_read/unref_read` 由 `syscall.c` 的 pipe/fork/dup2 路径调用；`pipe_write` 在 `readers==0` 时返回 `E_BADH` |

### P2

| ID | 位置 | 问题 | 建议 |
|---|---|---|---|
| R7 | `fs/ramfs.c` `fs_open` | 失败统一返回 0（路径/权限/表满不可区分）；`0` 同时是“非法句柄” | 已有 `fs_open_ex` 用 -1 缓解；如需诊断可加 `fs_last_error()` |
| R8 | `fs/ramfs.c` `fs_open` | `FS_OPEN_CREAT` 时节点已建、`obj_create` 失败 → 文件创建成功但 open 失败 | 可接受（O_CREAT 语义）；文档化 |
| R9 | `fs/ramfs.c` `fs_sync` | 数据 → inode 表 → 超级块，中途失败只返回错误，盘上是混合状态 | 需要双超级块/序号或整盘双缓冲；短期建议先在空闲区写再切换超级块 |
| R10 | `fs/ramfs.c` `fs_init` | 忽略 `fs_mount("system")` 失败（只有注释） | 返回 `int` 或 panic；当前硬件下不会发生 |
| R11 | `fs/ramfs.c` `fs_check` | 不检测“句柄指向已释放节点”（R2 修好后不再产生） | 可加句柄表遍历校验 |
| F9 | `fs/fat16.c` `fat16_open` 写路径 | 带 `FS_OPEN_WRITE` 即截断（把 O_TRUNC 混进 write）；截断后 close 失败会丢旧数据 | 明确 `O_TRUNC` 标志；或在 close 失败时保留旧目录项备份 |
| F10 | `fs/fat16.c` `cluster_free_chain` | `fat_set` 失败被忽略 → 静默簇泄漏 | 返回失败计数，unlink 时向上报错 |
| F11 | `fs/fat16.c` 全局 | 整个后端无锁，且 `sec_buf` 是全局共享缓冲；SMP 下两个 CPU 同时读写会串数据 | 加 `fat_lock`（公开入口加锁，`*_locked` 内部实现），或改成每调用局部缓冲 |
| F12 | `fs/fat16.c` | 写入只在 close 时更新目录项大小/首簇；write 后崩溃则数据占用簇但目录项为 0 | 定期或在 `fat16_write` 末尾回写目录项（掉电一致性） |
| V1 | `fs/vfs.c` `be_from_id` | 未知 backend id 默认当 ramfs | 返回 0 / `E_BADH` 并让 `fs_*_ex` 拒绝 |
| V2 | `fs/vfs.c` `route` | 路由失败只返回 -1，丢失原因 | 统一返回 `E_NOENT/E_INVAL` |
| B3 | `fs/blkfs.c`, `fs/svcfs.c` | `write` 缓存上限 128，`close` 只回写 `min(len,63)` 字节 | 明确最大文件大小并在 `write` 超限时报 `E_NOSPC`；或分块发送 |
| B4 | `fs/svcfs.c` `svc_stat` | 每次 stat 都新建一次服务连接（重），失败只返回 -1 | 缓存服务句柄；stat 失败区分 `E_NOENT` |
| B5 | `fs/blkfs.c`, `fs/svcfs.c` `open/close` | 100 次 connect 重试 + 每次 `proc_sleep_ticks(1)`，最坏阻塞 100 tick | 改成有界等待并在超时后返回明确错误 |
| M2 | `mm/pmm.c` | `pmm.free_pages` 把 16MB 内核堆算作空闲（`page_is_used` 排除但账目没扣） | 初始化时把堆页计入已用；会影响 `memtest` 基线数值，需同步调整断言 |
| M3 | `mm/pmm.c` `free_pages_locked` | 保留区判断用 `addr + count*PAGE_SIZE`，大 count 可回绕 | 先用 `count > total_pages` 拒绝，再判断重叠 |
| M4 | `mm/pmm.c` `pmm_mark_range` | 把堆区页也标 used 并减 `free_pages`（与 M2 相互作用，可能双重计数） | 与 M2 一起修 |
| H3 | `mm/heap.c` `free` | 块头有效但传入块中间地址时仍会误判（对齐检查只挡住 4 字节粒度） | 遍历块链表确认 ptr 属于某块（慢路径，仅 DEBUG） |
| H4 | `mm/heap.c` `heap_stats` | 遇到坏魔数静默停止统计 | 返回损坏标志 |
| O1 | `mm/object.c` `obj_deref` | 返回 data 裸指针但不持引用；拿到指针后另一 CPU close → UAF | 提供 `obj_get/obj_put`，或让调用方在 `obj_lock` 内完成操作 |
| O2 | `mm/object.c` | 对象层不回收 data 指向的底层资源（句柄泄漏=资源泄漏） | 各子系统在 close 路径保证释放（本次已修 fat16/blkfs/svcfs 的槽位泄漏） |
| D1 | `dev/device.c` `dev_init` | 忽略 3 个 `dev_register` 返回值 | 失败打印告警；`dev_init` 返回错误数 |
| D2 | `dev/device.c` `dev_open` | 返回 0 同时表示“设备不存在”和“对象表满” | 改 `int dev_open_ex(...)` 或输出错误码指针 |
| K3 | `dev/block/block.c` `blk_init` | 忽略 `blk_register(&ram0)` 失败 | 返回错误并让 `bootseq` 记录 |
| K4 | `dev/block/block.c` 缓存 | 8 项全局缓存无锁，SMP 下 `valid/dev/lba` 可能被并发改 | 加 `blk_cache_lock`，或用 per-CPU 缓存 |
| A1 | `dev/block/ata.c` `ata_pio_write` | 写完只等 BSY 清零，不检查 `ATA_SR_ERR` | `wait_ready` 增加 ERR 检查（注意与 IDENTIFY 路径共用） |
| A2 | `dev/block/ata.c` `ata_init` | 探测到一半失败时已注册设备保留，返回值只表示“至少一个成功” | 返回每个设备的结果；失败设备 `present=false` 并确保未注册 |
| A3 | `dev/block/ata.c` `ata_probe` | 失败后 `c->dev` 字段残留（`present=false` 兜底） | 失败时清理 `dev.read/write/sector_count` |
| S1 | `svc/service.c` `service_register` | 服务重启后同名注册永远 `E_EXIST`；无过期/覆盖机制 | 注册时校验 port 是否仍存在；或提供 `service_register_force` |
| S2 | `svc/service.c` `service_unregister` | 按 port 删除所有同名项 | 按 (name,port) 删除，或保证一 port 一名字 |
| S3 | `svc/service.c` `service_connect` | 取到 port 后释放锁再 connect，port 可能已被释放（TOCTOU） | 在锁内 `port_retain`，connect 失败后 `port_release` |
| U1 | `svc/supervisor.c` `supervisor_init` | 忽略 `service_register` 失败 | 失败时释放 port 并打印；或让 supervisor 走无注册降级 |
| U2 | `svc/supervisor.c` `supervisor_entry` | `(void)chan_send` 丢弃回复发送失败 | 失败计数 + 日志；服务端不应假装回复成功 |
| I1 | `ipc/port.c` `port_connect` | pending channel 入队但不 `chan_retain`；释放责任隐含在 `port_release` | 文档化所有权，或入队时 retain |
| I2 | `ipc/port.c` `port_accept_block` | 无超时，port 被释放后可能永久阻塞 | 增加 `port_accept_timeout` |
| C2 | `proc/process.c` `proc_exit` | 忽略 `fs_close_ex`/`pipe_unref_*` 的失败后直接 `memset` | 后端 close 现在会自清槽位（本次修复），进程侧可只记日志 |
| C3 | `proc/trap/syscall_util.c` `fd_alloc_kind` | 不清理整个 fd 结构，依赖 close 时 memset | 分配时 `memset(&p->fds[i], 0, sizeof)` 再填 |
| Y1 | `proc/trap/syscall_util.c` `make_abs_path` | 超长路径静默截断 | 返回 `E_INVAL`（需要改 syscall 层，不在本会话范围） |
| Y2 | `proc/trap/syscall_util.c` `copy_user_string` | 越界和超长都返回 `E_INVAL` | 返回区分错误码 |

### P3

| ID | 位置 | 问题 |
|---|---|---|
| R12 | `fs/ramfs.c` `fs_check` | 报告缓冲截断时静默（`strncpy` 后补 0），可返回“报告不完整”标志 |
| F13 | `fs/fat16.c` | `from_83` 对非法 8.3 名字的显示不做转义 |
| S4 | `svc/service.c` | `service_exists` 返回 bool 风格 0/1，与 `E_*` 混用（仅查询，影响小） |

---

## 3. “0 表示失败”与“0 表示合法值”专项

| 接口 | 0 的含义 | 冲突点 | 处理 |
|---|---|---|---|
| `fs_write` | 修复前：文件满 → 0（失败）；也等于成功写 0 字节 | 内核调用点用 `== len` 判成功，短写/0 写无法区分 | **已修**：满 → `E_NOSPC`；`len==0` 才返回 0 |
| `fs_open` | 0 = 失败（非法句柄） | 句柄永远非 0，故 0 可安全表示失败；但丢失错误原因 | 由 `fs_open_ex`（-1=失败、>=0=合法后端句柄）兜住 |
| `fs_open_ex` / `fat16_open` / `svc_open` / `blk_open` | svc/blk 后端的 **handle 0 是合法槽位** | 与 ramfs 的“0=失败”相反 | `vfs.c` 按后端约定统一成 -1=失败；`ramfs_be_open` 显式把 0 映射成 -1 |
| `pmm_alloc_page(s)` | 0 = 失败 | 合法物理地址都 >= 2MB，0 不会是真地址 | 安全 |
| `malloc` | NULL = 失败 | 指针 0 非法 | 安全 |
| `obj_create` / `dev_open` | 0 = 失败 | `dev_open` 还混入“对象表满” | 见 D2 |
| `pipe_create` / `port_create` | -1 = 失败，0 是合法槽位 | 调用点普遍 `>= 0` 判成功 | 安全；本次给 `pipe_read/write` 补了 used 校验 |
| `fat16_read` / `fs_read` | 0 = EOF（成功） | 失败用 -1/E_* | 安全；本次给 `fs_read` 补了 offset 下溢保护 |
| `service_exists` | 0 = 不存在 | 风格上是 bool，不是 `E_*` | 保持（查询语义明确） |

结论：真正危险的是 **`fs_write` 的“满 → 0/短计数”** 和 **后端句柄 0 的双重含义**；
前者已修，后者由 VFS 适配层统一，新增后端时必须遵守 `-1=失败、>=0=句柄`。

---

## 4. 验证

### 4.1 内建自检（随每次 `-Test` 自动运行）

`fs/vfs_selftest.c` 的 `fs_backend_selftest()` 末尾新增 3 组错误路径断言
（在 `bootseq.c` 原有调用点内，不新增启动步骤、不改 `kernel.c`/`build.ps1`）：

1. `ep_test_write_limit`：写满 `FS_FILE_MAX` 后 stat 必须是 `FS_FILE_MAX`；再写 1 字节必须
   返回 `E_NOSPC`；失败后大小不变；重新打开读开头 4 字节仍是原内容；最后删除。
2. `ep_test_write_atomic`：3 字节文件上一次写 `FS_FILE_MAX` 字节必须整次 `E_NOSPC`，
   `size` 仍为 3，内容仍为 `"abc"`（证明没有“先覆盖 32765 字节再失败”）。
3. `ep_test_unlink_open`：打开中的文件 `unlink` 必须失败、`size` 不变、新旧句柄都能读；
   全部关闭后 `unlink` 成功且 `stat` 失败。
4. FAT16（已挂载时）：把 `fat:/SELFTEST.TXT` 写 3 字节后、**在关闭前** `unlink` 必须失败，
   证明 `E_BUSY` 保护生效；随后原流程继续。

成功时串口打印：

```
fs: error-path selftest ok
fs: backend selftest ok
```

任何一步失败会打印 `fs: error-path selftest FAILED at step N` 并提前返回——
此时原有的 `fs: backend selftest ok` 不再出现，回归第 97 项
`VFS 后端一致性` 会直接失败，因此这些断言是**随回归强制执行**的。

### 4.2 独立自检脚本（不依赖 `build.ps1` 的断言表）

`tools/error-path-check.ps1`：跑隔离回归（`.obj-errap/.dist-errap/serial-errap.log`，
QMP 4511，自动排队测试锁），然后独立断言串口标记：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\error-path-check.ps1
# 只分析已有日志、不重新跑 QEMU：
powershell -NoProfile -ExecutionPolicy Bypass -File tools\error-path-check.ps1 -ReuseLog
```

退出码 0 表示：构建/回归成功 + `fs: error-path selftest ok` 存在 +
`fs: backend selftest ok` 存在 + 无 `error-path selftest FAILED` 标记。

### 4.3 本轮实际结果

见 §5（每次 `-Test` 的 97/97 与脚本输出会追加到本文末尾的“验证记录”）。

---

## 5. 验证记录

### 5.1 两次连续 `-Test`（隔离构建：`.obj-errap` / `.dist-errap` / `serial-errap.log` / QMP 4511）

来自 `.session/history.log`：

```
2026-10-24 21:01:44 name=errap test=True exit=0 elapsed=14.0s === TEST PASS (97/97) ===
2026-10-24 21:03:01 name=errap test=True exit=0 elapsed=23.2s === TEST PASS (97/97) ===
2026-10-24 21:09:43 name=errap test=True exit=0 elapsed=23.9s === TEST PASS (97/97) ===
```

前两次是“连续两次 97/97”；第三次由 `tools/error-path-check.ps1` 端到端驱动
（等 flake 会话修好 `build.ps1` 的 `EchoPort` 默认值后重跑），同样 97/97：

```
[error-path] checks:
  PASS  regression exit code is 0
  PASS  no error-path FAILED marker
  PASS  error-path selftest passed
  PASS  VFS backend selftest still passed
  PASS  no backend selftest FAILED marker
[error-path] marker:
    fs: error-path selftest ok
[error-path] verdict: === TEST PASS (97/97) ===
[error-path] PASS
```

第二次运行串口里的相关标记：

```
fat16: ata1 mounted as fat:/ (16223 clusters, 8111 KB)
disk: remount ok (ata0, sectors=16384, system:/bin/shell=29364 bytes)
fs: error-path selftest ok
fs: backend selftest ok
ushell: fs syscalls ok
```

`fs: error-path selftest ok` 出现在 `fs: backend selftest ok` 之前，说明 3 组新断言
（写满边界 / 整次拒绝 / 删除打开中的文件 + FAT16 同类保护）全部按预期返回错误，
且没有破坏正常路径。

### 5.2 `tools/error-path-check.ps1 -ReuseLog`（独立断言，不启动 QEMU）

```
[error-path] reuse log serial-errap.log (skip QEMU)
[error-path] checks:
  PASS  regression exit code is 0
  PASS  no error-path FAILED marker
  PASS  error-path selftest passed
  PASS  VFS backend selftest still passed
  PASS  no backend selftest FAILED marker
[error-path] marker:
    fs: error-path selftest ok
[error-path] verdict: === TEST PASS (97/97) ===
[error-path] PASS
```

### 5.3 并行会话干扰说明

21:03:50 起，另一会话（flake-hunt）正在参数化共享的 `build.ps1`，出现两个与该
文件相关的瞬时问题：

1. 写入过程中文件一度是无 BOM 的 UTF-8，Windows PowerShell 5.1 按 GBK 解析，
   报 `The hash literal was incomplete` 等错误，所有会话的 `-Test` 在 0.2s 内
   `no verdict captured` 失败；
2. 之后 `build.ps1` 的 `[int]$EchoPort = $EchoPort` 自引用默认值等于 0，
   `-Test` 的网络检查（ARP/ICMP/UDP/TCP）会失败。

这两点都与本次改动无关（源码未变，此前两次 97/97 均已通过），按约定本会话
**没有修改** `build.ps1`。`-ReuseLog` 复检直接使用最后一次成功运行的串口日志，
因此不受影响。flake 会话修复后（`EchoPort = 12345`、BOM/解析正常），本会话已用
`tools/error-path-check.ps1` 端到端重跑并通过（见 §5.1 第三条）。

### 5.4 提交

| 提交 | 内容 |
|---|---|
| `5977574`（`59775742667186619095ff436870fc2e814e5016`） | 22 处错误路径修复 + `fs/vfs_selftest.c` 断言 + `tools/error-path-check.ps1`（11 个文件，无 rename 相关内容） |
| `4fbcc6a` | 本文件 `docs/error-paths.md` |

> 边界：FS 功能会话（rename）会继续改 `fs/ramfs.c` / `fs/fat16.c`。本会话的
> 改动已在上表 `5977574` 一次性提交完毕，工作区里这两个文件之后的新增内容
> 属于 rename 会话，本会话不再 `git add` 它们。

