# mm 会话交付声明（P1：per-process RSS + 文件映射 mmap 基础）

- 会话：`mm`；所有权路径：`src/kernel/mm/**`、`src/kernel/include/mm.h`、本文件。
- 测试：`powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name mm -Test`
- 期望：既有 182 项全 PASS，另加本文“期望断言行”里的 3 行（集成会话把它们加进 `build.ps1`）。

## 期望断言行（串口，严格匹配）

1. `mm: rss accounting ok (pages=3, cow shared counted per process)`
2. `mm: ps pid=0 name=rss-selftest state=1 rss_pages=3 rss_kb=12`
   （`ps` 行格式演示；真实进程由 `mm_rss_dump()` 在有进程后打印同样格式）
3. `mm: file mmap ok (read-only MAP_PRIVATE, offset+len, cow private)`

失败时对应输出 `mm: rss accounting FAIL` / `mm: file mmap FAIL`，并让 `memtest:` 汇总行
出现 FAIL（`selftest: FAIL`）。这三行都在 `memtest` 启动步骤内打印（早于 `kernel: ready`）。

## 1. per-process RSS

### 口径

```
RSS(进程) = 该进程页目录中，用户布局区
            [USER_LAYOUT_CODE_BASE, USER_LAYOUT_CLONE_END)
            = [0x40000000, 0x40500000) 范围内
            PRESENT|USER 的 4KB 页数
```

- 包含：ELF 代码/数据、用户栈、堆（brk）、信号蹦床、匿名 mmap、文件映射页。
- **fork 后父子共享的 COW 页在每个进程各计 1 次**（与 Linux RSS 一致；不按物理页去重）。
- 不计：内核栈、页表/PDPT 页、内核高半区映射（vmalloc/MMIO/framebuffer）。
- 单位：`mm_rss_pages()` 返回页数，`mm_rss_bytes()` 返回字节数；`rss_kb` = 页数 × 4。

### 扣减方式（为什么不会漏记）

计数**直接扫描页表**（`vmm_user_page_count(dir)`），不做增量记账，因此下列路径天然正确：

| 事件 | 页表变化 | RSS 结果 |
|---|---|---|
| 进程退出 / `proc_reap` → `vmm_destroy_dir` | 用户页全部清除、`proc_t` 变 `PROC_FREE` | 查询返回 0 |
| `fork`（`vmm_clone_user_pages`） | 子进程复制父进程 PTE（共享 COW） | 父进程不变，子进程同样页数 |
| COW 写缺页（`vmm_cow_fault`） | 共享页换成私有页 | 页数不变（1 → 1） |
| `munmap` / 失败回滚（`mm_unmap_range`） | PTE 清 0、物理页释放/减引用 | 对应页立即不再计入 |
| `mprotect` | 只改权限位 | 不变 |

扫描成本：每进程最多 3 个 2MB PDE × 512 PTE = 1536 项，只用于 `res`/`ps` 展示路径。

### 接口（`src/kernel/mm/rss.h`，经 `include/mm.h` 导出）

- `uint32_t mm_rss_pages(const struct process *p)`：页数；`p==0`、`PROC_FREE`、无页目录 → 0。
- `uint32_t mm_rss_bytes(const struct process *p)`：字节数。
- `uint32_t mm_rss_total_pages(void)`：所有活动进程之和。
- `void mm_rss_print(const struct process *p)`：打印一行
  `mm: ps pid=.. name=.. state=.. rss_pages=.. rss_kb=..`
- `uint32_t mm_rss_dump(void)`：遍历进程表打印全部活动进程，返回行数。
- `int mm_rss_selftest(void)`：自检（3 页计数 → COW fork 各计一次 → 解除映射扣减 →
  退出归零 → PMM 账目复原）。

### 健壮性

`vmm_user_page_count()` 对 PDPT/PD/PT 物理地址做 **RAM 范围 + 页对齐**校验；另一颗 CPU
正在 reap 时读到 PMM 毒化值（0xAA）只会得到 0 页，不会把诊断路径读挂。接口是诊断用途、
非原子，不保证与调度器并发时的强一致快照。

## 2. 文件映射 mmap（`SYS_MMAP` 文件后端）

### 用户 ABI（**不新增 syscall 编号、不改 `include/abi.h`**）

`SYS_MMAP`（48）的寄存器语义复用/扩展如下（旧的匿名调用 `ecx=0/3, edx=0` 行为不变）：

| 参数 | 含义 |
|---|---|
| `ebx` | `len`（字节，内核向上取整到页） |
| `ecx` bits 0..7 | `prot`：`PROT_READ(1) / PROT_WRITE(2) / PROT_EXEC(4)`，复用 `abi.h` 的 `PROT_*` |
| `ecx` bit 8 (`0x100`) | `MAP_FILE`：文件后端；`edx` 作为 fd |
| `ecx` bit 9 (`0x200`) | `MAP_PRIVATE`：文件映射**必须置位**，否则 `-1` |
| `ecx` bits 16..31 | `offset` 的**页数**（`offset = 页数 × 4096`，仅 `MAP_FILE`；0 = 文件开头） |
| `edx` | fd（仅 `MAP_FILE`；`0` 表示匿名映射，兼容旧调用） |

例：`sc(SYS_MMAP, 8192, 0x100 | 0x200 | (1 << 16) | PROT_READ, fd)` = 从文件 offset 4096
映射 8KB 只读；`sc(SYS_MMAP, 4096, 3, 0)` 仍是原来的匿名 RW 映射。

为什么 offset 放在 `ecx` 高位：legacy `int 0x81` 只有 `ebx/ecx/edx` 三个参数寄存器
（`syscall3`），没有第 4 个；`abi.h` 只允许 net 改，mm 不能动。`offset` 上限 65535 页
（256MB），实现侧再把“有用的 offset”限制在 `FS_FILE_MAX`（64KB）以内。

### 语义（本轮：只读 MAP_PRIVATE 起步）

- fd 必须是 `FD_KIND_FILE` 且带 `FD_RIGHT_READ`，否则 `-1`。
- **一次性读入**（不做按需缺页）：mmap 时按页分配物理页，把 `[offset, offset+len)` 读进
  页面；文件短于映射区时，超出 EOF 的部分**零填充**（POSIX 的 SIGBUS 语义本轮不模拟）。
- `prot & PROT_WRITE == 0`：页真正只读（无 `PTE_WRITE`/`PTE_COW`），用户写触发 `#PF`，
  进程按 SIGSEGV 终止（**拒绝写**）。
- `prot & PROT_WRITE != 0`（MAP_PRIVATE）：页标 `PTE_COW`。首次写走 `vmm_cow_fault()`：
  这是一块只为映射分配的私有页（PMM 引用计数 1），直接把该页改成可写，**文件内容不受
  影响**，等价 MAP_PRIVATE 的“写时复制私有副本”。
- 映射页默认 `NX`（不可执行）；显式 `PROT_EXEC` 才去掉 NX（与 `mprotect` 的
  “要执行必须显式要”一致；`PROT_WRITE|PROT_EXEC` 在 NX 下仍按可写页处理，与匿名
  mmap 的保守默认相同）。
- `munmap(addr, len)` 与匿名映射共用同一套槽位/校验：清 PTE、释放物理页（COW 共享页只减
  引用计数），`RSS` 立即扣减。`fork` 复制 `mmaps[]` 表，子进程可独立 munmap。
- 限制（与匿名 mmap 共享）：mmap 虚拟区 1MB（`0x40400000..0x40500000`），每进程
  `PROC_MMAP_MAX=4` 个槽位，`mmap_next` 只增不减（munmap 不归还虚拟地址）。

### fs 层顺序读的限制（重要）

fs 没有 `lseek` 接口，`fs_read_ex()` 只能从 fd 当前位置顺序读。因此：

- `offset` 用“先读并丢弃 offset 字节，再读 len 字节”实现，成本 O(offset)，且
  **mmap 会消费该 fd 的读偏移**：映射完成后 fd 位于 `offset + 实际读到的字节数` 之后。
  需要继续顺序读该 fd 的调用方应另开一个句柄（ABI 目前没有 lseek / pread）。
- 文件上限 64KB（`FS_FILE_MAX`），所以一次文件 mmap 的读放大最多 64KB。

### 接口（`src/kernel/mm/filemap.h`，经 `include/mm.h` 导出）

- `int mm_file_map(base, len, offset, reader, ctx, prot)`：读页 + 映射核心，失败回滚全部
  已映射页；数据源用 `mm_file_reader_t` 注入（生产=fs 适配器，自检=内存源）。
- `void mm_unmap_range(base, len)`：munmap / 回滚共用的解除映射。
- `int mm_filemap_selftest(void)`：只读映射 3 页（含零填充）、offset 定位、MAP_PRIVATE
  页带 `PTE_COW` 且为私有副本（写页不影响文件源）、munmap 后页数归零、PMM/异常计数复原。
  自检**不调用** `vmm_cow_fault()`：那会给 `vmm_cow_copies` 计数并打印
  `vmm: first COW copy at ...`，而 `build.ps1` 用该行（地址 `0x401xxxx`）验证真正的
  fork COW，boot 自检不能抢走“第一次”。真实写缺页路径由既有用户态 fork 回归覆盖。

## 3. 改动文件

- `src/kernel/mm/vmm.c`、`src/kernel/mm/vmm.h`：新增 `vmm_user_page_count()`（RSS 扫描 +
  RAM 范围校验）。
- `src/kernel/mm/rss.c`、`src/kernel/mm/rss.h`（新增）：RSS 查询/打印/自检。
- `src/kernel/mm/filemap.c`、`src/kernel/mm/filemap.h`（新增）：文件映射核心/解除映射/自检。
- `src/kernel/mm/mm_syscall.c`：`SYS_MMAP` 参数解码 + 文件后端；`SYS_MUNMAP` 改用
  `mm_unmap_range()`；匿名路径行为不变。
- `src/kernel/mm/memtest.c`：`mm_selftest()` 末尾追加两项自检（打印上面的期望行）。
- `src/kernel/include/mm.h`：导出 `mm/rss.h`、`mm/filemap.h`。

不改 `build.ps1`、`include/abi.h`、`proc/**`、`fs/**`；文件读取只走 `fs_read_ex()` 接口。

## 4. 遗留（后续会话/集成）

1. **`res`/`ps` 用户界面未接线**（本会话按父会话决策不改 pkg/core 的文件）：`res` 走
   `SYS_RESINFO` 的固定 10×u32（`abi.h`），没有 RSS 字段；`ps` 命令不存在。后续方案：
   由集成阶段分配一个新 syscall（如 `SYS_MMINFO=80`，需 net/父会话改 `abi.h`）返回
   `{rss_pages, rss_total, ...}`，或让 `res` 追加一行；用户态打印用上面的 `ps` 行格式。
2. **没有 lseek/pread**：文件 mmap 的 offset 靠“读+丢弃”，且消费 fd 偏移（见上）。
3. **按需缺页未做**：文件页在 mmap 时一次性读入；大文件/稀疏映射要等缺页框架。
4. **MAP_SHARED / 可写回 / msync 未做**：写时 COW 只在 MAP_PRIVATE 语义下成立，不会
   回写文件。
5. **RSS 只覆盖用户布局区上界 `0x40500000`**；将来扩展 mmap 区或增加新用户段时要同步
   更新 `vmm_user_page_count()` 的范围（否则新段不计入 RSS）。
6. RSS 是展示用快照、非原子；若将来要做内存压力/回收决策，需要 per-CPU 锁或增量计数。
7. 不做 swap、真实硬件、GUI（任务边界）。
