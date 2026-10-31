# ng display server 设计（WP3-1，L2a）+ shell/WM（WP3-2，L2b）

> 状态：WP3-1 已实现（2026-10-31，`docs/claims/ng-WP3-1.md`）；WP3-2 在**不改
> v1 语义**的前提下把协议扩到 **v2** 并落了 L2b WM（`docs/claims/ng-WP3-2.md`，
> 见本文第 13 节）。
> 范围：L2a display server（合成/扫描输出/surface 生命周期/焦点/输入路由）+
> L3 demo + WP3-2 的 L2b shell/WM（面板/启动器/任务切换）；WP3-3 session
> manager、WP3-5 S1 故障注入都不在这里。
> 上位约束：`docs/design/architecture.md` 第 5 节不变量 7（控制面走消息、数据面
> 走共享内存，4 KiB 消息只用于控制）、不变量 2（L1/L2 不共享地址空间，只经
> IPC + cap）、第 3 节失败矩阵（L2a 崩 = session 级，不拖内核）。

## 1. 目标与边界

| 项 | 本 WP 做到 | 明确不做 |
|---|---|---|
| 内核机制 | syscall **45 SHM_ALLOC / 46 SHM_MAP / 47 FB_MAP**；共享页 refcount；进程退出/被 kill 的回收 | cap 授权/撤销（ticket 仍"知道就能映射"，owner/root 校验先落地） |
| display server | ring3 进程 `/bin/displayd`：LFB 映射、端口、surface 表、合成、光标、焦点、输入转发 | shell/WM 策略、面板、任务切换 |
| 协议 | CREATE / CREATE_REPLY / MAP / COMMIT / DESTROY / FOCUS / INPUT / BYE | 窗口装饰、resize、多显示器、vsync |
| 数据面 | ARGB8888 surface 在共享内存；displayd 与客户端映射同一批物理页 | GPU、DMA、buffer 队列/三缓冲 |
| demo | `/bin/demo`：读发现文件 → 400x300 surface → 渐变/矩形/文字 → COMMIT → 等 INPUT | 通用 GUI 工具包 |
| 自检 | run-ng 证据行 + **QMP screendump 像素断言** + kill displayd 的崩溃隔离 | S1（杀 shell/输入恢复）留给 WP3-5 |

## 2. 组件与启动

```text
内核 kmain
  wp23_selftest()          VBE 1280x800x32 + LFB 只映射内核 PML4 + PS/2
  proc_init()              种 /bin/{shell,proc,hello,displayd,demo}
  wp31_selftest()  ──spawn──> /bin/displayd   （L2a，ring3 进程）
        │                       │ SYS_FB_MAP(47)      -> LFB 用户映射
        │                       │ PORT_CREATE         -> ticket
        │                       │ 写 /run/display.port（ramfs 文本，发现入口）
        │                       │ 写 /run/display.status（证据/状态快照）
        │                       └ recv 循环：协议消息 + 轮询 readkey
        ├──等 /run/display.port──┘
        ├──spawn──> /bin/demo  （L3）
        │            读 /run/display.port -> CREATE -> SHM_MAP -> 画 -> COMMIT
        │            -> FOCUS -> 等 INPUT -> 退出 0
        ├──读 /run/display.status（surface/mapped/commits/focus/px）
        ├──spawn /bin/demo hold（另一个客户端）
        ├──kill(displayd, 9) -> status=265，shm_live=0，内核继续跑
        └──等 hold 自己超时退出 0（客户端不被 displayd 的死拖垮）
```

displayd 是**普通进程**：它挂了只回收自己的句柄/AS/共享页；内核只保留"共享内存
对象表 + LFB 映射"两条机制，没有 surface/窗口概念。

## 3. 内核机制：syscall 45/46/47

### 3.1 共享内存（45/46）

```text
SYS_SHM_ALLOC(size_pages, &va, &handle) -> 0 / -err     (45)
SYS_SHM_MAP(handle, &va)                -> 0 / -err     (46)
SYS_FB_MAP(&va, &info)                  -> 0 / -err     (47，root-only)
```

`src/ng/shm_core.c` 是**纯状态机**（`struct shm_table`，host 单测直接驱动），
`src/ng/shm.c` 是内核胶水（PMM 页 + `as_map` + uaccess）：

- **对象**：`SHM_MAX=8` 个槽位；每个对象有 key `pages`（≤1024 = 4 MiB）、
  `refcnt`（映射数）、`phys[]`（**从 heap 分配**，不占 .bss）、最多 8 条映射
  记录 `{pid, va}`。
- **handle**：`(gen<<16)|slot`，与端口 ticket 同构，但属于**独立命名空间**；
  槽位复用后 generation 递增，旧 handle 立即 `EBADF`。handle 可以经 IPC 从
  displayd 传给客户端（`CREATE_REPLY` 里的 `shm_handle`）。
- **权限**：调用者是 owner，或 `uid==0`（root）。当前所有进程 uid 都是 0，
  所以 displayd 能映射 demo 的 buffer、demo 也能映射 displayd 分配的 buffer；
  cap/角色令牌（WP3-3）之后会收紧这一点。纯表语义已经用非 root uid 单测过。
- **refcount**：ALLOC 记一个映射（refcnt=1），每 MAP 一次 +1，解除一次 -1；
  **最后一个映射解除时**才归还物理页。进程退出/被 kill 时按 pid 回收
  自己的映射（`shm_core_collect_pid` + `shm_core_release_pid`）。
- **VA**：每进程 bump（`proc->shm_next_va`），起点 `0x50000000`，对象之间留
  一页守护空洞；同一个对象在 displayd 和客户端里可以是不同 VA。
- **与 as_destroy 的顺序**：`as_destroy()` 会 free 每个 present 叶子页，所以
  进程退出/被 kill 时必须**先 `pg_unmap_in` 共享页（不 free）**，再让 core
  按 refcount 决定是否 free。顺序反了就是 double free。
- 没有 `SYS_SHM_FREE`：surface DESTROY 后 displayd 的映射要等 displayd 退出
  才释放（WP3-3 的 surface 生命周期再补）。

### 3.2 LFB 用户映射（47）

- 只给 root（当前 uid 都是 0；cap 后置）。
- 固定 VA `0x60000000`，逐页 `PG_USER|PG_WRITE|PG_PCD|PG_PWT|PG_NX`：MMIO
  不走可缓存路径，用户态不可执行。
- 返回 `struct ds_fb_info`：用户 VA、物理基址、size、pitch、bpp、width、
  height、flags（PCD/PWT/NX）。displayd 用它构造 `struct ds_screen`。
- 没有 LFB（`-vga none` 的 VGA 文本降级）返回 `-ENODEV`；内核自检打印
  `ng: WP3-1 selftest skipped (no fb)` 并跳过（不算失败）。
- 进程退出前 `fbmap_release_proc()` 把这段 PTE 摘掉；LFB 物理页不在 PMM
  窗口内，显式摘除比依赖 `pmm_free_page` 的 ERANGE 更干净。

## 4. 协议（控制面，IPC）

消息 = 20 字节头 + 固定长度 body，body 只用 u32；编解码是
`src/desktop/src/ds_protocol.c` 的**纯函数**（host 单测逐字段往返）。IPC 的
16 字节头由各进程自己拼（`struct ds_ipc_hdr` 与 `struct ipc_msg_hdr` 一致），
`hdr.type` 复用消息类型。

| 消息 | 方向 | body 字段 | 语义 |
|---|---|---|---|
| `CREATE` | client→server | w,h,flags [,x,y,owner_pid,title[4]] | 建 surface；server 分配共享内存。v1 只有 w,h,flags（16 B）；v2 40 B，`flags&DS_CREATE_F_POS` 时用 x/y，title 15 字符 |
| `CREATE_REPLY` | server→client | surface_id, shm_handle, pages, x,y,w,h, status | 返回 id/缓冲/放置 |
| `MAP` | client→server | surface_id | 客户端已 `SHM_MAP`（之后才允许 COMMIT） |
| `COMMIT` | client→server | surface_id, x,y,w,h | surface 局部脏矩形；server 合成到 LFB |
| `DESTROY` | client→server | surface_id | 销毁 surface（焦点回退到最上层） |
| `FOCUS` | client→server | surface_id | 置焦点 + 提到 z-order 最前 |
| `INPUT` | server→focus client | surface_id, kind, key, scancode, flags | PS/2 按下事件推送；热键副本带 `DS_INPUT_F_GLOBAL` |
| `BYE` | client→server | reason | 保留 |
| `LIST`（v2） | client→server | reserved | 要一份 surface 快照（WM 任务列表） |
| `LIST_REPLY`（v2） | server→client | count + entries[8] | 每条 44 B：id/owner_pid/x/y/w/h/focused/title[4]，z-order 后→前 |
| `REGISTER_HOTKEY`（v2） | client→server | surface_id,key,flags,reserved | `key=0`（ANY）时每个按键先给注册者一份 |
| `WM_STATUS`（v2） | client→server | surface_id,tasks,focus,switches,lastswitch,launches,lastpid | displayd 汇总进 `/run/display.status`（不新建 ramfs 文件） |

规则：

1. `CREATE` 的 reply 用 IPC `reply`（`hdr.from` 是 client 的端口 ticket）。
2. `COMMIT` 要求先 `MAP`，否则 `EBUSY`（避免合成未映射的脏缓冲）。
3. `COMMIT` 的矩形裁到 surface 内部；完全不可见 -> `EINVAL`（协议错误）。
4. 未知类型/坏 magic/长度多一个字节都拒绝（`ds_msg_decode` 严格校验），
   displayd 丢掉坏消息继续服务，不 panic。
5. `INPUT` 只发给焦点 surface 的 `client_ticket`（CREATE 时记下的 `hdr.from`）；
   没有焦点时按键被丢弃。
6. **v1/v2 兼容**：解码器接受 `version == 1 || 2`；v1 CREATE body 固定 16 字节，
   解码后 title 为空、owner_pid=0、忽略 x/y（走级联放置）。v1 下
   LIST/LIST_REPLY/REGISTER_HOTKEY/WM_STATUS 的 body 长度为 0，直接拒绝。
   编码器始终写 version 2。

## 5. surface 表、z-order 与裁剪

`src/desktop/src/ds_surface.c`（纯状态机）：

```text
struct ds_table
  slot[DS_SURFACES_MAX=8]   used/id/w/h/x,y/owner_pid/client_ticket/
                            shm_handle/shm_pages/pixels/mapped/commits/last_commit
  order[8]                  槽位下标，**后 -> 前**
  count/next_id/focus_id/screen_w/screen_h/created/destroyed/commit_total/rejects
```

- `id` 从 1 单调递增，永不复用；槽位复用不影响 id。
- 放置：第一个 `(160,120)`，之后每级联 `+32,+32`，超出屏幕贴边；surface 可以
  比屏幕大（合成时裁剪）。
- `FOCUS`：把槽位在 order 里移到末尾（最前），`focus_id` 更新；新 surface
  追加到末尾。
- `DESTROY`：清槽位、从 order 移除、焦点回退到当前最上层（没有则 0）。
- `COMMIT`：先裁到 surface 内部，记录 `last_commit`；完全不可见 -> `EINVAL`
  并记 `rejects`。
- 自动焦点：第一个 surface 自动获得焦点（之后由 `FOCUS` 显式改）。

## 6. 合成、像素格式与光标

`src/desktop/src/ds_composite.c`（纯函数）：

- 像素统一 `u32 = 0xAARRGGBB`；QEMU stdvga LFB 是 BGRX 小端，所以内存里的
  u32 也是 `0x00RRGGBB`，直接写即可。
- 合成顺序：`order[0..count)`（后 -> 前）逐 surface 做 source-over：
  - `A=0` 透明（跳过），`A=255` 直接拷贝 RGB（快路径，**逐位相等**），
    其余 `(src*a + dst*(255-a))/255`。
  - 屏幕裁剪 + region 裁剪都在 `ds_surface_blit` 里做；region 用屏幕坐标，
    surface 局部矩形通过 `hit - (sf->x,sf->y)` 转换。
- 光标：8x12 块光标（白色描边 + 黑色填充 + 一条高亮竖线），固定在屏幕右下角
  附近，越界自动裁剪。没有鼠标，所以光标位置固定（D1 的鼠标再动）。
- demo 表面：400x300 @ `(160,120)`；渐变 + 实心矩形 `#3070c0` + 三行文字。
  实心矩形在 surface 局部 `(20,20)-(159,89)`，探针在 `(25,25)`。

## 7. 发现与状态文件（ramfs 文本）

- `/run/display.port`：displayd 写自己的端口 ticket（十进制 + `\n`）；客户端
  读它建立 IPC。displayd 自己 `mkdir("/run")`。
- `/run/display.status`：`up=1 port=1 surface=N mapped=N commits=N focus=ID
  input=N px=0xXXXXXX\n`，每次状态变化重写。内核自检读它做 PASS/FAIL
  （surface/mapped/commits/focus 至少 1，`px == #3070c0`）。
- 两个文件都是 **ramfs 上的普通文件**，不引入新机制；VFS 的 fd 表仍是全局
  一份（协作式、所有 open/write/close 之间没有阻塞点）。

## 8. demo（L3）与 QMP 像素断言

`src/apps/demo/demo.c`：

1. 读 `/run/display.port` -> `server`；`PORT_CREATE` -> `my`。
2. 先建一个 64x48 的 **scratch surface**，`SHM_MAP` 后 `MAP` + `DESTROY`
   （把 DESTROY 路径在真机上跑到，同时不破坏主 surface 的状态证据）。
3. 建主 surface 400x300；`SHM_MAP(handle)` -> 数据面 VA；
   画渐变 + 实心矩形 + 文字（`src/desktop/src/ds_draw.c`）。
4. `MAP` / `COMMIT(0,0,400,300)` / `FOCUS`，打印 `ng: demo input ready`。
5. 等 `INPUT`（最多 100 × 20ms）；收到打印 `ng: input route ok (key=b ...)`，
   没收到（`-NoQmp`）打印 `ng: input route ok (timeout ...)`。
6. 退出 0（主 surface 由 displayd 保留到 displayd 退出，见遗留）。

run-ng 在看到 `ng: composite ok (px=0x... at=X,Y)` 后：

- QMP `screendump` 抓 PPM（P6 1280x800），解析头，读 `(X+25, Y+25)`；
- 断言 RGB == `#3070c0`；失败则 run-ng FAIL（默认 QMP 模式强制）。
- 在看到 `ng: demo input ready` 后用 QMP `input-send-event` 注入 qcode `b`；
  断言 demo 打印 `ng: input route ok (key=b ...)`。

## 9. 崩溃隔离

- displayd 被 `kill(pid, 9)`：进程状态 = `0x100|9 = 265`；端口关闭、句柄
  回收、AS 销毁（先摘共享页/LFB 再 `as_destroy`）、shm 对象 refcnt 归零后
  物理页归还。
- 内核证据行：`ng: displayd kill ok (status=265 live=0)`；
  另一个客户端（`demo hold`）自己超时退出 0：
  `ng: demo hold wait ok (status=0 waiters=0)`。
- 这不是 S1（杀 shell/输入恢复/面板回来）——那是 WP3-5；本 WP 只证明"display
  server 挂掉不拖内核、客户端能自行降级退出"。

## 10. 自检证据行

```text
ng: desktop bin ok (displayd=/bin/displayd demo=/bin/demo)
ng: displayd spawn ok (pid=8)
<displayd> ng: displayd up (fb=0xfd000000 1280x800 pitch=5120 bpp=32)
<displayd> ng: display port ok (ticket=917504 path=/run/display.port)
ng: display port file ok (ticket=917504)
ng: demo spawn ok (pid=9)
<displayd> ng: surface create ok (id=1 w=64 h=48 shm=0x10000 pages=3)      scratch
<displayd> ng: surface map ok (id=1)
<displayd> ng: surface create ok (id=2 w=400 h=300 shm=0x20001 pages=118)  main
<demo>     ng: demo surface ok (id=2 shm=0x20001 pages=118)
<demo>     ng: shm map ok (va=0x50004000 pages=118)
<demo>     ng: demo input ready (id=2)
ng: surface map ok (id=2)
<displayd> ng: composite ok (px=0x3070c0 at=160,120)
<displayd> ng: displayd input ok (key=b scancode=0x30)
<demo>     ng: input route ok (key=b id=2)
<demo>     ng: demo done (id=2 input=1)
ng: demo exit ok (status=0)
ng: shm refcount ok (live=2)          demo 退出后 displayd 还持有 2 个对象
ng: displayd status ok (surface=1 commits=1 input=1 px=0x3070c0)
ng: demo hold spawn ok (pid=10)
<displayd> ng: displayd kill ok (status=265 live=0)
<demo>     ng: demo hold ready (pid=10)
<demo>     ng: demo hold done (alive=1)
ng: demo hold wait ok (status=0 waiters=0)
ng: WP3-1 selftest PASS
run-ng: screendump pixel ok (185,145)=#3070c0
```

`-NoQmp`：没有注入，demo 走 timeout 分支（`ng: input route ok (timeout ...)`），
不要求 screendump；`-Fallback`（`-vga none`）：`ng: WP3-1 selftest skipped
(no fb)`。`-Smoke` 照跑 WP3-1（只跳过 shell）。

## 11. host 单测

`tests/unit-ng/test_desktop.c`（协议/表/合成/绘制）+ `test_shm.c`（refcount/
权限/handle/回收）。桌面部分 ≥80 checks 的要求在 4402 checks 的总盘子里超额
完成；`tests/unit-ng/test_syscall.c` 另测 45..47 的寄存器参数搬运。

## 12. 限制 / 遗留（按 WP 边界）

1. **cap 未实现**：SHM_MAP 的 owner/root 校验就是全部权限；ticket/handle 知道
   就能用。cap grant/revoke + role token 是 WP3-3/ABI 1.0。
2. **没有 SHM_FREE**：DESTROY 只清 surface 表项，displayd 的共享映射等进程退出
   才释放；长跑会话需要 surface 生命周期（WP3-3）。
3. **单 surface 客户端 + 单线程**：demo 只建一个主 surface；displayd 一次处理
   一条消息、没有并发客户端压力测试。
4. **输入只有键盘按下**：PS/2 轮询（无 IRQ），没有鼠标/按键释放/重复率；
   displayd 焦点固定（没有点击焦点切换）。
5. **没有 vsync/双缓冲/脏区合并**：每次 COMMIT 重合成 surface 覆盖的屏幕
   region（z-order 正确），像素量小所以够用；D1 再谈。
6. **没有 resize/装饰/多显示器/DPMS**：CREATE 后尺寸固定，server-side
   decoration 属 WP3-2（WP3-2 只做 WM 面板，没做应用装饰）。
7. **像素断言依赖 1280x800 stdvga + QEMU screendump PPM**：其它卡/VGA 文本
   走 `-Fallback` 跳过；真机在 D1 锁定硬件后再补。
8. **VFS fd 表全局一份**：displayd/demo 的 open/close 之间没有阻塞点，所以
   协作式单核下安全；每进程 fd 表属 fsd/WP3-x。

## 13. WP3-2 追加：shell/WM（L2b）与协议 v2

WP3-2 不新增内核机制（syscall/共享内存/LFB 都不动），只把 display 协议扩到
v2 并加一个 ring3 WM 进程。

### 13.1 组件

```text
displayd（L2a，v2）
  ├─ surface 表：每项多一个 title[16] + owner_pid
  ├─ LIST/LIST_REPLY：WM 的任务表快照
  ├─ REGISTER_HOTKEY：每个按键先给 WM 一份（GLOBAL），再按焦点转发
  ├─ WM_STATUS：WM 的 switch/launch 计数写进已有的 /run/display.status
  └─ 合成/焦点/输入语义与 WP3-1 相同

/bin/wm（L2b，ring3）
  ├─ CREATE(1280x32, title="Aster Panel", POS (0,0)) -> SHM_MAP -> 画面板
  ├─ MAP / COMMIT（整块 1280x32）
  ├─ REGISTER_HOTKEY(surface=1, key=ANY)
  ├─ 每 ~200ms LIST；任务按钮标题来自 LIST_REPLY
  ├─ Tab / Alt+Tab -> LIST -> next(排除自己) -> FOCUS -> 下一次 LIST 确认
  ├─ '1'..'9' -> 启动器可启动下标 -> spawn(/bin/<app>)
  └─ 变化时发 WM_STATUS（switch/lastswitch/launch/lastpid/tasks/focus）

/bin/demo（L3）
  ├─ 默认角色：CREATE(title=Demo) -> 画 -> COMMIT -> FOCUS -> 等 INPUT -> 退出
  └─ window 角色：CREATE(title=Demo) -> 画 -> COMMIT -> **不发 FOCUS**
                  -> 常驻等 INPUT（内核 kill）；WM 用 Tab 切焦点
```

WM 是 displayd 的普通客户端：不 `SYS_FB_MAP`、不写 surface 表、不持有应用
句柄。应用窗口的进程由内核自检或 WM 启动器 `spawn` 创建；WM 被 kill 时内核
只回收 WM 自己的 AS/端口/共享页，Demo 进程继续运行（S1 的最小前身）。

### 13.2 面板

```text
(0,0) 1280x32:
  [0..63]     标题 "Aster"（8,12 起画）
  [64..127]   启动器按钮 "Apps"
  [136..]     任务按钮：96x24、gap 6、最多 8 个（标题 11 字符截断；焦点高亮）
  [1112..]    时钟占位 "--:--"
  [1184..1272] 通知区（88x24，含一个 6x6 圆点）
  底部 y=30..31 强调线
```

颜色：背景 `#202838`、强调线/描边 `#30507A`、普通按钮 `#304460`、焦点按钮
`#4A7AC0`、通知点 `#FFC040`；探针 `(4,4)` 是纯背景（QMP 断言用）。
布局是 `ds_wm_layout_init()` 纯函数（1280/640/320 三种宽度都有 host 单测）。

### 13.3 启动器

- `listdir("/bin")` + `listdir("/System/Packages")`（syscall 25，单次 ≤8 条）。
- `/bin` 普通文件 → `DS_WM_APP_BIN`；`shell` / `displayd` / `wm` 标记
  **不可启动**（再起一个 shell 会抢串口，第二个 displayd 会覆盖
  `/run/display.port`）；`/System/Packages` 目录 → `DS_WM_APP_PKG`，只列出
  （entry 在 manifest 里，包启动属 WP3-3/4）。
- 插入排序（kind 后 name），保证键盘 1..9 的确定性：本次 `/bin` 里可启动的是
  `demo`（1）、`hello`（2）、`proc`（3）。
- `spawn` 走 proc syscall 40（WM 是父进程）；内核自检读到 `wmlaunch=` 后
  再回收这个 demo。

### 13.4 任务切换与热键

- WM 的“当前焦点”来自最近一次 LIST_REPLY 的 `focused` 位；可切换集合排除
  WM 自己的面板 surface。
- Tab 到达 WM 时 `ds_wm_tasks_next()` 选当前焦点的下一个（回绕）；`FOCUS`
  发出去之后**不假设成功**，下一次 LIST_REPLY 的 focus 等于目标才记一次
  `switches++` 并打印 `ng: wm task switch ok (focus=.. from=.. via=key)`。
- 热键路由在 displayd 的 `d_poll_input()` 里：注册者先拿一份
  （`flags | DS_INPUT_F_GLOBAL`），焦点客户端仍拿普通副本；两者相同则只发
  一次。`Alt` 没有独立修饰位，Alt+Tab 在驱动层已经只剩 Tab。
- 无 QMP 时 WM 有自检 fallback：看到窗口 3s 内没有 Tab 就自己切一次
  （`via=auto`），切换后再 3s 没有启动就自己启动第一个；真实交互不会走这条。

### 13.5 WM 状态与 ramfs 节点

`/run/display.status` 追加（仅 WM_STATUS 到达后）：

```text
wm=1 wmtasks=N wmfocus=ID wmswitch=N wmlast=ID wmlaunch=N wmlastpid=PID
```

不新建状态文件的原因：ramfs 节点池只有 32 个；WP3-2 多种了 `/bin/wm`，再建
`/run/wm.status` 会把用户态 shell 的 `/tmp/s.txt` 挤成 ENOSPC。内核 wp32 读同一
份 display.status，`ng: wm ready ok` / `wm tasks seen ok` / `wm switch seen ok` /
`wm launch seen ok` 都由它驱动。ring3 fs demo 也改成跑完 `unlink("/tmp/a")`。

### 13.6 证据（详见 `docs/claims/ng-WP3-2.md`）

```text
ng: wm up (pid=.. panel=1280x32)
ng: wm panel ok (px=0x202838 at=0,0)
ng: wm launcher ok (apps=3 pkgs=1)
ng: wm tasks ok (n=2 apps=1)
ng: displayd hotkey ok (key=0x9 scancode=0xf)
ng: wm task switch ok (focus=2 from=1 via=key)
ng: wm launch ok (app=/bin/demo pid=14 via=key)
ng: demo survives wm ok (alive=1 id=2)
ng: WP3-2 selftest PASS
QMP PPM: panel(4,4)=#202838 demo(217,177)=#3070c0
```
