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

## P3 提权与审计（1 周）

- sudo（wheel 组）+ su；sudoers-lite；
- passwd（普通用户要旧密码）、useradd/userdel/usermod（root）；
- who/last（utmp/wtmp）、登录失败计数与锁定（faillock 简化）；
- 审计：登录/提权/改权限/拒绝访问。

**验收**：-Test 连续 2 次全 PASS；失败 5 次锁定；审计行可断言；sudo 可用。

## 每阶段门禁

1. tools/ci.ps1 -Quick（静态）；
2. -Test 连续 2 次 **全 PASS**（项数见 build.ps1：P1 起 112）；
3. check-deps 0 违规；
4. 路径限定提交，前缀 [accounts]；
5. 更新本文件与 docs/accounts.md。

## 兼容与回退

- 测试模式自动登录 root，保证现有 98 项不被登录流程打断；
- 新增 syscall 只追加编号（当前 SYS_NR_MAX=63，从 64 起），bump ABI minor；
- 若 P2/P3 影响既有检查，先加测试模式旁路，不放宽生产策略。