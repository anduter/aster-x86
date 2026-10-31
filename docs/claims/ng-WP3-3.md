# ng-WP3-3：session manager（L2c）+ display 协议 v3（PASS）

## 结论

**PASS** —— E3 / WP3-3 出口达成：ng 第一次有 **ring3 session manager**
（`/bin/sessiond`），**应用生命周期归会话、不归 shell**：

- **启动顺序**：init 只 `spawn /bin/sessiond`；sessiond `spawn` `/bin/displayd`
  与 `/bin/wm`。sessiond 先 `unlink /run/display.port`（丢掉上一轮 displayd
  的陈旧 ticket）再等新文件，并用一次 `LIST → LIST_REPLY` 握手确认
  displayd 真的能服务（wp31/wp32 只打印票据、不发 IPC，所以历史上没暴露
  这个坑）。
- **会话通道**：sessiond 把自己的端口 ticket 经 argv 交给被启动进程
  （`session=<ticket>`、`token=0x<hex>`），**不新建 ramfs 发现文件**
  （节点池只有 32 个，`/bin/sessiond` 已占一个）。
- **应用生命周期**：WM 的启动器**不再 spawn**，改为 `LAUNCH(token,path)`
  → sessiond 校验「令牌存在 + 角色 = WM + running」后 `spawn` 应用，签发
  32 位 role_token（`seq|role|nonce`），登记 `{pid,token,role,surface,title}`；
  `EXIT`/`KILL`/心跳超时（800ms）回收窗口与进程。
- **窗口注册表**：`{surface_id, app_pid, title, token}`；应用 `REGISTER`
  上报窗口，sessiond 提供 `LOOKUP`/`LIST`，每 200ms 与 displayd 的 `LIST`
  同步（同 id / 同 owner_pid / 同 token 三字段），全部一致才打印
  `ng: session registry ok (apps=.. windows=.. procs=..)` 与
  `ng: role token ok (tokens=.. bound=.. mismatch=0)`。
- **焦点策略**：新窗口 REGISTER 后由 sessiond 发 `FOCUS(surface, token)`
  （令牌 = 窗口自己的）；WM 的任务切换仍直接 FOCUS，但 v3 的 FOCUS 带令牌，
  displayd 只接受「surface 自己的令牌」或「WM 令牌」——应用越权 FOCUS 别人
  的窗口会被拒绝（`ng: focus reject ok`）。
- **WM 崩溃隔离/重启**：kill WM 后 Demo 进程与窗口保活；sessiond 心跳超时
  判定死亡 -> `DESTROY` 旧面板 -> `kill/wait` -> 按 `restart_limit=3` +
  指数退避（20/40/80ms，TSC tick）`spawn` 新 WM（新令牌）；新面板回来后
  `ng: session wm restart ready ok`，第三张 QMP 截图同时断言面板与 Demo 窗口。
- **角色令牌**：32 位 `seq(8)|role(8)|nonce(16)`；0 永远不是合法令牌；
  CREATE v3 带 token、LIST v3 报 token、FOCUS v3 校验 token；运行期负向证据
  两条（应用用自己的令牌发 LAUNCH -> sessiond 拒绝；应用用自己的令牌 FOCUS
  面板 -> displayd 拒绝）。
- **host 单测**：**5025 checks, 0 failures**（4684 → +341：会话协议/令牌/
  会话表/注册表同步/退避/状态文本）。远超 ≥80 的要求。
- **无回归**：parity **100/100**（host 95/95）、`ci-ng` **7/7 PASS**、旧 32 位
  **305/305**；run-ng 默认 **159/159 行 + 75 正则**（NoQmp 143+70、Fallback
  122+36、Smoke 144+71、qemu64 159+75）。
- **内核镜像**：7 个用户 ELF 的 `.incbin` 从 `.rodata` 挪到 2 MiB 之后的
  `.bigdata`（PD[1] 恒等大页 + PMM 保留），`__kernel_end=0x1E3000`
  （2 MiB 窗口内留 ~118 KB）；内核镜像 2,082,264 字节。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/desktop/include/session_protocol.h` / `src/desktop/src/session_protocol.c` | 会话协议 + 角色令牌布局 + `session_arg_parse_u32` 纯函数（**新**） |
| `src/desktop/include/session_table.h` / `src/desktop/src/session_table.c` | 会话表/令牌签发/窗口注册/心跳/EXIT/KILL 权限/计数/作废令牌环/LIST 同步校验/指数退避/状态文本（**新**） |
| `src/desktop/session/sessiond.c` | ring3 `/bin/sessiond`：spawn displayd+wm、端口发现握手、LAUNCH/EXIT/KILL、注册表同步、心跳回收、WM 重启、SESSION_STATUS（**新**） |
| `src/desktop/include/ds_protocol.h` / `src/desktop/src/ds_protocol.c` | 协议 **v3**：CREATE token（body 44）、LIST entry token（48/body 392）、FOCUS token、SESSION_STATUS（68）；v1/v2 兼容；`DS_PROTO_VERSION=3`（**改**） |
| `src/desktop/include/ds_surface.h` / `src/desktop/src/ds_surface.c` | surface 增加 `token`；`ds_table_create_ex` 带 token；`ds_table_fill_list` 带 token（**改**） |
| `src/desktop/display/displayd.c` | v3：CREATE 记 token、LIST 报 token、FOCUS 令牌校验（surface 令牌 / WM 令牌）、从 WM 面板学到 `wm_token`、SESSION_STATUS 汇总进 `/run/display.status`（`s*` 字段）、状态缓冲 512（**改**） |
| `src/desktop/shell/wm.c` | 会话模式：argv 解析 `session=/token=`、REGISTER/HEARTBEAT/LIST、启动器发 LAUNCH（不再 spawn）、`wm session/registry` 证据；无会话通道时保留 WP3-2 直连兼容；无任务时 3s 自动启动第一个应用（NoQmp）（**改**） |
| `src/apps/demo/demo.c` | argv 会话参数、CREATE 带 token、窗口 REGISTER、心跳、EXIT、两条负向探针（越权 LAUNCH / 越权 FOCUS）（**改**） |
| `src/ng/proc.c` | 种 `/bin/sessiond`（第 7 个用户 ELF），`desktop bin ok (.. sessiond=/bin/sessiond)`（**改**） |
| `src/ng/wp33.h` / `src/ng/wp33_selftest.c` | 内核自检编排：spawn sessiond、状态等待（s* 字段）、kill WM、重启校验、应用存活/回收、清理并归还两个 ramfs 节点（**新**） |
| `src/ng/main.c` | `wp33_selftest()` + `ng: WP3-3 selftest PASS/FAIL`（**改**） |
| `src/ng/link.ld` / `src/ng/mem_selftest.c` | 用户 ELF blob 从 `.rodata` 挪到 2 MiB 后的 `.bigdata` 只读段，PMM 保留 `[__bigdata_start,__bigdata_end)`（**改**） |
| `src/ng/fs_syscall.h` / `src/ng/fs_syscall.c` | 诊断接口 `fs_sys_ramfs()`（清理时打印节点数）（**改**） |
| `tests/unit-ng/test_session.c` | 会话协议/令牌/path·argv/会话表/生命周期/同步/退避/状态文本（**新**，+341 checks） |
| `tests/unit-ng/test_desktop.c` / `test_wm.c` | v3 长度表（CREATE 44、LIST_REPLY 392、entry 48）、CREATE token 往返、v1/v2 解码 token=0、`ds_table_create_ex`/`ds_list_entry_set` 新签名（**改**） |
| `tests/unit-ng/unit_main.c` / `tools/unit-ng.ps1` | `test_session()` + `session_protocol.c`/`session_table.c` 源（**改**） |
| `tools/build-ng.ps1` | `sessiond.elf`（3 PT_LOAD）、`session_protocol.c`/`session_table.c`、用户 ELF blob 段 `.bigdata`、`wp33_selftest.c`（**改**） |
| `tools/run-ng.ps1` | WP3-3 期望行/正则；QMP 在 `ng: session wm ready ok` 后注入 `1`（WM LAUNCH）；第三张 screendump 断言 WM 重启后的面板 + Demo 窗口（**改**） |
| `docs/design/session-ng.md` | L2c 设计（启动/令牌/协议/生命周期/同步/重启/状态/`.bigdata`/遗留）（**新**） |
| `docs/design/display-server-ng.md` | 第 14 节：协议 v3 + L2c 指针（**改**） |
| `docs/design/evolution.md`、`docs/design/README.md` | E3 进度 / 文档索引（**改**） |

未修改 `src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h`；`tools/parity-ng.ps1`
和 `tools/ci-ng.ps1` 不需要改（parity 仍 100 项，CI 复用 run-ng 日志）。

## 环境

- 分支 `ng`，基线 HEAD `c4efabc`（`[ng][WP3-2]`）；仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）。
  内核：`clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
  -mno-red-zone -fno-stack-protector -mgeneral-regs-only`；
  用户程序：同一 target + `-mno-sse -fno-builtin -fno-pic -std=c11 -Os -Wall
  -Wextra -Werror -ffunction-sections -fdata-sections`，链接 `--gc-sections`。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`；QMP TCP
  用于 `input-send-event`（a/b/tab/1/1）与三次 `screendump`。

## 关键参数

| 项 | 值 |
|---|---|
| 会话表 | 8 槽（WM + 7 应用）；pid 单调；令牌 seq/nonce 单调 |
| role_token | 32 位：`seq(8)|role(8)|nonce(16)`；role WM=1/APP=2/SERVICE=3/SESSION=4；0 非法 |
| 会话协议 | 头 20 B；REGISTER 28、HEARTBEAT 8、LAUNCH 84、EXIT 8、KILL 12、LOOKUP 8、ENTRY 48、LIST 4、LIST_REPLY 336；`SESSION_MSG_MAX=512` |
| display 协议 v3 | 头 20 B；CREATE 44（v2 40/v1 16）、LIST_REPLY 392（v2 360）、FOCUS 8（第二字 = token）、SESSION_STATUS 68；`DS_MSG_MAX=512` |
| 心跳/超时 | 客户端每 ~200ms HEARTBEAT；`S_STALE_TICKS=800ms`（约 40 × 20ms 轮） |
| WM 重启 | `restart_limit=3`；退避 20/40/80ms（160ms 封顶）；`wmstate 0/1/2` |
| 端口发现 | unlink `/run/display.port` → 等新文件 → LIST 握手（20 × 20ms 重试，总 3s） |
| ramfs | 会话状态**不新建文件**：SESSION_STATUS → `/run/display.status`；wp33 清理时 unlink port/status 归还 2 个节点（池 32） |
| 内核布局 | `.bigdata` = 0x368000..0x389F20（PD[1] 内）；`__kernel_end=0x1E3000`（< 0x200000） |
| 内核镜像 | 2,082,264 字节（5 PT_LOAD） |
| 用户 ELF | displayd 27,768（3 PT_LOAD）/ demo 18,416（3）/ wm 24,784（3）/ sessiond 24,056（3）/ shell 18,848（3）/ proc 9,280 / hello 15,872 |
| 状态文件 | `/run/display.status` 追加 `sess=1 spid= sdpid= sprocs= sapps= swindows= stokens= slaunches= skills= sexits= sreaps= srestarts= swmpid= swmstate= sapppid= stokenfail= sregistry=` |

## display 协议 v3 变更（vs v2）

| 消息 | 方向 | body | 语义 |
|---|---|---|---|
| `CREATE`（v3） | client→server | w,h,flags,x,y,owner_pid,title[4],**token**（44 B） | token = sessiond 签发的 role_token；v2 客户端 40 B 解码后 token=0 |
| `LIST_REPLY`（v3） | server→client | count + entries[8]（392 B） | 每条 48 B：id/owner_pid/x/y/w/h/focused/**token**/title[4] |
| `FOCUS`（v3） | client→server | surface_id,**token**（8 B） | token=0 旧行为；非 0 时要求 `surface.token == token` 或 `token == wm_token`（displayd 从 WM 面板的 CREATE 学到） |
| `SESSION_STATUS`（新） | sessiond→server | pid,dpid,procs,apps,windows,tokens,launches,kills,exits,reaps,restarts,wmpid,wmstate,apppid,tokenfail,registry,reserved（68 B） | 汇总进 `/run/display.status`（不新建 ramfs 节点，内核 wp33 读 `s*` 字段） |

兼容性：

1. 解码器接受 `version == 1 || 2 || 3`；v1 CREATE 16 B、v2 CREATE 40 B，
   token 一律 0；v2 LIST_REPLY 仍按 360 B 解码。
2. 编码器始终写 v3；surface id / shm handle / 焦点语义不变。
3. `DESTROY` 仍无归属校验（cap/TODO，见遗留）；sessiond 用它回收死进程的窗口。
4. `FOCUS` 无 ack：WM 仍在**下一次 LIST_REPLY** 里确认焦点后才记一次切换。

## 会话协议（新）

| 消息 | 方向 | body | 语义 |
|---|---|---|---|
| `REGISTER` | client→sessiond | token,port,surface_id,title[4]（28 B） | 上报自己的窗口（WM 面板/应用窗口） |
| `HEARTBEAT` | client→sessiond | token,rounds（8 B） | 存活心跳 |
| `LAUNCH` | wm→sessiond | token,path[16],arg[4]（84 B） | 只有 WM 角色可以；sessiond `spawn` 并签发新令牌 |
| `EXIT` | client→sessiond | token,code（8 B） | 正常退出（回收窗口/进程） |
| `KILL` | wm→sessiond | token,pid,surface_id（12 B） | 会话内终止应用 |
| `LOOKUP` | client→sessiond | surface_id,pid（8 B） | 注册表查询 |
| `ENTRY` | sessiond→client | found + 40 B 记录（48 B） | LOOKUP 的回复 |
| `LIST` / `LIST_REPLY` | client↔sessiond | 4 / 336 B | 注册表快照（WM 启动时查一次） |
| `SESSION_STATUS` | client→sessiond | reserved（4 B） | 保留 |

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
=== build-ng (WP3-3) ===
...
=== user-desktop (WP3-3) ===
...
user-displayd: PASS (27768 bytes, 3 PT_LOAD, entry 0x400011bb, end 0x40005448)
user-demo: PASS (18416 bytes, 3 PT_LOAD, entry 0x40000000, end 0x40003008)
user-wm: PASS (24784 bytes, 3 PT_LOAD, entry 0x40000f38, end 0x40004720)
user-sessiond: PASS (24056 bytes, 3 PT_LOAD, entry 0x40001395, end 0x400042d0)
...
ELF64: PASS (2082264 bytes)
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

布局符号（`nm dist-ng/aster-ng.elf`）：

```text
0000000000200000 B __fat16_img_start
0000000000368000 R __fat16_img_end
0000000000368000 R __bigdata_start
0000000000389f20 R __bigdata_end
00000000001e3000 B __kernel_end
```

### 串口（`powershell -File tools/run-ng.ps1`，`-cpu max`）

```text
=== run-ng (WP3-1) ===
...
ng: WP3-2 selftest PASS
ng: sessiond spawn ok (pid=15)
<displayd> ng: displayd up (fb=0xfd000000 1280x800 pitch=5120 bpp=32)
<displayd> ng: display port ok (ticket=589825 path=/run/display.port)
<displayd> ng: displayd list ok (n=0 focus=0)
<sessiond> ng: sessiond up (pid=15 displayd=16 wm=17 port=1048576)
<displayd> ng: displayd session status ok (procs=1 apps=0 windows=0 restarts=0)
<displayd> ng: surface create ok (id=1 w=1280 h=32 shm=0x60000 pages=40)
<displayd> ng: surface token ok (id=1 token=0x21950102 role=1)
ng: session start ok (pid=15 displayd=16 wm=17)
ng: session wm ready ok (wm=17)
<wm>       ng: wm up (pid=17 panel=1280x32)
<wm>       ng: wm panel ok (px=0x202838 at=0,0)
<wm>       ng: wm launcher ok (apps=4 pkgs=1)
<wm>       ng: wm session ok (session=1048576 token=0x21950102)
<sessiond> ng: session wm register ok (wm=17 id=1)
<sessiond> ng: session focus ok (id=1 pid=17)
<wm>       ng: wm registry ok (apps=0 windows=1 procs=1)
<sessiond> ng: session registry ok (apps=0 windows=1 procs=1)
<sessiond> ng: role token ok (tokens=1 bound=1 mismatch=0)
<displayd> ng: displayd hotkey ok (key=0x31 scancode=0x2)      <- QMP 注入 '1'
<wm>       ng: wm launch req ok (app=/bin/demo via=key)
<sessiond> ng: session app launch ok (pid=18 app=/bin/demo token=0x553b0203)
<displayd> ng: surface token ok (id=2 token=0x553b0203 role=2)
<demo>     ng: demo window up (id=2 title=Demo)
<demo>     ng: demo session ok (session=1048576 token=0x553b0203)
<demo>     ng: demo role probe ok (path=/bin/demo)
<sessiond> ng: session window ok (pid=18 id=2 title=Demo)
<sessiond> ng: session focus ok (id=2 pid=18)
<sessiond> ng: session launch reject ok (token=0x553b0203 role=2 n=1)
<demo>     ng: demo focus probe ok (id=1 token=0x553b0203)
<displayd> ng: focus reject ok (id=1 token=0x553b0203 n=1)
ng: session launch seen ok (pid=18)
ng: session registry seen ok (apps=1 windows=2)
ng: role token seen ok (tokens=2 tokenfail=0)
ng: session wm killed ok (pid=17)
<sessiond> ng: session wm dead ok (pid=17)
<sessiond> ng: session wm restart sched ok (old=17 restarts=0 backoff_ticks=20000000)
<sessiond> ng: session wm restart ok (old=0 new=19 restarts=1)
ng: session wm restart seen ok (old=17 new=19 restarts=1)
ng: session survives wm ok (app=18 alive=1 wm=19 restarts=1)
<wm>       ng: wm up (pid=19 panel=1280x32)                      <- 新 WM
<displayd> ng: surface create ok (id=3 w=1280 h=32 shm=0x80002 pages=40)
<sessiond> ng: session wm restart ready ok (wm=19 id=3)
<wm>       ng: wm task ok (id=2 x=192 y=152 title=Demo)          <- 应用窗口还在
<sessiond> ng: session app reap ok (pid=18 why=stale)            <- kill 应用 -> 回收
ng: session app reap ok (pid=18 reaps=1)
ng: session cleanup ok (live=0 nodes=30)
ng: WP3-3 selftest PASS
...
run-ng: QMP sendkey 1 -> session LAUNCH (sessiond spawn demo)
run-ng: screendump pixel ok (185,145)=#3070c0
run-ng: wm screendump pixel ok panel(4,4)=#202838 demo(217,177)=#3070c0
run-ng: wp3-3 screendump pixel ok panel(4,4)=#202838 demo(217,177)=#3070c0
run-ng: matched 159/159 expected lines + 75 patterns
run-ng: PASS (all expected lines in 7.1s, cpu max)
```

### QMP 截图像素断言（第三张 PPM）

`ng: session wm restart ready ok` 之后 screendump（`screen3.ppm`）：

```text
run-ng: wp3-3 screendump pixel ok panel(4,4)=#202838 demo(217,177)=#3070c0
```

- `(4,4)`：**重启后的新 WM** 画的 1280×32 面板（id=3，`(0,0)`）已被合成；
  旧面板（id=1）已被 sessiond 在 WM 死亡时 DESTROY（`displayd list n=2`）。
- `(217,177) = (192+25, 152+25)`：Demo 窗口的实心矩形，证明**应用不属于
  WM** —— WM 重启后应用窗口仍在。
- 同一张 PPM 还隐含：新面板任务按钮没有盖住窗口、光标没盖住探针。

### 负向 / 降级 / 其它 CPU / Smoke

```text
run-ng -NoQmp        -> 143/143 lines + 70 patterns PASS（9.0s）
                        （WM 3s 无任务自动 LAUNCH（via=auto）；不做 screendump）
run-ng -Fallback     -> 122/122 lines + 36 patterns PASS（4.8s）
                        （-vga none：`ng: WP3-3 selftest skipped (no fb)`）
run-ng -Cpu qemu64   -> 159/159 lines + 75 patterns PASS（6.6s，SMEP/SMAP off）
build-ng -Smoke + run-ng -Smoke -> 144/144 lines + 71 patterns PASS（3.4s）
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
parity(host): 95/95 PASS (5 display/input cases are kernel-only)
5025 checks, 0 failures
unit-ng: PASS (5025 checks, 0 failures)
```

WP3-2 为 4684，本 WP **+341**（`tests/unit-ng/test_session.c`）：

- 协议尺寸表（register/heartbeat/launch/exit/kill/lookup/entry/list/reply/
  status、`SESSION_MSG_MAX`、session_msg 大小）；
- 消息编解码往返（REGISTER 标题、HEARTBEAT、LAUNCH path/arg、EXIT、KILL、
  LOOKUP、ENTRY、LIST、LIST_REPLY、SESSION_STATUS）；
- 拒绝路径：截断/多一字节、header len 不符、坏 magic、坏版本、未知类型、
  cap 不够、NULL；
- 令牌：make/role/seq/nonce、0/非法位型、归一化（role=0/seq=0/nonce=0）、
  `token_is`、不同 nonce/seq/role 产生不同令牌；
- path/arg 打包：空/NULL/超长截断/cap 截断/cap=0 不写；
- argv 解析：十进制、`0x`/`0X` 十六进制、缺数字、坏字符、前缀不符、NULL、
  32 位溢出、十进制溢出、前导 0；
- 会话表：init/alloc（pid/role/令牌/state/last_seen）、重复 pid、
  pid=0、非法角色、表满、`alloc_with_token`（合法/重复/角色不符/0）；
- 生命周期：REGISTER（标题/端口/surface）、同一 surface 冲突、未知令牌、
  NULL 标题、HEARTBEAT 计数、LAUNCH/KILL 权限（WM/APP/未知/0/NULL）、
  EXIT（状态/计数/幂等/EXIT 后 heartbeat/register/launch 被拒）、
  `mark_dead`（WM 不计 reaps、APP 计 reaps）；
- 计数：apps/windows/procs/last_app_pid/tokens；
- LIST 同步：一致（bound=2/registry_ok）、owner 不一致（绑定+反查各一次）、
  token 不一致、外来令牌、窗口缺失不清引用、token=0 旧客户端、作废令牌跳过、
  空/NULL LIST；
- 作废令牌环：最近 4 个、最旧被挤掉、0/NULL 忽略；
- 退避：`session_backoff_ticks`（0..100、封顶）、`session_should_restart`
  （limit=3）；
- 状态文本：`up/pid/dpid/procs/apps/windows/tokens/launches/restarts/wmpid/
  wmstate/apppid/tokenfail`、结尾 `\n`、cap 截断、NULL。

### 门禁

```text
tools/ci-ng.ps1
  PASS check-domains    0 violations; exit=0, 0.5s
  PASS check-deps       0 violations; exit=0, 0.5s
  PASS unit-ng          5025 checks; exit=0, 8.8s
  PASS build-ng         2082264 bytes, user-elf 15872 bytes; exit=0, 10.5s
  PASS run-ng           159/159 lines + 75 patterns; exit=0, 7.2s
  PASS parity-ng        100/100 items, 10 domains; exit=0, 0.2s
  PASS legacy-32bit     305/305 parity checks; exit=0, 41.8s
  7 passed, 0 failed, 0 skipped, 69.6s
=== NG CI PASS ===
```

旧 32 位独立 `build.ps1 -Test` 也是 **305/305**。

## 设计要点（详见 `docs/design/session-ng.md`）

### 应用不归 shell 管

```text
wm: '1' -> SESSION_LAUNCH{token=WM, path="/bin/demo"}
sessiond: session_launch_allowed() -> spawn("/bin/demo",
           {"window", "session=1048576", "token=0x553b0203"})
         session_alloc_with_token(pid=18, APP, token)
demo: CREATE(400x300, token) -> REGISTER(token, port, id=2, "Demo")
sessiond -> displayd FOCUS(id=2, token)     （新窗口获得焦点）
sessiond 每 200ms 发 LIST，校验 (id, owner_pid, token)
kill WM(17) -> sessiond: DESTROY 旧面板 -> kill/wait -> 退避 20ms -> spawn WM(19)
```

### 端口发现：陈旧文件 + 握手

wp31/wp32 的 displayd 被杀后 `/run/display.port` 仍在。sessiond 先 `unlink`，
再等新文件，再用 `LIST → LIST_REPLY` 确认对端活着；否则会像首次实现那样
拿到死 ticket、`PORT_SEND` 返回 `EBADF(-9)`，状态永远写不出去。

### ramfs 节点预算（32 个）

`/bin/sessiond` 是第 27 个节点；会话状态如果新建 `/run/session.status` 会把
节点池占满，后面的 ring3 shell 会话 `echo hi > /tmp/s.txt` 直接 `ENOSPC`。
所以会话状态走 displayd 的 `SESSION_STATUS` 汇总；wp33 清理时再 unlink
`/run/display.port` 与 `/run/display.status` 归还 2 个节点
（`ng: session cleanup ok (live=0 nodes=30)`，随后 shell 的 `s.txt=3` 通过）。

### 内核 2 MiB 窗口

7 个用户 ELF 的 `.incbin` 原先在 `.rodata`，加上 sessiond 后
`__kernel_end` 会到 `0x204000`（超 2 MiB）。把 blob 挪到 2 MiB 后的
`.bigdata`（PD[1] 恒等大页，只读、不执行）+ PMM 保留后，`__kernel_end`
回到 `0x1E3000`，余量 ~118 KB；`.bigdata` 结束地址 `0x389F20` 仍在
PD[1] 的 2 MiB 之内。

## 自检项清单

1. 启动：init 只 spawn sessiond；sessiond spawn displayd + wm；
   `ng: sessiond spawn ok` / `ng: sessiond up (pid=.. displayd=.. wm=.. port=..)`。
2. 会话通道：WM/应用经 argv 拿 ticket+token；
   `ng: wm session ok (session=.. token=0x..)` / `ng: demo session ok (...)`。
3. LAUNCH：WM 启动器发 LAUNCH（不 spawn）；
   `ng: wm launch req ok (app=/bin/demo via=key)` /
   `ng: session app launch ok (pid=18 app=/bin/demo token=0x..)`。
4. 注册表：REGISTER + 每 200ms LIST 同步；
   `ng: session window ok` / `ng: session registry ok (apps=1 windows=2 procs=2)`。
5. 令牌：CREATE/LIST/FOCUS 三处绑定校验；
   `ng: role token ok (tokens=2 bound=2 mismatch=0)`；
   负向：`ng: session launch reject ok (role=2)` / `ng: focus reject ok`。
6. WM 崩溃：`ng: session wm killed ok` -> `ng: session wm dead ok` ->
   `ng: session wm restart ok (old=.. new=.. restarts=1)` ->
   `ng: session wm restart ready ok`（新面板）。
7. 应用保活/回收：`ng: session survives wm ok (alive=1 ...)`；
   kill 应用 -> `ng: session app reap ok (pid=.. why=stale)`。
8. 焦点策略：新窗口 REGISTER 后 sessiond 发 FOCUS；
   `ng: session focus ok (id=2 pid=18)`。
9. 第三张 QMP 截图：`panel(4,4)=#202838 demo(217,177)=#3070c0`。
10. host 单测 5025 checks / 0 failures（+341）。
11. 门禁：run-ng 159+75（NoQmp 143+70 / Fallback 122+36 / Smoke 144+71 /
    qemu64 159+75）；parity 100/100（host 95/95）；ci-ng 7/7；旧 32 位 305/305。

## 未做 / 遗留（按 WP 边界）

- **没有 sessiond 崩溃后的接管**：sessiond 死掉，应用/displayd 变 init 孤儿
  继续跑，但注册表/重启策略没了（architecture 开放决策 2）。
- **cap 未实现**：role_token 是 32 位不透明值 + 角色位；知道 ticket 的进程
  仍能给 displayd 发 DESTROY/（带 WM 令牌的）FOCUS；`DESTROY` 没有归属校验，
  cap grant/revoke 属 ABI 1.0。
- **包启动未接入会话**：WM 的 `/System/Packages` 仍只列不启动；SDK 的
  `session_connect()`/包启动属 WP3-4。
- **应用不自动重启**：崩溃只回收（`reaps++`），`restart_limit` 只用于 WM。
- **EXIT 的 ring3 路径只被 host 单测覆盖**：常驻 Demo 被 kill 时走心跳回收。
- **状态汇总在 displayd**：displayd 死了 sessiond 的状态写不出去（S2）。
- **无输入恢复**：kill WM 后 Tab 热键由新 WM 重新注册；输入队列语义属 S1。
- **WM 直连兼容路径**：没有会话通道时（wp32 自检）启动器仍本地 `spawn`。
- **像素断言依赖 1280×800 stdvga + QMP screendump PPM**：其它卡/VGA 文本走
  `-Fallback` 跳过；真机在 D1 锁定硬件后再补。
- **单线程轮询**：sessiond 20ms 轮询、displayd 单线程；没有并发客户端压力测试。

## 出口

```text
powershell -File tools/build-ng.ps1              # PASS，2082264 bytes
powershell -File tools/build-ng.ps1 -Smoke       # PASS（跳过 shell）
powershell -File tools/run-ng.ps1                # PASS，159 行 + 75 正则 + 3 次截图像素
powershell -File tools/run-ng.ps1 -NoQmp         # PASS，143 行 + 70 正则（via=auto）
powershell -File tools/run-ng.ps1 -Fallback      # PASS，122 行 + 36 正则（WP3-3 skipped）
powershell -File tools/run-ng.ps1 -Cpu qemu64    # PASS，159 行 + 75 正则
powershell -File tools/unit-ng.ps1               # PASS，5025 checks, 0 failures
powershell -File tools/parity-ng.ps1             # PASS，100/100（host 95/95）
powershell -File tools/ci-ng.ps1                 # 7/7 PASS
powershell -File build.ps1 -Test                 # PASS，305/305（旧 32 位）
```
