# ng-WP1-5：线程模型 + 上下文切换 + 协作式调度（PASS）

## 结论

**PASS** —— ng 内核在 WP1-4 之后新增单核协作式调度：`thread_t`（tid、状态、
上下文、16 KiB heap 栈、wake_tick、TCB 链），`switch.S` 的 `switch_context`
只保存/恢复 rbx/rbp/r12–r15/rsp/rip，新线程经 `thread_entry` trampoline 首次
进入并在返回时 `thread_exit`。调度器是 FIFO round-robin（pick 队头、yield 放
队尾），`sched_start()` 创建 idle 线程（无 runnable/blocked 时 `hlt`），
`thread_exit()` 延迟到下一次别人调度时回收栈，`thread_sleep(ticks)` 用 TSC
做 deadline、`thread_wake(tid)` 显式唤醒，`sched_tick()` 为将来的 tick 抢占
留口。

内核自检创建 A/B/C 三个线程，各循环 3 次「打印 + yield」，输出严格交错为
`A0 B0 C0 A1 B1 C1 A2 B2 C2`（trace = `ABCABCABC`，20 次真实切换），三个线程
退出后全部回收（栈只剩 idle 的 1 个）；D 线程定时睡 5,000,000 TSC tick 后由
定时器唤醒（`wake_tsc >= deadline`），E 线程由主线程显式唤醒并验证
`EBAD/ENOENT` 错误码。串口 5 条出口行全部命中。

host 单测从 WP1-4 的 **166** 增至 **390 checks, 0 failures**（+224，纯状态机
fake 环境：fake 时钟/栈/switch，不切栈、不跑 QEMU）。旧 32 位
`build.ps1 -Test` 保持 **305/305**；`check-domains` / `check-deps` 0 违规。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/ng/thread.h` | `thread_context`（64 B，偏移 static_assert）、`thread_t`、状态/唤醒/错误码、`sched_core` 状态机与内核 API（**新**） |
| `src/ng/switch.S` | `switch_context`（保存 6 个 callee-saved + rsp/rip，`jmp` 恢复）、`thread_entry` 首帧 trampoline（**新**） |
| `src/ng/sched.c` | 纯状态机（ready/blocked/zombie 链、poll、reap）+ 内核胶水（yield/sleep/wake/exit/start/idle）（**新**） |
| `src/ng/sched_arch.h` | 6 个环境钩子：TSC、栈分配/释放、切换、新线程入口、idle（**新**） |
| `src/ng/sched_arch.c` | 内核实现：`rdtsc`、`kmalloc/kfree`、`switch_context`、`thread_entry`、`hlt`（**新**） |
| `src/ng/sched_selftest.c` / `.h` | A/B/C round-robin、exit 回收、sleep/wake 自检与 5 条出口行（**新**） |
| `src/ng/main.c` | WP1-4 之后跑 WP1-5 自检（**改**） |
| `tests/unit-ng/test_sched.c` | fake 环境的调度状态机测试（**新**） |
| `tests/unit-ng/unit_main.c` | 加 `test_sched()`（**改**） |
| `tools/unit-ng.ps1` | 加 `sched.c` + `test_sched.c`，输出 `N checks, M failures`（**改**） |
| `tools/build-ng.ps1` | 加 `switch.S`、`sched_arch.c`、`sched.c`、`sched_selftest.c`（13 → 17 步）（**改**） |
| `tools/run-ng.ps1` | 加 6 条固定行 + 1 条线程交错正则 + `ng: sched .* FAIL` / WP1-5 失败模式（**改**） |
| `docs/design/sched-ng.md` | 线程模型/切换/调度器/自检/fake 单测/边界（**新**） |
| `docs/design/decisions.md` | ADR-017（**追加**） |
| `docs/design/memory-ng.md` | §7 单核/锁的遗留说明更新为“WP1-5 协作式已加”（**改**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`src/ng` 的新
文件不进旧对象列表。

## 环境

- 分支 `ng`，基线 HEAD `55deabf`（WP1-4）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`），内核
  `--target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone
  -fno-stack-protector`；host 单测同一 clang 的默认 PE 目标。
- QEMU 11.0.0（`v11.0.0-12122-ga4bb4b10c9`），
  `qemu-system-x86_64 -kernel -m 256 -cpu max`。
- 时间源：`rdtsc`；自检的 5,000,000 tick 在 QEMU 下约几毫秒，不依赖
  TSC 频率换算。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
```

```text
=== build-ng (WP1-5) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe
  [1/17] entry.S
  [2/17] isr.S
  [3/17] switch.S
  [4/17] serial.c
  [5/17] memmap.c
  [6/17] pmm.c
  [7/17] heap.c
  [8/17] paging.c
  [9/17] gdt.c
  [10/17] trap.c
  [11/17] syscall.c
  [12/17] mem_selftest.c
  [13/17] sched_arch.c
  [14/17] sched.c
  [15/17] sched_selftest.c
  [16/17] main.c
  [17/17] link -> dist-ng/aster-ng.elf

ELF64: PASS (51360 bytes)
  magic    7f 45 4c 46
  class    2 (ELF64)
  machine  62 (EM_X86_64)
  type     2 (ET_EXEC)
  entry    0x0000000000101000
  phnum    6
segments: PASS
  4 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
  header   file offset 0x1000, length 24, checksum 0, 8-byte aligned
pvh-note: PASS
  entry    0x0000000000101000 (PT_NOTE, Xen type 18; QEMU -kernel)

build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

WP1-4 及其之前全部保持 PASS（pmm `free=65195`：内核 `.bss` 因 TCB 池/调度器
状态/新代码比 WP1-4 多 2 页；`tables=7` 不变）。WP1-5 段：

```text
ng: WP1-4 selftest PASS
ng: sched stack=16384 ctx=64 align=16 canary=ok
ng: sched create ok (3)
ng: thread A 0
ng: thread B 0
ng: thread C 0
ng: thread A 1
ng: thread B 1
ng: thread C 1
ng: thread A 2
ng: thread B 2
ng: thread C 2
ng: sched trace=ABCABCABC
ng: sched rounds=20 yields=17 reaps=3 stacks=1
ng: sched order ok (ABCABCABC)
ng: sched switch ok (rounds>=6)
ng: sched exit reap ok
ng: thread D sleep
ng: thread D woke
ng: sched sleep ticks=5000000 blocked=1 reason=1 late=356953
ng: sched sleep timer ok
ng: thread E sleep (explicit wake)
ng: thread E woken
ng: sched wake rc=111 blocked=1 reason=2
ng: sched wake explicit ok
ng: sched sleep/wake ok
ng: sched stacks=1 corrupt=0
ng: WP1-5 selftest PASS
------------------------------------------------
run-ng: matched 24/24 expected lines + 5 patterns
run-ng: PASS (all expected lines in 0.2s, cpu max)
```

证据解读：

- **交错**：`A0 B0 C0 A1 B1 C1 A2 B2 C2` 是因为创建顺序 A/B/C，`sched_start()`
  把 boot 线程放队尾后第一次 pick A；每轮 `yield` 依次让出，idle 不打印但参与
  轮转。内核把 trace 与字面量 `ABCABCABC` 逐字节比较，不一致直接
  `ng: sched switch FAIL`。
- **计数**：A/B/C 各 3 次（`iter_count` 断言）、`rounds=20`（真实
  `switch_context` 次数，>=6）、`yields=17`。
- **回收**：`reaps=3`、`stacks=1`（只剩 idle 的 16 KiB 栈）、zombie=0。
- **睡眠**：`blocked=1` 表示主线程确实观察到 D 进入 BLOCKED；`reason=1`
  = `THREAD_WAKE_TIMER`；`late=356953` tick 是唤醒时相对 deadline 的滞后
  （`wake_tsc >= deadline` 且 `wake_tsc - start >= 5,000,000`）。
- **显式唤醒**：`rc=111` 依次表示 `thread_wake(tid)==OK`、第二次
  `thread_wake(tid)==EBAD`、`thread_wake(0x7FFFFFFF)==ENOENT` 三项为真；
  `reason=2` = `THREAD_WAKE_EXPLICIT`。

`run-ng.ps1` 新增的期望：

```text
6 条固定行：
  ng: sched create ok (3)
  ng: sched order ok (ABCABCABC)
  ng: sched switch ok (rounds>=6)
  ng: sched exit reap ok
  ng: sched sleep/wake ok
  ng: WP1-5 selftest PASS
1 条正则（交错证据）：
  (?s)ng: thread A 0.*?ng: thread B 0.*?ng: thread C 0
失败早停模式新增：
  ng: sched .* FAIL / ng: WP1-5 selftest FAIL
```

### `-cpu qemu64`

```text
ng: smep=off smap=off
...
ng: sched create ok (3)
ng: thread A 0 / B 0 / C 0 ...
ng: sched trace=ABCABCABC
ng: sched rounds=20 yields=17 reaps=3 stacks=1
ng: sched sleep ticks=5000000 blocked=1 reason=1 late=346601
ng: sched sleep/wake ok
ng: WP1-5 selftest PASS
run-ng: matched 24/24 expected lines + 5 patterns
run-ng: PASS (all expected lines in 0.2s, cpu qemu64)
```

调度器不依赖 SMEP/SMAP，`qemu64` 与 `max` 的调度数字一致。

### host 单测

```text
powershell -File tools/unit-ng.ps1
```

```text
=== unit-ng (WP1-5 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
390 checks, 0 failures
unit-ng: PASS (390 checks, 0 failures)
```

新增 `test_sched.c`（fake 环境）覆盖：

- 状态机初始化、FIFO pick、4 轮 round-robin 顺序、每线程 4 次 switches；
- BLOCKED 不被 pick、状态机拒绝非法 block/wake；
- 定时 poll 边界（99/100/199/200）、`deadline=0` 永不超时、按 blocked 链
  顺序入 ready 队尾；
- 显式唤醒入队尾、`wakes_explicit` 计数、READY/UNUSED 上唤醒返回 `EBAD`；
- zombie/reap（含 `skip=current`）、reap 后 all 链摘除、`live_count` 语义；
- 3 线程 30 次 pick 的公平性（各 10 次）；
- `thread_create`：tid、16 KiB、栈基/栈顶/ctx.rsp 16 对齐、`ctx.rip` 入口、
  canary、FIFO 入队、`fake_allocs` 计数、TCB 槽上限 15（+boot=16）；
- `thread_sleep(0)` 等价 yield、无其它线程时不切换；
- `thread_wake` 的 `EBAD/ENOENT` 路径。

### panic 路径回归

`tools/build-ng.ps1 -PanicTest` + `tools/run-ng.ps1 -Panic` 仍 PASS：

```text
ng: panic selftest: triggering unrecoverable #UD
PANIC: vec=0x6 err=0x0 rip=0x107c78 cr2=0x108000
run-ng: PASS (panic path observed in 0.2s, cpu max)
```

随后重新执行普通 `build-ng.ps1`，产物回到正常镜像（51360 bytes）。

## 旧树回归与边界检查

```text
powershell -File build.ps1 -Test
...
=== compile: 1 compiled, 103 skipped ===
=== elapsed: 40.7s ===
=== TEST PASS (305/305) ===
```

```text
powershell -File tools/check-domains.ps1
checked: 205 files, 726 quoted includes
domain include violations: 0

powershell -File tools/check-deps.ps1
checked: 201 files, 650 quoted includes
module include violations: 0
```

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`src/ng` 的新
文件不进旧对象列表。

## 设计要点（详见 `docs/design/sched-ng.md`）

### 上下文布局与首帧

```text
struct thread_context (64B):
  0 rbx  8 rbp  16 r12  24 r13  32 r14  40 r15  48 rsp  56 rip
```

- `switch_context(old,new)`：保存 6 个 callee-saved；`leaq 8(%rsp)` 存
  “ret 之后”的 rsp，`(%rsp)`（return address）存 rip；恢复时
  `mov rsp` + `jmp *rip`。不 push/pop，不伪造返回地址。
- 新线程 `ctx.rip = thread_entry`、`ctx.rsp = stack_top`（16 对齐）。
  `thread_entry`：`cld` → `andq $-16,%rsp` → `call thread_bootstrap`；
  `thread_bootstrap()` 跑 `start(arg)`，再调 `thread_exit()`；真返回则 `ud2`。
- `thread.h` 用 `_Static_assert(__builtin_offsetof(...))` 把 8 个偏移和
  64 字节大小钉死，asm 布局漂移会编译失败。

### 调度器规则

- ready = FIFO（`ready_head/ready_tail`）；pick 队头，yield 放队尾。
- `sched_switch()`：回收 zombie（跳过 current）→ poll TSC → pick；
  `next==NULL && current RUNNING` 原地返回（不空切）；`next==current` 原地
  返回；否则 `current=next; switches++; switch_context()`。
- `thread_exit()`：canary 校验 → `zombie(self)` → switch（不返回）；
  栈由下一次调度回收，避免在自己正在用的栈上 `kfree`。
- `thread_sleep(ticks)`：`deadline = rdtsc()+ticks`；`ticks==0` 等价 yield；
  返回 `THREAD_WAKE_TIMER/EXPLICIT`。
- `sched_tick()`：只 poll 到期线程，供 WP1-6 的 tick IRQ 调用。

### 为什么协作式

ng 树目前没有接 PIT/PIC/APIC 的 IRQ 路径（WP1-3 只装了 IDT/异常与 syscall），
在没有 tick 的前提下，抢占式切换无法被验证。本 WP 把「到期唤醒」抽成
`sched_tick()`，TCB/状态机/回收路径与中断无关；WP1-6 接上 tick 后只需在 IRQ
里调用它并在安全点 yield，不需要改线程模型。

## 自检项清单

1. `thread_create`：3 个线程、16 KiB heap 栈、栈基/栈顶/ctx.rsp 16 对齐、
   tid 唯一、canary 有效。
2. `switch_context` 真实往返：A/B/C 交错 `ABCABCABC`、每线程 3 次、
   切换数 20（≥6）。
3. `thread_exit` 延迟回收：reaps=3、zombie=0、栈只剩 idle 的 1 个。
4. `thread_sleep`：先观察到 BLOCKED，再由 TSC 定时唤醒，`wake >= deadline`，
   原因 TIMER。
5. `thread_wake`：显式唤醒成功；重复唤醒 EBAD；不存在 tid ENOENT；
   原因 EXPLICIT。
6. idle：`sched_start()` 建 idle；自检结束 `stacks=1 corrupt=0`。
7. host 单测 390 checks / 0 failures（新增 224 项调度状态机检查）。
8. 旧 32 位 305/305、check-domains/check-deps 0 违规。

## 未做 / 遗留（按 WP 边界）

- **无抢占**：没有 IRQ tick 时间片；线程不让出就一直跑。
- **无 SMP**：全局 `sched`/`thread_pool`，无锁；每 CPU runqueue 留给后续。
- **无优先级/老化/时间片**：纯 FIFO round-robin。
- **无守护页**：2 MiB 恒等大页里 unmap 一页需要拆页表，本 WP 用栈底
  canary（只在 `thread_exit` 校验）。
- **等待队列**：BLOCKED 只有 sleep 一个原因；信号量/IPC 阻塞与 WP1-6 一起做。
- **用户态线程**：不做；`thread_t` 只表示内核线程。
- **TSC 频率**：不做校准，sleep 的 tick 是原始 TSC 增量。
- **TCB 池上限 16**：足够当前自检（boot + idle + 3 worker + D/E），服务/用户
  线程多了要扩池或改成 heap TCB。

## 出口

```text
powershell -File tools/build-ng.ps1    # PASS，dist-ng/aster-ng.elf（51360 bytes）
powershell -File tools/run-ng.ps1      # PASS，24 条期望行 + 5 条正则
powershell -File tools/unit-ng.ps1     # PASS，390 checks, 0 failures
powershell -File build.ps1 -Test       # PASS，305/305（旧 32 位）
powershell -File tools/check-domains.ps1   # 0 违规
```
