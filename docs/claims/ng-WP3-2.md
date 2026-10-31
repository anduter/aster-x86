# ng-WP3-2：shell/WM（L2b）+ display 协议 v2（PASS）

## 结论

**PASS** —— E3 / WP3-2 出口达成：ng 第一次有 **ring3 shell/WM**（`/bin/wm`），
它只是 displayd 的**普通客户端**（surface 像素走共享内存、控制面走 IPC），
不直接碰 LFB、不拥有应用进程：

- **面板**：1280×32 top bar（`(0,0)`）、标题 `Aster`、时钟占位 `--:--`、
  启动器按钮 `Apps`、任务按钮（标题来自协议）、通知区占位（含一个圆点）；
  面板背景 `#202838`、强调线 `#30507A`、焦点任务按钮 `#4A7AC0`。
- **启动器**：`/bin` 与 `/System/Packages` 两个目录各一次 listdir（syscall 25）；
  `/bin` 过滤掉 `shell` / `displayd` / `wm`（再起一个 shell 会抢串口、第二个
  displayd 会覆盖 `/run/display.port`），`/System/Packages/*` 只列出（entry 是
  包里的相对路径，WP3-2 不做包启动）；排序确定性：**键盘 1..9 的第 1 个是
  `/bin/demo`**。`spawn`（syscall 40）由 WM 发起。
- **任务切换**：v2 **LIST → LIST_REPLY** 拿 surface 快照
  `{id, owner_pid, title, x,y,w,h, focused}`；`Tab`/`Alt+Tab`（PS/2 没有 Alt
  修饰位，Alt+Tab 也以 Tab 到达）→ 选下一个窗口 → **FOCUS**；下一次 LIST
  确认焦点后才记一次“切换完成”。
- **全局热键**：`REGISTER_HOTKEY(surface, key=ANY)`；displayd 把每个按键
  **先给 WM 一份**（INPUT.flags 带 `DS_INPUT_F_GLOBAL`），再按焦点转发，
  两份不会重复投递（焦点客户端就是 WM 时只发一次）。
- **v2 最小扩展（保持 v1 兼容）**：CREATE 追加 `x,y,owner_pid,title`；
  LIST/LIST_REPLY；REGISTER_HOTKEY；WM_STATUS（WM 计数由 displayd 汇总进
  已有的 `/run/display.status`，**不新建 ramfs 文件/节点**）。解码器同时接受
  version 1 与 2：v1 客户端 16 字节 CREATE → 空标题、级联放置、owner_pid=0。
- **应用窗口**：demo 的 CREATE 带 `title=Demo`（窗口里画 `title: Demo`），
  新增 `demo window` 常驻角色（不抢焦点，焦点归 WM）；WM 启动器能 spawn
  `/bin/demo`。
- **自检（run-ng 全命中）**：`ng: wm up (pid=.. panel=1280x32)`、
  `ng: wm panel ok (px=0x202838 at=0,0)`、`ng: wm launcher ok (apps=3 pkgs=1)`、
  `ng: wm tasks ok (n=2 apps=1)`、`ng: wm task switch ok (focus=2 from=1 via=key)`、
  `ng: wm launch ok (app=/bin/demo pid=14 via=key)`、`ng: WP3-2 selftest PASS`；
  **QMP screendump** 同时断言 `(4,4)=#202838`（面板）与
  `(217,177)=#3070c0`（Demo 窗口的实心矩形）。
- **WM 崩溃隔离**：kill WM（`status=265`）后 Demo 窗口进程仍然活着
  （`ng: demo survives wm ok (alive=1 id=2)`），displayd 继续服务；再由内核
  依次回收 WM 启动的 demo、窗口 demo、displayd（`shm live=0`）。
- **host 单测**：**4684 checks, 0 failures**（4402 → +282：v2 协议/标题打包/
  surface 标题与位置/LIST 填充/面板布局/启动器/任务列表/热键状态机/面板绘制）。
- **无回归**：parity **100/100**（host 95/95）、`ci-ng` **7/7 PASS**、旧 32 位
  **305/305**；run-ng 默认 **147/147 行 + 61 正则**（NoQmp 131+56、Fallback
  121+36、Smoke 132+57、qemu64 147+61）。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/desktop/include/ds_wm.h` / `src/desktop/src/ds_wm.c` | WM 纯逻辑：面板布局（1280/640/320 三种宽度）、启动器解析/过滤/排序、LIST_REPLY 任务表、下一焦点、热键状态机、面板绘制（**新**） |
| `src/desktop/shell/wm.c` | ring3 `/bin/wm`：面板 surface + 共享内存绘制 + MAP/COMMIT + REGISTER_HOTKEY + LIST/FOCUS + spawn 启动器 + WM_STATUS 上报（**新**） |
| `src/desktop/include/ds_protocol.h` / `src/desktop/src/ds_protocol.c` | 协议 **v2**：CREATE `title/owner_pid/x,y`、LIST/LIST_REPLY、REGISTER_HOTKEY、WM_STATUS；v1 兼容解码；`ds_title_set/get`、`ds_list_entry_set`（**改**） |
| `src/desktop/include/ds_surface.h` / `src/desktop/src/ds_surface.c` | surface 增加 `title[16]`/`owner_pid`；`ds_table_create_ex`（指定位置/标题/贴边）；`ds_table_fill_list`；修复 FOCUS 在 >2 surface 时 z-order 留下空洞的 bug（**改**） |
| `src/desktop/src/ds_blend.c` | `ds_pixel_blend` 从合成器拆出（demo/wm 只链这个小对象，省用户 ELF 体积）（**新**） |
| `src/desktop/display/displayd.c` | v2：CREATE title/pos、LIST/LIST_REPLY、REGISTER_HOTKEY、热键副本路由、WM_STATUS 汇总进 `/run/display.status`（**改**） |
| `src/apps/demo/demo.c` | CREATE `title=Demo`、`ng: demo title ok`、`demo window` 常驻角色（不抢焦点）（**改**） |
| `src/ng/wp32.h` / `src/ng/wp32_selftest.c` | 内核自检编排：spawn displayd/wm/demo-window、等 display.status 的 wm 字段、kill WM 验应用存活、清理回收（**新**） |
| `src/ng/proc.c` | 种 `/bin/wm`、`desktop bin ok (.. wm=/bin/wm)`（**改**） |
| `src/ng/main.c` | `wp32_selftest()` + `ng: WP3-2 selftest PASS/FAIL`（**改**） |
| `src/ng/wp31_selftest.c` | 状态文件缓冲 128 → 256（v2 的 display.status 变长）（**改**） |
| `src/ng/user/hello.S` | ring3 fs demo 跑完 `unlink("/tmp/a")`（ramfs 节点池只有 32 个；WP3-2 多种一个 `/bin/wm` 后要给后面的 shell 会话留节点）（**改**） |
| `tests/unit-ng/test_wm.c` | v2 协议/标题/surface 扩展/布局/启动器/任务/热键/绘制（**新**） |
| `tests/unit-ng/test_desktop.c` | CREATE v2 body 40、v1 body 16 的长度期望（**改**） |
| `tests/unit-ng/unit_main.c` / `tools/unit-ng.ps1` | `test_wm()` + `ds_wm.c`/`ds_blend.c` 源（**改**） |
| `tools/build-ng.ps1` | `wm.elf`（3 PT_LOAD）、`ds_wm.c`/`ds_blend.c`、用户程序按需链 L2 对象、`-Os -ffunction-sections -fdata-sections` + `--gc-sections`（6 个用户 ELF 嵌入 2 MiB 窗口）（**改**） |
| `tools/run-ng.ps1` | WP3-2 期望行/正则；QMP 注入 `tab` → WM 任务切换、注入 `1` → 启动器 spawn；第二次 screendump 断言面板 + Demo 窗口；`-NoQmp` 的 `via=auto` 分支（**改**） |
| `docs/design/display-server-ng.md` | v2 协议 + L2b WM 设计（**改**） |
| `docs/design/evolution.md`、`docs/design/README.md` | E3 进度 / 文档索引（**改**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`tools/parity-ng.ps1`
和 `tools/ci-ng.ps1` 不需要改（parity 仍是 100 项，CI 复用 run-ng 日志）。

## 环境

- 分支 `ng`，基线 HEAD `b1c920a`（`[ng][WP3-1]`）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）。
  内核：`clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
  -mno-red-zone -fno-stack-protector -mgeneral-regs-only`；
  用户程序：同一 target + `-mno-sse -fno-builtin -fno-pic -std=c11 -Os
  -ffunction-sections -fdata-sections`，链接 `--gc-sections`；`src/ng/user/user.ld`
  链到 0x40000000。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`；QMP TCP 同时
  用于 `input-send-event`（a/b/tab/1）与两次 `screendump`。

## 关键参数

| 项 | 值 |
|---|---|
| 面板 surface | 1280×32 @ (0,0)，40 页共享内存；背景 `#202838`，探针 `(4,4)` |
| 面板布局 | 标题 0..63 / 启动器 64..127 / 任务区 x=136、按钮 96×24、gap 6、最多 8 个 / 时钟 x=1112 / 通知区 x=1184 w=88 |
| 启动器 | `/bin` 6 项（demo/displayd/hello/proc/shell/wm）→ 可启动 3（demo/hello/proc）；`/System/Packages` 1 项（hello，只列） |
| 任务按钮 | 标题来自 LIST_REPLY，11 字符截断；焦点按钮 `#4A7AC0`，普通 `#304460` |
| 协议 | 头仍 20 字节；CREATE v2 body 40（v1 16）、LIST 4、LIST_REPLY 360、REGISTER_HOTKEY 16、WM_STATUS 28；`DS_MSG_MAX=512` |
| 热键 | `DS_HOTKEY_ANY`；Tab = ascii 9（Alt+Tab 同）；1..9 = 启动器下标 0..8 |
| WM fallback | 无输入 150 轮（~3s）自动切一次、再 150 轮自动启动第一个（`-NoQmp`）；有 QMP 时 `via=key` |
| 时序 | WM 每 10 轮（~200ms）LIST；内核等 wm=1 3s、tasks 4s、switch/launch 各 6s |
| 内核镜像 | 2,045,088 字节（< 2 MiB；6 个用户 ELF 嵌 `.rodata`） |
| 用户 ELF | displayd 23,488（3 PT_LOAD）/ demo 15,648（2）/ wm 24,304（3）/ shell 18,848 / proc 9,280 / hello 15,872 |
| 状态文件 | `/run/display.status` 追加 `wm=1 wmtasks= wmfocus= wmswitch= wmlast= wmlaunch= wmlastpid=`（WM_STATUS 汇总） |

## 协议 v2 变更（vs v1）

| 消息 | 方向 | body | 语义 |
|---|---|---|---|
| `CREATE`（v2） | client→server | w,h,flags,x,y,owner_pid,title[4]（40 B） | `flags & DS_CREATE_F_POS` 时用 x/y；title 15 字符 + NUL；owner_pid 进 LIST |
| `LIST` | client→server | reserved（4 B） | 要一份 surface 快照（WM 每 ~200ms 轮询） |
| `LIST_REPLY` | server→client | count + entries[8]（360 B） | 每条 44 B：id/owner_pid/x/y/w/h/focused/title[4]，z-order 后→前 |
| `REGISTER_HOTKEY` | client→server | surface_id,key,flags,reserved（16 B） | key=0（ANY）时所有键先给注册者一份，再按焦点转发 |
| `WM_STATUS` | client→server | surface_id,tasks,focus,switches,lastswitch,launches,lastpid（28 B） | 汇总进 `/run/display.status`，内核自检读 |

兼容性：

1. 解码器接受 `version == 1 || version == 2`；v1 的 CREATE body 固定 16 字节，
   title 为空、owner_pid=0、忽略 x/y（走级联放置）。
2. v1 客户端不会发 LIST/LIST_REPLY/REGISTER_HOTKEY/WM_STATUS（这些类型在
   version 1 下 body 长度为 0，解码直接拒绝）。
3. 编码器始终写 version 2；surface id / shm handle / 焦点语义不变。
4. `FOCUS` 依旧没有 ack：WM 在**下一次 LIST_REPLY** 里确认焦点后才算切换完成。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
=== build-ng (WP3-1) ===   （脚本头注释尚未改标题，行为是 WP3-2）
...
=== user-desktop (WP3-2) ===
  ds_protocol.c -> obj-ng/ds_ds_protocol.o
  ds_surface.c -> obj-ng/ds_ds_surface.o
  ds_composite.c -> obj-ng/ds_ds_composite.o
  ds_blend.c -> obj-ng/ds_ds_blend.o
  ds_font8x8.c -> obj-ng/ds_ds_font8x8.o
  ds_draw.c -> obj-ng/ds_ds_draw.o
  ds_wm.c -> obj-ng/ds_ds_wm.o
  displayd.c -> obj-ng/user_displayd.o
  link    -> obj-ng/displayd.elf (base 0x40000000)
user-displayd: PASS (23488 bytes, 3 PT_LOAD, entry 0x40000e53, end 0x4000439c)
  demo.c -> obj-ng/user_demo.o
  link    -> obj-ng/demo.elf (base 0x40000000)
user-demo: PASS (15648 bytes, 2 PT_LOAD, entry 0x40000000, end 0x40002638)
  wm.c -> obj-ng/user_wm.o
  link    -> obj-ng/wm.elf (base 0x40000000)
user-wm: PASS (24304 bytes, 3 PT_LOAD, entry 0x40000e7c, end 0x400046f0)
...
  [42/60] wp32_selftest.c
...
  [60/60] link -> dist-ng/aster-ng.elf

ELF64: PASS (2045088 bytes)
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

```text
=== run-ng (WP3-2) ===
...
ng: WP3-1 selftest PASS
ng: wm displayd ok (pid=11 ticket=917504)
ng: wm spawn ok (pid=12)
<displayd> ng: displayd up (fb=0xfd000000 1280x800 pitch=5120 bpp=32)
<displayd> ng: display port ok (ticket=983040 path=/run/display.port)
<displayd> ng: surface create ok (id=1 w=1280 h=32 shm=0x30000 pages=40)
<displayd> ng: surface title ok (id=1 title=Aster Panel)
<wm>       ng: wm up (pid=12 panel=1280x32)
<wm>       ng: wm panel ok (px=0x202838 at=0,0)
<wm>       ng: wm launcher ok (apps=3 pkgs=1)
<wm>       ng: wm app ok (n=0 path=/bin/demo)
<wm>       ng: wm app ok (n=1 path=/bin/hello)
<wm>       ng: wm app ok (n=2 path=/bin/proc)
<wm>       ng: wm pkg ok (path=/System/Packages/hello)
<displayd> ng: hotkey registered ok (surface=1 key=any)
<displayd> ng: displayd wm status ok (switch=0 launch=0 tasks=0)
ng: wm ready ok (wm=1 hotkey=1)
ng: wm demo spawn ok (pid=13)
<displayd> ng: surface create ok (id=2 w=400 h=300 shm=0x40001 pages=118)
<displayd> ng: surface title ok (id=2 title=Demo)
<demo>     ng: demo window up (id=2 title=Demo)
<displayd> ng: displayd list ok (n=2 focus=1)
<wm>       ng: wm tasks ok (n=2 apps=1)
<wm>       ng: wm task ok (id=2 x=192 y=152 title=Demo)
<displayd> ng: displayd hotkey ok (key=0x9 scancode=0xf)      <- QMP 注入 Tab
<displayd> ng: displayd list ok (n=2 focus=2)
<wm>       ng: wm task switch ok (focus=2 from=1 via=key)
<displayd> ng: displayd wm status ok (switch=1 launch=0 tasks=1)
ng: wm switch seen ok (focus=2 switch=1)
<displayd> ng: displayd hotkey ok (key=0x31 scancode=0x2)     <- QMP 注入 '1'
<wm>       ng: wm launch ok (app=/bin/demo pid=14 via=key)
<displayd> ng: displayd wm status ok (switch=1 launch=1 tasks=1)
ng: wm launch seen ok (pid=14)
ng: wm display status ok (surface=3)
ng: wm kill ok (status=265 waiters=3)
ng: demo survives wm ok (alive=1 id=2)
ng: wm displayd kill ok (status=265 live=0)
ng: WP3-2 selftest PASS
...
run-ng: QMP sendkey tab -> WM hotkey task switch
run-ng: QMP sendkey 1 -> WM launcher spawn demo
run-ng: wm screendump pixel ok panel(4,4)=#202838 demo(217,177)=#3070c0
run-ng: screendump pixel: (185,145)=#3070c0 (QMP PPM)
run-ng: wm screendump pixel: panel(4,4)=#202838 demo(217,177)=#3070c0 (QMP PPM)
run-ng: matched 147/147 expected lines + 61 patterns
run-ng: PASS (all expected lines in 6.4s, cpu max)
```

### QMP 截图像素断言（两张 PPM）

1. WP3-1 的 screendump（`ng: composite ok` 触发）：`(185,145)=#3070c0`。
2. WP3-2 的 screendump（`ng: wm task switch ok` 触发，读取
   `ng: wm task ok (id=.. x=.. y=..)` 的 x/y 作为窗口原点）：

```text
run-ng: wm screendump pixel ok panel(4,4)=#202838 demo(217,177)=#3070c0
```

- `(4,4)`：面板纯背景（标题从 x=8 画起），证明 1280×32 面板在 `(0,0)` 且被
  合成到 LFB。
- `(217,177) = (192+25, 152+25)`：Demo 窗口 surface 内部实心矩形，证明窗口
  存在（第二块 surface，级联位置 `160+32, 120+32`）。
- 同一张 PPM 还隐含：面板任务按钮（x≥136）没有盖住窗口、光标没盖住探针。

### 负向 / 降级 / 其它 CPU / Smoke

```text
run-ng -NoQmp        -> 131/131 lines + 56 patterns PASS
                        （WM auto fallback：via=auto 任务切换 + 启动；不做 screendump）
run-ng -Fallback     -> 121/121 lines + 36 patterns PASS
                        （-vga none：`ng: WP3-2 selftest skipped (no fb)`）
run-ng -Cpu qemu64   -> 147/147 lines + 61 patterns PASS（SMEP/SMAP off）
build-ng -Smoke + run-ng -Smoke -> 132/132 lines + 57 patterns PASS
                        （只跳过 shell；WP3-2 + 两次 screendump 照跑）
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
parity(host): 95/95 PASS (5 display/input cases are kernel-only)
4684 checks, 0 failures
unit-ng: PASS (4684 checks, 0 failures)
```

WP3-1 为 4402，本 WP **+282**：

- `tests/unit-ng/test_wm.c`（新）：v2 长度表（CREATE 40 / CREATE v1 16 /
  LIST 4 / LIST_REPLY 360 / REGISTER_HOTKEY 16 / WM_STATUS 28）、CREATE v2
  往返（title/owner_pid/x/y）、v1 CREATE 解码（16 字节、空标题、owner_pid=0、
  version 1 + v2 长度/版本 3 拒绝）、LIST/LIST_REPLY 往返（count/条目字段/
  focused 归一化）、REGISTER_HOTKEY/WM_STATUS 往返、`ds_title_set/get`
  （空/NULL/15 字符/超长截断/cap 截断）、`ds_list_entry_set`；
  `ds_table_create_ex`（面板 (0,0)、标题、owner_pid、级联、显式位置贴边、
  v1 路径空标题）、`ds_table_fill_list`（z-order/焦点位/标题/max/NULL）、
  布局（1280 → 8 个按钮、640 → 更少、320 → 0、rect 边界、NULL 安全）、
  文本截断、面板绘制（背景/强调线/启动器/焦点与非焦点任务按钮/通知点/标题与
  时钟有字/整块 A=255/pixel helper/空任务表/NULL 安全）、启动器（/bin 过滤
  shell/displayd/wm、包目录只列、重复/空名/超长名/表满、排序、可启动下标、
  `at(3)==NULL`）、任务表（解析/标题/focused/owner_pid/坐标、exclude self、
  next 回绕/焦点不在列表/单窗口/无窗口 ENOENT、非法消息/越界 count）、热键
  （Tab/1..9/其它、index、consume 计数、GLOBAL 位）。
- `tests/unit-ng/test_desktop.c`（改）：CREATE v2 body 40、`-v` 版本长度表。

### 门禁

```text
tools/check-domains.ps1
checked: 252 files, 793 quoted includes
domain include violations: 0

tools/check-deps.ps1
OK: no cross-module include violations.

tools/ci-ng.ps1
  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          4684 checks; exit=0, 8.3s
  PASS build-ng         2045088 bytes, user-elf 15872 bytes; exit=0, 9.4s
  PASS run-ng           147/147 lines + 61 patterns; exit=0, 6.9s
  PASS parity-ng        100/100 items, 10 domains; exit=0, 0.2s
  PASS legacy-32bit     305/305 parity checks; exit=0, 39.4s
  7 passed, 0 failed, 0 skipped, 65.2s
=== NG CI PASS ===
```

旧 32 位独立 `build.ps1 -Test` 也是 **305/305**。

## 设计要点（详见 `docs/design/display-server-ng.md`）

### WM 是普通客户端

```text
CREATE(1280x32, title="Aster Panel", flags=POS, x=0, y=0)
  displayd: SYS_SHM_ALLOC(40) -> {id=1, handle=0x30000, pages=40}
wm: SYS_SHM_MAP(handle) -> va；ds_wm_draw_panel() 画 top bar
  MAP/COMMIT -> displayd 合成到 LFB（探针 (4,4)=#202838）
  REGISTER_HOTKEY(surface=1, key=ANY)
  LIST(每 ~200ms) -> LIST_REPLY{entries[..], focused}
  on INPUT(GLOBAL): Tab -> LIST -> next -> FOCUS(id)
                    '1' -> spawn("/bin/demo")
  WM_STATUS{switch,lastswitch,launch,lastpid} -> displayd -> /run/display.status
```

WM 不映射 LFB、不写 surface 表、不持有应用句柄；Demo 进程由内核（自检）或
WM（启动器）spawn，WM 死了 Demo 还活着（S1 的最小前身；S1 本体是 WP3-5）。

### 热键路由顺序（displayd）

```text
readkey -> ascii/scancode/flags
  if (hotkey_client != 0 && (hotkey_key == ANY || ascii == hotkey_key))
        INPUT{hotkey_surface, flags|GLOBAL} -> WM       （先）
  focus = table.focus_id
  if (focus.client_ticket != 0 && focus.client_ticket != hotkey_client)
        INPUT{focus.id, flags} -> 焦点客户端           （后）
  g_inputs++ / 证据行
```

- 焦点客户端恰好是 WM（面板）时只发一次，避免重复按键。
- Alt 没有独立修饰位（PS/2 驱动只有 SHIFT/CAPS/CTRL/EXT），所以 Alt+Tab 与
  Tab 在 WM 侧是同一个动作；这一点在 ds_wm.h 与 host 单测里写明。

### v2 兼容与 ramfs 节点

- 解码器接受 v1/v2；v1 CREATE 16 字节 → 空标题 + 级联放置。
- WM 的状态**不写新文件**：WM_STATUS 让 displayd 汇总进已有的
  `/run/display.status`。原因是 ramfs 节点池只有 32 个：种入 `/bin/wm` 后
  再新建一个状态文件会把用户态 shell 的 `/tmp/s.txt` 挤成 ENOSPC。
- hello.S 的 ring3 fs demo 跑完删除自己的 `/tmp/a`，把节点还给会话。

### 用户 ELF 尺寸（2 MiB 映像窗口）

- 6 个用户 ELF 都 `-Os -ffunction-sections -fdata-sections` 编译、`--gc-sections`
  链接；每个程序只链自己用到的 L2 对象（displayd 不链 ds_draw/ds_font8x8/
  ds_wm；demo/wm 不链 ds_surface；`ds_pixel_blend` 单独成 `ds_blend.c`）。
- 内核镜像 2,045,088 字节，仍满足 `link.ld` 的 `__kernel_end <= 0x200000`。

## 自检项清单

1. 面板：1280×32 @ (0,0)、标题/时钟占位/启动器按钮/任务按钮/通知区；
   `ng: wm panel ok (px=0x202838 at=0,0)` + QMP `(4,4)=#202838`。
2. 启动器：`/bin` + `/System/Packages` listdir、过滤与排序、`1..9` spawn；
   `ng: wm launcher ok (apps=3 pkgs=1)` + `ng: wm launch ok (app=/bin/demo)`。
3. 任务切换：LIST 快照 → Tab → FOCUS → 下一次 LIST 确认；
   `ng: wm tasks ok (n=2 apps=1)` / `ng: wm task switch ok (focus=2 from=1 via=key)`。
4. 全局热键：REGISTER_HOTKEY + GLOBAL 副本 + 焦点仍收到按键；
   `ng: displayd hotkey ok (key=0x9 scancode=0xf)`、demo 的 `input route ok`。
5. v2 协议：CREATE title/owner_pid/pos、LIST/LIST_REPLY、REGISTER_HOTKEY、
   WM_STATUS；v1 兼容（host 单测覆盖 16 字节 CREATE）。
6. 应用窗口：demo `title=Demo`、`demo window` 常驻、可被 WM 聚焦；QMP 探针
   `(217,177)=#3070c0`。
7. WM 独立进程：kill WM → `status=265`、Demo 存活、displayd 继续服务。
8. host 单测 4684 checks / 0 failures（+282）。
9. 门禁：run-ng 147+61（NoQmp 131+56 / Fallback 121+36 / Smoke 132+57 /
   qemu64 147+61）；parity 100/100（host 95/95）；ci-ng 7/7；旧 32 位 305/305。

## 未做 / 遗留（按 WP 边界）

- **没有 session manager（WP3-3）**：应用生命周期仍归内核自检/调用者；WM 的
  启动器只是直接 `spawn`，不记录/回收子进程，也不做“应用注册表/角色令牌”。
- **没有窗口装饰/resize/拖拽/点击**：displayd 不做 SSD，WM 不做装饰；窗口
  位置来自 CREATE 的位置或 displayd 的级联策略，WM 只切焦点不动几何。
- **输入只有键盘按下**：没有鼠标/按键释放/重复率，因此没有点击焦点、没有
  启动器菜单展开；`Alt+Tab` 与 `Tab` 同义（PS/2 驱动没有 Alt 修饰位）。
- **WM 崩溃后它的面板 surface 还在 displayd 表里**（客户端没发 DESTROY/没有
  断连回收）：S1 要求“面板回来”，那是 WP3-5；本轮只证明 kill WM 后应用不死、
  displayd 继续服务。
- **包启动器只列不启动**：`/System/Packages/<name>` 的 entry 在 manifest 里
  （相对路径），解 manifest + 版本选择属于 WP3-3/WP3-4；本轮 `spawnable=0`。
- **单客户端轮询、无背压**：WM 每 ~200ms LIST；displayd 单线程顺序处理，
  没有并发客户端压力测试。
- **WM 的 auto fallback 只服务自检**：无 QMP 时 3s 后自动切一次/启动一次
  （`via=auto`）；真实会话里应由用户输入触发。
- **像素断言依赖 1280×800 stdvga + QMP screendump PPM**：其它卡/VGA 文本走
  `-Fallback` 跳过；真机在 D1 锁定硬件后再补。

## 出口

```text
powershell -File tools/build-ng.ps1              # PASS，2045088 bytes，60 步
powershell -File tools/build-ng.ps1 -Smoke       # PASS（跳过 shell）
powershell -File tools/run-ng.ps1                # PASS，147 行 + 61 正则 + 2 次截图像素
powershell -File tools/run-ng.ps1 -NoQmp         # PASS，131 行 + 56 正则（via=auto）
powershell -File tools/run-ng.ps1 -Fallback      # PASS，121 行 + 36 正则（WP3-2 skipped）
powershell -File tools/run-ng.ps1 -Cpu qemu64    # PASS，147 行 + 61 正则
powershell -File tools/unit-ng.ps1               # PASS，4684 checks, 0 failures
powershell -File tools/parity-ng.ps1             # PASS，100/100（host 95/95）
powershell -File tools/ci-ng.ps1                 # 7/7 PASS
powershell -File build.ps1 -Test                 # PASS，305/305（旧 32 位）
```
