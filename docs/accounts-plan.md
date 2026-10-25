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
osuid（FAT16）；
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
- -Test 连续 2 次 98/98；
- 多用户矩阵：A 读不到 B 的 0600 文件；普通用户 setuid(0) 被拒；fork/exec 凭据正确；
- 新增检查项（≥ 8 条）。

## P2 文件边界（1 周）

- 目录 x 沿途检查（/a/b/c 每层都要 x）；
- setuid/setgid/sticky 位；chown 清 setuid；
- FAT16 挂载 
osuid，不信任宿主 mode；
- umask 生效、创建时权限正确。

**验收**：多用户矩阵 + -Test 2×98/98；非法路径穿越被拒。

## P3 提权与审计（1 周）

- sudo（wheel 组）+ su；sudoers-lite；
- passwd（普通用户要旧密码）、useradd/userdel/usermod（root）；
- who/last（utmp/wtmp）、登录失败计数与锁定（aillock 简化）；
- 审计：登录/提权/改权限/拒绝访问。

**验收**：-Test 2×98/98；失败 5 次锁定；审计行可断言；sudo 可用。

## 每阶段门禁

1. 	ools/ci.ps1 -Quick（静态）；
2. -Test 连续 2 次 **98/98**；
3. check-deps 0 违规；
4. 路径限定提交，前缀 [accounts]；
5. 更新本文件与 docs/accounts.md。

## 兼容与回退

- 测试模式自动登录 root，保证现有 98 项不被登录流程打断；
- 新增 syscall 只追加编号（当前 SYS_NR_MAX=63，从 64 起），bump ABI minor；
- 若 P2/P3 影响既有检查，先加测试模式旁路，不放宽生产策略。