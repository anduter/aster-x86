# ng display/session 协议审计（第二轮）

分支 `ng`，起点 `60ea988`（上一轮 partial 加固）。本轮只做 **显示/会话协议 + 状态机**，
不碰 `src/ng/**`（syscall 审计会话的范围）、`sdk/**`、`build.ps1`、`tools/ci*.ps1`。

## 交付物

| 路径 | 说明 |
|---|---|
| `tools/audit-ng-ui.ps1` | 宿主驱动：编译 ring3 探针（+`ds_protocol.c`/`session_protocol.c`）→ FAT16 注入 → 独立 QEMU（**随机空闲 QMP 端口**，显式避开 11995/11996/11997/4444/4481/4491/4542/4543/4631）→ QMP 注入 `spawn`/`wait` → 串口断言（含本轮 7 条修复回归断言） |
| `tests/audit-ng/ui/audit_ui.c` | in-guest 探针：自己 spawn `/bin/displayd`、`/bin/sessiond`，只经 IPC 协议攻击，不读串口、不改内核；本轮新增 `wm-claim` / `focus-token` / `focus-own` / `title-inject` / v2 `LIST_REPLY` 投毒 用例，shm 越界写改为 opt-in |
| `docs/claims/ng-audit-ui.md` | 本文件 |

驱动用法：

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-ng-ui.ps1
powershell ... -File tools\audit-ng-ui.ps1 -ShmOob       # 已知限制对照（ring3 越界写）
powershell ... -File tools\audit-ng-ui.ps1 -KeepVm -MinChecks 26
```

证据目录 `obj-ng/audit-ui/`（不进 git）：`driver-run*.log`、`serial-ui*.log`、
`serial-ui-oob.log`、`serial-ui-prefix-v2.log`、`unit-round2.log`、`runng-round2.log`。

## 基线（修复前，HEAD `60ea988` 的 dist-ng）

1. 探针在 **shm 越界写** 用例处把内核打进 PANIC，后面的 sessiond 用例根本没跑到；
2. 伪造 `WM_STATUS`/`SESSION_STATUS` 之后，串口反复出现
   `ng: displayd status FAIL (write)`（**33 行**），状态文件冻结；
3. 其余 display/session 用例全 PASS（上一轮的加固有效）。

## 发现 / 修复（每项：复现 → 根因 → 修复 → 回归）

### 1. 伪造 WM 角色令牌 → 抢占 WM 权威（已修）

**复现**（`a_attack_authz`）：探针造一个"形状合法"的 WM 角色令牌
`0x7A5A0109`（role=WM, seq=9, nonce=0x7A5A），用它 CREATE 一个 8x8 surface；
displayd 立刻把它记成 `g_wm_token`。此后探针用该令牌 DESTROY 掉别的客户端窗口
（串口 `ng: audit destroy wm-token ok (5)`）、REGISTER_HOTKEY 抢全局按键，全部成功。

**根因**：`d_handle_create()` 只用 `session_token_valid()` 校验令牌**位域形状**；
IPC 头里的 `from` 只是端口 ticket、内核不填 peer 身份（`src/ng/ipc_core.c` 只校验
ticket 存活），所以 displayd 无法区分 session manager 签发的 WM 令牌和客户端自造的。
**最后一个 CREATE 里的 WM 令牌永远获胜**。

**修复**：纯函数 `ds_wm_claim_ok(wm_token, token)`：没有 WM 时第一个 WM 令牌认领；
已有 WM 时只接受同一个令牌，别人的 WM 令牌 CREATE 在**分配 SHM 之前**以 `DS_EPERM`
拒绝并打证据 `ng: surface wm-claim reject ok (token=.. n=..)`（有打印闸门）。

**回归**：`unit-ng` 5 条断言；in-guest `display wm-claim ok`（第二令牌被拒 + 表里没有
新窗口）；宿主 `fix: forged WM token claim rejected`；`run-ng` 的 WM 面板创建/重启门禁。

### 2. WM 令牌永不回收（`d_handle_destroy` 死代码）（已修）

**复现**：销毁 WM 面板（令牌 `A_FAKE_WM_TOKEN`）后，再用**同一个旧令牌** FOCUS 一个
带令牌的窗口 —— 修复前焦点被改（`LIST` 里 `focused=1`），说明旧 WM 令牌还是权威；
串口永远没有 `ng: displayd wm token cleared ok`。

**根因**：`d_handle_destroy()` 在 `ds_table_destroy_checked()` **之后**才判断
`sf->token == g_wm_token`，而 `ds_table_destroy()` 内部已 `ds_table_zero()` 清槽位 ——
条件恒假。注释里写的"WM 面板没了：它的跨窗口权限也失效"从来没生效过。

**修复**：销毁**之前**保存 `sf_token`，成功后清 `g_wm_token` 并清全局热键通道
（`g_hotkey_client/surface`）；销毁热键注册者的窗口时只清热键通道。
证据：`ng: displayd wm token cleared ok (id=..)` / `ng: displayd hotkey cleared ok (id=..)`。
因为 WM 重启是"sessiond 先 DESTROY 旧面板 → 再 spawn 新 WM"，新 WM 仍能重新认领。

**回归**：in-guest `display focus-token`；宿主 `fix: WM token revoked with its panel`；
`run-ng` 产品路径实测（见"门禁"里 wp33 的 WM 重启段）。

### 3. FOCUS 的 `token=0` 越权（知道 id 就能抢焦点）（已修）

**复现**：用 APP 令牌建窗口 V；从**外来端口**发 `FOCUS{V, token=0}` → 修复前焦点被
改到 V，之后按键全部投递给 V（输入劫持）。

**根因**：`d_handle_focus()` 只有 `token != 0` 才校验，`token == 0` 直接
`ds_table_focus()`（v1/v2 兼容遗留）。会话模式下每个窗口都有令牌，等于
"任何客户端都能抢任何窗口的焦点"。

**修复**：纯函数 `ds_focus_token_ok(surface_token, wm_token, token)`：无令牌窗口保持
旧行为；有令牌的窗口必须出示自己的令牌或 WM 令牌，`token=0` 一律拒绝。
`src/apps/demo/demo.c` 的 FOCUS 改为携带自己的 `e_token`（直连模式为 0，行为不变）。

**回归**：`unit-ng` 7 条断言；in-guest `display focus-token`（token=0 与旧 WM 令牌都被
拒、锚点窗口 B 保持焦点）+ `display focus-own`（自己的令牌成功）；`run-ng` 依赖的
`ng: focus reject ok (...)`（demo 越权探测）照旧出现。

### 4. 客户端 title/path 原样进串口 → 伪造证据行（已修）

**复现**：`CREATE` 的 title 填 `"\r\nPANIC: vec=0xe"`（协议留 15 字节，截成
`"\r\nPANIC: vec=0x"`），串口里就多出一行**以 `PANIC:` 开头**的内容 —— 宿主门禁
`-not $t.Contains('PANIC:')` 会误判成内核 panic。sessiond 的
`ng: session window ok (... title=%s)` / `ng: session app launch ok (... app=%s)`
同理（path 最长 63 字节，足够伪造一整行）。

**根因**：`d_put(title)` / `s_put(title|path)` 把客户端控制的字节直接写进串口，
而串口是所有门禁的唯一证据源。

**修复**：纯函数 `ds_str_sanitize()`（只留 `0x20..0x7E`，其余 → `.`，始终 NUL 结尾）；
displayd 的 title、sessiond 的 title/path 打印前净化。二进制字段（`LIST_REPLY` 的
title）不变，客户端渲染不受影响。

**回归**：`unit-ng` 22 条断言；宿主 `fix: title cannot forge an evidence line`
（`(?m)^PANIC:` 零行）+ `fix: injected title was sanitized`（`title=..PANIC: vec=0x`）。

### 5. 状态行 > 512 字节 → 之后每次状态写都失败，`/run/display.status` 冻结（已修）

**复现**：伪造全 `0xFFFFFFFF` 的 `WM_STATUS` + 全 `0xFFFFFFFF` 的 `SESSION_STATUS`，
再随便 COMMIT 一次；串口从那一刻起每行都是 `ng: displayd status FAIL (write)`
（基线 33 行），状态文件停在旧内容上。

**根因**：`SYS_WRITE_FD` 走内核 `fs_sys_write()`，bounce buffer 上限
`FS_SYS_IO_MAX = 512`（`src/ng/fs_syscall.c`：`len > 512` 直接 `EINVAL`）。
状态行最坏 = 基本字段 + WM 块 + 会话块（每字段 10 位十进制）+ `px=0x` + 8 位 hex
≈ 520 字节。上一轮把栈缓冲 512 → 1024（消掉栈溢出），但**没考虑 syscall 的长度
上限**：伪造的大数字让整行超过 512，于是写永远失败。字段顺序上 `px` 在会话块之前，
COMMIT 把 `px=0x0` 变成 `px=0x101820`（+10 字节）正好跨过 512。

**修复**：整行钳到 `D_STATUS_MAX_LINE = 500`（< 512 且给 `'\n'` 留位）；字段顺序保证
显示面最先写、会话面（`s*`，内核 wp33 读）最后写；第一次真正截断时打
`ng: displayd status trunc ok (cap=500)`。

**回归**：宿主 `fix: status write never fails (line <= 512)`（串口零
`status FAIL (write)`）；in-guest `display status-poison`；`run-ng` 全程零失败。

### 6. 拒绝/重复路径的无限证据输出 + 每条消息一次 ramfs 写（已修）

**复现**：`a_attack_authz` 连发 40 条**完全相同**的 `REGISTER_HOTKEY`；基线串口 40 行
`ng: hotkey registered ok`，每次还 `d_status_write()`（open/write/close
`/run/display.status`）。`DESTROY`/`FOCUS` 拒绝路径同样无上限 —— 一条 16 深度的端口
队列就能稳定刷串口 + ramfs。

**根因**：这些路径全部由客户端消息触发，却没有打印闸门，且**无条件**重写状态文件
（被拒请求根本不改变状态）。

**修复**：每类证据行最多 `D_MAX_PRINTS = 8` 行（计数器继续累计）；被拒请求不再重写
状态文件；热键注册只有参数真的变化时才重写。

**回归**：in-guest `hotkey flood`（40 条，带背压重试）；`run-ng` 仍能看到
`ng: hotkey registered ok`（前 8 行内）。

### 7. REGISTER_HOTKEY 的 ack 会把客户端端口队列塞满（已修）

**复现**：就是第 6 条那个洪泛用例：探针发 40 条注册后马上 LIST，`LIST_REPLY` 收不到。
修复前 displayd 每受理一次就 `d_reply_status(from)` 回一条，客户端端口只有 16 深，
16 条垃圾 ack 占满队列后 displayd 的 `LIST_REPLY` 被非阻塞 `port_reply` 直接丢掉。

**根因**：协议**没有** REGISTER_HOTKEY 的回复约定（`ds_protocol.h` 只定义"注册全局
热键"；`wm.c` 发了就走、从不读回复），ack 是纯队列污染。更糟：IPC 的 `from` 是发送方
自己填的（见"未修项 2"），攻击者可以用 `from=<受害者 ticket>` 把**别人的**队列塞满，
让受害者的 `CREATE_REPLY`/`LIST_REPLY` 全部静默丢失。

**修复**：去掉 ack（`d_handle_register_hotkey` 不再 `d_reply_status`）。
`run-ng` 的 WM 路径不受影响（WM 从不读这个回复）。

**回归**：in-guest `hotkey flood` 现在 PASS（修复前 FAIL）。

### 8. 同一条会话项重复 REGISTER 不幂等（已修）

**复现**（host 单测）：`session_register_window(t, token, port, 同一个 surface, ...)`
第二次返回 `DS_EBUSY` 且 `t->rejects++`。客户端丢了回复后重试会**永远**注册不上。

**根因**：`session_by_surface(surface_id) != NULL` 不区分"自己的窗口"和"别人的窗口"。

**修复**：`owner == e` 时幂等更新（port/surface/title/last_seen）返回 `DS_OK`；别人的
窗口仍然 `DS_EBUSY`。

**回归**：`unit-ng` 6 条断言（rejects 不增、字段更新、`session_windows` 不变）。

### 9. sessiond 用 v3 布局解析 v2 `LIST_REPLY`（已修，有 in-guest 复现）

**复现**：探针手工拼一条 **version=2**、body 360 字节（entry 44 字节）的 `LIST_REPLY`
（count=1，entry 区全 `0xAB`）广播给所有候选 DS 端口。把 `sessiond.c` 的
`m.version >= 3u` 临时改回 `if (1)` 重编后，串口出现：

```
ng: session registry mismatch (n=1 tokenfail=1)
```

（证据：`obj-ng/audit-ui/serial-ui-prefix-v2.log:759`，驱动
`obj-ng/audit-ui/driver-prefix-v2.log` 在 `fix: v2 LIST_REPLY ignored` 上 FAIL。）

**根因**：`ds_msg_decode()` 接受 v1/v2/v3，而 `s_on_list()` 按 v3 的
`struct ds_list_reply_body` 读 entries（v3 entry 48 字节、v2 entry 44 字节）——
一条伪造的 v2 消息让 `entries[1..]` 全部错位，按 v3 读到的 `token` 其实是 v2 的
title 区，于是 `session_by_token()` 查不到、`token_fail++`，把内核 wp33 读的
`tokenfail`/`registry` 证据搅乱。

**修复**：`s_display_ping()` / `s_poll_display()` 只接受 `m.version >= 3` 的
`LIST_REPLY`（合法对端只有本仓库的 v3 displayd）。

**回归**：宿主 `fix: v2 LIST_REPLY ignored (no registry mismatch)`；恢复修复后
串口零 mismatch（`driver-run6.log` PASS）。

## 未修 / 已知限制（claims）

1. **ring3 越界写 → 内核 PANIC**。`spawn /mnt/disk/audit.elf oob`（脚本 `-ShmOob`）让
   子进程写共享内存映射末尾一页：
   `#pf cr2=0x50004000 error=0x6` → `PANIC: vec=0xe err=0x6 rip=0x...`。
   根因在 ring3 异常隔离（`src/ng/trap.c` / `isr.S`），**本会话禁改**；与上一轮
   `docs/claims/ng-audit-wip.md` 一致。证据 `obj-ng/audit-ui/serial-ui-oob.log`，
   驱动 `-ShmOob` 现在把它当"已知限制对照"记录（PASS + INFO 行）。
2. **IPC `from` 可伪造**。内核只校验 `from` 是"某个存活端口"，不校验是不是发送者自己
   的端口。所以 sessiond 的"端口绑定"（`s_channel_ok`）与 displayd 的 DESTROY
   `token=0` 归属校验（`from == client_ticket`）都只是**最低限度加固**；偷到令牌 +
   枚举到端口 ticket 的攻击者仍能冒充。第 7 条的队列投毒也是这个根因的利用面。
   真正修复要在 syscall 边界填 peer 身份（`src/ng/ipc_core.c`，不在本会话范围）。
3. **SHM 对象池只有 8 个，且没有 free syscall**。`src/ng/shm.h` 自己写了"DESTROY 后
   displayd 的映射要等进程退出才释放"；实测**同一个 displayd 生命周期内第 9 个
   CREATE 失败**（`ng: surface create FAIL (shm rc=28)`，探针 `display table` 用例
   就断言这个）。displayd 侧复用 handle 会把上一轮的像素泄漏给下一个客户端
   （跨客户端信息泄漏），不做；正解是 `SYS_SHM_FREE`（`src/ng`）。
   好在第 1 条的拒绝发生在 SHM 分配**之前**，所以被拒的伪造 CREATE 不消耗对象。
4. **COMMIT/MAP 没有归属校验**。这两个 body 里没有 token 字段：任何客户端知道
   surface id 就能 MAP（标记 mapped）或触发一次全量合成。要修需要协议 v4。
5. **WM 令牌 nonce 只有 16 位**（`(tsc>>7) ^ (pid<<16)`）。配合第 2 条（from 可伪造），
   理论上可暴力枚举 WM 令牌。本轮只把失败打印加了闸门（`D_MAX_PRINTS`）。
6. **状态文件内容本身没有认证**。`WM_STATUS`/`SESSION_STATUS` 是未认证消息，任何客户端
   都能往里写任意计数；本轮只保证"写得下去 + 不被截断卡死"，不保证内容真实。

## 提交（路径限定，不 push）

| 提交 | 路径 | 内容 |
|---|---|---|
| `639189b` | `src/desktop/include/ds_protocol.h`、`src/desktop/src/ds_protocol.c`、`tests/unit-ng/test_desktop.c` | 纯函数：`ds_wm_claim_ok` / `ds_focus_token_ok` / `ds_str_sanitize` + 41 checks |
| `8a619e4` | `src/desktop/display/displayd.c` | WM 令牌回收、焦点策略、状态行 ≤500、拒绝路径打印闸门、title 净化 |
| `70f2f96` | `src/desktop/session/sessiond.c`、`src/desktop/src/session_table.c`、`src/apps/demo/demo.c`、`tests/unit-ng/test_session.c` | 幂等 REGISTER、只认 v3 LIST_REPLY、title/path 净化、demo FOCUS 带令牌 + 6 checks |
| `651360a` | `src/desktop/display/displayd.c` | 去掉 REGISTER_HOTKEY ack（队列投毒） |
| `6b7fa04` | `tests/audit-ng/ui/audit_ui.c`、`tools/audit-ng-ui.ps1` | 探针第二轮用例 + 宿主驱动 |
| `6ac066d` | `tests/audit-ng/ui/audit_ui.c`、`tools/audit-ng-ui.ps1`、`docs/claims/ng-audit-ui.md` | v2 投毒用例 + 驱动断言 + claims |

## 门禁（最终树，全部在 QEMU 空闲时跑）

```
powershell -File tools/unit-ng.ps1      # PASS: 5262 checks / 0 failures（+41）
powershell -File tools/build-ng.ps1     # PASS -> dist-ng/aster-ng.elf
powershell -File tools/audit-ng-ui.ps1  # PASS: 28/28 用例 + 7 条修复回归断言
powershell -File tools/run-ng.ps1       # PASS: 159/159 期望行 + 75 patterns
```

产品路径证据（`run-ng` 串口）：

```
ng: focus reject ok (id=1 token=0x6e870203 n=1)      <- demo 越权 FOCUS 仍被拒（门禁保持）
ng: displayd wm token cleared ok (id=1)              <- WM 面板随 WM 死亡被销毁、令牌回收
ng: surface create ok (id=3 ...) / surface token ok  <- 新 WM 用新令牌重新认领
ng: hotkey registered ok (surface=3 key=any)         <- 新 WM 重新拿到热键通道
```

## 复现命令

```
# 全量协议畸形用例（默认跳过会 PANIC 的 ring3 越界写）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-ng-ui.ps1 -TimeoutSec 300

# 已知限制对照（期望 PANIC vec=0xe）
powershell ... -File tools\audit-ng-ui.ps1 -ShmOob

# 单条修复的定点复现：
#   状态行 512：探针 a_attack_status_poison（伪造 WM_STATUS/SESSION_STATUS）+ 一次 COMMIT
#   WM 令牌回收：a_attack_authz 里 destroy wm-token 之后用旧令牌 FOCUS
#   v2 LIST_REPLY：把 sessiond.c 的两处 `if (m.version >= 3u)` 改成 `if (1)` 重编
```
