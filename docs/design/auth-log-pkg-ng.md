# auth / log / pkg（L1 系统服务，WP2-5）

> 状态：D0 最小可用，2026-10-31。
> 范围：`src/services/auth`、`src/services/log`、`src/services/pkg`，以及
> `src/ng` 的 syscall 31..35 接线。不含签名/RSA（D1）、fsd 进程化、shell（WP2-6）。

## 1. 分层与依赖

```
L3/L2 用户态
   │  syscall 31..35（rax=号，rdi/rsi/rdx，负=errno）
   ▼
L0 src/ng/svc_syscall.c   用户指针 copyin/copyout（uaccess）
   │  直接函数调用（L1 库，D0 暂不进程化）
   ▼
L1 src/services/{auth,log,pkg}   只依赖 types.h / vfs.h / sha256.h
```

- L1 服务**不 include** `src/ng` 或 `src/kernel` 的内部头；用户指针只在
  `svc_syscall.c` 处理。
- 三个服务都是静态池/静态缓冲，不用 heap；单核协作式（IF=0 + 显式调度点），
  与 VFS/ramfs 的并发假设一致。
- `SHA-256` 实现在 `src/services/auth/sha256.c`，auth 与 pkg 共用（L1 内部
  原语；pkg 用相对路径 include，不依赖 auth 的登录语义）。

## 2. SHA-256

- FIPS 180-4，纯 C、流式 update、64 位位长、大端输出；hex 小写 64 字符。
- API：`sha256_init/update/final/sha256`、`sha256_hex`、`sha256_hex_parse/valid`。
- host 单测覆盖空串、`abc`、56/112 字节标准消息、1,000,000 个 `a`、分块
  update 与一次性结果一致、hex 大小写/长度边界。

## 3. auth

### 3.1 用户表与口令参数

| 用户 | uid | 初始口令 |
|---|---|---|
| root | 0 | `root` |
| alice | 1000 | `alice` |

- salt：16 字节，D0 由 `SHA-256("aster-ng-auth-salt:" + name)` 的前 16 字节
  派生（每用户不同、启动可重现；**不是**随机源，D1 换熵源）。
- 迭代哈希：`h0 = SHA-256(salt || pass)`，`h(i) = SHA-256(h(i-1) || salt)`，
  **4096 轮**；用户记录里存 `iterations`，便于以后升级参数。
- 比较是逐字节 OR 累积（不提前返回），减少时序差异；生产级常量时间仍属 D1。
- 不做明文存储；`/etc/passwd` 记录里同时带 salt/hash（D0 简化，D1 拆
  `/etc/shadow` 并加审计）。

### 3.2 `/etc/passwd` 格式

启动时 `auth_init()` 在 ramfs 写 `/etc/passwd`（0600，root 所有）：

```
name:uid:salt_hex(32):hash_hex(64):iterations\n
```

示例：

```
root:0:1f2e...:9a...:4096
alice:1000:3c...:d1...:4096
```

该文件是**快照/导出**；登录判定源是内存用户表（同一份 salt/hash）。后续
D1 可改成“从文件加载 + 原子更新”。

### 3.3 API 与错误码

```c
int auth_init(struct vfs_ctx *c);          /* 种用户 + 写 /etc/passwd */
int auth_login(const char *name, const char *pass); /* uid / -errno */
int auth_hash_password(const char *pass, const u8 salt[16], u32 iter,
                       u8 out[32]);
u32 auth_user_count(void);
const struct auth_user *auth_user_at(u32 i);
const struct auth_stats *auth_stat(void);
```

- 成功：uid（root 为 0）；未知用户 `-ENOENT(-2)`；口令错或已锁定
  `-EACCES(-13)`；参数非法/未初始化 `-EINVAL(-22)`。
- 连续失败 `AUTH_MAX_FAILS=5` 次锁定该用户；锁定后正确口令也返回 EACCES，
  只能通过重新 `auth_init()`（本 WP 没有时钟，不做超时解锁）。成功登录清零
  失败计数。
- 上限：8 用户、名字 31 字符、口令 63 字符；`/etc/passwd` 缓冲 512 字节。

## 4. log（64 KiB 环形缓冲）

- 容量 `LOG_CAPACITY=65536` 字节，**按整行**保存；行格式 `tag: msg\n`。
- tag 1..15 字符、msg 1..127 字符，只接受可打印 ASCII（0x20..0x7E），
  tag 不允许 `':'`；单行最长 `LOG_LINE_MAX=145`。
- 写入时如果 `used + line > 64 KiB`，从最旧整行开始丢弃；不会留下半行。
- 读取（`log_read(buf,max)`）返回**最近的**、能放进 `max` 的若干整行，
  按时间顺序；如果最新一行本身超过 `max`，返回该行末尾 `max` 字节。
- 读取是 O(used) 反向找行首；64 KiB 上限下开销可接受，未做行索引。
- API：

```c
int log_init(void);
int log_write(const char *tag, const char *msg); /* 字节数 / -errno */
u32 log_read(char *buf, u32 max);                /* 拷贝字节数 */
u32 log_bytes_used/line_count/dropped_lines(void);
u64 log_written_lines(void);
u32 log_capacity(void);
```

- 内核关键路径写入：`main.c` 启动行（`boot: ng: hello 64-bit`、paging）、
  每个 WP 的 PASS/FAIL（`selftest: WPx-y PASS`）、`trap.c` panic
  （`panic: unrecoverable exception`）。

## 5. pkg（最小包目录）

### 5.1 目录与种子

```
/System/Packages/<name>/<version>/manifest
/System/Packages/<name>/<version>/<entry>       # entry 来自清单，相对路径
```

`pkg_init()` 种入 `hello/1.0.0`：

- `bin/hello` 内容 `"hello from ng pkg\n"`；
- `manifest`：

```
name=hello
version=1.0.0
type=app
entry=bin/hello
sha256=<bin/hello 的 SHA-256 小写 hex>
```

### 5.2 manifest 解析规则

- 严格 `key=value`，允许 key/value 两侧空白、`\r\n`、空行、`#` 注释；
- 只接受 5 个字段：`name/version/type/entry/sha256`；未知字段、重复字段、
  缺字段全部 `-EINVAL`；
- `name`：1..31，`[a-z0-9._-]`，不以 `.` 开头；
- `version`：1..15，`[A-Za-z0-9._-]`，不以 `.` 开头；
- `type`：`app` / `service` / `lib`；
- `entry`：相对路径、1..63、无空组件、无 `.`/`..` 组件、不含 `\` 等字符；
- `sha256`：正好 64 hex，大小写均可。

### 5.3 列表与校验

```c
int pkg_init(struct vfs_ctx *c);
int pkg_list(char *buf, u32 max);   /* "name version\n" 若干行，返回包数 */
int pkg_verify(const char *name);   /* 0 / -ENOENT / -EINVAL / -EIO */
int pkg_manifest_parse(const char *text, u32 len, struct pkg_manifest *out);
```

- `pkg_list` 需要 `max` 容纳全部文本 + NUL，否则 `-ENOSPC`；最多 16 个包、
  每包最多 8 个版本（超出截断，D1 改动态）。
- `pkg_verify`：列版本目录 → 读 manifest → 解析/校验字段 → 要求清单
  `name/version` 与目录一致 → 对 `entry` 文件重算 SHA-256 → 比对；
  哈希不符 `-EIO(-5)`，清单/路径非法 `-EINVAL`，包不存在 `-ENOENT`。
- **不做签名/RSA**：verify 只证明“清单与文件自洽”，不证明发布者可信；
  D1 在 manifest 增加 `signature=`/公钥链并走签名通道。

## 6. syscall 31..35

| 号 | 名称 | 参数（rdi/rsi/rdx） | 返回 |
|---|---|---|---|
| 31 | login | name, pass | uid / -ENOENT / -EACCES / -EINVAL / -EFAULT |
| 32 | dmesg | buf, max（max ≤ 4096） | 字节数 / -EINVAL / -EFAULT |
| 33 | log_write | tag, msg | 0 / -EINVAL / -EFAULT |
| 34 | pkg_list | buf, max（max ≤ 512） | 包数 / -ENOSPC / -EINVAL / -EFAULT |
| 35 | pkg_verify | name | 0 / -ENOENT / -EINVAL / -EIO / -EFAULT |

- 用户字符串经 `copyin_str`，缓冲经 `copyin/copyout`；坏指针返回 `-EFAULT`，
  不触发 `#PF`（SMAP 下走 stac/clac）。
- 内核自检把一页 PMM 以 `U=1` 临时映射到 `0x60000000`，用真实用户 VA
  跑通 31..35 的成功路径；ring3 demo（`hello.S`）再在内核 ELF64 进程里跑
  同一组调用。

## 7. 内核接线与顺序

`main.c`：

```
serial_init -> log_init（最早）-> ... -> fs_selftest（WP2-1，ramfs 就绪）
-> wp25_selftest（auth 种 /etc/passwd、pkg 种 /System/Packages、31..35）
-> ... -> elf_selftest（ring3 svc demo）
```

- 每个 WP 汇总后 `log_wp("WPx-y", fails)` 写 `selftest: WPx-y PASS/FAIL`。
- `trap.c` panic 先写 `panic: unrecoverable exception` 再打印 PANIC 行。
- auth/pkg 的 VFS 上下文复用 `fs_sys_ctx()`（与 WP2-1 同一 ramfs）。

## 8. 当前限制（D0）

1. 单 AS、单核协作式，无锁；多核/抢占时加 spinlock，API 不变。
2. 无随机数：salt 可重现；口令常量时间比较是“近似”，D1 换正式实现。
3. `/etc/passwd` 与内存表是两份数据（只为种子/导出）；D1 做加载/原子更新、
   拆 shadow、加审计。
4. pkg 无签名/依赖解析/安装事务/网络仓库；`pkg_list` 静态上限 16 包。
5. log 无行索引、无优先级/限速、无持久化；D1 由 logd 进程接管。
6. auth/log/pkg 仍以内核内 L1 库形式链接；port+cap 的服务进程化是后续 WP。
