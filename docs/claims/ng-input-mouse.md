# ng-input-mouse claims：输入服务化 + PS/2 鼠标（WP5-0h）

- 状态：**PASS（待监工复核/统一 push）**
- 会话原名：WP5-0e（作废；`wps.md` 的 0e 是 FPU/SIMD，本项登记为 **WP5-0h**）
- 基线：分支 `ng`，进入时 HEAD `f9345b4`；执行期间其它会话把 HEAD 推进到
  `4e3c51e`（docs/WP5-0g），本提交只包含本项文件。
- 约束遵守：8h 浸泡 QEMU（PID 3420，QMP 11993/11994）**全程未碰**；QEMU 实测
  只用 11990；构建用 `obj-ng/wp5e/obj` + `obj-ng/wp5e/dist`，不抢 `obj-ng/`。

## 1. 结论

1. **键盘路由失效的根因不是 displayd 路由代码，而是“输入所有权”**：shell 与
   displayd 同时 `readkey`（syscall 30）从同一个内核队列 pop，谁先调用谁消费；
   shell 在提示符下忙等且不让出 CPU，QMP 注入的键被 shell 吃掉，WM 只拿到
   auto fallback（`via=auto`）。
2. 已实现**内核侧最小输入服务**：PS/2 键盘 + **PS/2 鼠标（aux/0xF4/0xF3/0xE8）**
   统一进一条事件队列；syscall **96 `input_event`** 非阻塞出队、syscall **97
   `input_claim`** 做所有权；displayd 启动即认领，shell 的 readkey 在桌面存活时
   返回 `-EAGAIN` 并让出一轮。
3. 轮询不再只依赖 idle：`ng_input_poll()` 挂在 **`sched_yield()`**（所有
   sleep/阻塞/空闲都会经过），`readkey` 对 boot 同步 ring3 演示兜底；`main`
   末尾不再 `hlt`（IF=0 会永久停 CPU），改为 `sched_yield()`。
4. displayd 消费 96：键盘 -> WM 热键副本 + 焦点窗口；鼠标 -> 移动 8×12 光标、
   左键按下命中窗口即给焦点；软件光标用 save-under，移动/合成不留残影。
5. QEMU 实测通过：shell 与桌面共存时 QMP `1` 产生 **3 条 `via=key`**，shell
   不再回显；`mouse_move` 前后两张 PPM **diff=192 px**，旧/新光标像素互换。
6. 回归：`run-ng` **161/161 expected + 76 patterns PASS**（原 159/159 全保留）；
   `unit-ng` **5594 checks, 0 failures**（新增 host 用例覆盖鼠标解码/队列/
   所有权/syscall 96/97）。

## 2. 缺陷复现（先证明，再修）

基线镜像（HEAD `f9345b4` 构建）在 shell 提示符下 `spawn /bin/sessiond`，等
sessiond 起 displayd/WM 后 QMP 注入 5 次 `1`：

```text
shell ready; spawning sessiond
WARN: session/wm ready not seen          <- shell 不让出 CPU，sessiond 饿死
ng: shell spawn ok (pid=21 path=/bin/sessiond)
ng> 11111                                <- 5 个键全被 shell 吃掉
PPM diff: 0 pixels                       <- 没有鼠标，光标不动
```

同一镜像里 `QMP 'a'` 在 ring3 hello 阶段能通、wp32/wp33 的 Tab/1 也能产生
`via=key`——说明“PS/2 键盘 + displayd 路由”本身可用；**坏在 shell 与桌面同时
活着时**（D0 冻结镜像的实测场景）：两个进程都从 `readkey` 的单一队列取键，
且 shell 忙等不让出，displayd/WM 永远轮不到。

`ng: mouse init ok` 在基线日志里不存在（WP2-3 只初始化键盘），QMP
`mouse_move` 前后 screendump 完全一致。

## 3. 根因

1. **单一队列/多消费者**：`ng_input_readkey()` 自己轮询 i8042 并从
   `g_input.ring` pop；displayd（`d_poll_input`）和 shell（主循环）都调它，
   pop 即消费。没有属主概念。
2. **shell 忙等不让出**：`sh_idle_hint()` 只 `pause`，没有 syscall/sleep；
   displayd 的 `port_recv` 超时轮询排不上，桌面被饿死。
3. **只靠 idle 轮询不可靠**：`ng_input_poll()` 原先只在 main 末尾循环调用，
   而 main 要等 shell 退出才走到；桌面运行期的键只能由某个 `readkey` 顺带轮询。
4. **鼠标完全没实现**：`ps2_init()` 只做键盘 0xAE/0xF4；没有 aux 0xA8、
   没有 0xF6/0xF3/0xE8/0xF4，没有 3 字节包解码。
5. 修复过程中还发现一个**不能碰的边界**：boot/init（pid 1）的同步 ring3
   演示没有独立 `syscall_top`/`pml4`，在它的 syscall 里 `thread_sleep(0)` 让出
   CPU 会让调度器把 CR3 切回内核表，SYSRET 回用户态取指 #PF
   （`cr2=-11` 的假象）。因此 yield 只对非 boot 线程做。

## 4. 修复

### 4.1 内核输入服务（`src/drivers/input/ps2.c`、`input.h`、`ng/wp23_selftest.c`）

- `input_queue`：64 项 `input_packet`（type/key/x/y/buttons/scancode/flags），
  FIFO + `lost`；键盘按下事件既进旧 `input_state.ring`（readkey 兼容）也进
  统一队列（syscall 96）。
- `input_mouse`：3 字节包状态机；byte0 bit0..2 按键、bit3 同步、bit4/5 符号、
  bit6/7 溢出；`input_mouse_decode()` 把 PS/2 的 y 向上翻成屏幕坐标系；
  `input_mouse_feed()` 在包边界忽略 0xFA/0xFE，按键变化发 BUTTON，位移非 0
  发 MOVE。
- `ps2_init_mouse()`：0xA7 -> 读配置(0x20)/写回(0x60，开 IRQ12/aux clock) ->
  0xA8 -> 0xF6 -> 0xF3(100) -> 0xE8(3) -> 0xF4；逐条等 ACK，RESEND 重试。
- `input_owner`：pid + claims/denials/releases 纯状态机；`owner_alive` 由内核
  注入，属主已死自动回收。
- `ng_input_poll()`：读 0x64 状态；bit5=1 走鼠标、否则键盘；每次最多 64 字节。
- 诊断：`ng: input poll key/mouse ok (... polls=.. keys=.. aux=.. idle=..)`，
  claim 时打印 `ng: input claim ok (pid=.. polls=.. keys=.. aux=.. idle=..)`。

### 4.2 syscall 96/97（DRAFT ABI 1.1，**不改 docs/abi**）

```text
96 input_event(struct input_packet *out) -> 0 / -EAGAIN / -EFAULT
   struct input_packet { u32 type; u32 key; i32 x; i32 y;
                         u32 buttons; u32 scancode; u32 flags; }  // 28 B
   type: 1=KEY 2=MOVE 3=BUTTON；MOVE 的 x/y 是相对位移（右/下为正）
97 input_claim(pid, flags) -> 0 / -EBUSY(-16) / -EPERM(-1)
   pid=0 表示当前进程；flags bit0=释放
```

dispatch 侧在 `copyout()` 前后做用户页校验；`readkey`（30）语义保持：
`(flags<<24)|(ascii<<16)|scancode` / `-EAGAIN`，但非属主在桌面存活时返回
`-EAGAIN` 并 `thread_sleep(0)` 让出；boot 线程（`THREAD_F_BOOT`）例外，只做
兜底轮询不 yield。

### 4.3 轮询点（`src/ng/sched.c`、`src/ng/main.c`）

```c
void sched_yield(void) {
#ifndef NG_HOST_TEST
    ng_input_poll();      /* WP5-0h：所有 sleep/阻塞/空闲都经过这里 */
#endif
    ...
}
```

- `sched_idle_body()` 不再承担输入轮询；其 hlt 仅在所有线程都无 ready/blocked
  时发生（桌面运行时 displayd 总有 sleep 期限，实际走不到）。
- `main.c` 末尾 `ng_input_poll(); hlt;` -> `sched_yield();`（IF=0 的 hlt 会永久
  停 CPU；tick IRQ 的 HLT idle 留给 WP5-0f）。
- 新增 `sched_idle_runs()` 只读 accessor（不改调度结构）。

### 4.4 displayd（`src/desktop/display/displayd.c`）

- 启动 `input_claim(0)`，打印 `ng: displayd input claim ok (pid=..)`；失败退出。
- 主循环超时分支改调 syscall 96：KEY -> 原热键副本 + 焦点转发；MOVE -> 移动
  8×12 软件光标；BUTTON 左键按下 -> 命中测试（z-order 从上到下）给焦点。
- 光标 save-under：paint 前保存 8×12 背景，erase 恢复；合成前 erase、合成后
  paint；初始化时先用光标左侧像素把右下角旧光标区域刷成背景，避免上一次
  displayd 被 kill 后留下的白色残影被当成背景。
- 证据行：`ng: displayd mouse move ok (x=.. y=.. dx=.. dy=.. n=..)`、
  `ng: displayd mouse click ok (x=.. y=.. focus=.. n=..)`。

## 5. QEMU 证据

环境：`qemu-system-x86_64`，`-cpu max -m 256 -vga std`，QMP 11990，串口文件；
构建 `-ObjDir obj-ng/wp5e/obj -DistDir obj-ng/wp5e/dist`。

### 5.1 idle/hlt：idle=0 + polls 增长

```text
ng: input claim ok (pid=8  polls=5000017  keys=0  aux=0 idle=0)   [WP3-1 displayd]
ng: input claim ok (pid=11 polls=12859726 keys=0  aux=0 idle=0)   [WP3-2 displayd]
ng: input claim ok (pid=16 polls=23695067 keys=0  aux=0 idle=0)   [WP3-3 displayd]
ng: input claim ok (pid=22 polls=31027775 keys=40 aux=0 idle=0)   [shell 里新起 sessiond]
```

`polls` 随每次让出增长，`idle` 始终为 0：idle 线程从未走到 hlt；输入不依赖
idle 是否被调度到。`main` 末尾也已改成 `sched_yield()`，不再 IF=0 hlt。

### 5.2 shell/桌面共存：QMP `1` -> 3× `via=key`，shell 不再吃键

场景：boot 到 `ng: shell ready` -> shell 里 `spawn /bin/sessiond` ->
等 displayd 认领、WM 注册热键 -> 立即注入 `1` ×3（每次 80ms）。

```text
ng: input claim ok (pid=22 polls=31027775 keys=40 aux=0 idle=0)
ng: displayd input claim ok (pid=22)
ng: displayd hotkey ok (key=0x31 scancode=0x2)
ng: displayd input ok (key=1 scancode=0x2)
ng: wm launch req ok (app=/bin/demo via=key)
ng: displayd hotkey ok (key=0x31 scancode=0x2)
ng: displayd input ok (key=1 scancode=0x2)
ng: wm launch req ok (app=/bin/demo via=key)
ng: displayd hotkey ok (key=0x31 scancode=0x2)
ng: displayd input ok (key=1 scancode=0x2)
ng: wm launch req ok (app=/bin/demo via=key)
EVIDENCE via=key: True
EVIDENCE shell stole key: False
```

（对照基线：同一路径 `ng> 11111`，无任何 `via=key`。）

### 5.3 鼠标：init + QMP 相对移动 + 两帧 PPM

```text
ng: mouse init ok (aux=0x60 irq12=1 f4/f3/e8)
ng: input claim ok (pid=21 polls=31059996 keys=40 aux=0 idle=0)
ng: displayd input claim ok (pid=21)
ng: input poll mouse ok (packets=1 rounds=31086589 idle=0)
ng: input poll mouse ok (packets=2 rounds=31086589 idle=0)
ng: input poll mouse ok (packets=3 rounds=31086589 idle=0)
ng: displayd mouse move ok (x=1141 y=657 dx=127 dy=127 n=1)
ng: displayd mouse move ok (x=1014 y=584 dx=127 dy=73 n=2)
ng: displayd mouse move ok (x=968 y=584 dx=46 dy=0 n=3)
```

QMP HMP `mouse_move -300 -200` 前后 two screendumps（1280×800 PPM）：

```text
PPM diff: 192 pixels bbox=(968,585)-(1275,796)
pixel old-cursor(1268,784): before=#ffffff after=#101820
pixel new-cursor(968,584):  before=#101820 after=#ffffff
EVIDENCE cursor moved: True
```

192 = 旧 8×12 擦除 + 新 8×12 绘制；旧光标像素恢复成背景、新位置画出白色光标。

### 5.4 回归

```text
run-ng: matched 161/161 expected lines + 76 patterns
run-ng: PASS (all expected lines in 7.2s, cpu max)

unit-ng: PASS (5594 checks, 0 failures)
```

`run-ng` 新增并要求 `ng: mouse init ok (aux=0x60 irq12=1 f4/f3/e8)` 与
`ng: displayd input claim ok (pid=..)`；原 159 条全部保留（161 = 159+2）。

## 6. 未修 / 限制

1. **没有 IRQ1/IRQ12**：仍轮询，只是把点从“idle/readkey”移到所有调度点；若
   所有线程都纯计算且不 syscall/yield，输入延迟无界（WP5-0g 的 ring3 抢占/
   WP5-0f 的 tick 会补；本 WP 不做 IRQ 路由）。
2. **鼠标事件不转发给客户端**：只驱动 displayd 光标和点击焦点；没有 drag/
   double-click/wheel（PS/2 3 字节包无滚轮），也没有给应用层的 mouse INPUT
   协议消息。
3. **光标是 8×12 软件光标**（沿用 WP3-1 形状），像素级 save-under；大面积
   桌面重绘时依赖 erase/paint 顺序。
4. **96/97 只是 DRAFT**：按约定不改 `docs/abi/**`，ABI 1.1 文本由后续 WP 统一；
   本文件记录编号、布局、语义和错误码。
5. **所有权靠进程表**：属主 pid 的槽位被回收后才算死；ZOMBIE 视为死。
   displayd 认领失败会退出（防止双 displayd 抢输入）。
6. **boot/init 的同步 ring3 演示**不能 yield（无独立 syscall 栈/PML4）；readkey
   对它保留兜底轮询。
7. **QMP 注入是时序敏感**：`sessiond` 从 shell 起、WM auto-launch 之前有窗口；
   脚本在 `hotkey registered` 后立即注入 3 次。run-ng 的 wp32/wp33 路径不受影响。
8. 真实硬件/其它 i8042 兼容芯片未测；QEMU stdvga 路径已实测。

## 7. 复跑

```powershell
# 构建（独立目录，不抢 obj-ng/）
powershell -File tools/build-ng.ps1 -ObjDir obj-ng/wp5e/obj -DistDir obj-ng/wp5e/dist

# 原有门禁
powershell -File tools/run-ng.ps1 -ElfPath obj-ng/wp5e/dist/aster-ng.elf `
    -DiskImg obj-ng/wp5e/dist/disk.img -SerialLog obj-ng/wp5e/serial-runng-final.log
powershell -File tools/unit-ng.ps1 -ObjDir obj-ng/wp5e/unit

# 手工输入证据（QMP 11990；脚本在 obj-ng/wp5e/drive.ps1，构建产物不入库）
powershell -File obj-ng/wp5e/drive.ps1 -Mode key   -Tag wpkey2
powershell -File obj-ng/wp5e/drive.ps1 -Mode mouse -Tag wpmouse2
```

## 8. 文件清单

| 路径 | 内容 |
|---|---|
| `src/drivers/input/ps2.c` | 鼠标解码/队列/所有权纯逻辑 + `ps2_init_mouse` + `input_feed_ev` |
| `src/drivers/include/input.h` | 新结构/常量/API（`input_packet`、`input_queue`、`input_mouse`、`input_owner`） |
| `src/ng/wp23_selftest.c` | 内核输入服务状态、`ng_input_poll`、96/97 实现、鼠标初始化/证据 |
| `src/ng/wp23.h` | 服务 API 声明 |
| `src/ng/syscall.c/h` | 96/97 分发 + `copyout` 校验 + 注释/ENOSYS 号段调整 |
| `src/ng/sched.c` | `sched_yield()` 轮询点 + `sched_idle_runs()` accessor |
| `src/ng/thread.h` | accessor 声明 |
| `src/ng/main.c` | 末尾 `sched_yield()`（删 IF=0 hlt） |
| `src/desktop/display/displayd.c` | claim、96 消费、热键/焦点、光标 save-under、点击焦点 |
| `tests/unit-ng/test_drivers.c` | 鼠标解码/状态机/队列/所有权/aux 初始化 host 用例 |
| `tests/unit-ng/test_syscall.c` | 96/97 分发 + copyout/EFAULT/EAGAIN 用例 |
| `tests/unit-ng/fake_ng_env.c/h` | fake 96/97 |
| `tools/run-ng.ps1` | 新增 mouse init / displayd claim 证据行 |
| `docs/claims/ng-input-mouse.md` | 本文件 |
| `docs/design/display-server-ng.md` | §15 输入服务化/光标补充 |
| `docs/design/sched-ng.md` | §8 调度点轮询补充 |
