# 账号系统实施计划（Linux 路线，2026-10-26）

> 决策：**先按 Linux 式账号系统推进**；后续若改“平等账号/无 root”，在此文档上迭代。
> 触发：8h 浸泡（docs/soak-2026-10-25b.md，预计 02:47）PASS 后，自动按 P1 → P2 → P3 顺序执行。

## 目标形态

- 有 root（uid 0），可 su/sudo 提权；
- /etc/passwd、/etc/shadow（0600）、/etc/group；
- 系统服务 uid 1–99（不可登录），普通用户 uid 1000+；
- /home/<user> 0700，/root 0700；
- getty + login（PAM-lite：auth/account/session 三个钩子）；
- setuid/setgid 位、umask、目录 x 检查、
nosuid（FAT16）；
- sudo + wheel 组、失败锁定、who/last、审计；
- 测试模式：-Test 自动登录 root，系统可写；生产镜像要求登录。

## P1 身份（1–1.5 周）

**内核**
- proc_t 增加 euid/egid/suid/sgid、groups[]；
- setuid/seteuid/setgid/setegid/geteuid/getegid/getgroups/setgroups；
- 权限检查统一用 euid；root 保留 CAP_DAC_OVERRIDE；
- umask 系统调用。

**文件**
- system:/etc/passwd（0644）、/etc/shadow（0600）、/etc/group（0644）；
- system:/home/<user>（0700）、/root（0700）。

**用户态**
- getty（串口）、login（校验、建会话、setuid、exec shell）；
- -Test 自动登录 root（内核参数/构建开关）。

**验收**
- -Test 连续 2 次全 PASS（P1 起 112 项）；
- 多用户矩阵：A 读不到 B 的 0600 文件；普通用户 setuid(0) 被拒；fork/exec 凭据正确；
- 新增检查项（≥ 8 条）。

### P1 实施状态（2026-10-26）

| 项 | 状态 | 落地 |
| --- | --- | --- |
| proc_t 凭据（euid/egid/suid/sgid/groups[16]/umask/login_required） | 完成 | `proc/proc.h`、`proc/process.c` |
| fork/exec 继承凭据 | 完成 | `process.c` fork 整组复制；exec 不重置 |
| 新 syscall 64..72（SETEUID/SETEGID/SETGID/GETEUID/GETEGID/GETGROUPS/SETGROUPS/SETUMASK/LOGIN） | 完成 | `include/abi.h`、`security/security_syscall.c`，ABI minor 1.2 -> 1.3 |
| 判权统一用 euid + 附加组 + umask（`fs_cred_t`） | 完成 | `fs/fs.h`、`fs/ramfs.c`、`fs/vfs.c`、`fs/fs_syscall.c` |
| /etc/passwd|shadow|group + /home + /root + /tmp 幂等播种 | 完成 | `security/accounts.c`（`acct_init`） |
| getty（tty 标记 + exec shell）与 login 前端（提示/不回显/失败延迟） | 完成 | `apps/getty.c`、`user_shell.c::login_screen` |
| 测试模式自动登录 root（`autologin=root`） | 完成 | `core/bootargs.c` + `build.ps1` 的 `-DOS_TEST_BUILD=1` |
| 多用户检查项 | 完成（+14 条，98 -> 112） | `build.ps1` |
| -Test 连续 2 次全 PASS | 见提交信息 | — |

**提前落地到 P1 的 P2 基础**：目录 x 沿途检查、sticky 删除规则、chown 清 setuid、
`fs_cred_t.umask` 生效——它们和"判权统一到 cred"是同一处代码，放在 P1 一次改完，
P2 只补 exec 时的 setuid/setgid、FAT16 nosuid 与多用户矩阵的检查项。

细节与分工见 [accounts.md](accounts.md)。

## P2 文件边界（1 周）

- 目录 x 沿途检查（/a/b/c 每层都要 x）；
- setuid/setgid/sticky 位；chown 清 setuid；
- FAT16 挂载 
 nosuid，不信任宿主 mode；
- umask 生效、创建时权限正确。

**验收**：多用户矩阵 + -Test 连续 2 次全 PASS；非法路径穿越被拒。
### P2 实施状态（2026-10-26）

判权基础设施（目录 x 沿途检查、sticky、`fs_cred_t.umask`、chown 清 setuid）随 P1
的 `fs_cred_t` 改造一起落地（见上文）；P2 补完 exec 侧语义与可断言的检查项：

| 项 | 状态 | 落地 |
| --- | --- | --- |
| 目录 x 沿途检查（每层都要 x，缺 x -> E_ACCES） | 完成 | `fs/ramfs.c::walk_rel_ex` |
| setuid/setgid 位在 chmod 中保留、chown 清除 | 完成 | `fs/ramfs.c::fs_chmod_impl/fs_chown_impl` |
| sticky 目录删除规则（/tmp 01777） | 完成 | `fs/ramfs.c::may_unlink_in` |
| exec 时应用 setuid/setgid（04000/02000） | 完成 | `security/accounts.c::acct_exec_cred` + `user/user_mode.c` |
| FAT16 挂载 nosuid（不信任宿主 mode/uid） | 完成 | `acct_exec_cred` 里的 `fs_is_fat()` 分支 |
| umask 生效（`mode & ~umask`，setuid 位不受 umask 影响） | 完成 | `fs/ramfs.c::new_mode_umasked` |
| 多用户矩阵（0600 / sticky / 穿越 / setuid / nosuid / umask） | 完成（+10 条，112 -> 122） | `build.ps1`、`acct_selftest` 的 `file_matrix_selftest` |
| -Test 连续 2 次 122/122 | 通过 | 两轮 `build.ps1 -Test`（隔离目录），见提交信息 |

**FAT16 的 nosuid 边界**：FAT 后端的 `stat` 返回 `mode=0444 / uid=0`，本身没有
setuid 位；`acct_exec_cred()` 仍显式按挂载点判一次（`fs_is_fat()`），避免以后给
FAT 加上宿主 mode 透传时悄悄引入提权面。

## P3 提权与审计（1 周）

- sudo（wheel 组）+ su；sudoers-lite；
- passwd（普通用户要旧密码）、useradd/userdel/usermod（root）；
- who/last（utmp/wtmp）、登录失败计数与锁定（faillock 简化）；
- 审计：登录/提权/改权限/拒绝访问。

**验收**：-Test 连续 2 次全 PASS；失败 5 次锁定；审计行可断言；sudo 可用。
### P3 实施状态（2026-10-26）

| 项 | 状态 | 落地 |
| --- | --- | --- |
| `su`（要口令）/`sudo`（wheel 组，root 免口令） | 完成 | shell 命令 + `acct_su`/`acct_sudo`/`acct_sudo_end` |
| sudoers-lite：只认 wheel（无 sudoers 文件解析） | 完成 | `acct_is_sudoer()` / `acct_in_group(name, 10)` |
| `passwd`（普通用户必须旧口令；root 可改他人） | 完成 | `acct_passwd` + `LOGIN_PASSWD`/`LOGIN_PASSWD_USER` |
| `useradd`/`userdel`/`usermod`（root） | 完成 | `acct_useradd`/`acct_userdel`/`acct_usermod_add_group` |
| `who`（内核会话表）/`last`（审计环） | 完成 | `acct_who`/`acct_last` + `LOGIN_WHO`/`LOGIN_LAST` |
| 登录失败锁定（连续 5 次 -> 15s） | 完成 | `fail_bump`/`fail_locked`，锁定后连正确口令也拒 |
| 审计：登录/提权/改权限/拒绝访问 | 完成 | `acct_audit`（`audit: <event> <detail>` 串口行 + 环形缓冲，`sync` 刷进 `/var/log/audit.log`） |
| 检查项 | 完成（+9 条，122 -> 131） | `build.ps1` |
| -Test 连续 2 次 131/131 | 通过 | 两轮隔离目录 `build.ps1 -Test` |

**顺带修复（P3 期间发现）**：`FS_FILE_MAX` 从 32KB 提到 40KB 后内核 `.bss`
越过了 `HEAP_BASE`（8MB），堆的第一批块头落在内核数据上——表现为随机 oops +
内核栈 canary 被踩。`mm/heap.c` 现在在 `heap_init()` 里把 arena 起点抬到
`_kernel_end`（4KB 对齐）之后再建空闲链表，并打印
`heap: arena … (… KB usable)`；`heap_stats/heap_check/free` 一律用这对真实边界。
这一改动让"内核变大"不再需要手工调链接脚本。

命令行示例（生产镜像，登录后）：

```
osteachos login: alice
Password: ********
alice@osteachos:~$ id
uid=1000 euid=1000 gid=1000 egid=1000 groups=100 name=alice
alice@osteachos:~$ sudo cat /etc/shadow
sudo: euid=0
cat: permission denied or no such file          # 见下方限制：cat 走 shell 的 fd 判权
alice@osteachos:~$ su
Password: ********
su: now root (euid=0 uid=0)
```

## 每阶段门禁

1. tools/ci.ps1 -Quick（静态）；
2. -Test 连续 2 次 **全 PASS**（项数见 build.ps1：P1 112、P2 122、P3 131）；
3. check-deps 0 违规；
4. 路径限定提交，前缀 [accounts]；
5. 更新本文件与 docs/accounts.md。

## 兼容与回退

- 测试模式自动登录 root，保证原有 98 项不被登录流程打断（P1 起共 112/122/… 项）；
- 新增 syscall 只追加编号（当前 SYS_NR_MAX=63，从 64 起），bump ABI minor；
- 若 P2/P3 影响既有检查，先加测试模式旁路，不放宽生产策略。