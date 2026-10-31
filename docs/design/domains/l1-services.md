# L1 系统服务（plane=l1）

> 状态：WP0-2 域文档补全，2026-10-31；描述 ng HEAD `c4efabc` 的已提交实现。
> 上位：`../architecture.md` §2.2/§3、ADR-004/005/007。
> 证据：`../../claims/ng-WP2-1.md`、`ng-WP2-2.md`、`ng-WP2-5.md`。
> 注意：**L1 当前是内核内的库，不是独立进程**；本文把“设计接口”和“当前形态”分开写。

## 职责

| 服务 | 当前内容 | 代码 | 状态 |
|---|---|---|---|
| VFS / ramfs | 挂载、路径规范化、节点/fd/权限、符号链接、fd 代际 | `src/services/fs/{vfs,ramfs}.c` | 已移植（WP2-1） |
| FAT16 | 第二个 VFS 后端：BPB 校验、FAT 缓存、簇链、8.3 目录项、读写/创建/删除/改名 | `src/services/fs/fat16.c` | 已移植（WP2-2）；挂 `/mnt/fat`（RAM 盘）与 `/mnt/disk`（ATA） |
| auth | root/alice、16 B salt + SHA-256 4096 轮、`/etc/passwd`、5 次失败锁定 | `src/services/auth/**` | 已移植（WP2-5） |
| log | 64 KiB 按行环形缓冲、`log_write/log_read`、panic/启动/WP 汇总写入 | `src/services/log/**` | 已移植（WP2-5） |
| pkg | `/System/Packages/<name>/<version>/manifest`、SHA-256 校验、列表 | `src/services/pkg/**` | 已移植（WP2-5）；无签名 |
| 命名空间/挂载 | VFS 挂载表（8 个挂载点）、路径前缀选择 | `src/services/fs/vfs.c` | 部分（无 per-process namespace） |
| 网络协议栈 | — | — | **未移植**；旧树有，ng 未做 |
| fsd / netd / logd / authd / pkgd 进程 | — | — | **未做**；D0 允许 VFS 暂留内核 |
| 设备管理、时间/locale | — | — | 设计条目，未实现 |

## 不做什么

1. **不碰硬件**：块设备/输入/显示在 L1D；L1 只通过 `blk_env`（FAT16）或内核注入的
   回调取数据。
2. **不做桌面/应用生命周期**：shell/WM/session manager 归 L2；L1 不解析角色令牌。
3. **不在内核实现 POSIX**：`libc/POSIX Profile` 是 A 域 D1 的用户态 shim（ADR-005）。
4. **不做签名/RSA/更新事务**：pkg 只做“清单与文件自洽”的 SHA-256 校验；签名在 D1/B。
5. **不按进程隔离 fd/cwd**：`vfs_ctx` 仍全局一份；每进程 fd 表属 fsd 化之后。
6. **不提供独立重启域**：当前 L1 库与内核同生共死；没有端口/健康检查/重启器。

## 接口（syscall / IPC / ABI）

### syscall 20–27（VFS）

| 号 | 签名 | 返回 / 限制 |
|---|---|---|
| 20 | `open(path, flags, mode)` | fd（`(gen<<16)\|slot`，每 AS 16 个）；路径缓冲 256 B（含 NUL；当前实现拒绝长度 ≥255，实际可用 ≤254 字符）；flags 见下 |
| 21 | `read(fd, buf, len)` | 字节数；`len` 1..512；目录 → `EISDIR` |
| 22 | `write(fd, buf, len)` | 字节数；`len` 1..512；`O_APPEND` 每次写前定位文件尾 |
| 23 | `close(fd)` | 0；坏 fd/旧 generation → `EBADF` |
| 24 | `stat(path, &vfs_stat)` | 0；`vfs_stat` 24 B：`{u32 type; u32 mode; u64 size; u64 ino}` |
| 25 | `listdir(path, ents, max)` | 条数；`max` 1..8（超 8 截到 8，0 → `EFAULT`）；`vfs_dirent` 80 B |
| 26 | `mkdir(path, mode)` | 0；已存在 `EEXIST`、父目录缺失 `ENOENT` |
| 27 | `unlink(path)` | 0；目录 `EISDIR`、打开中 FAT 文件 `EBUSY` |

`flags`（`VFS_O_*`）：`RDONLY=0`、`WRONLY=1`、`RDWR=2`、`CREAT=0x40`、`EXCL=0x80`、
`TRUNC=0x200`、`APPEND=0x400`、`DIRECTORY=0x800`；`O_FLAGS_MASK=0xEC3`，其余位拒绝。
`mode` 用 `VFS_S_IF*` + 9 位权限（ramfs/FAT16 只实现 owner 位检查）。

路径语义：先词法规范化（`//`、`.`、`..`、尾 `/`），再按最长挂载前缀 + 组件边界选
挂载点；符号链接深度 >8 → `ELOOP`；路径缓冲 256 B（含 NUL；当前 wrapper 拒绝长度
≥255，因此有效路径 ≤254 字符），组件 ≤63 字符 → 否则 `ENAMETOOLONG`。

### syscall 31–35（auth/log/pkg）

| 号 | 签名 | 返回 / 限制 |
|---|---|---|
| 31 | `login(name, pass)` | uid（root=0、alice=1000）；未知用户 `ENOENT`、口令错/锁定 `EACCES`；名字/口令缓冲 32/64 B（wrapper 拒绝长度 ≥31/63，实际 ≤30/62） |
| 32 | `dmesg(buf, max)` | 字节数；`max` ≤4096 |
| 33 | `log_write(tag, msg)` | 0；服务层 tag 1..15、msg 1..127 可打印 ASCII；syscall wrapper 拒绝长度 ≥15/127（实际 ≤14/126）→ `ENAMETOOLONG`；内容非法/空 → `EINVAL`（D10） |
| 34 | `pkg_list(buf, max)` | 包数；`max` ≤512；放不下 `ENOSPC` |
| 35 | `pkg_verify(name)` | 0；名字缓冲 32 B（wrapper 实际 ≤30）；包不存在 `ENOENT`、清单/路径非法 `EINVAL`、哈希不符 `EIO` |

用户指针全部经 `src/ng/uaccess.c`：`copyin_str` 截断/坏指针 → `EFAULT`（路径超长会
变成 `ENAMETOOLONG`），SMAP=on 时 `stac/clac` 包住拷贝。

### 包 manifest（A 域格式，L1 解析）

严格 `key=value`，只接受 5 个字段：

```text
name=hello            # [a-z0-9._-]，1..31，不以 . 开头
version=1.0.0         # [A-Za-z0-9._-]，1..15
type=app              # app | service | lib
entry=bin/hello       # 相对路径，1..63，无 . / .. 组件
sha256=<64 hex>       # entry 文件的 SHA-256
```

未知/重复/缺失字段都 `EINVAL`；**`plane/criticality/fallback/health` 目前会被拒绝**（D04）。
包目录：`/System/Packages/<name>/<version>/{manifest,<entry>}`；最多 16 包、8 版本。

### IPC / ABI 边界

- L1 当前**没有端口**：调用是直接函数调用（`src/ng/*_syscall.c` → L1 库）。
  设计上的 `port + cap` 客户端接口属于 A 域，fsd 化后才有。
- 没有 capability 传递；`vfs_ctx.uid` 仍是 0（root），权限检查被绕过（D14）。
- ABI 结构体/错误码的权威表在 [`../../abi/abi-1.0-draft.md`](../../abi/abi-1.0-draft.md)。

## 失败等级与恢复

| 设计 | 目标行为 | 当前行为 | 差距 |
|---|---|---|---|
| `core` | fsd/netd/authd 快速重启 + 客户端降级（离线/只读/缓存） | 库链接进内核；一个空指针/断言可 panic 全系统 | **实际 fatal**（D05） |
| `core` | blkd 掉线时 `/mnt/disk` 返回 `EIO`，RAM 盘/FAT 仍可用 | ATA 超时/错误返回 `EIO/ETIMEDOUT`，不 panic；但设备不可热重启 | 部分 |
| `optional` | logd 崩了不影响文件/登录 | log 是内核内环；写失败只返回错误 | 无独立重启 |
| 数据损坏 | 拒绝并报错，不扩散 | FAT16 校验 BPB/簇链/越界；VFS 负 errno；ramfs 上限回滚 | 已实现 |

恢复策略现状：**没有 supervisor/svcwatch 的 ring3 版本**；错误通过负 errno 返回给
调用方（shell/自检），由调用方决定降级。长跑会话的只读/缓存降级要等 fsd。

## manifest 五元组

设计值（当前无解析器，D04）：

| 组件 | plane | criticality | fallback | health | restart |
|---|---|---|---|---|---|
| `fsd`（未来进程） | `l1` | `core` | `kernel-vfs`（D0 过渡） | `heartbeat:5s` | `on-failure`，3 次 |
| `authd` | `l1` | `core` | `deny-login`（缓存凭据只读） | `heartbeat:10s` | `on-failure`，3 次 |
| `logd` | `l1` | `optional` | `serial-ring` | `heartbeat:10s` | `on-failure`，3 次 |
| `pkgd` | `l1` | `core` | `read-only-manifests` | `heartbeat:30s` | `on-failure`，3 次 |
| `netd`（未实现） | `l1` | `core` | `no-network` | `heartbeat:5s` | `on-failure`，3 次 |
| `timed/locale`（未实现） | `l1` | `optional` | `utc-default` | `none` | `on-failure` |

规则：`optional` 不得被 `core` 同步依赖；`fsd` 的 fallback 只能是 D0 的内核 VFS，
fsd 真正落地后改为“只读缓存/上次快照”。当前 VFS 就在内核里，所以 `kernel-vfs`
不是降级目标而是**现状**，这也是 D05 的一部分。

## 当前实现状态（ng WP）

| WP | 内容 | 证据 |
|---|---|---|
| WP2-1 | VFS 核心 + ramfs + syscall 20..27 + ring3 fs demo | `../../claims/ng-WP2-1.md` |
| WP2-2 | FAT16 后端、`blk_env`、1.44 MB RAM 盘、8 MB 磁盘镜像 | `../../claims/ng-WP2-2.md` |
| WP2-4 | ATA PIO 经 `blk_env` 挂 `/mnt/disk`（L1D 驱动 + L1 VFS） | `../../claims/ng-WP2-4.md` |
| WP2-5 | auth（salt+SHA-256 4096 轮）、64 KiB log 环、pkg 清单/校验、31..35 | `../../claims/ng-WP2-5.md` |
| WP2-7 | parity 100 项覆盖 vfs 20、ramfs 15、fat16 20、auth 10、log 5、pkg 10 | `../../claims/ng-WP2-7.md` |

实现细节：ramfs 32 节点 / 64 目录项 / 512 数据页（2 MiB）/ 单文件 1 MiB；
FAT16 无 LFN、无 rmdir、无跨目录 rename、打开中 unlink `EBUSY`；
auth 的 salt 由 SHA-256 派生（非随机）、比较是逐字节 OR（非严格常量时间）；
pkg 无签名/依赖/事务。详见 `../vfs-ng.md`、`../fat16-ng.md`、`../auth-log-pkg-ng.md`。

## 待办

1. **fsd 进程化**：port + cap 客户端、独立 uid、每进程 fd/cwd；把“core 可重启”变成真的。
2. **manifest v2**：接受 `plane/criticality/fallback/health`，与 A 域包格式一起定（D04）。
3. **每个进程的 `vfs_ctx`**：当前全局一份；`proc->cwd` 只是占位（D14）。
4. **网络**：D1 的 TCP/UDP/DNS/TLS；旧树 net 作为 oracle。
5. **auth 强化**：随机 salt/熵源、常量时间比较、`/etc/shadow`、会话/uid 生效（D14）。
6. **pkg 签名**：manifest `signature=`/公钥链、安装事务、卸载、版本选择。
7. **log 服务化**：logd、优先级/限速/持久化；先修 `LOG_ENAMETOOLONG` 与行为的差异（D10）。
8. **长跑降级**：fsd 重启后的只读缓存/重连语义（S2 相关）。
