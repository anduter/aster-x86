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
预期：`=== TEST PASS (98/98) ===`（flake 会话在 `build.ps1` 加了 rename 检查，
所以是 98 不是 97），并且 `serial-pm.log` 里能看到：

```
sched: priority ok (order=0,1,2)
sched: aging ok (base=2 aged=1 eff=2)
pmm: poison ok (alloc=0x00 free=0xaa)
pmm: double-free detected
heap: check ok (...)
```

## 5. B 部分：waitpid(WNOHANG) / SIGCHLD / getppid / mprotect（设计与实现）

> 状态：**已实现并合入**（ABI v1.1，minor=1）。实现记录与实测串口行见 §5.8。
> 本节保留设计理由，便于后续维护；与实现不一致的地方以 §5.8 为准。

### 5.1 ABI：只追加编号，不改既有语义

`SYS_WAIT`（20）保持“阻塞等待”，语义/返回值不变（冻结的 ABI）。新增：

| 编号 | 名称 | 参数（ebx/ecx/edx） | 返回 |
| --- | --- | --- | --- |
| 60 | `SYS_WAITPID` | pid / `int *status` / options | 子 pid / 0（WNOHANG 无僵尸）/ -1 |
| 61 | `SYS_GETPPID` | — | 父 pid（0 = 无） |
| 62 | `SYS_MPROTECT` | addr / len / prot | 0 / -1 |

`include/abi.h` 追加用户可见常量（数值沿用 POSIX）：

```c
#define WNOHANG     1
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4
#define SIGCHLD     17
#define SIG_DFL     0u
#define SIG_IGN     1u
```

同时按 `abi.h` 的版本规则：`OS_ABI_VERSION_MINOR` 0→1、`SYS_NR_MAX` 59→62；
`SYS_ABI_VERSION` 的 feature bits 可新增 `OS_ABI_F_WAITPID` / `OS_ABI_F_MPROTECT`
（只增不减）。`SYS_WAIT` 的 edx 当前被忽略，仍保持忽略，不拿它当 options，
避免“同编号两种语义”。

### 5.2 waitpid(pid, status, WNOHANG)

内核实现（`proc/process.c`）：

```c
int proc_wait_options(int pid, int *status, int options);   /* 新 */
int proc_wait(int pid, int *status)                          /* 旧入口，等价 options=0 */
{
    return proc_wait_options(pid, status, 0);
}
```

循环扫描 `proc_table[]` 中 `parent_pid == current->pid` 的进程：

| 情况 | options == 0（SYS_WAIT） | options & WNOHANG |
| --- | --- | --- |
| 找到 ZOMBIE | `proc_reap()`，写 status，返回子 pid | 同左 |
| 有子进程但都活着 | `proc_block(PROC_WAIT_CHILD)` 后重扫 | **立即返回 0** |
| 没有匹配的子进程 | 返回 -1（保持现状） | **返回 0** |
| 当前无进程 / 参数非法 | -1 | -1 |

`pid` 约定：`pid > 0` 精确匹配；`pid == 0` / `pid < 0` 匹配任意子进程
（教学简化：没有进程组；与 POSIX `waitpid(-pgid)` 不同，文档注明）。
`status` 只在真正回收到子进程时通过 `copy_to_user` 写回；WNOHANG 返回 0
时不碰用户缓冲。

**取舍**：POSIX 在“没有子进程”时返回 -1（ECHILD），这里按本任务要求统一
返回 0（“无子进程可回收时立即返回 0”）。这样调用方只需区分 `>0`（回收了）
和 `==0`（还没好，可继续轮询/做别的事），代价是拿不到 ECHILD。

`proc/proc_syscall.c`：`SYS_WAITPID` 用 `f->ebx/ecx/edx`，status 拷贝失败时
返回 -1（不泄漏“已回收”的假象）；`proc_syscall_owns()` 增加 `SYS_WAITPID`
/`SYS_GETPPID`。

### 5.3 SIGCHLD

- `proc.h` 增加 `#define SIGCHLD 17`（与 `abi.h` 的 17 保持一致；测试
  `_Static_assert(SIGCHLD == 17)` 可放在 process.c）。
- 子进程退出路径：`proc_exit_finish()` 在 `sched_dequeue` + 写 `exit_code` +
  `state = PROC_ZOMBIE` **之后**，按 `parent_pid` 找父进程（跳过
  `PROC_FREE`/`PROC_ZOMBIE`/`is_user == false`），`proc_send_signal(parent,
  SIGCHLD)`。`proc_send_signal` 会置 `pending_sig`，父进程 BLOCKED 时同时
  `sched_wake()`，所以阻塞在 `proc_wait()` 的父进程会被叫醒并重扫子进程表。
- **默认动作必须是“忽略”**：`proc_check_signals()` 现在对 `SIG_DFL` 一律
  `proc_exit(128+sig)`；增加特例：

```c
if (handler == SIG_DFL && sig == SIGCHLD) { p->pending_sig = 0; return; }
```

  否则 shell 会因为子进程正常退出而被杀掉。`SIG_IGN` 仍走现有的“清
  pending 返回”；`handler > SIG_IGN` 走 `deliver_signal()` 投递到用户 handler。
- 已知限制（沿用现状，文档注明）：
  - `pending_sig` 只有一个槽，多个子进程连续退出会被合并成一次 SIGCHLD
    （POSIX 允许对 SIGCHLD 合并）；其它信号也可能覆盖它。
  - 不实现 SIG_IGN 的“自动回收”：子进程仍是 ZOMBIE，仍需 `waitpid` 回收。
  - 父进程先退出时孤儿僵尸没有 reparent 到 pid 1，仍沿用现状（后续可做
    “退出时把 ZOMBIE 挂到 pid 1”或父进程退出时清理）。

### 5.4 getppid

`proc_getppid()`（process.c）：

```c
int proc_getppid(void)
{
    proc_t *p = proc_current();
    return (p != 0) ? (int)p->parent_pid : 0;
}
```

`proc_spawn()` 的内核线程 `parent_pid = 0`；`proc_fork()` 已经写入
`child->parent_pid = parent->pid`，所以不需要改 fork。SYS_GETPPID 直接返回它；
用户库 `getppid()` 封装。

### 5.5 mprotect(addr, len, prot)

**prot 位 → PTE**（`mm/vmm.c` 新增 `int vmm_protect_page(unsigned int virt,
unsigned int prot)`，`mm/vmm.h` 声明）：

| prot | PTE 效果 |
| --- | --- |
| `PROT_READ` | `PRESENT|USER`；清 `WRITE`；置 NX（除非同时带 `PROT_EXEC`） |
| `PROT_WRITE` | 加 `WRITE`（x86 写隐含读）；**不隐含执行**，仍置 NX |
| `PROT_EXEC` | 清 NX（bit 63，仅 `vmm_nx_enabled` 时）；读权限按 READ 位 |
| `PROT_NONE` | **v1 简化 = 只读 + NX**（仍然可读） |

`PROT_NONE` 真正“不可读”需要把 PTE 的 present 清 0，同时让
`vmm_clear_user_space()` / `vmm_clone_user_pages()` 认识“非 present 但仍持有
物理页”的 PTE，否则会泄漏物理页或让 fork 丢映射。这超出 B 的范围，列为
后续项；本版在文档和注释里明确写“PROT_NONE 暂等价只读+NX”，避免误用。

**校验（`mm/mm_syscall.c`，先整段校验、再逐页修改，避免非法区间改一半）**：

1. `p = proc_current()` 非空；`len > 0`；`addr` 4KB 对齐；
2. `addr + len` 不回绕，且整个区间落在 `[USER_LAYOUT_CODE_BASE,
   USER_LAYOUT_MMAP_END)`；
3. `prot & ~(PROT_READ|PROT_WRITE|PROT_EXEC)` == 0（`PROT_NONE`=0 合法）；
4. 每一页 `vmm_user_flags()` 必须 `PRESENT|USER`（guard 页未映射 → -1，
   正好保持 guard 不可 mprotect 成可写）。

**COW 处理**（关键，否则会破坏 fork 语义）：

- 请求 `PROT_WRITE` 且 PTE 带 `PTE_COW`（fork 共享页）：先
  `vmm_cow_fault(va)` 私有化当前进程的页（分配新页、清 COW、置 WRITE）；
  失败（OOM）→ -1，并回滚临时置的 COW 位。
- 请求 `PROT_WRITE` 但 PTE 没有 COW、而 `pmm_page_ref(phys) > 1`（“先
  mprotect 只读、再 fork”会把页变成共享只读且无 COW）：先临时打上
  `PTE_COW` 再走 `vmm_cow_fault()`，保证复制后才可写。
- **清除 `PROT_WRITE` 时必须同时清 `PTE_COW`**：`#PF` 的写时复制路径看到
  COW 就会把页改成可写，如果保留 COW，`mprotect(PROT_READ)` 就形同虚设。
  显式只读的页以后要写，必须再次 `mprotect(PROT_WRITE)`（或 fork 后的写
  缺页），由上面的“请求写先私有化”保证不会写坏共享者。
- 保留 `PWT/PCD/DIRTY` 等既有位，只改 `WRITE/COW/NX`。

**TLB flush 策略**：

- `vmm_protect_page()` 内对每个页做一次 `invlpg`（和现有 `vmm_map_page`
  一致），保证当前 CPU 立即看到新权限；
- **v1 不做远端 shootdown**：每个进程只有一个线程，某 CPU 上的 TLB 只会在
  它加载该进程 CR3 时缓存该地址空间；进程迁移离开时 CR3 已切走（无 PCID，
  CR3 写入会刷新非全局 TLB），所以不存在别的 CPU 持有本进程的旧 PTE。
- 若未来支持“同一地址空间多 CPU 并发”或 PCID，则在 `mm_syscall.c` 整段
  修改完成后调用**一次** `smp_tlb_shootdown()`（`proc/smp.h` 已有），不要
  逐页广播。文档里保留这个 TODO，避免后人误以为漏了。

**其它语义**：

- 部分成功：整段校验通过后逐页改；若中途 COW 复制 OOM，前面的页已生效、
  返回 -1（POSIX 是全有或全无）。这是教学实现的已知简化，测试只覆盖
  合法区间和非法区间，不注入 OOM。
- 用户页不强制 W^X：`PROT_WRITE|PROT_EXEC` 允许（mmap 默认 NX，要执行
  必须显式 `PROT_EXEC`），符合现有 mmap 的保守默认。
- mprotect 可以把本进程的代码页改成可写/可执行：这是进程对自身地址空间的
  权限，不是跨进程越权；现有 `sectest: code write ... status=142`（未调用
  mprotect）不受影响。若以后要禁止自修改代码，需要记录每个 mapping 的
  原始权限并在 mprotect 时比较，列为后续项。
- NX 不可用时（`vmm_nx_enabled == 0`）：`PROT_READ` 的页仍可执行；自检里
  NX 相关断言按 `vmm_nx_enabled` 条件跳过（QEMU `+nx` 下始终可用）。

### 5.6 要改的文件清单（按依赖顺序）

| 顺序 | 文件 | 改动 | 当前归属/状态 |
| --- | --- | --- | --- |
| 1 | `src/kernel/include/abi.h` | 编号 60..62、`WNOHANG`/`PROT_*`/`SIGCHLD`、minor++、`SYS_NR_MAX=62` | API 会话（7ebab102）在飞 |
| 2 | `src/kernel/proc/proc.h` | `SIGCHLD 17`；`proc_wait_options`/`proc_getppid` 声明 | 本会话 |
| 3 | `src/kernel/proc/process.c` | `proc_wait_options`、`proc_getppid`、exit 时发 SIGCHLD、SIGCHLD 默认忽略 | 本会话（已干净） |
| 4 | `src/kernel/mm/vmm.h` / `vmm.c` | `vmm_protect_page()`：PTE 更新 + COW + NX + invlpg | 本会话（已干净） |
| 5 | `src/kernel/mm/mm_syscall.c` | `SYS_MPROTECT` case、区间校验、`owns()` | 474e5d40 / 待让出 |
| 6 | `src/kernel/proc/proc_syscall.c` | `SYS_WAITPID` / `SYS_GETPPID` case、`owns()` | 474e5d40 / 待让出 |
| 7 | `src/kernel/proc/trap/syscall.h` | 无需改（只是转发 `abi.h`） | — |
| 8 | `src/user/ulib.h` / `ulib.c` | `waitpid()` / `getppid()` / `mprotect()` / `signal()` 封装 | API 会话 |
| 9 | `src/user/user_shell.c` | `sectest` 增加 B 自检块 | API/#PF 会话 |
| 10 | `build.ps1` | 新增 `sectest:` 检查项（98 → 98+N） | flake 会话 |
| 11 | `docs/process-memory.md` | 本节 + 实测记录 | 本会话 |

依赖顺序：1 → 2/3/4 → 5/6 → 8 → 9 → 10。2/3/4 可以先做（不依赖 1 的
具体编号，先写死 `SIGCHLD 17`），但编译要等 1 的常量。

### 5.7 B 自检草案（只进文档，代码待解锁后再提交）

放在现有 `sectest` 命令里（`build.ps1` 已经注入 `sectest`，不用改注入
按键），输出沿用现有 `sectest: ... ok/FAIL` 风格。用户库先加封装：

```c
/* ulib.h */
int waitpid(int pid, int *status, int options);
int getppid(void);
int mprotect(void *addr, unsigned int len, int prot);
int signal(int sig, unsigned int handler);     /* SIG_DFL/SIG_IGN/用户地址 */
```

> 以下自检是**实现时的设计草案**，最终代码已按它落地（见 §5.8）；输出行保持一致。
> 沿用 `user_shell.c` 现有的 `sc()/putstr()/printf()` 风格；`fork()/exit()/
> mmap()/sleep_ms()` 是现有 `sc(SYS_*)` 的简写。

#### 测试 1：getppid（用管道把子进程看到的 parent pid 带回父进程比较）

```c
int fds[2];
sc(SYS_PIPE, (int)fds, 0, 0);
int pid = sc(SYS_FORK, 0, 0, 0);
if (pid == 0) {
    int pp = getppid();
    sc(SYS_FWRITE, fds[1], (int)&pp, sizeof(pp));
    sc(SYS_EXIT, 0, 0, 0);
}
int pp = -1, st;
sc(SYS_FREAD, fds[0], (int)&pp, sizeof(pp));
waitpid(pid, &st, 0);
printf("sectest: getppid %s (child sees %d, self %d)\n",
       (pp == getpid()) ? "ok" : "FAIL", pp, getpid());
```

#### 测试 2：waitpid WNOHANG

```c
int pid = sc(SYS_FORK, 0, 0, 0);
if (pid == 0) { sleep_ms(200); sc(SYS_EXIT, 7, 0, 0); }

int st = -1;
int first = waitpid(pid, &st, WNOHANG);      /* 子进程还活着 -> 0 */
int spins = 0, r = first;
while (r == 0 && spins++ < 2000) { sc(SYS_YIELD, 0, 0, 0); r = waitpid(pid, &st, WNOHANG); }
if (first == 0 && r == pid && st == 7) putstr("sectest: waitpid wnohang ok\n");
else putstr("sectest: waitpid wnohang FAIL\n");

/* 没有匹配子进程时也必须立即返回 0（本任务约定，见 5.2） */
if (waitpid(99999, &st, WNOHANG) == 0) putstr("sectest: waitpid no-child ok\n");
else putstr("sectest: waitpid no-child FAIL\n");
```

#### 测试 3：SIGCHLD（默认忽略 / 显式 handler / SIG_IGN）

```c
static volatile int g_sigchld;
static void sigchld_handler(int sig) { (void)sig; g_sigchld++; }

/* a) 默认动作：父进程不能被 SIGCHLD 杀掉 */
int pid = fork(); if (pid == 0) exit(0);
sleep_ms(50);
if (waitpid(pid, 0, 0) == pid) putstr("sectest: sigchld default ignored ok\n");
else putstr("sectest: sigchld default ignored FAIL\n");

/* b) 显式 handler：子进程退出后 handler 至少跑一次 */
signal(SIGCHLD, sigchld_handler);
g_sigchld = 0;
pid = fork(); if (pid == 0) exit(0);
sleep_ms(100);
int w = waitpid(pid, 0, 0);
printf("sectest: sigchld handler %s (sigchld=%d wait=%d)\n",
       (g_sigchld > 0 && w == pid) ? "ok" : "FAIL", g_sigchld, w);

/* c) SIG_IGN：父进程存活，SIGCHLD 被丢弃；僵尸仍需 waitpid */
signal(SIGCHLD, SIG_IGN);
pid = fork(); if (pid == 0) exit(0);
sleep_ms(50);
printf("sectest: sigchld ignored %s\n", (waitpid(pid, 0, 0) == pid) ? "ok" : "FAIL");
signal(SIGCHLD, SIG_DFL);       /* 恢复默认（默认=忽略，见 5.3） */
```

注意 `sleep_ms` 被 SIGCHLD 唤醒会提前返回（`proc_sleep_ticks` 目前不区分
信号唤醒），所以测试要用 `waitpid` 兜底，不能依赖 sleep 时长。

#### 测试 4：mprotect（写保护 / 可执行 / COW / 非法参数）

```c
/* a) RO -> 子进程写 -> 142 */
unsigned char *p = mmap(4096);
p[0] = 0x41;
int pid = fork();
if (pid == 0) {
    if (mprotect(p, 4096, PROT_READ) != 0) exit(99);
    p[1] = 0x42;                         /* 应 #PF */
    putstr("sectest: mprotect ro SURVIVED\n");
    exit(0);
}
int st; waitpid(pid, &st, 0);
printf("sectest: mprotect ro deny %s (status=%d)\n", (st == 142) ? "ok" : "FAIL", st);

/* b) NX -> EXEC：往 mmap 页写一条 ret，mprotect 后可执行 */
pid = fork();
if (pid == 0) {
    unsigned char *q = mmap(4096);
    q[0] = 0xC3;                         /* ret */
    if (mprotect(q, 4096, PROT_READ | PROT_EXEC) != 0) exit(98);
    ((void (*)(void))q)();
    exit(0);
}
waitpid(pid, &st, 0);
printf("sectest: mprotect exec %s (status=%d)\n", (st == 0) ? "ok" : "FAIL", st);

/* c) EXEC -> NX：先给执行权、再收回，子进程执行应 142 */
pid = fork();
if (pid == 0) {
    unsigned char *q = mmap(4096);
    q[0] = 0xC3;                                    /* ret */
    if (mprotect(q, 4096, PROT_READ | PROT_EXEC) != 0) exit(97);
    if (mprotect(q, 4096, PROT_READ) != 0) exit(96);/* 收回执行权 */
    ((void (*)(void))q)();                          /* 应 #PF */
    putstr("sectest: mprotect nx SURVIVED\n");
    exit(0);
}
waitpid(pid, &st, 0);
printf("sectest: mprotect nx deny %s (status=%d)\n", (st == 142) ? "ok" : "FAIL", st);

/* d) COW：父页 0xAB -> fork -> 子 mprotect(WRITE) 后写 0xCD；
      父进程读回必须仍是 0xAB（mprotect 触发 COW 私有化） */
p[0] = 0xAB;
pid = fork();
if (pid == 0) {
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0) exit(95);
    p[0] = 0xCD;                                    /* 只改子进程的私有副本 */
    exit(0);
}
waitpid(pid, &st, 0);
printf("sectest: mprotect cow %s (parent byte=0x%02x status=%d)\n",
       (st == 0 && p[0] == 0xAB) ? "ok" : "FAIL", p[0], st);

/* e) 非法参数：非对齐 / len=0 / 内核地址 / 未知 prot 位，全部必须 -1 */
int bad = 0;
if (mprotect(p + 1, 4096, PROT_READ) == -1) bad++;
if (mprotect(p, 0, PROT_READ) == -1) bad++;
if (mprotect((void *)0x80000000u, 4096, PROT_READ) == -1) bad++;
if (mprotect(p, 4096, 0x8) == -1) bad++;
printf("sectest: mprotect invalid %s\n", (bad == 4) ? "ok" : "FAIL");
```

#### 期望的串口行（build.ps1 后续按这些加检查）

```
sectest: getppid ok (child sees <pid>, self <pid>)
sectest: waitpid wnohang ok
sectest: waitpid no-child ok
sectest: sigchld default ignored ok
sectest: sigchld handler ok (sigchld=1 wait=<pid>)
sectest: sigchld ignored ok
sectest: sigchld cow ok
sectest: mprotect ro deny ok (status=142)
sectest: mprotect exec ok (status=0)
sectest: mprotect nx deny ok (status=142)
sectest: mprotect cow ok
sectest: mprotect invalid ok
```

`build.ps1` 检查草案（flake 会话加；7 条，预计 98 → 105）：

```powershell
@{ Name='waitpid WNOHANG'; Ok = ($out -match 'sectest: waitpid wnohang ok') -and
                                     ($out -match 'sectest: waitpid no-child ok') },
@{ Name='SIGCHLD 默认忽略'; Ok = ($out -match 'sectest: sigchld default ignored ok') },
@{ Name='SIGCHLD handler'; Ok = ($out -match 'sectest: sigchld handler ok') },
@{ Name='getppid';          Ok = ($out -match 'sectest: getppid ok') },
@{ Name='mprotect 写保护';  Ok = ($out -match 'sectest: mprotect ro deny ok \(status=142\)') },
@{ Name='mprotect NX';      Ok = (($out -match 'sectest: mprotect exec ok') -and
                                  ($out -match 'sectest: mprotect nx deny ok')) },
@{ Name='mprotect COW/非法'; Ok = (($out -match 'sectest: mprotect cow ok') -and
                                   ($out -match 'sectest: mprotect invalid ok')) },
```

#### 回归风险与不变量

- `SYS_WAIT`（20）行为不变：`proc_wait()` 仍是 `options=0` 的包装；现有
  `fork`/`sectest`/`native process_create/wait` 全部走旧路径。
- SIGCHLD 默认忽略：所有“子进程正常退出后父进程继续跑”的现有回归都必须
  仍然通过（这是 B 最大的行为风险点，先加默认忽略再实现发信号）。
- `mprotect` 只改用户页 PTE，不碰内核映射；`PROT_NONE` 不做 present=0，
  不给 teardown/fork 增加新状态。
- 新 syscall 编号只追加，`SYS_ABI_VERSION` minor++，不改任何既有编号。

### 5.8 实现记录（2026-10-24，ABI v1.1）

落地内容：

- `include/abi.h`：`SYS_WAITPID=60`、`SYS_GETPPID=61`、`SYS_MPROTECT=62`、
  `WNOHANG`/`PROT_*`/`SIGCHLD`/`SIG_DFL`/`SIG_IGN`，`SYS_NR_MAX=62`，
  `OS_ABI_VERSION_MINOR` 0→1。`SYS_WAIT`(20) 未改。
- `proc/process.c`：`proc_wait_options()`（`proc_wait()` 为 options=0 包装）、
  `proc_getppid()`；`proc_exit_finish()` 发布 ZOMBIE 后按 `parent_pid` 发
  SIGCHLD（ppid 在发布前取好，避免父进程并发 reap 后读到清空值）；
  `proc_check_signals()` 对 `SIGCHLD + SIG_DFL` 清 pending 返回。
- `proc/proc_syscall.c`：`SYS_WAITPID`/`SYS_GETPPID` 的 owns+switch。
- `mm/vmm.c`：`vmm_protect_page()`；`mm/mm_syscall.c`：`SYS_MPROTECT`
  （整段校验 → 逐页改，COW 私有化，NX，`invlpg`）。
- `src/user/ulib.*`：`waitpid/getppid/mprotect/signal` 封装；
  `src/user/user_shell.c`：`sectest` 末尾增加 12 项 B 自检（含 P0 COW 回归）。

实测（`serial-pm.log`，连续 2 次 98/98）：

```
sectest: getppid ok (child sees 6, self 6)
sectest: waitpid wnohang ok
sectest: waitpid no-child ok
sectest: sigchld default ignored ok
sectest: sigchld handler ok (sigchld=1 wait=31)
sectest: sigchld ignored ok
sectest: sigchld cow ok
sectest: mprotect ro deny ok (status=142)
sectest: mprotect exec ok (status=0)
sectest: mprotect nx deny ok (status=142)
sectest: mprotect cow ok (parent byte=0xab status=0)
sectest: mprotect invalid ok
```

实现中踩到/确认的点：

1. **COW 语义修正**：清写必须同时清 `PTE_COW`（详见 §5.5），否则
   `mprotect(PROT_READ)` 的页会在写缺页时被静默复制成可写；共享页要写时
   由“请求写先私有化”路径处理。
2. **shell 的 32KB 上限**：`/bin/shell` 装在 ramfs，单文件上限
   `FS_FILE_MAX = 32768`。B 自检在 -O0 下把 `.text` 顶过 `0x40004000`，
   `.rodata` 后移一页，ELF 变成 34856 字节，安装失败（`installed (-3 bytes)`，
   shell 回退到内核内建版本、`ushell: ready` 检查失败）。
   处理：只对“sectest 区块 + `cmd_sectest`”加
   `#pragma GCC push_options / optimize("Os") / pop_options`（语义不变，
   仅体积），`.text` ≈16338、ELF ≈30532，回到上限内；其余 shell 仍 -O0。
   余量只剩几十字节（`.text` 必须 < 16384，否则 `.rodata` 后移一页、
   ELF 立刻超过 32768），改动这段代码后务必用
   `i686-elf-size -A .obj-pm/user_shell.elf` 确认 < 32768。
3. **WNOHANG 测试节奏**：子进程先 `SYS_SLEEP 200ms`，父进程轮询用
   `SYS_SLEEP 10ms` 循环（纯 `SYS_YIELD` 会在子进程醒来前就转完 2000 次）。
4. **exec 自检写法**：mmap 页写一条 `ret`，`mprotect(READ|EXEC)` 后
   `call` 它；能正常返回 = 执行权已打开，`#PF(142)` = NX 仍生效。
5. `SYS_NR_MAX` 59→62 只用于编号校验和注释；`syscall_mask` 是 64 位，
   bit62 仍安全，没有按旧上限分配的数组/位图。
6. 已知限制沿用 §5.3：SIGCHLD 单 pending 槽会合并；SIG_IGN 不自动回收；
   父进程先退出的孤儿僵尸仍不会被回收。
7. **P0 修复：信号帧写在 COW 只读栈页上会内核态 oops**（修复前现场：
   `oops: exception 14 (page fault) at eip=0x0011182c err=3 cr2=0x4018bc1c`，
   `irq_dispatch → proc_check_signals → deliver_signal → memcpy`）。
   fork 之后父进程栈页是 COW 只读页，`deliver_signal()` 以内核态写栈帧，
   而 #PF 的 COW 兜底只处理 ring3 缺页（`idt.c` 的 `(f->cs&3)==3`），
   内核态写直接 oops，shell（pid 6）被杀，后续 `echo hi`/`reexec` 没输入，
   `exec 地址空间切换`/`命令行参数传递` 两项失败且 harness 白等 90s。
   修复（两层）：
   - `deliver_signal()` 写帧前对 `[sp, frame_addr+sizeof(frame))` 覆盖的每个页
     调 `vmm_make_writable()`（新增，vmm.c）：COW/共享页先私有化，显式只读
     页返回 -1；失败则不写帧、返回 0，`proc_check_signals()` 丢弃这次投递
     （打印 `proc: pid .. signal .. dropped`），绝不按默认动作杀进程。
   - `idt.c` 增加内核态兜底：`err=3`、cr2 在用户区间、当前有进程且
     `vmm_cow_fault()` 确认是 COW 页时，复制后重试；其它内核态缺页仍 oops。
   回归：`sectest` 增加 `sectest: sigchld cow ok`——fork 后父进程用内联汇编
   直接 sleep 等 SIGCHLD，中间没有任何用户态栈写入，强制信号帧落在 COW
   只读栈页上（修复前必现 oops）。
8. **P0 连带修复：SIGRETURN 清掉被中断系统调用的返回值**。
   `syscall_dispatch()` 在调用 `proc_sigreturn()` 后统一执行 `f->eax = ret`，
   而 sigreturn 刚从信号帧恢复的 `eax` 因此被清零。平时 handler 打断的
   syscall 返回值多为 0 不明显，但新回归里 fork 的返回值（子 pid）在
   SIGCHLD handler 返回后变成 0，父进程误以为自己就是子进程，直接
   `exit(0)` 把 shell 结束掉。
   修复（`proc/proc_syscall.c`）：`SYS_SIGRETURN` 成功时把恢复后的
   `f->eax` 作为 `ret` 返回，dispatch 再写回同一个值。该 bug 影响所有
   “系统调用返回后被信号打断”的返回值（read/write/fork/wait 等），
   不只是自检。

## 6. 已知边界 / 待办

- **B 部分**：已实现并验证（见 §5.8）。剩余可做项：`PROT_NONE` 真正的
  present=0、SIG_IGN 自动回收、孤儿 reparent、多线程/PCID 下的远端
  TLB shootdown、用户态 `setpriority` 与 fork 优先级继承。
- 优先级目前只在内核内部（`sched_set_priority`）可调，还没有用户态
  `setpriority` 系统调用；fork 子进程走 `sched_enqueue` 的默认优先级 4，
  暂不继承父进程优先级（需要改 `process.c`）。
- `heap_stats` 的签名没有扩展（调用点在另一个会话负责的 `core/`），碎片
  信息通过新增的 `heap_frag_stats` 提供。
- 页毒化默认开启会带来每次分配/释放一次 4KB 填充的开销；需要测性能时用
  `-DPMM_NO_POISON` 关闭（`build.ps1` 的 `-D` 由构建会话统一加）。
