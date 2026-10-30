# pkgd 会话 claim（P4：pkgd 服务化，ring3 策略 + ring0 回退）

- 批次：2026-10-31 三层架构 P4（`docs/design/three-layer.md` §8）
- 基线 HEAD：`4fe9c55`（288 项检查 / `ci -Full` 288/288 ×2 / 工作区干净）
- 交付提交：`484765d`（实现）、`bca1e17`（claims/design）、`364fc44`
  （ring3 list 回复文本缓冲 1KiB）、`aedde60`（CI 记录）
- 允许改动范围：`src/user/pkgd.c`、`src/user/ulib.*`、`src/kernel/pkg/**`、
  `src/kernel/include/pkg.h`、`src/kernel/svc/supervisor.c`（只加启动钩子）、
  `build.ps1`（pkgd 编译/镜像/检查项）、`docs/claims/pkgd.md`、
  `docs/design/three-layer.md`（契约补充）
- 未改：`include/abi.h`（`SYS_PKG=77` 已存在；只扩了它的 op 空间，见 §2）、
  `core/**`、`net/**`、`fs/**`（含 SIP）、`proc/**`、`security/**`

## 1. 目标与结论

把组件的**流程/策略**从内核搬到 ring3 守护进程，内核只保留“验签/落盘原语”：

| 层 | 职责 |
|---|---|
| ring3 `pkgd`（`src/user/pkgd.c`） | 解析命令、**自己读 .pkg 文件**、驱动原语顺序、通知 supervisor、失败回滚、服务注册/心跳、legacy 兼容注册 |
| ring0 `pkg` 模块 | `pkg_verify`（APKG 结构/文件哈希/RSA）、staging 目录、逐文件落盘/哈希复验、原子 rename、组件 DB 读写、组件 ELF 启动、删目录 |
| 内核分发器 | 按启动模式把 `SYS_PKG(0..3)` 转发给 ring3（IPC）或直接走 ring0；记录客户端 euid；auto 模式失败回退 |

结果：

- 默认 `pkgd=auto`：ring3 失败回退 ring0；`pkgd=ring0` 完全走原实现。
- 杀掉 pkgd 后内核与 shell 存活；svcwatch 按 `restart=always` 重启；分发器
  重新 `svc_lookup` 拿到新 port；重启窗口内 auto 自动回退 ring0。
- 现有 `pkg:` 15 条断言行全部保留；新增 11 条 `pkgd:` 断言行。

## 2. 内核原语（`SYS_PKG=77`，op 6..17）

**未新增系统调用号**：复用 77，只扩 op（`src/kernel/include/pkg.h`）。原语只接受
**已注册的 ring3 pkgd** 调用：`proc_current()->pid == svc_registry_get("pkgd").pid`
且 `euid==0`、`is_user`；其它进程一律 `-1`。

| op | 名字 | 参数 | 语义 |
|---|---|---|---|
| 6 | `SESSION_BEGIN` | `a=包字节/路径串, b=字节数(0=路径)` | 读包（ring3 传来的用户缓冲用 `copy_from_user`）→ `pkg_verify` → 策略（service=root/account=root/保留 cap；app=自己且 account 匹配）→ 查重 → 建 staging 会话 |
| 7 | `SESSION_INFO` | `a=buf, b=max` | 回 `name= version= type= files= elf= program=`（ring3 据此决定通知 supervisor） |
| 8 | `SESSION_STAGE` | `a=文件下标` | 从内核包缓冲写 staging + SHA-256 复验 + chmod |
| 9 | `SESSION_COMMIT` | — | 组件 ELF 落 `<stage>/bin/<name>` → 原子 rename → 追加 DB 行 |
| 10 | `SESSION_START` | — | `pkg_component_start()`（ring3 exec）；失败清目录+DB 并返回错误 |
| 11 | `SESSION_ABORT` | — | 未提交删 staging；已提交停组件+删目录+删 DB |
| 12 | `REMOVE_BEGIN` | `a=组件名` | 策略/DB 校验，建卸载会话（不删任何东西） |
| 13 | `REMOVE_INFO` | `a=buf, b=max` | 回 `type= program= root=` |
| 14 | `REMOVE_COMMIT` | — | 停组件 → 删目录 → 删 DB 行 |
| 15 | `REMOVE_ABORT` | — | 取消卸载会话 |
| 16 | `DB_READ` | `a=buf, b=max` | 回组件 DB 文本（列表的解析/过滤在 ring3） |
| 17 | `VERIFY_RAW` | `a=组件名` | 已安装完整性（DB+目录+每文件哈希+ELF 哈希） |

所有原语在 `pkgd_primitive()` 里统一 `sip_updater_enter/exit`：staging/rename/DB
都是 `system:/System` 下的写操作，SIP-lite 的规则与 ring0 的
`pkg_install/pkg_remove` 完全一致，**没有放宽**。

## 3. ring3 pkgd 守护进程

`src/user/pkgd.c`（独立 ELF，`user.ld` 链接到 `0x40000000`）：

1. 启动：`PORT_CREATE` → `SVC_REGISTER`（manifest：
   `name=pkgd version=1.0.0 type=service entry=pkgd requires=fsd
   restart=always restart_limit=3 uid=0`）→ legacy `SERVICE_REGISTER("pkgd")`
   （旧 `service_connect` 客户端兼容）→ 打印 `pkgd: ring3 online (pid= port=)`。
2. 循环：`PORT_RECV`（300ms 超时）→ 处理请求 → `SVC_HEARTBEAT`。
3. 请求帧（16B IPC 头之后的 payload，小端）：

   ```text
   请求: magic("PKG2") ticket req_id op euid arg_len arg[arg_len]
   回复: magic("PKG3") req_id status count text_len text
   ```

4. `install`：`fs_stat`+`fs_open/read` 读真实 `.pkg`（读不到才把名字交给内建包
   目录）→ `SESSION_BEGIN` → `SESSION_INFO` → 逐文件 `SESSION_STAGE` →
   `SESSION_COMMIT` → 若 service 用 native `SERVICE_CONNECT("supervisor")` 发
   `start:<program>` → `SESSION_START`；任一步失败按会话状态 `ABORT`。
5. `remove`：`REMOVE_BEGIN` → `REMOVE_INFO` → 通知 supervisor `stop:<program>`
   → `REMOVE_COMMIT`。
6. `list`：`DB_READ` 取文本，在用户态逐行解析成 `name version type`。
7. `verify`：`VERIFY_RAW`，把状态码映射成 `ok/err <code> <detail>`。

## 4. 入口切换与回退

- 启动参数：`pkgd=ring0|ring3|auto`（`pkg_cmdline_has`；构建期
  `-DPKGD_FORCE_MODE=1/2` / `build.ps1 -PkgdMode` 也可强制）。
- `ring0`：`pkgd_init()` 按旧行为建 port/注册；`SYS_PKG(0..3)` 直接调用
  `pkg_install_path/pkg_remove/pkg_verify_installed/pkg_list`。
- `ring3`：不建 ring0 端口；`pkgd` 线程加载内嵌 ELF 进入 ring3；失败**不**
  回退（显式强制模式）。
- `auto`（默认）：先 ring3；ring3 不可用/启动失败时同一线程回退 ring0
  `pkgd_ring0_loop()`。运行期（pkgd 被杀、svcwatch 还没重启）分发器发现
  `svc_lookup` 失败或 pid 已死，也直接回退 ring0。
- 分发器在 auto 下转发失败才回退；强制 ring3 下返回
  `err -8 pkgd ring3 unavailable`（不静默改语义）。

### 4.1 内嵌 ELF 的信任链（设计取舍）

ring3 pkgd 的 ELF 由 `build.ps1` 编译后 **incbin 进内核镜像**
（`obj/pkgd_blob.asm`），启动线程用 `pkg/pkgd.c` 里的 ELF 加载器
（PT_LOAD + W^X + ASLR 栈）映射进用户空间。信任根是**内核镜像签名**（boot
verifier 已验），不走 `system:/bin` 的 shell 签名校验——因为
`user_mode.c` 的 `trust_verify_shell()` 只接受“shell 那一份签名”，没有多映像
信任锚（见 §8 设计请求 1）。内核启动后仍把同一份映像写到
`system:/bin/pkgd` 供审计/查看；同一次启动内重启不重写。

### 4.2 启动时序（-Test 构建）

`svc_selftest()` 用**虚拟 now** 驱动整张服务表（心跳/退避/隔离），会把已注册
的服务误判为心跳超时。因此 `supervisor_entry()` 在自检结束后调用
`pkgd_supervisor_ready()`；pkgd 线程先等这个标志再注册（生产构建 supervisor
线程一启动就置位，几乎无延迟）。这一行是 `supervisor.c` 唯一新增的钩子。

## 5. 期望断言行（新增 11 条，`299 = 288 + 11`）

```text
pkgd: ring3 online (pid=<n> port=<n>)
pkgd: ring3 list ok
pkgd: ring3 verify ok
pkgd: ring3 remove ok
pkgd: ring3 install ok (echosvc)
pkgd: ring3 service online ok
pkgd: ring3 kill isolation ok (shell pid=<n> alive, +<n> ticks)
pkgd: svcwatch restart ok (pid=<old> -> <new>)
pkgd: fallback ring0 ok
pkgd: ring0 mode verify ok
pkgd: selftest PASS
```

`build.ps1` 检查项（前缀/包含匹配）：

```powershell
@{ Name = 'pkgd ring3 上线';       Ok = ($out.Contains('pkgd: ring3 online')) },
@{ Name = 'pkgd ring3 列表';       Ok = ($out.Contains('pkgd: ring3 list ok')) },
@{ Name = 'pkgd ring3 校验';       Ok = ($out.Contains('pkgd: ring3 verify ok')) },
@{ Name = 'pkgd ring3 卸载';       Ok = ($out.Contains('pkgd: ring3 remove ok')) },
@{ Name = 'pkgd ring3 安装';       Ok = ($out.Contains('pkgd: ring3 install ok')) },
@{ Name = 'pkgd ring3 服务在线';   Ok = ($out.Contains('pkgd: ring3 service online ok')) },
@{ Name = 'pkgd 故障隔离';         Ok = ($out.Contains('pkgd: ring3 kill isolation ok')) },
@{ Name = 'pkgd svcwatch 重启';    Ok = ($out.Contains('pkgd: svcwatch restart ok')) },
@{ Name = 'pkgd ring0 回退';       Ok = ($out.Contains('pkgd: fallback ring0 ok')) },
@{ Name = 'pkgd ring0 强制模式';   Ok = ($out.Contains('pkgd: ring0 mode verify ok')) },
@{ Name = 'pkgd 自检通过';         Ok = ($out.Contains('pkgd: selftest PASS')) }
```

自检流程（`pkg_selftest_entry` 末尾，`pkgd_selftest()`）：等 ring3 上线 →
list/verify 走 ring3 → 把 echosvc 包写到 `system:/tmp` → **ring3 卸载** →
（等 legacy svcwatch 重启 echo，顺带保留原有断言）→ **ring3 安装（真实
.pkg 文件）** → 等组件服务在线 → SIGKILL pkgd → 立刻验证 auto 回退 ring0 →
找 shell pid 证明隔离 → 等 svcwatch 重启出新 pid → 强制 `ring0` 模式 verify。

## 6. 验证记录

### 6.1 回归

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name pkgd -Test
# === TEST PASS (299/299) ===   42.0s
# === TEST PASS (299/299) ===   42.6s
# === TEST PASS (299/299) ===   41.7s   （恢复/重放改动后）
# === TEST PASS (299/299) ===   42.1s   （list 缓冲 1KiB 修正后）
```

### 6.2 CI（提交 `484765d` 的 clean tree）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ci.ps1 -Full -StrictBom
```

- 第 1 次：**CI PASS**，122.4s；静态 5 项全 PASS（worktree clean / BOM+parse
  39 files / no artifacts 355 / check-deps 0 / unit 10 suites）；`build.ps1`
  PASS；`-Test` **299/299 ×2**（56.7s / 41.7s）；结束后 worktree 仍 clean。
- 第 2 次：**CI PASS**，122.5s；同样 5 项静态 PASS、`-Test` **299/299 ×2**
  （56.5s / 41.9s）、worktree clean。
- 最终 HEAD `bca1e17`（claims/design 提交后）上再连续跑 2 次：**CI PASS**
  120.9s / 122.0s；两次都是静态 5 项 PASS、`-Test` **299/299 ×2**
  （54.9s / 41.7s、55.6s / 42.0s）、结束后 worktree clean。
- 最终代码提交 `364fc44`（list 缓冲修正）上再连续跑 2 次：**CI PASS**
  120.7s / 121.5s；静态 5 项 PASS、`-Test` **299/299 ×2**
  （54.7s / 41.7s、55.0s / 42.2s）、worktree clean。
- `tools\unit.ps1`：**UNIT PASS (10/10)**，520 checks。
- `tools\check-deps.ps1`：**0 violations**（199 files / 642 includes）。

## 7. 串口证据（`serial-pkgd.log`，299/299 那次）

启动：

```text
pkgd: mode=auto
pkgd: ring3 daemon launch deferred to pkgd thread
pkgd: ring3 image installed (system:/bin/pkgd 22916 bytes)
pkgd: service online port=258
pkgd: ring3 online (pid=6 port=48)
```

ring3 自检：

```text
pkgd: mode=auto
pkgd: ring3 online (pid=6 port=48)
pkgd: ring3 list ok
pkgd: ring3 verify ok
pkgd: ring3 removed echosvc
pkgd: ring3 remove ok
svcwatch: restart echo -> pid 80
pkgd: ring3 installed echosvc-1.0.0 (1 file(s))
pkgd: ring3 install ok (echosvc)
pkgd: ring3 service online ok
pkgd: ring3 kill isolation ok (shell pid=7 alive, +11 ticks)
pkgd: fallback ring0 ok
pkgd: ring3 image present (system:/bin/pkgd)
pkgd: ring3 online (pid=82 port=64)
pkgd: svcwatch restart ok (pid=6 -> 82)
pkgd: ring0 mode verify ok
pkgd: selftest PASS
```

非 root 路径（第二个 shell 的 `--pkgcli`，经过内核分发器 → ring3，euid=1000）：

```text
pkgd: echosvc -> ok                 # verify
pkgd: removed helloapp
pkgd: echosvc-1.0.0.pkg -> policy   # alice 装 service 被 ring3 侧策略拒绝
```

## 8. 设计问题（未改契约，记录待评审）

1. **多映像信任锚缺失**：`user_mode.c` 的 `trust_verify_shell()` 只认 shell 的
   那一份签名，所以 ring3 pkgd 目前只能从内核镜像内嵌 ELF 启动（内核签名即
   信任根）。建议 ABI/安全会话增加“按路径/按名字的签名清单”，让 ring3 服务
   可以从 `system:/bin/*` 正常加载。本会话未改 `security/**`、
   `src/kernel/user/**`。
2. **SIP updater scope 是全局单槽**（`fs/sip.c` 的 depth+owner）：两个线程同时
   进入时后者的写会被前者的 owner 挡住。ring0 的 `pkg_install` 一直有同样
   限制；ring3 原语现在也用它。本会话用“同一次启动不重写 /bin/pkgd + 失败
   重试”规避了镜像写的竞争；真正的修法是 per-thread scope 或 capability 化。
3. **legacy 端口没有 owner 回收**：pkgd 每次重启都会重新
   `SERVICE_REGISTER("pkgd")`（可能 E_EXIST 旧条目），旧 port 槽不会自动释放；
   本会话只在兼容层使用它，新路径全部走 IPC + `SYS_SVC_*`。长期需要
   `SYS_PORT_CLOSE`/owner reaper（P1 claims 已记录同一问题）。
4. **转发上下文单槽**：内核分发器同一时刻只允许一个未完成请求（busy 等待上限
   2s），超时回退 ring0；并发 pkg CLI 会排队。没有为每个客户端保存请求槽。
5. **超时后的迟到回复**：`pkgd_forward` 10s 超时后释放回复端口，pkgd 之后的
   reply 会因句柄 generation 失效而失败，不会误投给下一个请求（但安装动作
   可能已经在 pkgd 侧完成——与 P1 `svc_call` 的迟到回复同族）。
6. **`restart=always` 仍与 `on-failure` 等价**：没有主动 stop API（沿用 P3
   claims）。pkgd 正常退出也会被当作失败重启。
7. **`pkgd=ring3` 强制模式不回退**：这是有意语义；验收只用默认 auto 与
   `pkgd=ring0` 两者。
8. **转发时客户端身份依赖内核记录**：pkgd 是 root 系统服务，原语只接受
   “已注册 pkgd pid”；euid 来自内核转发请求时写下的 `pkgd_ctx`，pkgd 不能
   自行声明别人的身份。若未来允许客户端直接 IPC 连 pkgd，需要消息里带
   不可伪造的 sender uid（`SYS_PORT_RECV` 目前不输出发送者身份）。

## 9. 遗留

1. 8h 浸泡未做（P4 只要求回归；按设计 P5 起统一浸泡）。
2. `-smp 4` 手工压力未做：分发器有 busy 串行化、注册表有自旋锁；并发
   install 压力留给服务化后续。
3. `pkgd_selftest()` 只在 `-Test` 构建运行；生产构建没有 `pkgd:` 自检行（与
   pkg/svc 的既有做法一致）。
4. 文档：`docs/api.md` 的 ABI 1.5 说明未补 `SYS_PKG` 原语（不在本会话范围）；
   §2 已是可直接引用的契约。
