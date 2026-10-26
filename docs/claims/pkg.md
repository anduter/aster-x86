# pkg 会话交付声明（P0：组件 ELF entry + 用户态 pkg CLI）

- 会话：`pkg`
- 所有权路径：`src/kernel/pkg/**`、`src/kernel/user/**`、`src/kernel/include/pkg.h`、
  `src/kernel/core/misc_syscall.c`（只加 SYS_PKG(77) 分发）、`src/user/user_shell.c`（ring3 shell）、
  `docs/claims/pkg.md`
- 测试命令：

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name pkg -Test
  ```

- 未改 `build.ps1`；未改 `include/abi.h`（只使用已预留的 `SYS_PKG = 77`）；未新增系统调用号。

## 1. 期望断言行（串口，严格匹配）

集成会话把下面这些加进 `build.ps1` 检查项即可：

| # | 断言行 | 覆盖 |
| --- | --- | --- |
| 1 | `pkg: elf entry ok (echosvc entry=bin/echosvc)` | 安装时组件 ELF 落盘、验哈希+签名、ring3 exec 成功 |
| 2 | `pkg: elf entry verify ok (hash+signature)` | 已安装 ELF 的 SHA-256 + APKG/RSA 复验 |
| 3 | `pkg: elf entry service online ok` | 组件自己的服务（同名 `echosvc`）在线 |
| 4 | `pkg: elf entry tamper rejected (hash)` | 改一个字节被哈希校验拒绝 |
| 5 | `pkg: elf entry restore ok` | 恢复后校验重新通过 |
| 6 | `pkg: component echosvc running pid=<n> service=echosvc` | 组件进程与 pid 记录 |
| 7 | `pkg: component echosvc stopped (pid=<n> ok)` | 卸载时先停组件进程 |
| 8 | `pkg: user cli list ok` | `pkg list`（SYS_PKG op 0） |
| 9 | `pkg: user cli verify ok` | `pkg verify echosvc`（op 3） |
| 10 | `pkg: user cli install ok` | alice（ring3 fork + SYS_SETUID）`pkg remove/install/verify helloapp` |
| 11 | `pkg: user cli policy deny ok` | 普通用户装 service 被 policy 拒绝 |
| 12 | `pkg: user cli done (status=0)` | CLI 自检整体成功 |

失败时对应行会变成 `... FAILED` / `... FAIL`，不会静默通过。

原有 15 条 pkg 断言行保持兼容（`pkg: install echosvc-1.0.0 ok`、
`pkg: supervisor start echosvc ...`、`pkg: uninstall running service ok`、
`pkg: reinstall service online ok` 等），原因见 §3 的过渡设计。

## 2. SYS_PKG(77) op 编码

`int 0x81`，`ebx=op, ecx=a, edx=b`，返回值 `eax`：`0` 成功 / `-1` 失败。
`LIST` 返回组件条数（`>= 0`）；除 `LIST` 外的回复文本写进 `b` 指向的用户缓冲
（≤ `PKG_CLI_REPLY_MAX = 128` 字节，NUL 结尾，形如 `ok ...` / `err <code> <detail>`）。

| op | 名字 | a (ecx) | b (edx) | 返回 |
| --- | --- | --- | --- | --- |
| 0 | `PKG_OP_LIST` | 用户缓冲 | 缓冲长度（≤ 4096） | 组件条数 / -1；每行 `name version type` |
| 1 | `PKG_OP_INSTALL` | 包名或 `.pkg` 路径（用户串） | 回复缓冲 | 0 / -1 |
| 2 | `PKG_OP_REMOVE` | 组件名（用户串） | 回复缓冲 | 0 / -1 |
| 3 | `PKG_OP_VERIFY` | 组件名（用户串） | 回复缓冲 | 0 / -1 |
| 4 | `PKG_OP_COMPONENT_PORT` | port object id | — | 仅 `_pkgd` 启动的组件进程可调用 |
| 5 | `PKG_OP_COMPONENT_UNREG` | port object id | — | 仅 `_pkgd` 启动的组件进程可调用 |

`op 4/5` 是组件运行时的内部握手：组件进程通过 native `HANDLE_INFO` 拿到自己的
port `object_id`，注册后报告给 `_pkgd`，退出前让 `_pkgd` 从服务注册表注销，
避免留下指向已释放 port 的僵尸条目。身份取 `proc_current()->pid`，别的进程
冒充不了（不在 `_pkgd` 的运行表里直接 -1）。

### 权限

- 身份一律取 `proc_current()` 的 `euid`，不由命令行自述。
- `service` 需要 root（`pkg_euid_is_admin()`：`euid == 0`，等价 `CAP_PKG_ADMIN`；
  SIP-lite 的签名更新器作用域同样只给 root）。普通用户得到
  `err -5 service requires root/CAP_PKG_ADMIN`。
- 普通用户只能安装/卸载自己 `<home>/Apps` 下的 `type=app`，manifest 的
  `account=` 必须等于自己（原有策略不变）。
- 普通用户点名系统组件做 remove 时返回 `PKG_E_POLICY`（而不是含糊的 `noent`）。

## 3. 组件 ELF 加载路径（P0）

### 安装时

1. 验 APKG（结构/manifest/文件哈希/RSA 签名/requires/策略）——不变；
2. 写 staging 并逐文件复验哈希——不变；
3. **新增** `prepare_component_elf()`：
   - 包内如果自带 ELF（平铺文件名，包格式的文件名不允许 `/`），把它复制到
     `<stage>/bin/<name>`；
   - 包内没有 ELF 时，从构建期签名过的 `system:/bin/shell` **运行时装**复制一份
     到 `<stage>/bin/<name>`（过渡机制，见 §6 限制 1）；
   - 无论哪种来源，都算 SHA-256 写进组件 DB 的 `elf=` / `elfsha=` 字段；
4. staging → 正式目录原子 rename（不变）；
5. DB 追加一行，service 形如：

   ```
   name=echosvc version=1.0.0 type=service entry=bin/echosvc program=echosvc \
   euid=0 root=system:/System/Packages/echosvc/1.0.0 files=1 \
   elf=bin/echosvc elfsha=<64 hex>
   ```

   `entry` 已经是 ELF 相对路径；`program=` 是过渡期内建 echo 回退，见 §6；
6. `pkg_component_start()`：
   - 读 `<root>/bin/<name>`，SHA-256 必须等于 DB 的 `elfsha`（否则 `PKG_E_HASH`）；
   - 重验签名：内建包 → 重跑 `pkg_verify()`（APKG/RSA）；外部/过渡组件 →
     `trust_verify_shell()`（同一 RSA-2048 信任根）；
   - `user_component_exec()` 新建进程（程序 `comprun`），在**新进程**里加载 ELF、
     映射为用户页、构造 `argv[0] = "svc:<name>"` 的启动栈，`iret` 进 ring3；
   - 等组件注册同名服务（≤3s）；失败 SIGKILL + 返回错误；
7. **失败回滚**：ELF 启动失败 → 删正式目录 + 删 DB 行 + 清运行表，并返回
   `PKG_E_IO`；staging 阶段的失败只删 staging。

### 卸载时

1. `pkg_component_stop()`：向同名服务发 `stop`，等组件自己注销 port 并退出
   （≤2s），超时 `SIGKILL`；最后兜底 `service_unregister(port)`；
2. 再走原有 supervisor `stop:<program>`（内建 echo 回退）；
3. 删组件目录 + DB 行（不变）。

### ELF 运行时约定（组件侧）

`src/user/user_shell.c` 同时是"组件运行时"：`argv[0]` 以 `svc:` 开头时进入
`component_service_main()`，用 native ABI（`PORT_CREATE/SERVICE_REGISTER/
PORT_ACCEPT_BLOCK/CHANNEL_RECV_BLOCK/CHANNEL_SEND`）注册 `<name>` 服务并回显
消息；收到 `stop` 时先 `SYS_PKG(5)` 注销、再退出。普通 shell 路径完全不受影响。

## 4. 用户态 `pkg` CLI

ring3 shell 新增：

```
pkg install <file>     # 内建包名（xxx-1.0.0.pkg）或 system:/path/x.pkg
pkg list
pkg remove <name>
pkg verify <name>
```

全部经 `SYS_PKG` 到内核 `_pkgd`，内核侧实现在 `src/kernel/pkg/pkg_syscall.c`；
`core/misc_syscall.c` 只增加 `SYS_PKG` 的 owns/case，两处，不动其它 case。
`pkg install` 支持真实 `.pkg` 文件（`pkg_install_path()`：先查内建包目录，
再按路径读文件、走同一套 `pkg_verify`/`pkg_install`）。

CLI 自检由 `reexec` 传入 `--pkgcli` 触发（`build.ps1 -Test` 默认注入的命令里
本来就有 `reexec`），只在**第二个 shell** 跑一次，避免和 `pkgtest` 的安装/
卸载并发抢时序；正常启动/生产路径不执行安装动作。

## 5. 验证记录

- 构建：`tools\session-run.ps1 -Name pkg` → exit 0（kernel 358656 bytes）。
- **本会话完整回归**（提交 `15d15b0` 之后，共享树 HEAD `5a27b09`）：

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name pkg -QmpPort 4521 -Test
  # === TEST PASS (182/182) ===   elapsed=53.3s
  ```

  串口里 §1 的 12 行全部出现，另有原有 15 条 pkg 行：
  `pkg: install echosvc-1.0.0 ok`、`pkg: supervisor start echosvc ok`、
  `pkg: uninstall running service ok`、`pkg: reinstall service online ok` 等。
- 交叉证据：同一工作树上的 `fs` / `eng` / `net` / `mm` 会话 `-Test` 也各出现
  `=== TEST PASS (182/182) ===`，说明 77 分发与 ELF/argv 启动栈没有破坏原路径。
- 并行环境注记：`12345` 要同时看 **TCP 和 UDP**；直接跑 `build.ps1 -Test`
  的会话不受 `session-run` 锁保护，会抢端口/QMP。`pkg` 固定用
  `-QmpPort 4521` 避开默认 4511 与其他会话的 QMP 撞车。

## 6. 已知限制

1. **过渡：echosvc 的 ELF 来自签名 shell 运行时。** `build.ps1` 的 pkg spec
   被冻结（`Files='echosvc.conf'`、`Entry='echosvc'`），不能给包加 `bin/*`；
   安装器因此把构建期签名过的 `system:/bin/shell` 复制成
   `<root>/bin/echosvc`，并在 DB 里记录哈希。`entry` 已经是 ELF 路径、
   启动/校验/回滚都走 ELF 路径；等 build.ps1 允许包内带 ELF 时，把
   `prepare_component_elf()` 的 2) 分支删掉即可，1) 分支已经就绪。
2. **echosvc 的内建 echo 回退还在。** `supervisor start:<program>` 仍然被通知，
   以保持启动期 svcwatch/echo 的既有回归（182 项里的 echo/svcwatch 断言行）。
   组件 ELF 注册的是自己的服务名 `echosvc`，与内建 `echo` 不冲突。
3. 组件 ELF 目前要求 shell 运行时签到同一把 RSA 密钥（`trust_verify_shell`）；
   包内自带 ELF 的通用路径已经实现校验（APKG + 逐文件哈希），但还没有
   独立签名的第三方 ELF 信任链。
4. 重启后不会自动拉起已安装组件的 ELF（`_pkgd` 运行表在内存里）；与
   "组件 DB 落盘依赖显式 sync" 的既有行为一致。
5. 没有多版本升级/回滚；同名组件仍然只能装一个版本。

## 7. 环境注记（并行会话）

- `-Test` 的串行锁是 `.session/test.lock`。若锁记录里的 PID 已不存在
  （`Get-Process -Id <pid>` 失败），可以安全删除后重试；PID 还活着时等它释放，
  不要强删。
- 共享树里其它会话的未提交改动可能让与 pkg 无关的检查变红；本会话只对
  `pkg/**`、`src/kernel/user/**`、`src/kernel/include/pkg.h`、
  `src/kernel/core/misc_syscall.c`、`src/user/user_shell.c` 负责。
