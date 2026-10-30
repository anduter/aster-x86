# svc 会话 claim（P3：服务注册 + 生命周期 + 重启）

- 批次：2026-10-30 三层架构 P3（`docs/design/three-layer.md` §5/§7）
- 基线 HEAD：`48816d5`（277 项检查 / `ci -Full` 277/277 ×2 / 工作区干净）
- 允许改动范围：`src/kernel/svc/**`、`src/kernel/include/svc.h`、
  `src/user/ulib.*`、`src/kernel/apps/svcwatch.c`、`docs/claims/svc.md`
- 未改：`include/abi.h`（86/87/88/93 编号已定）、`build.ps1`、`core/**`、
  `net/**`、`fs/**`、`proc/**`

## 1. 改动文件清单

| 文件 | 改动 |
|---|---|
| `src/kernel/svc/registry.h` | 新增：manifest/表项/状态/重启策略/拓扑/自检的内核内部 API |
| `src/kernel/svc/registry.c` | 新增：manifest 文本解析、注册表（16 项）、lookup/heartbeat、内置 manifest 表 + requires 拓扑排序（Kahn）、svcwatch 生命周期引擎（心跳超时/进程消失/退避重启/隔离） |
| `src/kernel/svc/registry_selftest.c` | 新增：`svc: ...` 自检（走 `int 0x81` 真 syscall 路径 + 真实 `proc_spawn`） |
| `src/kernel/svc/svc_syscall.c` | 新增 86 `SYS_SVC_REGISTER`、87 `SYS_SVC_LOOKUP`、88 `SYS_SVC_HEARTBEAT`、93 `SYS_LOG_WRITE` 的 owns/handler；用户指针/长度/NUL 校验 |
| `src/kernel/svc/ipc.c` `ipc.h` | 新增 `ipc_port_owner_pid/uid`（注册时校验“只能登记自己的端口”） |
| `src/kernel/svc/supervisor.c` | `supervisor_init()` 改为按内置 manifest 拓扑启动（`services=legacy` 回退旧硬编码顺序）；`supervisor_entry()` 在 `-Test` 构建里跑 `svc_selftest()` |
| `src/kernel/apps/svcwatch.c` | 每 200ms 驱动 `svc_supervise_tick()`；保留旧 `echo` 看护（`svcwatch: restart echo -> pid N` 行为不变） |
| `src/kernel/include/svc.h` | 导出 `svc/registry.h` |
| `src/user/ulib.c` `ulib.h` | `svc_register / svc_lookup / svc_heartbeat / svc_log_write / svc_call / svc_call_name`（客户端重连） |

## 2. manifest 格式（设计 §5，内核解析）

内置表与 `SYS_SVC_REGISTER` 用同一份文本格式，一行一个 `key=value`：

```ini
# 期望路径下的服务（节选自 src/kernel/svc/registry.c 的内置表）
name=netd
version=1.0.0
type=driver
entry=netd
requires=logd,fsd
capabilities=MMIO:0xFEBC0000:0x20000,IRQ:11,DMA:65536
restart=on-failure
restart_limit=3
uid=0
```

| 键 | 取值/校验 | 缺省 |
|---|---|---|
| `name` | `[A-Za-z0-9_.-]`，1..31 字节，必须 | 无（缺失即拒绝） |
| `version` | `[A-Za-z0-9_.-]`，≤15 字节 | `""`（表项记录空版本） |
| `type` | `service` / `driver` / `app` | `service` |
| `entry` | 程序名或 `bin/xxx` 路径，`[A-Za-z0-9_.-/]`，≤31 | `name` |
| `requires` | 逗号分隔，≤4 个服务名；排序用，注册时不要求已存在 | 空 |
| `capabilities` | 逗号分隔：`MMIO:base:len`、`IRQ:n`(<256)、`DMA:len`；`CAP_*` 是包权限位，接受但不进硬件表 | 空 |
| `restart` | `never` / `on-failure` / `always` | `on-failure` |
| `restart_limit` | 十进制 0..16 | `3` |
| `uid` | 十进制 | `0` |

`#`/`;` 开头是注释；未知键、坏数字、坏名字、坏 cap 项一律**返回 -1（拒绝）**，
不做静默忽略。内核内置表（7 项）：`logd, fsd, netd, pkgd, shell, echosvc,
svcwatch`，其中 `fsd/pkgd/shell` 由 bootseq 负责 spawn（内置表里标记
bootseq-owned 占位），`echosvc/svcwatch` 由 supervisor 按拓扑序 spawn。

`SYS_SVC_REGISTER` 的 manifest 是用户态字符串（≤1024B，必须 NUL 结尾），
内核逐字节 `copy_from_user`（避免跨页整块拷贝误判），解析后**只保留**内存
结构；`SYS_SVC_LOOKUP` 的 name 必须与 `len` 对齐且以 NUL 结尾。

## 3. 实现要点

### 3.1 注册表（`svc/registry.c`）

表项字段（设计 §5）：`name[32] / version[16] / type / port / pid / uid /
caps_mask / state / restart_policy / restart_limit / fail_count / restarts /
last_heartbeat / restart_at / start_at / unhealthy_at / requires[]`，共 16 项
（`SVC_TABLE_MAX`），静态 BSS，`spinlock_t` 保护；所有打印/`proc_spawn`/
`proc_find` 都在锁外，锁内只做判定与落账。

- **register**：仅 root（`svc_register_allowed()` 是唯一判定点；per-process
  `CAP_SYS_ADMIN` 位图尚未落地，见 §7）；只能登记自己拥有的 IPC 端口
  （`ipc_port_owner_pid(port)==caller->pid`，root 例外）；重名返回 -1；
  端口被别的表项占用返回 -1。表项 `pid=caller`、`state=RUNNING`、
  `last_heartbeat=now`。
- **重启后的重新注册**：只有 `state==STARTING` 的已有表项允许新实例接管
  （更新 port/pid/version/caps → `RUNNING`），其它状态一律按重复注册拒绝。
- **lookup**：任意进程；只返回 `RUNNING && port!=0 && ipc_port_exists(port)`
  的端口，否则 -2（syscall 层 -1）。服务重启/隔离期间 lookup 必然失败，
  客户端靠这个失败重连。
- **heartbeat**：只接受表项记录的 `pid == caller->pid`（伪造别人的 port 被拒）；
  刷新 `last_heartbeat`，若此前 `UNHEALTHY` 则回到 `RUNNING`（自愈）。

### 3.2 生命周期引擎（svcwatch 线程，`svc_supervise_tick`）

| 状态 | 判定 | 动作 |
|---|---|---|
| `RUNNING` | `proc_find(pid)==0` 或进程 `ZOMBIE/FREE` | 记失败（进程消失） |
| `RUNNING` | `now-last_heartbeat >= 500 tick`（5s） | 标 `UNHEALTHY`，打印 `svc: heartbeat timeout ...` |
| `UNHEALTHY` | `restart=never` | 只报，不重启（`lookup` 已失败） |
| `UNHEALTHY` | 策略非 never：ring3 进程先 `SIGKILL`；宽限 100 tick 后仍活着 | 杀不掉（内核线程服务）→ `QUARANTINED` 并打印原因 |
| `STARTING` | 进程退出 / 300 tick 内没重新注册 | 记失败（`exited before register` / `register timeout`） |
| `RESTARTING` | `now >= restart_at` | `proc_spawn(entry)`；成功 → `STARTING`（port=0，等注册）；失败 → fail_count++，重排退避或隔离 |

**失败→退避**：`fail_count=1/2/3` → 100/200/400 tick（1s/2s/4s，封顶 400）；
`fail_count > restart_limit(3)` → `QUARANTINED`（`svc: quarantine <name> after
N failure(s)`），不再自动拉起，`lookup` 返回 -1。`restart=never` 的失败终态
是 `STOPPED`。

**重启与旧客户端**：记失败时立刻 `ipc_port_release(old_port)` 并把表项 port
清 0。旧客户端再 `SEND` 旧句柄直接 -1，`LOOKUP` 也 -1（`-ENOENT` 语义）；
服务重新注册后 `lookup` 返回**新 port**（IPC 句柄 generation 已变，旧句柄
不可能复活）。

`restart=always` 与 `on-failure` 当前**实现等价**（没有主动 stop API，
只有失败会触发重启），见 §7。

### 3.3 拓扑启动（`supervisor_init`）

内置 manifest 表 → `svc_topo_sort()`（Kahn，同层取表序最前，输出稳定）：
缺依赖的节点连同依赖它的节点一起“跳过”（`skipped`），有环返回 -1；
`requires` 允许指向表外**已存在**的服务（`service_exists()` 或新注册表里已有，
例如 `echosvc requires supervisor`）。

串口实测：

```text
svc: topology plan ok (logd->fsd->netd->pkgd->shell->echosvc->svcwatch)
svc: topology skip logd (no program)
svc: topology skip netd (no program)
svc: topology start ok (echosvc, svcwatch)
```

- 缺省 = manifest 拓扑模式；`services=legacy`（boot cmdline，`boot_cmdline_has`）
  回退 0.74.2 的硬编码顺序 `echosvc -> svcwatch`，并打印
  `svc: topology legacy (services=legacy)`。
- 计划失败（内置表解析坏/有环）时 supervisor 打印
  `supervisor: topology failed -> legacy fallback` 并退回硬编码 spawn，
  保证系统仍可起。
- 本阶段 `logd/netd` 还没有 ring3 程序 → `skip (no program)`；`fsd/pkgd/shell`
  由 bootseq spawn（`core/**` 不在本会话可改范围），内置表只占位、不重复
  spawn。等 P4/P6 把服务真正迁到 ring3 后，把 builtin 表的 owner 从
  bootseq 改回 supervisor 即可启用完整拓扑启动。

### 3.4 syscall（svc 组新增，编号按 `abi.h` 预留，未改 abi.h）

| 号 | 参数 | 返回 | 校验 |
|---|---|---|---|
| 86 `SYS_SVC_REGISTER` | `ebx=manifest_ptr, ecx=port` | 0 / -1 | 权限 root、manifest 解析、port 存在且归属 caller、重名/表满 |
| 87 `SYS_SVC_LOOKUP` | `ebx=name, ecx=len` | port / -1 | `len ∈ 1..31`、用户窗口、`name[len]==NUL`、无内嵌 NUL |
| 88 `SYS_SVC_HEARTBEAT` | `ebx=port` | 0 / -1 | 表项存在且 `pid == caller->pid` |
| 93 `SYS_LOG_WRITE` | `ebx=tag, ecx=msg, edx=len` | 0 / -1 | tag NUL 结尾 ≤24B、msg ≤256B、用户窗口 |

`svc_syscall_owns()` 认领 80–84 + 86–88 + 93；`SYS_NR_MAX=95` 不变，
未认领编号仍由分发器统一返回 -1（不 panic）。

### 3.5 ulib 客户端

`svc_register / svc_lookup / svc_heartbeat / svc_log_write / svc_call /
svc_call_name`。`svc_call` 把 `4B ticket + req` 作为 payload 用
`NEED_REPLY` 发给服务，等回复后只把 payload 拷回调用方；`svc_call_name`
在 call 返回 -1（服务刚重启）时重新 `svc_lookup` 再试一次，就是“客户端
重连”。没有 `SYS_PORT_CLOSE`，所以每进程缓存一个客户端端口（带 pid，
fork 后子进程重新建），见 §7。

## 4. 期望自检行（`-Test` 构建，supervisor 线程）

```text
svc: register ok (name=testsvc port=32)
svc: lookup ok (name=testsvc port=32)
svc: duplicate rejected
svc: non-root register denied
svc: dependency order ok (logd->fsd->netd->pkgd->shell)
svc: cycle rejected
svc: missing requires skipped (2 node(s))
svc: heartbeat ok (age=0 tick)
svc: heartbeat timeout testsvc (age=502 ticks) -> unhealthy
svc: heartbeat timeout -> unhealthy
svc: tsvc-restart failed (simulated crash), fail_count=1 -> restart in 100 tick(s)
svc: restart tsvc-restart -> pid 45 (attempt 2)
svc: restart on-failure ok (old port=19 new port=35 pid=45)
svc: tsvc-quar failed (simulated crash), fail_count=1 -> restart in 100 tick(s)
svc: restart tsvc-quar spawn failed, retry in 200 tick(s)
svc: restart tsvc-quar spawn failed, retry in 400 tick(s)
svc: quarantine tsvc-quar after 4 failure(s)
svc: quarantine after limit ok (fail_count=4 state=quarantined)
svc: log write ok (tag=svclog)
svc: selftest PASS
```

自检走真实路径：86/87/88/93 用 `int 0x81` 触发（内核线程的静态缓冲在 3GB
以下，uaccess 对非用户上下文只查上界，与 klog 的 `SYS_DMESG` 自检同一手法）；
重启/隔离用**真实 `proc_spawn`**（`entry=hello` 成功、`entry=nosuch-program`
失败）与确定的 `now` 驱动 `svc_supervise_step()`，不 sleep。自检期间
`svc_supervise_pause(true)` 暂停 svcwatch 的 tick，避免抢状态机；结束时清理
测试表项并归还端口。

建议的 `build.ps1` 断言（前缀匹配，父会话集成用）：

```powershell
@{ Name = 'svc 注册/查询';      Ok = (($out -match 'svc: register ok') -and ($out -match 'svc: lookup ok')) },
@{ Name = 'svc 重复/权限拒绝';  Ok = (($out -match 'svc: duplicate rejected') -and ($out -match 'svc: non-root register denied')) },
@{ Name = 'svc 依赖拓扑';       Ok = (($out -match 'svc: dependency order ok') -and ($out -match 'svc: cycle rejected')) },
@{ Name = 'svc 心跳';           Ok = (($out -match 'svc: heartbeat ok') -and ($out -match 'svc: heartbeat timeout -> unhealthy')) },
@{ Name = 'svc 重启/隔离';      Ok = (($out -match 'svc: restart on-failure ok') -and ($out -match 'svc: quarantine after limit ok')) },
@{ Name = 'svc 自检';           Ok = ($out -match 'svc: selftest PASS') }
```

## 5. 回归结果

- `tools\session-run.ps1 -Name svc -Test`（2026-10-31 00:38:26，51.6s，
  `.obj-svc`/`.dist-svc`，`serial-svc.log`）：**277/277 PASS**
  （`=== TEST PASS (277/277) ===`），`svc: selftest PASS` 全行见 §4。
- `tools\session-run.ps1 -Name svc -Test` 第二次（00:39:15，38.8s，增量构建）：
  **277/277 PASS**；`svc: selftest PASS`、`svcwatch: restart echo -> pid 54`
  均出现（旧看护行为未变）。
- 基线 277 项全绿；新增自检行随 `-Test` 串口输出，**未改 `build.ps1`**。
- `ci.ps1 -Full -StrictBom`：见 §5.2（需 clean tree，提交后单独跑）。

## 6. 串口摘录（`serial-svc.log`，277/277 那次）

启动（supervisor_init，拓扑启动）：

```text
svc: topology plan ok (logd->fsd->netd->pkgd->shell->echosvc->svcwatch)
svc: topology skip logd (no program)
svc: topology skip netd (no program)
svc: topology start ok (echosvc, svcwatch)
```

调度器启动后（supervisor 线程，紧接着 `ipc: selftest PASS`）：

```text
svc: register ok (name=testsvc port=32)
svc: lookup ok (name=testsvc port=32)
svc: duplicate rejected
svc: non-root register denied
svc: dependency order ok (logd->fsd->netd->pkgd->shell)
svc: cycle rejected
svc: missing requires skipped (2 node(s))
svc: heartbeat ok (age=0 tick)
svc: heartbeat timeout testsvc (age=502 ticks) -> unhealthy
svc: heartbeat timeout -> unhealthy
svc: tsvc-restart failed (simulated crash), fail_count=1 -> restart in 100 tick(s)
svc: restart tsvc-restart -> pid 45 (attempt 2)
svc: restart on-failure ok (old port=19 new port=35 pid=45)
svc: tsvc-quar failed (simulated crash), fail_count=1 -> restart in 100 tick(s)
svc: restart tsvc-quar spawn failed, retry in 200 tick(s)
svc: restart tsvc-quar spawn failed, retry in 400 tick(s)
svc: quarantine tsvc-quar after 4 failure(s)
svc: quarantine after limit ok (fail_count=4 state=quarantined)
svc: log write ok (tag=svclog)
svc: selftest PASS
```

旧 echo 看护（未改行为）：

```text
svcwatch: restart echo -> pid 44
```

## 7. 设计问题（未改契约，记录待评审）

1. **`CAP_SYS_ADMIN` 仍是空位**：`proc_t` 没有 per-process capability 位图
   （`PKG_CAP_SYS_ADMIN` 只是包 manifest 的权限位），register 的判定退化为
   “root only”。判定点已收在 `svc_register_allowed()`，位图落地后改一行。
2. **内核线程服务无法终止**：`SIGKILL` 只在返回用户态时投递，内核线程服务
   （本阶段的 echosvc/pkgd/fsd 都是内核线程入口）心跳超时后杀不掉；引擎对
   “还活着但 unhealthy” 的服务只能隔离（`svc: quarantine ... cannot
   terminate`）。需要 `proc_kill()` 或让服务自己检查停止标志（P4 服务化时
   一起做）。
3. **`restart=always` 与 `on-failure` 的实现等价**：没有主动 stop API，
   正常退出与失败不可区分；`always` 目前只在失败路径触发。
4. **没有 `SYS_PORT_CLOSE`**（P1 已记录）：`ulib svc_call` 每进程缓存一个
   客户端端口，进程退出后由 create 的 reaper 回收；但全局端口表只有 16 个，
   多进程客户端同时在线会紧张。建议 ABI 1.6 加 close/handle 化。
5. **`svc_call` 超时后的迟到回复**：没有关联 ID（ticket 是固定的
   `(pid<<16)|port`），上一次超时的回复可能被下一次 `svc_call` 读到。建议
   消息头加 request id 或提供 flush/drain 语义。
6. **重启的身份/世代**：表项只认 `pid`，`proc_t` 没有 generation；极端情况下
   pid 复用会让引擎误判“还活着”。注册超时（3s）兜底；建议后续加进程
   generation 或在表项里存 start tick。
7. **依赖排序不跨表**：旧 `service_*` 注册的服务只能作为“外部已满足”
   （`provided()` 检查 `service_exists()`），不参与拓扑序；P4 统一注册表后
   消失。
8. **bootseq 仍拥有 `fsd/blkd/pkgd/shell` 的 spawn**（`core/**` 不可改）：
   manifest 里它们是 bootseq-owned 占位；`logd/netd` 没有 ring3 程序，拓扑
   计划里保留位置但 `skip (no program)`。完整“manifest 驱动启动”要等
   bootseq 把 spawn 交给 supervisor（P4/P7）。
9. **`SYS_LOG_WRITE` 不做权限/频率限制**：任何进程都能写 klog（刷环/日志
   注入），只限制了 tag/长度。建议后续按服务身份或速率限制。
10. **表项固定 16、caps 只记类型位图**：`capabilities=` 解析出的 MMIO/IRQ/DMA
    只进 `caps_mask` 供后续授权，P3 不把它变成真实 cap；irq/mmio 的授权在
    P5/P6 接上。
11. **沙箱进程用不了 86–88/93**：`proc_t.syscall_mask` 只有 64 位，
    `syscall_dispatch` 对 sandboxed 进程 `>=64` 一律拒绝（与 ipc claims #11
    同一限制）；本阶段服务都不沙箱，P4 放服务进沙箱前需要加宽位图。
12. **心跳只能服务自身发**：没有“代理心跳”机制；内核代跑的服务（echosvc
    等）没接新注册表，仍在旧 `service_*` 表里，不受心跳影响。

## 8. 遗留

1. 8h 浸泡未做（P3 只要求回归；P5 起统一浸泡）。
2. 真实 ring3 服务（`SYS_SVC_REGISTER` 的调用方）要等 P4 把 fsd/pkgd 迁
   用户态；本会话的注册/重启用例在内核线程里用真实 syscall + 真实
   `proc_spawn` 覆盖，客户端 ulib 只做了编译级验证（`user_shell.c` 不在可改
   范围，没有加 ring3 演示命令）。
3. `docs/api.md` 的 ABI 1.5 说明未更新（不在可改范围）；§3.4 已给出参数约定。
4. `build.ps1` 检查项未加（按规矩不改；§4 给了可直接粘贴的断言）。
5. `-smp 4` 手工验证未做：注册表有自旋锁，状态机单线程驱动；并发注册的
   压力留给 P4 服务化后补。
