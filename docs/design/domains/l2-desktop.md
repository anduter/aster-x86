# L2 桌面会话（plane=l2a/l2b/l2c）

> 状态：WP0-2 域文档补全，2026-10-31；描述 ng HEAD `c4efabc` 的已提交实现。
> 上位：`../architecture.md` §2.4、§3、ADR-006/011。
> 证据：`../../claims/ng-WP3-1.md`、`ng-WP3-2.md`。
> **WP3-3（L2c session manager）正在另一会话进行，未提交；本文不把它算作已实现。**

## 职责

L2 按重启域拆成三个组件：

| 组件 | 职责 | 代码 | 状态 |
|---|---|---|---|
| **L2a display server** | 扫描输出、surface 生命周期、合成、z-order、焦点、输入路由、全局热键副本 | `src/desktop/display/displayd.c`、`src/desktop/src/ds_{protocol,surface,composite,draw}.c` | 已实现（WP3-1/3-2） |
| **L2b shell/WM** | 顶栏面板、启动器、任务按钮、任务切换、通知占位；**不拥有应用** | `src/desktop/shell/wm.c`、`src/desktop/src/ds_wm.c` | 已实现（WP3-2） |
| **L2c session manager** | 应用生命周期、窗口注册表、shell 角色令牌、孤儿回收 | —（工作区 `session_*.c` 未提交） | **进行中（WP3-3）** |
| 输入法/剪贴板/设置/无障碍/文件选择器 | L2 其他组件 | — | 设计条目，未实现 |

L2a 的内核机制只有三条：`SYS_SHM_ALLOC`（45）、`SYS_SHM_MAP`（46）、`SYS_FB_MAP`（47）。
surface/窗口/焦点/装饰都不在内核；displayd 是普通 ring3 进程，挂了只回收自己的
句柄/AS/共享页（`../display-server-ng.md` §2/§9）。

## 不做什么

1. **shell 不拥有应用**：应用生命周期归 L2c；WM 的启动器只是 `spawn`，不记录/回收子进程
   （WP3-2 明确遗留）。
2. **displayd 不做窗口策略/装饰**：没有 SSD/resize/拖拽/点击焦点；窗口位置来自 CREATE
   或级联策略。
3. **不做 GPU 3D/多显示器/HDR/vsync/双缓冲/脏区合并**：每次 COMMIT 重合成覆盖区域。
4. **不做鼠标/按键释放/重复率**：只有键盘按下；没有点击焦点、没有菜单展开；
   `Alt+Tab` 与 `Tab` 同义（PS/2 驱动没有 Alt 修饰位）。
5. **不做 cap/角色令牌**：当前所有进程 uid=0；SHM_MAP 的 owner/root 校验就是全部权限，
   ticket/handle“知道就能用”（WP3-3/ABI 1.0 收紧）。
6. **不做 S2/S3**：display server 崩溃后的重连、GPU 恢复不在 D0；S1 本身也还没验收。
7. **协议不是 ABI 1.0**：display 协议是 L2 内部协议；只有 syscall 45/46/47 属于 native ABI。

## 接口（syscall / IPC / ABI）

### syscall（机制）

| 号 | 签名 | 语义 / 限制 |
|---|---|---|
| 45 | `shm_alloc(pages, &va, &handle)` | 8 个对象、每对象 ≤1024 页（4 MiB）；首映射 refcnt=1；返回 8 B VA + 4 B handle |
| 46 | `shm_map(handle, &va)` | owner 或 root；每对象最多 8 条映射；VA 由每进程 bump 分配（起点 `0x50000000`） |
| 47 | `fb_map(&va, &info)` | root-only；固定 VA `0x60000000`；返回 48 B `ds_fb_info`；无 LFB → `ENODEV` |
| 10–14 | `port_*` | displayd 的端口/ticket；消息 ≤4 KiB 控制面 |
| 40–44 | `spawn/wait/getpid/kill/getppid` | WM 启动器、displayd 客户端管理 |

**没有 `SYS_SHM_FREE`**：surface DESTROY 只清表项，displayd 的映射要等进程退出
才释放（D15）。进程退出/被 kill 时先 `pg_unmap_in` 共享页（不 free），再让 core
按 refcount 决定是否归还物理页；顺序反了会 double free（`shm.c` 注释）。

### display 协议 v2（L2 内部）

消息 = 20 B 头 `{u32 magic; u16 version; u16 type; u32 len; i32 status; u32 seq}`
+ 固定 body（body 只用 u32）；整条 ≤512 B。外层 IPC 头 16 B 与 `ipc_msg_hdr` 一致。

| 消息 | 方向 | body / 语义 |
|---|---|---|
| `CREATE` | client→server | v1 16 B `{w,h,flags,reserved}`；v2 40 B 追加 `x,y,owner_pid,title[4]`；`flags&1` 时用 x/y |
| `CREATE_REPLY` | server→client | `{surface_id, shm_handle, pages, x, y, w, h}`；客户端再 `SHM_MAP` |
| `MAP` | client→server | `{surface_id, reserved}`；COMMIT 前必须 MAP，否则 `EBUSY` |
| `COMMIT` | client→server | `{surface_id, x,y,w,h}` 脏矩形；裁到 surface 内部 |
| `DESTROY` | client→server | 销毁 surface，焦点回退到最上层 |
| `FOCUS` | client→server | 置焦点 + 提到 z-order 最前 |
| `INPUT` | server→焦点 client | `{surface_id, kind, key, scancode, flags}`；全局热键副本带 `DS_INPUT_F_GLOBAL=0x100` |
| `LIST` / `LIST_REPLY` | 双向 | v2 任务表；entry 44 B `{id,owner_pid,x,y,w,h,focused,title[4]}`，最多 8 条 |
| `REGISTER_HOTKEY` | client→server | `{surface_id,key,flags,reserved}`；`key=0` 表示所有按键先给注册者一份 |
| `WM_STATUS` | client→server | v2 `{surface_id,tasks,focus,switches,lastswitch,launches,lastpid}`；displayd 汇总进 `/run/display.status` |
| `BYE` | client→server | 保留 |

规则：

1. 解码严格校验 magic/version/type/len；未知类型/多一字节都拒绝，displayd 丢坏消息继续服务。
2. v1/v2 兼容：解码器接受 version 1/2；v1 CREATE 16 B → 空标题、owner_pid=0、级联放置；
   v1 下 LIST/LIST_REPLY/REGISTER_HOTKEY/WM_STATUS 长度为 0，直接拒绝。编码器写 v2。
3. `COMMIT` 先裁到 surface；完全不可见 → `EINVAL` 并记 `rejects`。
4. `INPUT` 只发焦点 surface 的 `client_ticket`；没有焦点时丢弃；热键注册者先拿一份副本。
5. **工作区里的 v3（`token` 字段）尚未提交**，不属于本基线（D13）。

发现/状态文件：`/run/display.port`（displayd 端口 ticket，十进制文本）、
`/run/display.status`（`up=1 port=1 surface=N mapped=N commits=N focus=ID input=N px=...`，
v2 追加 `wm=...` 字段）。两者都是 ramfs 普通文件，不引入新机制。

### ABI 边界

- 控制面 IPC 使用 native ABI 的 10–14；数据面用 45/46 的共享页；LFB 用 47。
- display 协议的 magic/version/body 是 L2 私有契约，不是 ABI 1.0 的一部分；
  应用应通过 SDK/工具包使用，不直接依赖 wire format。

## 失败等级与恢复

| 组件 | 设计等级 | 当前隔离证据 | 恢复/差距 |
|---|---|---|---|
| displayd（L2a） | `session` | kill(displayd,9) → `status=265`、`live=0`，内核继续跑；另一个客户端超时退出 0 | 客户端自行降级；没有自动重启 displayd；S2 重连未做 |
| wm（L2b） | `session` | kill WM 后 Demo 进程仍活（`ng: demo survives wm ok`），displayd 继续服务 | **面板不会自动回来**；输入恢复未证明 → S1 未验收（D16） |
| session manager（L2c） | `session` | 无 | WP3-3 进行中 |
| surface 对象 | `optional` | DESTROY 清表；共享映射等进程退出 | 无 `SHM_FREE`（D15） |
| 输入路由 | `session` | 键盘按下经 displayd 转发到焦点；全局热键副本 | 无鼠标/释放；焦点固定（无点击） |

**S1 的准确定义**（architecture §3）：shell/WM 崩溃或卡死时 L3 进程、文档状态、
输入不丢；允许视觉中断、焦点/IME 重置、重绘。当前只证明了“WM 被 kill 后 Demo
不死、displayd 继续服务”，**没有证明面板重建和输入恢复**。D0 出口尚未达成。

## manifest 五元组

设计值（当前无解析器，D04）：

| 组件 | plane | criticality | fallback | health | restart |
|---|---|---|---|---|---|
| `displayd` | `l2a` | `session` | `vga-text/no-composite` | `status:/run/display.status` | `on-failure`，3 次 |
| `wm` | `l2b` | `session` | `minimal-panel`（未实现） | `status:/run/display.status`（wm 字段） | `on-failure`，3 次 |
| `session-mgr` | `l2c` | `session` | `reap-orphans` + 应用重连（未实现） | `heartbeat:5s` | `on-failure`，3 次 |
| `wallpaperd`（示例） | `l2` | `optional` | `compositor.solid_color` | `heartbeat:5s` | `on-failure`，3 次 |

规则：L2 组件不允许 `fallback=none`；`optional` 不得被 `session`/`core` 同步依赖；
shell 不拥有应用（ADR-006）。当前 `displayd` 的“fallback”在 VGA 文本下是整段跳过
（`ng: WP3-1 selftest skipped (no fb)`），不是运行时热切换。

## 当前实现状态（ng WP）

| WP | 内容 | 证据 |
|---|---|---|
| WP3-1 | L2a `/bin/displayd`：LFB、surface 表、z-order/裁剪、ARGB source-over、光标、焦点、输入转发；`/bin/demo`；QMP 像素断言 | `../../claims/ng-WP3-1.md` |
| WP3-2 | L2b `/bin/wm`：1280×32 面板/启动器/任务切换/热键；协议 v2；kill WM 应用存活 | `../../claims/ng-WP3-2.md` |
| WP3-3 | L2c session manager / 角色令牌 | **进行中，未提交**；不引用工作区 `session_*` |

关键证据行：`ng: displayd up`、`ng: composite ok (px=0x3070c0 at=160,120)`、
`ng: input route ok (key=b)`、`ng: wm up`、`ng: wm panel ok (px=0x202838 at=0,0)`、
`ng: wm task switch ok`、`ng: wm launch ok`、`ng: demo survives wm ok`；
第二张 QMP 截图断言 `(4,4)=#202838` 与 `(217,177)=#3070c0`。

## 待办

1. **WP3-3 session manager**：应用注册表/窗口表/角色令牌/孤儿回收；shell 不拥有应用。
2. **WP3-5 S1 故障注入**：杀 shell/WM 后断言 L3 存活、输入恢复、面板回来；
   再把 architecture 的 D0 出口标成达成（D16）。
3. **S2**：displayd 重连、surface 重建、输入队列语义；S3：GPU 恢复。
4. **cap/role token**：FOCUS/INPUT/SHM_MAP 的归属校验，替代“uid=0 + owner/root”（D14）。
5. **surface 生命周期**：`SHM_FREE` 或引用释放（D15）；DESTROY 后长跑不耗尽对象。
6. **SSD/装饰/resize/拖拽/点击焦点**：需要鼠标/点击事件。
7. **输入**：IRQ、按键释放、重复率、鼠标、IME；Alt 修饰位。
8. **vsync/双缓冲/脏区合并/多显示器**：D1。
9. **协议 v3 落地后再更新本文**：token 字段、session 协议、兼容矩阵。
