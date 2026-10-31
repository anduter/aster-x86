# ng-WP3-1：display server（L2a）+ demo（PASS）

## 结论

**PASS** —— E3 / WP3-1 出口达成：ng 第一次有 **ring3 display server** 和一个
真正跑在它上面的 GUI 客户端，控制面走 IPC、数据面走共享内存，屏幕像素由
**QMP screendump** 直接断言。

- **内核最小机制（只加机制）**：syscall **45 `SYS_SHM_ALLOC(size_pages, &va,
  &handle)`**、**46 `SYS_SHM_MAP(handle, &va)`**、**47 `SYS_FB_MAP(&va, &info)`**。
  共享内存对象表（8 槽、单对象 ≤1024 页 = 4 MiB、每对象 ≤8 条映射记录、
  handle = `(gen<<16)|slot`、refcount、owner/root 校验）；LFB 以
  `PG_USER|PG_WRITE|PG_PCD|PG_PWT|PG_NX` 映射进调用者用户 AS（root-only）。
  进程退出/被 kill 时**先 `pg_unmap_in` 共享页/LFB（不 free）再 `as_destroy`**，
  避免 `as_destroy` 对同一批物理页 double free。新增编号，不改 40..44 语义。
- **display server（L2a，ring3 `/bin/displayd`）**：`SYS_FB_MAP` 拿 LFB →
  `PORT_CREATE` 建显示端口 → ticket 写 `/run/display.port`（ramfs 文本）→
  `recv` 循环处理 **CREATE / MAP / COMMIT / DESTROY / FOCUS / INPUT / BYE**，
  同时轮询 PS/2 `readkey` 把按键推给焦点客户端；surface 表 8 槽、z-order
  后→前、COMMIT 矩形裁到 surface、ARGB8888 source-over 合成（A=255 直接拷贝）、
  z-order region 重合成、8x12 基础光标；状态写 `/run/display.status`。
- **共享内存数据面**：CREATE 时 displayd `SYS_SHM_ALLOC(118 pages)`，客户端
  用 reply 里的 `shm_handle` 调 `SYS_SHM_MAP` 映射**同一批物理页**；refcount
  在客户端退出后仍为 1（displayd 持有），displayd 被 kill 后归 0（`live=0`）。
- **demo（L3，`/bin/demo`）**：读 `/run/display.port` → 建 400×300 surface →
  `SYS_SHM_MAP` → 画渐变/实心矩形 `#3070c0`/三行文字 → MAP/COMMIT/FOCUS →
  等 INPUT（QMP 注入 `b`）→ `ng: input route ok (key=b)` → 退出 0；
  另有一个 64×48 scratch surface 走 MAP→DESTROY 验证生命周期。
- **自检（run-ng 全命中）**：`ng: displayd up (fb=0xfd000000 1280x800 ...)`、
  `ng: display port ok`、`ng: surface create ok`、`ng: shm map ok`、
  `ng: composite ok (px=0x3070c0 ...)`、`ng: input route ok (key=b)`、
  `ng: WP3-1 selftest PASS`；**QMP screendump** 断言
  `(185,145)=#3070c0`（PPM P6 1280×800）。
- **崩溃隔离**：`kill(displayd, 9)` → `status=265`、`shm live=0`、内核继续跑；
  另一个客户端 `demo hold` 自己超时退出 0（`ng: demo hold wait ok
  (status=0 waiters=0)`）。
- **host 单测**：**4402 checks, 0 failures**（4043 → +359：协议编解码 8 种
  消息、surface 表/z-order/裁剪、合成 alpha/裁剪、光标、绘制/字模、共享内存
  refcount/权限/handle/回收、syscall 45..47 参数搬运）。
- **无回归**：parity **100/100**（host 95/95）、`ci-ng` **7/7 PASS**、旧 32 位
  **305/305**；run-ng 默认 **136/136 行 + 49 正则**（NoQmp 120+45、Fallback
  120+36、Smoke 121+45、qemu64 136+49）。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/desktop/include/ds_protocol.h` | L2a 协议：8 种消息、20 字节头 + 固定 body、`ds_fb_info`、发现/状态路径、表面/demo 契约常量（**新**） |
| `src/desktop/src/ds_protocol.c` | 编解码纯函数（严格 magic/version/len、类型/长度校验）（**新**） |
| `src/desktop/include/ds_surface.h` / `src/desktop/src/ds_surface.c` | surface 表纯状态机：id 单调、级联放置/贴边、z-order order[]、MAP 顺序、COMMIT 裁剪、FOCUS/DESTROY 焦点回退（**新**） |
| `src/desktop/include/ds_composite.h` / `src/desktop/src/ds_composite.c` | 软件合成器：ARGB source-over（A=0/255 快路径）、屏幕/region 裁剪、按 z-order 重合成、8x12 块光标（**新**） |
| `src/desktop/include/ds_draw.h` / `src/desktop/src/ds_draw.c` | surface 绘制：像素/填充/描边/渐变/8x8 文字（不透明画布）（**新**） |
| `src/desktop/include/ds_font8x8.h` / `src/desktop/src/ds_font8x8.c` | 8x8 ASCII 字模（由 L1D 8x16 按行对 OR 降采样，95 字形）（**新**） |
| `src/desktop/display/displayd.c` | ring3 display server（L2a）：FB_MAP、端口/发现、协议、合成、光标、输入转发、状态文件（**新**） |
| `src/apps/demo/demo.c` | L3 demo：scratch DESTROY、400×300 主 surface、渐变/矩形/文字、COMMIT、等 INPUT、hold 角色（**新**） |
| `src/ng/shm.h` / `shm_core.c` | 共享内存对象表纯状态机 + 内核/syscall API/文档（**新**） |
| `src/ng/shm.c` | 内核胶水：PMM 页 + `as_map` + uaccess + 45/46 + 进程回收（**新**） |
| `src/ng/fbmap.h` / `fbmap.c` | `SYS_FB_MAP`（47）：root-only、PCD/PWT/NX、固定 VA、释放（**新**） |
| `src/ng/wp31.h` / `wp31_selftest.c` | 内核自检编排：spawn displayd/demo、等发现文件、读状态、kill 隔离（**新**） |
| `src/ng/syscall.h` / `syscall.c` | 调用号 45/46/47 + 分发（**改**） |
| `src/ng/proc.h` / `proc.c` | `proc->shm_next_va`；种 `/bin/displayd`、`/bin/demo`；退出/kill 先 `shm_release_proc`+`fbmap_release_proc`（**改**） |
| `src/ng/main.c` | `shm_init()` + WP3-1 自检块 + PASS/log（**改**） |
| `tests/unit-ng/test_desktop.c` | 协议/表/合成/绘制 host 单测（**新**） |
| `tests/unit-ng/test_shm.c` | 共享内存 refcount/权限/handle/回收 host 单测（**新**） |
| `tests/unit-ng/fake_ng_env.[ch]` | fake `shm_sys_*` / `fbmap_sys_map`（45..47 分发）（**改**） |
| `tests/unit-ng/test_syscall.c` | 45..47 参数搬运用例（**改**） |
| `tests/unit-ng/unit_main.c` | `test_shm()` / `test_desktop()`（**改**） |
| `tools/build-ng.ps1` | L2 include、`displayd.elf`/`demo.elf` 编译/校验/.incbin、4 个内核源、59 步（**改**） |
| `tools/run-ng.ps1` | WP3-1 16 条期望行 + 7 条正则、QMP 注入 `b`、screendump+PPM 像素断言、失败模式（**改**） |
| `tools/unit-ng.ps1` | L2 源 + 2 个测试 + `-I src/desktop/include`（**改**） |
| `docs/design/display-server-ng.md` | 设计：机制/协议/surface/合成/输入/QMP/限制（**新**） |
| `docs/design/evolution.md`、`docs/design/README.md` | E3 进度 / 文档索引（**改**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`tools/parity-ng.ps1`
和 `tools/ci-ng.ps1` 也不需要改（parity 仍是 100 项，CI 复用 run-ng 日志）。

## 环境

- 分支 `ng`，基线 HEAD `0bbaded`（`[ng][WP3-0]`）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）。
  内核：`clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
  -mno-red-zone -fno-stack-protector -mgeneral-regs-only`；
  displayd/demo（用户程序）：同一 target + `-mno-sse -fno-builtin -fno-pic
  -std=c11 -O2 -I src/ng -I src/desktop/include`，`src/ng/user/user.ld` 链到
  0x40000000。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`；QMP TCP 同时
  用于 `input-send-event` 与 `screendump`。

## 关键参数

| 项 | 值 |
|---|---|
| 共享内存对象 | 8 槽；单对象 1..1024 页（4 MiB）；每对象 ≤8 条映射；`phys[]` 从 heap 分配（不占 .bss） |
| handle | `(gen<<16)|slot`；槽位复用 gen+1；旧 handle 立即 EBADF |
| SHM VA 窗口 | `0x50000000` .. `0x5F000000`；每进程 bump，对象之间一页守护 |
| FB VA | `0x60000000`；LFB 1000 页（1280×800×32 的 pitch×height） |
| 合成 | ARGB8888；A=0 跳过 / A=255 直接拷贝 / 其余 `(s*a+d*(255-a))/255` |
| surface 表 | 8 槽；最大 1280×800；首个 `(160,120)`、级联 +32 |
| demo | 400×300；实心矩形 `#3070c0`；探针 surface 局部 `(25,25)` → 屏幕 `(185,145)` |
| 协议 | 头 20 字节（magic `0x4453314D` / v1 / type / len / status / seq），整条 ≤64 |
| displayd 轮询 | recv timeout 20e6 TSC tick（QEMU TCG ≈20ms）；demo 等输入 100×20e6 |
| 内核镜像 | 2048800 字节（body < 2 MiB；.bss 里 shm 表只剩对象头） |
| 用户 ELF | displayd 35560 字节（3 PT_LOAD，end 0x40007304）；demo 24344（2 PT_LOAD，end 0x40004538） |

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
=== build-ng (WP3-1) ===
...
=== user-desktop (WP3-1) ===
  ds_protocol.c -> obj-ng/ds_ds_protocol.o
  ds_surface.c -> obj-ng/ds_ds_surface.o
  ds_composite.c -> obj-ng/ds_ds_composite.o
  ds_font8x8.c -> obj-ng/ds_ds_font8x8.o
  ds_draw.c -> obj-ng/ds_ds_draw.o
  displayd.c -> obj-ng/user_displayd.o
  link    -> obj-ng/displayd.elf (base 0x40000000)
user-displayd: PASS (35560 bytes, 3 PT_LOAD, entry 0x40002a50, end 0x40007304)
  demo.c -> obj-ng/user_demo.o
  link    -> obj-ng/demo.elf (base 0x40000000)
user-demo: PASS (24344 bytes, 2 PT_LOAD, entry 0x40000000, end 0x40004538)
...
  [38/59] shm_core.c
  [39/59] shm.c
  [40/59] fbmap.c
  [41/59] wp31_selftest.c
...
  [59/59] link -> dist-ng/aster-ng.elf

ELF64: PASS (2048800 bytes)
  entry    0x0000000000101000
  phnum    7
segments: PASS
  5 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
pvh-note: PASS
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

```text
=== run-ng (WP3-1) ===
...
ng: WP3-0 selftest PASS
ng: desktop bin ok (displayd=/bin/displayd demo=/bin/demo)
ng: displayd spawn ok (pid=8)
ng: displayd up (fb=0xfd000000 1280x800 pitch=5120 bpp=32)
ng: display port ok (ticket=917504 path=/run/display.port)
ng: display port file ok (ticket=917504)
ng: demo spawn ok (pid=9)
ng: surface create ok (id=1 w=64 h=48 shm=0x10000 pages=3)
ng: surface map ok (id=1)
ng: surface create ok (id=2 w=400 h=300 shm=0x20001 pages=118)
ng: demo surface ok (id=2 shm=0x20001 pages=118)
ng: shm map ok (va=0x50004000 pages=118)
ng: demo input ready (id=2)
ng: surface map ok (id=2)
ng: composite ok (px=0x3070c0 at=160,120)
ng: displayd input ok (key=b scancode=0x30)
ng: input route ok (key=b id=2)
ng: demo done (id=2 input=1)
ng: demo exit ok (status=0)
ng: shm refcount ok (live=2)
ng: displayd status ok (surface=1 commits=1 input=1 px=0x3070c0)
ng: demo hold spawn ok (pid=10)
ng: demo hold ready (pid=10)
ng: displayd kill ok (status=265 live=0)
ng: demo hold done (alive=1)
ng: demo hold wait ok (status=0 waiters=0)
ng: WP3-1 selftest PASS
...
run-ng: QMP connected (sendkey injection armed)
run-ng: QMP sendkey a -> ring3 readkey
run-ng: QMP sendkey b -> displayd input route
run-ng: screendump pixel ok (185,145)=#3070c0
run-ng: QMP typing WP2-6 shell commands
...
run-ng: screendump pixel: (185,145)=#3070c0 (QMP PPM)
run-ng: matched 136/136 expected lines + 49 patterns
run-ng: PASS (all expected lines in 6.4s, cpu max)
```

`run-ng.ps1` 新增期望（16 条固定行 + 7 条正则，其中 1 条是 `-NoQmp` 分支）：

```text
ng: desktop bin ok (displayd=/bin/displayd demo=/bin/demo)
ng: displayd spawn ok (pid=
ng: displayd up (fb=0xfd000000 1280x800
ng: display port ok (ticket=
ng: display port file ok (ticket=
ng: demo spawn ok (pid=
ng: surface create ok (id=
ng: surface map ok (id=
ng: shm map ok (va=0x
ng: composite ok (px=0x
ng: input route ok (
ng: demo exit ok (status=0)
ng: shm refcount ok (live=
ng: displayd status ok (surface=1
ng: demo hold spawn ok (pid=
ng: displayd kill ok (status=265
ng: demo hold wait ok (status=0

ng: displayd up \(fb=0x[0-9a-f]+ 1280x800 pitch=[0-9]+ bpp=32\)
ng: surface create ok \(id=[0-9]+ w=400 h=300 shm=0x[0-9a-f]+ pages=118\)
ng: shm map ok \(va=0x[0-9a-f]+ pages=118\)
ng: composite ok \(px=0x3070c0 at=[0-9]+,[0-9]+\)
ng: displayd status ok \(surface=1 commits=[0-9]+ input=[0-9]+ px=0x3070c0\)
ng: demo hold wait ok \(status=0 waiters=0\)
（QMP 模式）ng: input route ok \(key=b id=[0-9]+\)
（-NoQmp）  ng: input route ok \(timeout id=[0-9]+\)
（-Fallback）ng: WP3-1 selftest skipped \(no fb\)
失败早停模式新增：
ng: WP3-1 .* FAIL / ng: displayd .* FAIL / ng: surface .* FAIL /
ng: composite .* FAIL / ng: demo .* FAIL
```

### QMP 截图像素断言

run-ng 在看到 `ng: composite ok (px=0x3070c0 at=160,120)` 后用 QMP
`screendump` 写 PPM（P6 / 1280×800 / maxval 255），解析头后读
`(160+25, 120+25) = (185,145)`：

```text
run-ng: QMP screendump response: {"return": {}}
run-ng: screendump pixel ok (185,145)=#3070c0
run-ng: screendump pixel: (185,145)=#3070c0 (QMP PPM)
```

辅助像素（同一张 PPM）：`(160,120)=#000040`（渐变的左上角）、
`(200,200)=#3070c0`（实心矩形内部）、`(0,0)=#101820`（内核 fb 控制台背景，
证明 surface 只覆盖它自己的区域）。

### 负向 / 降级 / 其他 CPU / Smoke

```text
run-ng -NoQmp        -> 120/120 lines + 45 patterns PASS（input 走 timeout）
run-ng -Fallback     -> 120/120 lines + 36 patterns PASS（-vga none，
                        ng: WP3-1 selftest skipped (no fb)）
run-ng -Cpu qemu64   -> 136/136 lines + 49 patterns PASS（SMEP/SMAP off）
build-ng -Smoke + run-ng -Smoke -> 121/121 lines + 45 patterns PASS
                        （只跳过 shell；WP3-1 + screendump 照跑）
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
parity(host): 95/95 PASS (5 display/input cases are kernel-only)
4402 checks, 0 failures
unit-ng: PASS (4402 checks, 0 failures)
```

WP3-0 为 4043，本 WP **+359**：

- `tests/unit-ng/test_desktop.c`（新）：8 种消息的 body 长度/往返/严格拒绝
  （bad magic/version/type/len/truncation/unknown）、surface 表 id 单调/级联
  放置/贴边/表满/非法几何与页数/MAP 顺序/COMMIT 裁剪（部分、右下角、完全
  不可见、空矩形）/FOCUS 提层/DESTROY 焦点回退/几何 helper、合成 z-order
  （覆盖顺序 + 焦点后反转）、alpha（0/128/255）、屏幕与 region 裁剪、
  光标绘制/裁剪、绘制边界/渐变公式/8x8 字形朝向/文字步进；
- `tests/unit-ng/test_shm.c`（新）：handle 编解码与 generation 复用、owner/
  root/非 root 权限、refcount 到 0 才 free、每对象映射上限、表满、页数上限、
  collect/release_pid、abort 前置条件、统计计数；
- `tests/unit-ng/test_syscall.c`（+）：45/46/47 的寄存器→参数搬运与返回值
  透传；`fake_ng_env` 新增 fake 胶水。

### 门禁

```text
tools/check-domains.ps1
checked: 248 files, 784 quoted includes
domain include violations: 0

tools/check-deps.ps1
OK: no cross-module include violations.

tools/ci-ng.ps1
  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          4402 checks; exit=0, 8.0s
  PASS build-ng         2048800 bytes, user-elf 15872 bytes; exit=0, 9.2s
  PASS run-ng           136/136 lines + 49 patterns; exit=0, 6.2s
  PASS parity-ng        100/100 items, 10 domains; exit=0, 0.2s
  PASS legacy-32bit     305/305 parity checks; exit=0, 39.9s
  7 passed, 0 failed, 0 skipped, 64.4s
=== NG CI PASS ===
```

旧 32 位独立 `build.ps1 -Test` 也是 **305/305**。

## 设计要点（详见 `docs/design/display-server-ng.md`）

### 控制面 / 数据面

```text
CREATE(w,h) --> displayd
  SYS_SHM_ALLOC(118) -> {va_d, handle=0x20001}
  <-- CREATE_REPLY{id, handle, pages, x,y,w,h}
demo: SYS_SHM_MAP(handle) -> va_c                 同一批物理页
  MAP/COMMIT/FOCUS --> 像素在 shm 里，消息只有几十字节
  <-- INPUT{id,key,scancode,flags}（只发给焦点客户端）
DESTROY(id) -->（scratch surface 走这条）
```

### 共享内存 refcount 与 as_destroy 的顺序

```text
ALLOC: refcnt=1（owner 映射）
MAP  : refcnt++（记录 {pid,va}）
退出 : shm_release_proc()
         1) collect pid 的所有 VA
         2) pg_unmap_in(AS, va)          <-- 只摘 PTE，不 free 物理页
         3) core release_pid: refcnt--；=0 才 pmm_free_page(phys[i])
       然后才 as_destroy()（它会 free 所有 present 叶子页）
```

### 像素格式 / 合成

```text
surface 缓冲: u32 = 0xAARRGGBB（画布不透明，A=255）
LFB:          QEMU stdvga BGRX 小端 => 内存 u32 = 0x00RRGGBB，逐位相等
A=255 -> 直接拷贝 RGB（screendump 断言依赖）
A=0   -> 跳过
其余  -> (s*a + d*(255-a)) / 255
region 用屏幕坐标，surface 裁剪用 hit-(sf->x,sf->y) 转局部
```

## 自检项清单

1. 内核机制：45/46/47；共享页 refcount；owner/root；LFB PCD/PWT/NX 用户映射；
   uaccess 校验用户指针；进程退出/kill 回收（host 单测 + 自检 live=2→0）。
2. displayd：FB_MAP、PORT_CREATE、`/run/display.port`、协议 8 种消息、surface
   表/z-order/裁剪、source-over 合成、光标、焦点、PS/2 输入转发、状态文件。
3. 共享内存：displayd 分配、demo 映射同一批页（`shm map ok va=0x50004000`），
   客户端退出后 `live=2`，displayd 被杀后 `live=0`。
4. 自检证据行：见上文 run-ng 串口块；QMP 截图像素 `(185,145)=#3070c0`。
5. 崩溃隔离：kill displayd -> status=265、内核继续、hold 客户端自行退出 0。
6. host 单测 4402 checks / 0 failures（+359）。
7. 门禁：run-ng 136+49（NoQmp 120+45 / Fallback 120+36 / Smoke 121+45 /
   qemu64 136+49）；parity 100/100（host 95/95）；ci-ng 7/7；旧 32 位 305/305。

## 未做 / 遗留（按 WP 边界）

- **cap 未实现**：SHM_MAP 的 owner/root 校验就是全部权限（当前所有进程 uid=0，
  所以等于"知道 handle 就能映射"）；cap grant/revoke 与 ticket 防伪留给 ABI
  1.0 / WP3-3。非 root uid 的表语义已由 host 单测覆盖。
- **没有 SYS_SHM_FREE**：surface DESTROY 只清表项，displayd 的共享映射要等
  displayd 退出才释放；长跑会话的 surface 生命周期属 WP3-3。
- **单线程、单客户端**：displayd 一次处理一条消息，没有并发客户端/背压；
  demo 只建一个主 surface（scratch 只走 MAP/DESTROY）。
- **输入只有键盘按下**：PS/2 轮询（无 IRQ）、没有鼠标/按键释放/重复率；
  displayd 焦点固定，没有点击焦点切换。
- **没有 vsync/双缓冲/脏区合并**：每次 COMMIT 重合成 surface 覆盖的屏幕
  region（z-order 正确）；D1 再谈 buffer 队列。
- **没有 resize/装饰/多显示器/DPMS**：CREATE 后尺寸固定；server-side
  decoration 属 WP3-2。
- **像素断言依赖 QEMU stdvga + QMP screendump PPM**：其它卡/VGA 文本走
  `-Fallback` 跳过；真机在 D1 锁定硬件后再补。
- **VFS fd 表仍全局一份**：displayd/demo 的 open/close 之间没有阻塞点，协作式
  单核下安全；每进程 fd 表属 fsd / WP3-x。

## 出口

```text
powershell -File tools/build-ng.ps1              # PASS，2048800 bytes，59 步
powershell -File tools/build-ng.ps1 -Smoke       # PASS（跳过 shell）
powershell -File tools/run-ng.ps1                # PASS，136 行 + 49 正则 + 截图像素
powershell -File tools/run-ng.ps1 -NoQmp         # PASS，120 行 + 45 正则
powershell -File tools/run-ng.ps1 -Fallback      # PASS，120 行 + 36 正则
powershell -File tools/run-ng.ps1 -Cpu qemu64    # PASS，136 行 + 49 正则
powershell -File tools/unit-ng.ps1               # PASS，4402 checks, 0 failures
powershell -File tools/parity-ng.ps1             # PASS，100/100（host 95/95）
powershell -File tools/ci-ng.ps1                 # 7/7 PASS
powershell -File build.ps1 -Test                 # PASS，305/305（旧 32 位）
```
