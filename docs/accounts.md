# 账号系统实现说明（Linux 路线）

> 决策：先按 Linux 式账号系统推进（root + 系统服务 uid + 普通用户 1000+，
> `/etc/passwd|shadow|group`，getty+login，euid/egid/附加组，sudo+wheel）。
> 计划与阶段进度见 [accounts-plan.md](accounts-plan.md)。

## 1. 分工：内核校验，用户态前端（"PAM-lite"）

| 层 | 负责 | 代码 |
| --- | --- | --- |
| 内核 | 账户数据库解析、口令校验（sha256 + 恒定时间比较）、失败锁定、凭据落地（ruid/euid/suid + 附加组）、会话表、审计环 | `security/accounts.c` |
| 内核 | 新 syscall：`SETEUID/SETEGID/SETGID/GETEUID/GETEGID/GETGROUPS/SETGROUPS/SETUMASK/LOGIN` | `security/security_syscall.c` |
| 内核 | tty 入口：`getty` 置 `login_required` 并 exec `/bin/shell` | `apps/getty.c` |
| 用户态 | 登录前端：提示 `login:`/`Password:`、口令不回显、失败延迟、重试上限 | `src/user/user_shell.c` 的 `login_screen()` |
| 用户态 | `id` / `umask` / `whoami`（P1）、`su` / `sudo` / `passwd` / `who` / `last`（P3） | `src/user/user_shell.c` |

**为什么口令校验放核内**：`/etc/shadow` 是 0600 root，普通用户读不到；口令
hash 与比较都在核内完成，用户态只拿得到"通过/失败"。这样即使 shell 被替换，
也拿不到 shadow 内容。代价是 `SYS_LOGIN` 成了一个认证 oracle——它带有失败计数
和锁定，且只允许自己的 tty 使用（与 Linux 的 PAM 调用面等价）。

**为什么 getty 是内核程序**：当前镜像只有一份签名过的用户态 ELF（`/bin/shell`），
新增用户态二进制要再走一遍签名/嵌入/验签链路。getty 的全部工作只是"置标志 +
exec"，放核内不扩大攻击面；登录交互仍然在用户态。

## 2. 凭据模型

`proc_t`（`proc/proc.h`）在 ABI v1.3 增加：

```c
uint32_t uid, gid;        /* 真实身份（ruid/rgid） */
uint32_t euid, egid;      /* 生效身份：所有权限判定用它 */
uint32_t suid, sgid;      /* 保存身份：非 root 可在 ruid<->suid 之间切换 */
uint8_t  ngroups; uint32_t groups[16];   /* 附加组 */
uint32_t umask;           /* 创建文件/目录的权限掩码，默认 022 */
bool     login_required;  /* getty 置位：本 tty 必须先登录 */
bool     is_login;        /* 会话已建立（who/last 用） */
```

- `fork` 整组复制（含附加组、umask）；`exec` 不重置；
- `SYS_SETUID(45)` 保留旧语义（root 才能调用，四个字段一起落）；
- 非 root 的 `seteuid/setegid` 只能在 `{ruid,suid}` / `{rgid,sgid}` 之间切换；
- root 判定一律是 `euid == 0`（对应 CAP_DAC_OVERRIDE），不再是 `uid == 0`。

## 3. 权限判定（fs 层）

`fs/fs.h` 新增 `fs_cred_t { euid, egid, ngroups, groups[16], umask }` 与
`fs_*_cred()` 系列入口；`fs/ramfs.c` 的判权只有一处：

```
euid == n->uid              -> user  位
egid 或任一附加组 == n->gid -> group 位
否则                        -> other 位
euid == 0                   -> 直接放行（root）
```

- 路径解析 `walk_rel_ex()` **逐段检查 x**（访问 `/a/b/c` 时 a、b 每层都要 x），
  中间目录缺 x 返回 `E_ACCES`——这是 chroot/路径穿越防护的基础；
- `mkdir/unlink/rename` 要求父目录 `w+x`；`sticky` 目录（`/tmp` 01777）里
  只有文件属主、目录属主或 root 能删除/改名；
- `chmod` 保留 04000/02000/01000（只有 root 能设 setuid/setgid 位）；
- `chown` 清 setuid/setgid 位；
- `exec` 时按文件的 04000/02000 把 euid/egid 提为属主/属组（`acct_exec_cred`）；FAT16 是
  宿主提供的卷，按 **nosuid** 挂载处理，整卷忽略 setuid 位。

## 4. /etc 文件与默认账户（教学镜像）

```
/etc/passwd  0644 root:root   root(0) daemon(1) alice(1000) bob(1001)
/etc/shadow  0600 root:root   name:sha256(hex):0:0:99999:7:::
/etc/group   0644 root:root   root(0) wheel(10) daemon(1) users(100) alice(1000) bob(1001)
/home/alice  0700 alice:alice
/home/bob    0700 bob:bob
/root        0700 root:root
/tmp         1777 root:root
```

默认口令（仅教学镜像，**生产镜像必须改掉**）：`root`/`root`、`alice`/`alice`、
`bob`/`bob`。`uid 1..99` 是系统服务账户（`daemon`），`login` 拒绝它们登录。

启动时 `acct_init()` 幂等播种：文件不存在才写，存在则只纠正属主/权限
（磁盘上的 `/etc` 不会被每次启动覆盖）。

## 5. 测试模式 vs 生产

| | 测试模式（`-Test` / `-Shot` 构建） | 生产构建 |
| --- | --- | --- |
| 内核参数 | `autologin=root console=tty0`（编译期注入） | 空 |
| 启动入口 | `proc_spawn("shell")`（与引入账号前完全一致） | `proc_spawn("getty")` -> exec `/bin/shell` -> 登录 |
| 权限 | shell 以 root 运行，既有 98 项回归不受影响 | 必须登录，普通用户受 0700/0600 与目录 x 约束 |

实现：`core/bootargs.c` 提供 `boot_cmdline()/boot_cmdline_autologin_root()`；
`build.ps1` 在 `-Test`/`-Shot` 时追加 `-DOS_TEST_BUILD=1`。

**安全边界**：`autologin=root` 会跳过认证，因此只允许出现在开发/回归构建里；
生产构建的 cmdline 为空，`getty` 必然要求登录。别把 `-Test` 构建的镜像当产品发。

## 6. 审计与锁定

- 审计事件：`LOGIN_OK/LOGIN_FAIL/LOGIN_LOCK/LOGIN_DENY/LOGOUT/SU_OK/SUDO_OK/
  SUDO_DENY/SUDO_FAIL/PASSWD_OK/PASSWD_FAIL/CHMOD/CHOWN/DENY`；
- 每条同时写串口行 `audit: <event> <detail>`（回归断言用）和内核环形缓冲
  （`last` 读它）；`sync` 时整文件重写到 `/var/log/audit.log`；
- 审计函数**不碰文件系统**（避免 fs 锁重入），刷盘由 syscall 层显式调用；
- 失败锁定：同一账户连续 5 次失败锁定 15s（`ACCT_FAIL_MAX`/`ACCT_LOCK_TICKS`），
  锁定期间返回 `-2`，用户态打印提示并 `SYS_SLEEP 15000`。

## 7. ABI

- `OS_ABI_VERSION_MINOR` 1.2 -> 1.3；新调用号 64..72，`SYS_NR_MAX = 72`；
- 旧调用号/结构体布局未变；`ulib.h` 同步了全部包装（`geteuid()/login_auth()` 等）；
- 沙箱进程（`userdemo`）默认拒绝 64+ 调用：allowlist 位图只有低 64 位。

## 8. 已知限制

1. `-Test` 构建等于 root 全权，不能代表生产权限边界；
2. 只有一份用户态 ELF（`/bin/shell`），`getty`/`login` 前端因此合并在 shell 里；
3. `getty` 只服务控制台 tty0，没有多 tty/串口会话；
4. 口令 hash 是单次 sha256（无 salt/迭代），教学用途，抗离线爆破能力弱；
5. 用户态写 `/etc/passwd|group`（`useradd`）依赖 root，格式由用户态拼；
6. `who` 的会话表在内核内存里，重启丢失（`last` 从审计环读）。
