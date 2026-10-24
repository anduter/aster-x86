# 进程调度与内存记账（process / memory）

本文件记录 2026-10-24 进程/内存补缺会话（pm）的设计、验证方法与已知边界。
代码范围：`proc/sched.c`、`proc/sched_selftest.c`（新增）、`proc/proc.h`、
`proc/proc_internal.h`、`mm/pmm.c`、`mm/pmm.h`、`mm/heap.c`、`mm/heap.h`、
`mm/memtest.c`。**没有**改动 `proc/process.c`、`proc/switch.asm`、
`proc/trap/*`、`mm/vmm.c`、`include/`、`core/`、`fs/`、`net/`、`build.ps1`。

## 1. 调度器：优先级 + aging

### 数据结构

`proc_t` 新增（`proc/proc.h`）：

| 字段 | 含义 |
| --- | --- |
| `uint8_t base_prio` | 静态优先级 0..7，**数值越小优先级越高**，默认 4 |
| `uint8_t eff_prio` | 当前有效优先级；aging 提升后临时小于 base |
| `uint16_t wait_ticks` | 在 runqueue 上连续等待的 tick 数 |
| `bool prio_set` | 优先级是否已初始化（新进程首次入队时置默认 4） |
| `uint32_t runs` | 被调度上 CPU 的累计次数（诊断） |
| `uint32_t aged` | aging 提升次数（诊断） |

`SCHED_AGING_TICKS` 默认 20 tick（100Hz 下 200ms）。`PROC_PRIO_LEVELS = 8`。

### runqueue 结构

每个 CPU 保留原来的 `per_cpu[cpu].runq_mask`，另外在 `sched.c` 内部维护
`static uint32_t sched_bucket[SMP_MAX_CPUS][PROC_PRIO_LEVELS]`：

- `runq_mask` 是 8 个桶的并集，供负载统计/迁移使用；
- 入队、唤醒、退出、优先级变更、负载迁移都走
  `rq_unlink_locked()` / `rq_link_locked()` / `rq_reset_bucket_locked()`，
  保证“一个进程只在一个 CPU 的一个优先级桶里”；
- 选择时先单遍扫描 runq_mask 找出最小的 `eff_prio`（最高优先级），得到该
  优先级的候选位图，再在候选位图里**从 cur+1 环形扫描**。

因此当所有进程都是默认优先级 4 时，候选集合与扫描顺序和改造前的纯轮转
完全一致——默认行为等价（这是 97 项回归不受影响的前提）。

### 时间片与抢占

`sched_tick()` 在时间片到期时先把 `cur->state` 从 `RUNNING` 放回 `READY`
再调用 `sched_next()`。否则当前进程会被“只选 READY”的扫描排除，高优先级
线程会在自己时间片结束时反被低优先级线程抢占。防御性地只在
`state == PROC_RUNNING` 时这样做，避免 BLOCKED/ZOMBIE 被时间片“复活”。

`sched_next()` 选中进程后：

```
p->eff_prio   = p->base_prio;   /* 运行后回落到 base */
p->wait_ticks = 0;
p->runs++;
```

### aging

每次时钟 tick（`sched_tick` / `sched_ap_tick`）调用
`sched_age_runqueue(cur)`：本 CPU runqueue 上状态为 `READY` 且不是当前进程
的进程 `wait_ticks++`；达到 `sched_aging_ticks` 就 `eff_prio--`（最低 0）、
`aged++` 并迁移到新优先级桶。低优先级进程等待足够久后会被提升，不会被
高优先级线程饿死。

### 亲和与迁移

`sched_balance()` 现在跳过 `cpu_pin >= 0` 的进程。否则负载均衡会把固定到
AP 的探针或本自检的 BSP 线程搬走，破坏“顺序可判定”和 AP 回归。

### 自检（`proc/sched_selftest.c`）

`sched_start()` 在选第一个进程前调用 `sched_selftest_start()`：

1. 运行时注册程序 `schedprio`（不占 `PROG_MAX` 的静态名额之外，共 13 个）；
2. 创建 3 个忙等线程，`cpu_pin = 0` 且 `sched_enqueue(p, 0)` 固定 BSP：
   - prio 0 忙等 30 tick（跑得够久，让低优先级触发 aging）
   - prio 1 / prio 2 各忙等 4 tick

   三个优先级都高于系统默认优先级 4，因此自检会在 shell 等默认优先级进程
   之前跑完，串口输出不与启动日志交错；低优先级在等待 20 tick 后仍会被
   aging 提升（`aged > 0`）。
3. 每个线程在第一次获得 CPU 时记录自己的 `base_prio`，第三个进入的线程
   判定顺序并打印：

```
sched: priority ok (order=0,1,2)
sched: aging ok (base=2 aged=1 eff=2)
```

`aged > 0` 证明等待期间被 aging 提升过；`eff == base` 证明上 CPU 后已回落。
失败时打印 `sched: priority FAIL (...)` / `sched: aging FAIL (...)`。

由于三个线程固定在同一 CPU 的同一个 runqueue，且高优先级线程的时间片
到期后会重新被选中，顺序是确定的；如果将来的时间片/负载策略让严格顺序
不稳定，可退化为“200 tick 内各线程运行次数比例”（`runs` 字段已提供）。

### 内部接口（`proc/proc_internal.h`）

| 接口 | 用途 |
| --- | --- |
| `sched_set_priority(p, prio)` | 设置 0..7 优先级并同步优先级桶 |
| `sched_set_aging_ticks(n)` | 调整 aging 窗口（自检/实验用） |
| `sched_selftest_start()` | 自检入口，仅 `sched_start()` 调用 |

## 2. PMM：记账、毒化、double-free

### 记账（`mm/pmm.h` 导出）

| 计数器 | 含义 |
| --- | --- |
| `pmm_alloc_count` | 成功分配的页数 |
| `pmm_alloc_fail` | 分配失败次数（无足够页/无连续段） |
| `pmm_free_count` | 真正归还给 PMM 的页数（引用计数归零） |
| `pmm_double_free` | double-free 检测次数 |
| `pmm_ref_anomalies` | 位图已用但引用计数为 0 的次数 |

仍保留 `pmm_faults` 作为“所有 WARN 的累计数”。

### 页毒化

`PMM_POISON` **默认开启**（无需改 `build.ps1`），构建时定义
`PMM_NO_POISON` 可关闭：

- 分配时整页填 `0x00`（新页主看不到上一任数据）；
- 真正释放时填 `0xAA`（use-after-free 会立刻看到 `0xAAAAAAAA`）；
- COW 共享（`page_ref > 1`）只减引用，不毒化。

### 异常检测

`free_pages_locked()` 逐页检查：

1. 位图已空闲 → `pmm_double_free++` + `pmm: WARN double free at 0x...`，
   **不动位图/账目**（保证 double-free 不会把页重复发出去）；
2. 位图已用但 `page_ref == 0` → `pmm_ref_anomalies++` +
   `pmm: WARN refcount anomaly at 0x...`，拒绝归还（多半是把 E820 保留区
   或已损坏的页当普通页释放）；
3. 引用计数 >1 → 只减引用；
4. 归零 → 清位图、`free_pages++`、`pmm_free_count++`、毒化。

### 自检（`mm/memtest.c` 追加，1..8 的标签保持不变以兼容回归）

```
pmm: poison ok (alloc=0x00 free=0xaa)
pmm: accounting ok (100 pages, free restored)
pmm: WARN double free at 0x0195F000
pmm: double-free detected
```

- 100 页分配后 `free_pages` 减少 100、`pmm_alloc_count` 增加 100；释放后
  `free_pages` 复原、`pmm_free_count` 增加 100，且释放页首尾字节为 `0xAA`；
- double-free 使用同一地址释放两次：第二次被检测（`pmm_double_free` 加 1、
  WARN、`free_pages` 不变），打印 `pmm: double-free detected`；
- 8/8 的异常计数检查改为“本轮增量”，所以重复运行 `memtest` 不会被
  故意触发的 WARN 误判。

## 3. 堆诊断

`mm/heap.c` / `mm/heap.h`：

- `heap_stats(used,total)` 保持原签名（`core/misc_syscall.c` 还在调用）；
  新增 `heap_frag_stats(&largest_free, &free_blocks)` 提供**最大空闲块**和
  **空闲块个数**，遍历时同时做堆区间保护；
- `heap_check()` 全链表遍历，检查：首块在 `HEAP_BASE`、块头魔数、`used` 只
  取 0/1、`prev` 互指、`next` 首尾相接、块不越界、没有相邻两个空闲块
  （合并正确性）、末尾正好到 `HEAP_BASE + HEAP_SIZE`。错误返回负的编号并
  打印首个出错地址；
- `free()` 遇到魔数损坏打印 `heap: bad block magic <magic> at <addr>`，
  遇到 `used==0` 打印 `heap: double free at <addr>`；每个错误最多打印 8 次；
- 自检调用 `heap_check()` 后打印：

```
heap: check ok (largest_free=16777196 free_blocks=1)
```

`heap_check` 只在诊断/自检路径调用，不在 malloc/free 热路径。

## 4. 验证

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name pm -Test
```

产物：`.obj-pm/`、`.dist-pm/`、`serial-pm.log`、`logs-session-pm.log`（均不提交）。
预期：`=== TEST PASS (97/97) ===`，并且 `serial-pm.log` 里能看到：

```
sched: priority ok (order=0,1,2)
sched: aging ok (base=2 aged=1 eff=2)
pmm: poison ok (alloc=0x00 free=0xaa)
pmm: double-free detected
heap: check ok (...)
```

## 5. 已知边界 / 待办

- **B 部分未做**：`waitpid(WNOHANG)`、`SIGCHLD`、`getppid()`、`mprotect()`
  依赖 `proc/process.c` / `mm/vmm.c`，本会话开始时这两个文件已被 #PF 会话
  （`a58f7dd5`）修改且未提交（`git status` 显示 `M`），按约定不碰，等其
  提交后再做。
- 优先级目前只在内核内部（`sched_set_priority`）可调，还没有用户态
  `setpriority` 系统调用；fork 子进程走 `sched_enqueue` 的默认优先级 4，
  暂不继承父进程优先级（需要改 `process.c`）。
- `heap_stats` 的签名没有扩展（调用点在另一个会话负责的 `core/`），碎片
  信息通过新增的 `heap_frag_stats` 提供。
- 页毒化默认开启会带来每次分配/释放一次 4KB 填充的开销；需要测性能时用
  `-DPMM_NO_POISON` 关闭（`build.ps1` 的 `-D` 由构建会话统一加）。
