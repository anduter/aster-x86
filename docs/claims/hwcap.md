# hwcap 会话 claim（P5：硬件 capability MMIO / IRQ / DMA）

- 批次：2026-10-31 三层架构 P5 前置（`docs/design/three-layer.md` §3.3/§6）
- 基线 HEAD：`277ea4e`（299 项检查 / `ci -Full` 299/299 ×2 / 工作区干净）
- 允许改动范围：`src/kernel/dev/**`（新增 `hwcap.c|h`）、`src/kernel/svc/**`
  （syscall 89–92 + cap 后端）、`src/kernel/include/dev.h`、
  `src/kernel/mm/**`（PTE/teardown 映射辅助）、`src/kernel/proc/trap/irq.c`
  （IRQ 投递/回收钩子）、`build.ps1`（只加检查项）、`docs/claims/hwcap.md`
- 未改：`include/abi.h`（89–92 编号已定）、`core/**`、`net/**`、`fs/**`、
  `security/**`、`proc/**`（除 `trap/irq.c` 的钩子）

## 1. 目标与结论

P1 的 `SYS_CAP_GRANT` 只把 `(type,id,rights)` 记在端口上；本会话补上真实
硬件后端，让 ring3 驱动能拿到 MMIO/IRQ/DMA：

| cap | Linux 类比 | 本实现 |
|---|---|---|
| `CAP_MMIO(2)` | `mmap(/dev/mem)` | `SYS_MMIO_MAP(90)` 把已授权物理区间映射进调用者用户空间（页对齐、RW/RO、NX、PCD/PWT） |
| `CAP_IRQ(3)` | `request_irq` + `read(eventfd)` | `SYS_CAP_GRANT` 绑定 IRQ 到端口；IRQ 到达投递事件并计数；`SYS_IRQ_WAIT(89)` 有界等待、成功即 ack |
| `CAP_DMA(4)` | `dma_alloc_coherent` | `SYS_DMA_ALLOC(91)` 分配物理连续页（≤64KB）并映射；`SYS_DMA_FREE(92)` 解映射释放 |

结果：MMIO 读写、只读/NX 强制、IRQ 绑定/等待/超时/EINTR/风暴保护、DMA
分配/写图案/释放/重复释放拒绝全部有内核侧动态证据；现有 299 项全保留，
总检查数 **305**。

## 2. 改动文件清单

| 文件 | 改动 |
|---|---|
| `src/kernel/dev/hwcap.h` | 新增：硬件 cap 契约（VA 窗口、上限、事件类型、内核 API） |
| `src/kernel/dev/hwcap.c` | 新增：MMIO 映射、IRQ 绑定/投递/等待/回收、DMA 分配/释放、内核自检 |
| `src/kernel/include/dev.h` | 导出 `dev/hwcap.h` |
| `src/kernel/svc/ipc.h` | `ipc_cap_grant_ex`（带 MMIO 页数）、`ipc_cap_lookup/revoke`、`ipc_port_post` |
| `src/kernel/svc/ipc.c` | grant 表加 `len_pages`；内核事件入队；端口释放回调解绑 IRQ |
| `src/kernel/svc/svc_syscall.c` | 认领并实现 89–92；`CAP_GRANT` 解析 MMIO 长度/IRQ 绑定回滚 |
| `src/kernel/svc/supervisor.c` | `-Test` 启动时在 `ipc_selftest()` 后跑 `hwcap_selftest()` |
| `src/kernel/proc/trap/irq.c` | `irq_dispatch` 先问 `hwcap_irq_deliver()`；时钟每 10 tick `hwcap_poll()` |
| `src/kernel/mm/vmm.h` | 新增软件位 `PTE_HWCAP` |
| `src/kernel/mm/vmm.c` | 拆除用户区时，`PTE_HWCAP` 设备物理页不交还 PMM |
| `src/kernel/mm/pmm.h/.c` | 新增 `pmm_range_managed()`（判断物理页是否归 PMM） |
| `build.ps1` | 新增 6 条 `hwcap:` 检查（299 → 305） |
| `docs/claims/hwcap.md` | 本文件 |

## 3. 接口与语义

### 3.1 `SYS_CAP_GRANT(84)` 的硬件扩展（未改 abi.h）

原有参数仍是 `ebx=port, ecx=type, edx=id, esi=rights`。为了表达 MMIO 的
“基址+长度”而 ABI 只有 3 个参数位，本实现用 `esi` 的高 16 位传页数：

```text
esi = (len_pages << 16) | rights      # rights 仍只有低 4 位 READ/WRITE/GRANT/ADMIN
len_pages == 0 -> 1 页（老的三参 cap_grant 兼容）
```

| type | id 含义 | 额外校验 |
|---|---|---|
| `CAP_MMIO` | 物理基址 | root；非 0、页对齐、`len_pages <= 256`（1MB） |
| `CAP_IRQ` | IRQ 号 | root；`0..15`，且该线没有内核 handler（不抢 0/1/4/11/12） |
| `CAP_DMA` | 允许的最大字节数（配额） | root；非 0、`>= 实际分配` |

`CAP_IRQ` grant 成功后立刻 `hwcap_irq_bind()`；绑定失败（线被占用/已绑定/
非法）会撤销 grant 并让 `SYS_CAP_GRANT` 返回 `-1`，不会留下“有 cap 但收不到
中断”的半状态。

### 3.2 `SYS_MMIO_MAP(90)`

```text
ebx = 物理基址（= grant 的 id）
ecx = &va（u32*，成功回填映射地址）
返回 0 / -1
```

- 只允许**拥有该 grant 的端口 owner** 使用；grant 本身只能 root 授出。
- 物理基址必须页对齐；长度来自 grant（默认 1 页，最大 256 页）。
- 虚拟地址从 `0x41000000..0x42000000` 的 16MB 窗口里 first-fit 分配（在
  user_layout 的 `0x40500000` 之上，不参与 fork 克隆）。
- PTE 强制 `USER|PRESENT|PCD|PWT|NX`；grant 没有 `WRITE` 时 PTE 只读。
- 同一 owner 对同一基址只允许映射一次；未授权基址/0 基址/重复映射拒绝。
- 没有 `SYS_MMIO_UNMAP`：映射随进程地址空间拆除；PTE_HWCAP 保证设备物理页
  不会被 `pmm_free_pages()` 误回收（假 MMIO 的 PMM 页仍会正常归还）。

### 3.3 `SYS_IRQ_WAIT(89)`

```text
ebx = irq, ecx = timeout_ticks（0 = 100 tick / 1s，上限 60000）
返回 irq（>=0，事件带 IRQ 号） / -EAGAIN（超时） / -EINTR（未屏蔽信号） / -1
```

- 绑定：`SYS_CAP_GRANT(port, CAP_IRQ, irq)`，owner 由端口决定；绑定后
  `irq_unmask(irq)`。
- 投递：`irq_dispatch` 先调 `hwcap_irq_deliver()`；命中的线不再走默认
  handler/自动掩线。每次投递：`count++`、向绑定端口投递 16B IPC 头 +
  12B payload `{irq, seq, dropped}`（类型 `HWCAP_EV_IRQ="IRQ1"`），队列满
  时消息丢弃但计数保留。
- ack：`SYS_IRQ_WAIT` 成功返回即消费一个事件；`count==0` 时清 pending 并
  重新放行 IRQ。
- 风暴保护：pending 未 ack 时每多一个事件 `dropped++`，达到 8 就
  `irq_mask(irq)`；ack 清空后重新 `irq_unmask`。这样用户态不 ack 也不会
  被一条电平线打断死。
- 生命周期：显式 `ipc_port_release()` 立即解绑并恢复默认屏蔽；进程退出/
  端口被回收由时钟每 10 tick（100ms）的 `hwcap_poll()` 兜底解绑。
- 只支持传统 IRQ 0–15（GSI 编码在 8259 上就是 IRQ 号）；拒绝级联线 2、
  伪中断线 7/15，以及已有内核 handler 的线。GSI >= 16 的 MSI/IOAPIC 直接
  投递留给 P6 netd + vIOMMU。

### 3.4 `SYS_DMA_ALLOC(91)` / `SYS_DMA_FREE(92)`

```text
ALLOC: ebx = len（1..65536）, ecx = align（0 或 4096）, edx = &va（u32*，可为 0）
       返回 cap id（>0） / -1
FREE:  ebx = cap
       返回 0 / -1
```

- 必须持有 `CAP_DMA` grant，且 `grant.id >= len`、rights 含 READ|WRITE。
- `pmm_alloc_pages()` 分配物理连续页（页对齐、≤64KB），逐页映射到
  `0x41000000` 窗口，PTE `USER|WRITE|NX`（cacheable，x86 一致 DMA）。
- cap id = `(generation << 8) | slot`，每次分配 generation 递增；释放/重复
  释放用 generation 拒绝旧句柄。
- `SYS_DMA_FREE` 先校验 cap id + owner + 当前地址空间里的 PTE 仍指向记录
  的物理页（防止 pid 复用误释放别人的页），再解映射 + `free_pages()`。
- 坏 cap/越界长度/非 4KB 对齐/无 grant/重复释放全部 `-1`。

### 3.5 `SYS_MMIO_MAP` 的 cap 身份与权限

`SYS_CAP_GRANT` 的硬件类型仍只允许 root（`CAP_SYS_ADMIN` 尚未落地，见 P1
claims 设计问题 #5）。root 可以把 cap 授给一个非 root 进程拥有的端口，之后
该 owner 可以 `SYS_MMIO_MAP`/`SYS_IRQ_WAIT`/`SYS_DMA_ALLOC`——这是有意
的“root 授权给驱动进程”，不是普通用户可自行提权：没有 root grant 时
`ipc_cap_lookup()` 找不到任何 cap，四个 syscall 全部 `-1`。

## 4. 期望自检行（`-Test` 构建，supervisor 线程）

```text
hwcap: mmio map ok (cap=0x<phys> len=4096 rw=1 ro=1 nx=1)
hwcap: mmio bad cap rejected
hwcap: irq bind/wait ok (irq=5 guard=8)
hwcap: dma alloc/free ok (len=8192 align=4096)
hwcap: dma double free rejected
hwcap: selftest PASS
```

`build.ps1` 检查项（299 → 305）：

```powershell
@{ Name = 'hwcap MMIO 映射/读写';      Ok = ($out.Contains('hwcap: mmio map ok')) },
@{ Name = 'hwcap MMIO 坏 cap 拒绝';    Ok = ($out.Contains('hwcap: mmio bad cap rejected')) },
@{ Name = 'hwcap IRQ 绑定/等待';       Ok = ($out.Contains('hwcap: irq bind/wait ok')) },
@{ Name = 'hwcap DMA 分配/释放';       Ok = ($out.Contains('hwcap: dma alloc/free ok')) },
@{ Name = 'hwcap DMA 重复释放拒绝';    Ok = ($out.Contains('hwcap: dma double free rejected')) },
@{ Name = 'hwcap 自检通过';            Ok = ($out.Contains('hwcap: selftest PASS')) }
```

## 5. 动态证据（自检覆盖）

1. **MMIO**：从 PMM 分配 2 页“假 MMIO”（第一页 RW、第二页 RO grant）。
   - 写入 64B 图案再读回一致；`vmm_get_phys(va)==fake`。
   - PTE 检查：`PRESENT|USER|WRITE|NOEXEC|PCD|PWT`；RO 页没有 `WRITE` 且有
     `NOEXEC`；对 RO 页 `copy_to_user` 必须失败（只读强制）。
   - 未授权基址、0 基址、同 owner 重复映射全部 `-1`。
2. **IRQ**：绑定 IRQ5（QEMU 未使用线），用 `int $0x25` 软件触发真实 IDT
   桩（不碰 0/1/4/11/12）：
   - 事件帧到达端口，payload `irq=5`；
   - `SYS_IRQ_WAIT` 返回 5；无事件 5 tick 后 `-EAGAIN`；置
     `pending_sig=SIGINT` 后 `-EINTR`；
   - 连续 9 次触发未 ack：`dropped>=8` 且 IRQ 被掩；9 次 wait 清空后重新
     放行；
   - 释放端口后绑定消失（`irq_name` 恢复 unused / 线重新屏蔽）。
3. **DMA**：`CAP_DMA` 配额 65536，分配 8192 字节：
   - PTE `PRESENT|USER|WRITE|NX`，物理页 4KB 对齐；
   - 写图案/读回一致；
   - FREE 返回 0，第二次 FREE 返回 -1（重复释放拒绝）；
   - 超过 64KB、align=512、无 grant/伪造 cap 全部拒绝。
4. **未授权路径**：MMIO/DMA/IRQ 的 cap 查找都要求 owner；没有 root grant
   时四个 syscall 返回 -1（自检里用坏 cap/未授权基址覆盖）。

## 6. 验证记录

### 6.1 回归

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name hwcap -Test
# === TEST PASS (305/305) ===   40.3s   （2026-10-31 01:41，clean tree）
# === TEST PASS (305/305) ===   41.7s   （stale-cap/PTE 反查加固后）
```

另有一次 run 出现 7 项既有用户态/并发偶发失败（`fs: fat16 smp lock FAIL`、
`ipc: ring3 ping/reply FAIL` 等，`hwcap:` 6 行本身全 PASS）；同一提交立即重跑
**305/305 PASS**。与 `docs/parallel-sessions.md` 记录的 SMP/用户态偶发同族，
本会话未改这些模块。

### 6.2 静态

- `tools\check-deps.ps1`：**0 violations**（201 files / 650 includes）。
- `tools\unit.ps1`：**UNIT PASS (10/10)**，520 checks。

### 6.3 CI（clean tree，`-Full -StrictBom`）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci.ps1 -Full -StrictBom
```

- 提交 `621b2b9`（实现 + claims 初稿）：
  - 第 1 次：**CI PASS**，118.2s；静态 5 项全 PASS（worktree clean / BOM+parse
    39 files / no artifacts 359 / check-deps 0 / unit 10 suites）；`build.ps1`
    PASS；`-Test` **305/305 ×2**（53.8s / 40.0s）；结束后 worktree clean。
  - 第 2 次：**CI PASS**，118.1s；同样静态 5 项 PASS、`-Test` **305/305 ×2**
    （53.5s / 40.2s）、worktree clean。
- 提交 `a48d58a`（IRQ 绑定/端口复用加固 + 失效 cap 清理）：
  - 第 1 次：**CI PASS**，118.2s；`-Test` **305/305 ×2**（53.6s / 40.0s）。
  - 第 2 次：**CI PASS**，119.7s；`-Test` **305/305 ×2**（55.3s / 39.8s）。
- `305 = 299 + 6`（本会话新增 6 条 `hwcap:` 检查，未删除/修改既有 299 项）。
- 最终代码提交为 `a48d58a`；本文件在其后单独提交（仅文档）。

## 7. 串口摘录（`serial-hwcap.log`，305/305 那次）

```text
ipc: selftest PASS
hwcap: mmio map ok (cap=0x<phys> len=4096 rw=1 ro=1 nx=1)
hwcap: mmio bad cap rejected
hwcap: irq bind/wait ok (irq=5 guard=8)
hwcap: dma alloc/free ok (len=8192 align=4096)
hwcap: dma double free rejected
hwcap: selftest PASS
```

## 8. 安全边界与已知风险

### 8.1 已落地

- 只有 root 能通过 `SYS_CAP_GRANT` 获取 `CAP_MMIO/IRQ/DMA`；`CAP_SYS_ADMIN`
  仍未实现，root 是唯一授权源。
- 使用侧只认“grant 所在端口的 owner”，跨进程不能拿别人的 cap id 映射/释放。
- MMIO：NX 强制；只读 grant 的 PTE 真只读；PCD/PWT；1MB 上限；重复映射拒绝。
- IRQ：只绑未使用线；不抢内核 handler 的线；用户态不 ack 时计数 + 8 次后
  掩线；端口释放/进程退出解绑并恢复默认屏蔽。
- DMA：≤64KB、4KB 对齐、配额来自 grant；释放校验 owner + PTE 反查，
  防止 pid 复用误释放。
- vmm 拆除用户区时不会把设备 MMIO 物理页交给 PMM（PTE_HWCAP +
  `pmm_range_managed()`）。

### 8.2 无 IOMMU 的 DMA 风险（必须写清）

**没有 IOMMU 时，设备 DMA 仍可能越界访问任意物理内存。** 本会话只保证
`SYS_DMA_ALLOC` 返回的缓冲是内核分配的物理连续页、映射/释放受 cap 控制；
设备（例如 e1000）拿到缓冲地址后仍可发起越界 DMA，内核无法拦截。因此：

- ring3 **netd 必须等 vIOMMU**（设计 §6 / P6）；在此之前 netd 不进 ring3，
  或只允许使用受控的 bounce buffer + 设备侧限制。
- blkd/inputd 的 DMA 同样受此限制；P5 阶段只能作为“cap 机制可用”的证据，
  不能宣称安全隔离。
- QEMU 验证 P6 时要开 `-device intel-iommu`；`drivers=ring3` 在无 IOMMU 的
  机器上只应启用 MMIO/IRQ 驱动，DMA 驱动保持 ring0 或受限模式。

### 8.3 其它边界

- `SYS_MMIO_MAP` 只校验基址页对齐/长度/grant，不校验物理地址是否是设备
  BAR（root 本来就能授任意物理段）；这是 root 权限的语义，不是漏洞。
- PTE_HWCAP 对“恰好落在 PMM 管理区内的真实 MMIO”会按 PMM 页处理；QEMU
  的设备 BAR 都在 RAM 之上，实际不会触发。测试用假 MMIO 就是 PMM 页，
  所以这条路径有覆盖。
- IRQ 事件端口队列满时消息丢弃，但 `count` 仍保留；只用 `RECV` 不用
  `IRQ_WAIT` 的驱动会在 8 次后遇到线被掩（文档已说明 `IRQ_WAIT` 才是 ack）。

## 9. 设计问题（未改契约，记录待评审）

1. **ABI 缺参数位**：`SYS_MMIO_MAP` 的 cap 身份用物理基址、MMIO 长度塞在
   `SYS_CAP_GRANT` 的 `esi` 高 16 位、DMA 的 VA 用 `edx` 回填。这些都是
   不改 `abi.h` 的兼容扩展，但 ABI 1.6 应该显式加 `len`/`out_va` 参数。
2. **没有 `SYS_MMIO_UNMAP`**：MMIO 映射只能随进程退出回收；长驻驱动反复
   map 不同 BAR 会消耗 VA 窗口和 PTE。建议 ABI 1.6 加 unmap，或把映射挂到
   fd/handle 上。
3. **`CAP_SYS_ADMIN` 未落地**：三类硬件 cap 仍只能 root 授；P5 的 ring3
   驱动由 supervisor（root）代授，权限模型还没细到“服务级授权”。
4. **GSI/MSI 未支持**：当前只覆盖 8259 的 0–15；IOAPIC GSI >=16、MSI/MSI-X
   要等 netd/vIOMMU 会话。`irq_name()` 的 “unused” 判定也依赖 8259 槽位，
   不是通用 IRQ 描述符。
5. **进程退出靠轮询回收**：`proc/**` 不在本会话可改范围，没有 exit 回调；
   `hwcap_poll()` 100ms 兜底。PID 复用窗口内用 PTE 反查兜底，但更干净的做法
   是 proc 退出时回调 cap 模块（或把 cap 挂到 port 生命周期）。
6. **DMA 没有 bounce buffer / cache 维护 API**：x86 一致 DMA 下 WB 可工作；
   若以后支持非一致设备，需要 `dma_sync_*`。当前 `flags` 是 cacheable。
7. **自检只在内核态**：`src/user/ulib.*` 不在本会话允许路径，没有新增
   ring3 wrapper/命令；89–92 的 handler 是薄封装，动态证据来自
   `hwcap_selftest()`（真实 `SYS_CAP_GRANT`/端口/页表/IDT 路径）。

## 10. 遗留

1. 8h 浸泡未做（P5 只要求机制 + 回归；设计 P5 起统一浸泡）。
2. ring3 驱动接入（blkd/inputd）未做：本会话只交付 cap 后端和自检；
   `drivers=ring3` 的 blkd/inputd 迁移是下一个会话。
3. `-smp 4` 下 IRQ 投递/回收的并发压力未单独跑；锁顺序为
   `hwcap_lock -> ipc_lock`，`ipc_port_release` 在释放 ipc 锁后才回调
   hwcap，未发现反向嵌套。
4. 文档：`docs/api.md` 的 ABI 1.5 说明未补 89–92（不在本会话范围）。
5. 8h soak 与 netd 的 vIOMMU 验证按设计 P6 执行。
