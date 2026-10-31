# ng session manager 设计（WP3-3，L2c）

> 状态：已实现（2026-10-31，`docs/claims/ng-WP3-3.md`）。
> 范围：L2c —— 应用生命周期、窗口注册表、角色令牌；display 协议升到 **v3**
> （CREATE/LIST/FOCUS 带 token）；L2b WM 的启动器改成向 sessiond 发 LAUNCH。
> 不在这里：S1 故障注入矩阵（WP3-5）、display server
> 重连（S2）、surface 的 cap/撤销。
> **WP3-4 补充（2026-10-31）**：公开 SDK 的会话客户端（`sdk/include/aster/
> session.h`）现在镜像本文第 4 节的协议；本文第 11 节记录 SDK 侧的边界。
> 上位约束：`docs/design/architecture.md` 第 3 节失败矩阵（L2c 是 session
> 级、L3 保活）、第 4 节规则 3（**shell 不拥有应用；应用生命周期归 session
> manager**）、第 5 节不变量 2/3/7（不共享地址空间、L3 默认无根/设备/网络、
> 控制面走消息 + 数据面走共享内存）。

## 1. 目标与边界

| 项 | 本 WP 做到 | 明确不做 |
|---|---|---|
| 会话进程 | ring3 `/bin/sessiond`；init 只 spawn 它，由它 spawn displayd + wm | sessiond 自身崩溃后的应用接管（architecture 开放决策 2） |
| 应用生命周期 | 只有 WM 能 `LAUNCH`；sessiond `spawn` + 记录 pid/token/窗口；`EXIT`/`KILL`/心跳超时回收 | 应用重启策略（optioanl 应用重启属后续） |
| 窗口注册表 | `{surface_id, app_pid, title, token}`；`LOOKUP`/`LIST`；每 200ms 与 displayd `LIST` 同步 | display server 死后的重建（S2） |
| 角色令牌 | 32 位 role_token（role/seq/nonce），经 argv 交给进程；CREATE/LIST/FOCUS 三处校验 | cap grant/revoke、真正的不可伪造能力（ABI 1.0） |
| WM 崩溃 | 心跳超时 -> 回收窗口/进程 -> 指数退避（TSC tick）重启 WM（limit=3） | 输入恢复/焦点保持（S1/WP3-5） |
| 焦点策略 | 新窗口 REGISTER 后由 sessiond 发 FOCUS（令牌 = 窗口自己的）；WM 可切任何窗口 | 点击焦点、多显示器、装饰 |

## 2. 启动与端口发现

```text
kmain
 ├─ proc_init()                 种 /bin/{shell,proc,hello,displayd,demo,wm,sessiond}
 ├─ wp31/wp32 自检              旧路径：内核自己 spawn displayd/wm（回归保留）
 └─ wp33_selftest()
      └─ spawn /bin/sessiond
           ├─ port_create()                         -> 会话主端口 ticket（1048576 起）
           ├─ unlink /run/display.port              （丢掉上一轮的死 ticket）
           ├─ spawn /bin/displayd
           │    └─ 写 /run/display.port（新 ticket）
           ├─ 读端口文件 + **LIST/LIST_REPLY 握手**  确认对端活着（失败重试，最多 3s）
           ├─ port_create()                         -> display 客户端端口
           ├─ spawn /bin/wm session=<ticket> token=0x<hex>
           └─ 写状态：SESSION_STATUS -> displayd -> /run/display.status
```

**端口发现的两个坑（本 WP 修掉的）**：

1. `/run/display.port` 是**上一轮 displayd 留下的陈旧文件**：wp31/wp32 的
   displayd 被杀后文件还在，直接读会拿到死进程的 ticket（send 返回 EBADF）。
   sessiond 先 `unlink` 再等新文件出现。
2. 端口文件出现 ≠ displayd 已经能服务：sessiond 用一次 `LIST` → `LIST_REPLY`
   握手确认（20 次 × 20ms），失败就重新读/重试。wp31/wp32 只打印票据、不
   发 IPC，所以历史上没有暴露这两个坑；L2c 真的要给 displayd 发消息。

**argv 通道（不新建 ramfs 发现文件）**：

```text
/bin/wm     session=<sessiond ticket>  token=0x<WM role_token>
/bin/<app>  [window] session=<ticket>  token=0x<app role_token>
```

`session_arg_parse_u32()` 支持十进制与 `0x` 十六进制；未知/多余参数忽略。
`/run/display.port` 仍是 **display** 的发现入口（客户端读它连 displayd）。
不写 `/run/session.port`：ramfs 节点池只有 32 个，`/bin/sessiond` 已经占一个；
被 sessiond 启动的进程都能从 argv 拿到会话通道。

## 3. 会话表与角色令牌

```text
struct session_entry   每个会话项：pid / token / role / state / surface_id /
                       port / title / heartbeats / last_seen / exit_code
struct session_table   8 槽 + next_seq/next_nonce + retired[4]（作废令牌环）
                       + 计数：launches/kills/exits/reaps/rejects/restarts/
                       token_checks/token_fail/registry_syncs/registry_ok
```

规则（`src/desktop/src/session_table.c`，host 单测 150+ 项）：

- 表 8 槽（WM + 7 个应用）；pid 单调（内核分配，不复用）；令牌由 sessiond 签发。
- **role_token 布局**：`bit0..7 seq | bit8..15 role | bit16..31 nonce`；
  `role ∈ {WM=1, APP=2, SERVICE=3, SESSION=4}`；0 永远不是合法令牌
  （make 时归一化：role=0→APP、seq=0→1、nonce=0→1）。
- 令牌**不出现在公开状态里**（状态文件只有数量），只在 argv 与被启动进程之间
  传递；`sessiond` 用「令牌 → 会话项」反查 pid，不信任消息里自报的 pid。
- **作废令牌环**：进程退出/重启后旧窗口的 `LIST` 快照可能还在 displayd 的
  回复队列里；最近 4 个作废令牌看到时不算「外来令牌」，避免瞬时 mismatch。

## 4. 会话协议（控制面）

消息 = 20 字节头（magic `SN1M`/version/type/len/status/seq）+ 固定 body，
编解码纯函数（`src/desktop/src/session_protocol.c`，`SESSION_MSG_MAX=512`）。

| 消息 | 方向 | body | 语义 |
|---|---|---|---|
| `REGISTER` | client→sessiond | token,port,surface_id,title[4]（28 B） | 上报自己的窗口（WM 报面板、应用报 Demo） |
| `HEARTBEAT` | client→sessiond | token,rounds（8 B） | 存活心跳（每 ~200ms） |
| `LAUNCH` | **wm→sessiond** | token,path[16],arg[4]（84 B） | 启动应用（只有 WM 角色可以） |
| `EXIT` | client→sessiond | token,code（8 B） | 正常退出 |
| `KILL` | wm→sessiond | token,pid,surface_id（12 B） | 会话内终止应用（WM 角色） |
| `LOOKUP` | client→sessiond | surface_id,pid（8 B） | 注册表查询 |
| `ENTRY` | sessiond→client | found,entry（48 B） | LOOKUP 的回复 |
| `LIST` | client→sessiond | reserved（4 B） | 注册表快照（WM 启动时查一次） |
| `LIST_REPLY` | sessiond→client | count,procs,apps,windows,entries[8]（336 B） | 每条 40 B：pid/token/surface/state/role/title |
| `SESSION_STATUS` | client→sessiond | reserved（4 B） | 保留 |

权限（纯函数 `session_launch_allowed()` / `session_kill_allowed()`）：
令牌必须存在、角色 = WM、状态 RUNNING；APP 发 `LAUNCH` 或伪造令牌都会
`EPERM/ENOENT` 并计入 `rejects`（运行期证据见第 8 节）。

## 5. 生命周期

```text
WM 启动器 '1'..'9' ──LAUNCH(token,path)──> sessiond
   sessiond: session_launch_allowed() -> spawn(path, [window], session=, token=)
             session_alloc_with_token(pid, APP, token)  令牌先签发、后按 pid 登记
             ng: session app launch ok (pid=.. app=.. token=0x..)
应用启动后
   CREATE(..., token)  -> displayd 记住 surface.token（v3）
   REGISTER(token,port,surface_id,title) -> sessiond 绑定窗口
   sessiond -> displayd FOCUS(surface, token)（新窗口获得焦点）
   HEARTBEAT 每 ~200ms
正常退出 EXIT(token,code) / WM KILL(token,pid)
   -> 回收：DESTROY 窗口 + kill(9)/wait + session_retire_token
崩溃（心跳超过 800ms 没有更新）
   -> 同样的回收路径（why=stale），ng: session app reap ok (pid=..)
```

- 注册表与 displayd 的 `LIST` 每 200ms 同步一次：每个 running 且有窗口的
  会话项，必须在 LIST 里找到**同 id / 同 owner_pid / 同 token** 的记录；
  不一致计 `token_fail`，全部一致打印
  `ng: session registry ok (apps=.. windows=.. procs=..)` 与
  `ng: role token ok (tokens=.. bound=.. mismatch=0)`。
- 「窗口不在 LIST 里」**不清引用**：REGISTER 之后收到的第一份 LIST 可能是
  窗口 CREATE 之前拍的快照（displayd 与 sessiond 是两个进程）。窗口回收只由
  EXIT/KILL/心跳路径负责。

## 6. display 协议 v3（只追加，v1/v2 兼容）

| 消息 | 变更 | 语义 |
|---|---|---|
| `CREATE` | body 40 → **44**：追加 `token` | v1 16 B、v2 40 B 仍可解码（token=0） |
| `LIST_REPLY` | entry 44 → **48**：追加 `token` | v2 客户端仍按 360 B 解码 |
| `FOCUS` | 第二个 u32 从 reserved 变成 `token` | 令牌匹配 surface 自己的、或令牌角色是 WM（displayd 从 WM 面板的 CREATE 学到 `wm_token`）才允许；否则 `ng: focus reject ok` |
| `SESSION_STATUS` | **新消息**（68 B） | sessiond 把会话计数交给 displayd，汇总进 `/run/display.status` 的 `s*` 字段 |

`ds_protocol.h` 的版本升到 3：编码器始终写 v3，解码器接受 1..3 并按版本选
body 长度（多一个字节也拒绝）。

## 7. WM 重启（restart_limit=3，TSC tick 退避）

- sessiond 记录 WM 的 `last_seen`（REGISTER/HEARTBEAT）。超过
  `S_STALE_TICKS = 800ms`（约 40 个 20ms 轮次）没有心跳 -> 判定 WM 死亡。
- 死亡回收：`DESTROY` 旧面板（displayd 里旧 surface 消失，`LIST` 从 3 条
  回到 2 条）-> `kill(9)` + `wait` -> 作废 WM 令牌 -> 释放槽位。
- 退避：`session_backoff_ticks(n) = 20ms << (n-1)`，封顶 160ms
  （20/40/80/160ms），`restart_limit=3`；`session_should_restart()` 是纯函数，
  host 单测逐档校验。等待期间 `wmstate=2`，到期后 spawn 新 WM（新令牌）。
- 重启完成后新 WM 重新 CREATE 面板 → REGISTER → sessiond 打印
  `ng: session wm restart ready ok (wm=.. id=..)`；run-ng 以此触发第三张
  QMP 截图（面板回来 + Demo 窗口仍在）。
- 应用不归 WM：WM 死的时候 Demo 进程、它的 surface、它的注册表项都由
  sessiond 保活；证据是 `ng: session survives wm ok (app=.. alive=1 ...)`。

## 8. 状态与证据

**状态文件**：不新建 ramfs 节点（节点池 32 个，`/bin/sessiond` 又占一个）。
sessiond 发 `SESSION_STATUS`（display 协议 v3），displayd 汇总进已有的
`/run/display.status`：

```text
... px=0x202838 sess=1 spid=15 sdpid=16 sprocs=2 sapps=1 swindows=2 stokens=2
    slaunches=1 skills=0 sexits=0 sreaps=0 srestarts=1 swmpid=19 swmstate=1
    sapppid=18 stokenfail=0 sregistry=1
```

`session_status_format()`（`session_table.c`）是这些字段的规范实现（host
单测逐字段校验，cap 截断/NULL 都有用例）；ring3 侧由 sessiond 把同样的字段
装进 `SESSION_STATUS` 消息。

**ring3 证据行**：

```text
<sessiond> ng: sessiond up (pid=15 displayd=16 wm=17 port=1048576)
<displayd> ng: surface token ok (id=1 token=0x21950102 role=1)
<wm>       ng: wm session ok (session=1048576 token=0x21950102)
<sessiond> ng: session wm register ok (wm=17 id=1)
<sessiond> ng: session focus ok (id=1 pid=17)
<wm>       ng: wm launch req ok (app=/bin/demo via=key)
<sessiond> ng: session app launch ok (pid=18 app=/bin/demo token=0x553b0203)
<demo>     ng: demo session ok (session=1048576 token=0x553b0203)
<sessiond> ng: session window ok (pid=18 id=2 title=Demo)
<sessiond> ng: session focus ok (id=2 pid=18)
<sessiond> ng: session launch reject ok (token=0x553b0203 role=2 n=1)
<displayd> ng: focus reject ok (id=1 token=0x553b0203 n=1)
<sessiond> ng: session registry ok (apps=1 windows=2 procs=2)
<sessiond> ng: role token ok (tokens=2 bound=2 mismatch=0)
<sessiond> ng: session wm dead ok (pid=17)
<sessiond> ng: session wm restart sched ok (old=17 restarts=0 backoff_ticks=20000000)
<sessiond> ng: session wm restart ok (old=0 new=19 restarts=1)
<sessiond> ng: session wm restart ready ok (wm=19 id=3)
<sessiond> ng: session app reap ok (pid=18 why=stale)
```

**内核自检（wp33）**：`ng: sessiond spawn ok`、`ng: session start ok`、
`ng: session wm ready ok`、`ng: session launch seen ok`、
`ng: session registry seen ok`、`ng: role token seen ok`、
`ng: session wm killed ok`、`ng: session wm restart seen ok`、
`ng: session survives wm ok`、`ng: session app reap ok`、
`ng: session cleanup ok (live=0 nodes=30)`、`ng: WP3-3 selftest PASS`。
清理时归还 `/run/display.port` 与 `/run/display.status` 两个 ramfs 节点
（给后面的 ring3 shell 会话留 `/tmp/s.txt` 的空间）。

## 9. 内核镜像布局（为什么要动 link.ld）

7 个用户 ELF（shell/proc/hello/displayd/demo/wm/sessiond）合计 136 KB。
WP3-3 新增 `sessiond` 24 KB 后，`.incbin` 放在 `.rodata` 会让
`__kernel_end` 越过第一个 2 MiB 窗口（PD[0] 是唯一的 4 KiB 页窗口）。

处理：用户 ELF blob 从 `.rodata` 挪到 **2 MiB 之后**的 `.bigdata` 只读段
（PD[1] 的 2 MiB 恒等大页 RW+NX），紧挨着 WP2-2 的 FAT16 镜像；
`mem_boot_init()` 把 `[__bigdata_start, __bigdata_end)` 一并 `pmm_reserve`。
结果：`__kernel_end = 0x1E3000`（< 0x200000，余量 ~118 KB），
`.bigdata` = `0x368000..0x389F20`（< 0x400000）。布局细节见 `link.ld` 注释。

## 10. 限制 / 遗留（按 WP 边界）

1. **sessiond 自己崩溃没有接管**：应用/displayd 会变成 init 的孤儿继续跑，
   但没有新 sessiond 接管注册表（architecture 开放决策 2）。
2. **cap 未实现**：令牌是 32 位不透明值 + role 位；能拿到 ticket 的进程仍能
   给 displayd 发 DESTROY/FOCUS（DESTROY 没有归属校验，FOCUS 有令牌校验）。
3. **会话发现只走 argv**：手动从 shell 启动的 GUI 应用拿不到会话通道；
   包启动（WP3-4）需要 SDK 侧的 `session_connect()`。
4. **状态汇总在 displayd 里**：displayd 死了以后 sessiond 的状态写不出去
   （displayd 重连是 S2）。
5. **没有应用重启策略**：应用崩溃只回收（`reaps++`），不自动重启；
   `restart_limit` 只用于 WM。
6. **EXIT 只在干净退出路径**：常驻 GUI 应用被 kill 时走心跳回收；
   `SESSION_MSG_EXIT` 的 ring3 路径由 host 单测覆盖。
7. **单线程协作**：sessiond 用 20ms `PORT_RECV` 超时轮询；没有并发压力测试。
8. **WM 直连兼容路径**：没有会话通道时（WP3-2 的内核自检）启动器仍会本地
   `spawn`；真实启动（init→sessiond→wm）永远走 LAUNCH。

## 11. SDK 会话通道（WP3-4 补充）

公开 SDK 在 `sdk/include/aster/session.h` 里逐字节镜像第 4 节的会话协议，
并把“应用怎么拿到会话”收口成一个入口：

```text
aster_session_connect(&sess, argc, argv)
  -> 解析 argv 里的 session=<dec> / token=<0xhex|dec>
  -> 没有 session= 返回 -ENOENT（shell 直连 spawn 的正常路径）
  -> 有：为会话单独 port_create() 一个回复端口（与 display 客户端端口分开，
     避免 INPUT 与 ENTRY/LIST_REPLY 在同一个端口上互相干扰）
```

之后应用依次用 `aster_session_register()` / `aster_session_heartbeat()` /
`aster_session_exit()`；WM 侧用 `aster_session_launch()` /
`aster_session_kill()`；查询注册表用 `aster_session_lookup()` /
`aster_session_list()`。令牌由 sessiond 签发、经 argv 交给应用，SDK 只做
“原样使用 + 校验角色位”，**不提供伪造 WM 令牌的捷径**（`token_make` 主要
给模拟器/host 测试）。

对应地，display v3 的 `CREATE`/`FOCUS` 在 SDK 里增加 `token` 字段
（`aster_app_config.token`、`aster_app_focus()`），`LIST_REPLY` 解码带
token；没有会话时 token=0，displayd 回落到 v1/v2 的无校验语义。

证据路径：`tools/sdk-demo.ps1` 用 `build-ng.ps1 -SdkDemo` 把
`sdk/examples/hello_gui` 编成 `/bin/demo` 的 blob（符号 `ng_demo_elf`
不变），于是 **init → sessiond → WM 的 LAUNCH → sessiond spawn** 这条真实
产品链会拉起 SDK 应用；脚本断言
`ng: sdk session ok` / `ng: sdk token ok` /
`ng: session window ok (... title=Aster SDK)` /
`ng: wm task ok (... title=Aster SDK)` 与 QMP 截图像素。细节与已知限制见
`docs/claims/ng-WP3-4.md`。

这条 SDK 通道**不改变** sessiond 的所有权模型：sessiond 仍是应用生命周期
的唯一入口；SDK 只是客户端。sessiond 自己崩溃后的接管、cap/撤销、displayd
重连仍是本文第 10 节的遗留。

## 12. S1 故障注入（WP3-5）

> 状态：已实现（2026-10-31，`docs/claims/ng-WP3-5.md`）。
> 工具：`tools/s1-fault.ps1`；客户机代理/纯逻辑：`tests/s1/**`；
> host 单测：`tests/unit-ng/test_s1.c`。
> 上位约束：`docs/design/architecture.md` §3 故障等级（S1 的 L3 存活 SLA）。

### 12.1 断言口径

S1：**shell/WM 崩溃或卡死时，L3 进程、文档状态、输入不丢**；允许视觉中断、
焦点/IME 重置、重绘。本 WP 只测 WM（L2b）与输入路径；displayd 崩溃存活是
S2、GPU 恢复是 S3，都不在 S1 范围内。

D0 还没有编辑器类应用，所以"文档状态"用**进程/窗口/注册表不丢**作为可观测
代理：Demo 进程 pid、surface 窗口、sessiond 注册表项在 WM 双重重启后仍在。
真正的文档内容属于 E4/D1 的编辑器接入。

### 12.2 注入通道（为什么代理走磁盘）

S1 需要"外部杀进程"，但产品里没有给 host 的调试 syscall，而本 WP 不允许改
`build-ng.ps1`/内核镜像里的用户 ELF。做法：

```text
host: clang 编译 tests/s1/s1fault.c + s1_logic.c -> s1fault.elf
      tools/mkfat-ng.ps1 生成 8MB FAT16 副本
      tests/s1/fat-add.ps1 把 s1fault.elf 写成根目录 S1FAULT.ELF
guest: ring3 shell  spawn /mnt/disk/S1FAULT.ELF <a|b|c|d> ; wait <pid>
       代理 spawn /bin/sessiond（真实启动路径：sessiond -> displayd + wm）
       代理按步骤读 /run/display.status，kill WM / 焦点应用 / sessiond
host: QMP input-send-event 注入 1 / f12；screendump + PPM 像素断言
```

两个关键点：

1. **子进程必须是 VFS 可读的 ELF**：`proc_spawn` 支持任意 VFS 路径，
   `/mnt/disk` 是 WP2-4 已挂好的 FAT16，正好当测试载荷通道；不改内核、
   不改产品 ramfs 种子。
2. **代理不能提前退出**：父 shell 的 `wait` 一旦返回，shell 会重新
   `readkey` 忙等，和 displayd 抢同一个内核按键队列。所以代理在结束、
   失败、以及 d 场景 arm 之后都**阻塞在 port_recv 超时轮询上不退出**，
   QEMU 由 host 结束。

### 12.3 场景与断言

每个场景独立起一个 QEMU（复用 run-ng 的 `-kernel`/`-drive`/`-vga std`/
`-m 256`/`-cpu max` 参数，独立随机 QMP 端口，`-display none` + serial file）。

| 场景 | 注入 | 断言 | 结果 |
|---|---|---|---|
| a 杀 WM | `kill(wm, 9)` | Demo pid/窗口/注册表存活；sessiond `srestarts+1` 且 `swmpid` 换新；面板重新 CREATE+COMMIT（像素）；键盘 `1` 走通新 WM 的 LAUNCH（host 断言 `via=key`） | PASS |
| b 杀输入应用 | `kill(sapppid, 9)`（焦点 Demo） | sessiond 心跳回收（`sreaps+1`）、WM 不重启；键盘 `1` 仍启动新应用（`slaunches+1`、`sapppid` 变） | PASS |
| c 杀 sessiond | `kill(spid, 9)` | session 域崩溃：无接管/无重启（`srestarts` 不变、状态停在最后一份）；WM/Demo/输入/面板仍在（host 断言 `via=key` 的 LAUNCH 请求仍发出） | PASS（记录行为，不要求恢复） |
| d WM 死循环 | WM `fault=spin` 收到 F12 后自旋 | 协作式调度无抢占：serial 冻结、sessiond 无法检测/重启、整机停住；冻结最后一帧仍有面板+窗口 | KNOWN（S1 SLA 不满足，已知限制） |

场景 a 的一个时序坑（实现时踩到）：新 WM 的 `wm launcher ok` 行可能比
agent 的 `s1 wm restart ok` 证据行更早（两个进程），用它做 `-After` 条件会
一直等不到、直到**下一个** WM 周期（~8s），期间新 WM 的自检 fallback 已经
auto-launch 一次。现在改用 sessiond 的 `session wm restart ready ok` 计数
（新 WM REGISTER 之后）确认热键就绪，并连按两次 `1`：新 WM 的 fallback 最多
auto-launch 一次，所以 agent 的"launches 至少 +2"只有在至少一次按键真的走通
时才成立；host 另外单独断言 `via=key` 行。

### 12.4 故障钩子（只影响 S1 调用）

为了能真的让 WM"卡死"，加了两处**只在 S1 场景 d 的启动参数下生效**的钩子：

```text
sessiond argv 含 fault=spin   ->  spawn /bin/wm 时追加 argv "fault=spin"
wm argv 含 fault=spin         ->  收到 F12（PS/2 scancode 0x58）时打印
                                   ng: wm fault spin (scancode=0x58) 后死循环
```

正式启动路径（init → sessiond → wm）没有这个参数，行为与 WP3-3 完全一致；
`run-ng`/`ci-ng` 不受影响（本 WP 的 run-ng 仍是 159/159 + 75 正则）。

### 12.5 证据与像素

串口证据（每个场景的 `ng: s1 ...` 行由代理/纯逻辑打印）：

```text
ng: s1 agent up (pid=.. mode=a)
ng: s1 sessiond spawn ok (pid=22)
ng: s1 session up (sessiond=22 displayd=23 wm=24 app=25 launches=1 windows=2)
ng: s1 wm killed ok (pid=24)
ng: s1 wm restart ok (old=24 new=26 restarts=1)
ng: s1 demo alive ok (app=25 apps=1 windows=2 reaps=0)
ng: s1 input req (scenario=a)              <- host 从这里开始注入
ng: s1 input ok (launches=3 app=28)
ng: s1 scenario a PASS (step=4)
```

像素断言（QMP `screendump` 的 PPM，1280x800）：

- `(4,4) == #202838`：WM 面板背景（尺寸 1280x32，固定贴在左上角）；
- `(217,177) == #3070c0`：Demo 窗口的实心矩形。Demo 窗口 400x300 由 displayd
  级联放在 `(160 + 32*count, 120 + 32*count)`，新会话里第一个应用窗口是
  `(192,152)`，矩形在其内部 `(20,20)-(160,90)`，探针 `(192+25, 152+25)`。
- 场景 d 在注入 F12 **之前**先抓一帧（面板+窗口），冻结后再抓一次；两次都能
  读到同一帧说明"整机停住但最后一帧还在"。QMP 在自旋时偶尔会被 QEMU main
  loop 饿死（screendump 3s 无响应），工具把这种情况也当 KNOWN 证据记录。

### 12.6 结果（2026-10-31）

```text
s1-fault: scenario a PASS  (12.9s) WM restarted with new pid; Demo survived; panel back; 2x key -> via=key LAUNCH ok
s1-fault: scenario b PASS  (12.3s) focused app killed/reaped; WM stayed up; key launched a new app (input recovery)
s1-fault: scenario c PASS  (12.5s) sessiond died (no takeover, D0 known limitation); WM/Demo/input/panel survived
s1-fault: scenario d KNOWN (13.4s) WM spin froze the whole VM ... frozen frame still has panel + window - S1 SLA not met
s1-fault: 3 pass, 1 known, 0 fail in 51.1s
```

host 单测 5025（WP3-3）→ 5034（输入边界加固）→ **5183 checks**（本 WP +149）；
host 单测直接跑 `tests/s1/s1_logic.c`，和客户机断言是同一份源码。

### 12.7 已知限制

1. **WM 死循环 = 整机冻结**：调度器是协作式（`thread.h`：只在
   `sched_yield/thread_sleep/thread_exit` 切换），WM 在 ring3 自旋后
   sessiond/displayd 都拿不到 CPU，也没有看门狗/抢占。d 记录为 KNOWN，
   修复需要内核抢占或 watchdog（不在本 WP 范围）。
2. **sessiond 自己崩溃没有接管**：c 只记录行为（应用/displayd/WM 变孤儿继续
   跑、无注册表/重启策略），不要求恢复（第 10 节遗留、architecture 开放
   决策 2）。
3. **displayd 崩溃（S2）没测**：杀 displayd 的保活/重连属 S2。
4. **输入只测键盘**：PS/2 + QMP `input-send-event`；鼠标/IME 还没实现。
5. **"文档状态"用进程/窗口/注册表代替**：D0 还没有文档型 L3 应用；编辑器
   接入后需要补"内容不丢"的断言。
6. **像素断言依赖 stdvga 1280x800 + QMP screendump**：`-vga none` 没有 LFB，
   场景不适用（和 run-ng `-Fallback` 同一约束）。
7. **FAT16 代理是测试通道**：产品路径不加载它；正式外部开发者测试（E4）
   仍然用 SDK/包安装。
8. **d 场景的 QMP 行为不稳定**：WM 自旋时 screendump 可能被 QEMU main loop
   饥饿（3s 超时）；工具两种结果都接受，并把"抓不到"也记为冻结证据。
