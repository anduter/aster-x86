#ifndef APPS_GETTY_H
#define APPS_GETTY_H

/* getty（内核内建程序）：控制台 tty0 的登录会话管理 + 重启。
 *
 * 生产启动路径：bootseq -> proc_spawn("getty") -> 父 getty 循环：
 *   spawn 子 getty -> 子进程置 login_required -> user_exec("/bin/shell")
 *   -> shell 的 login 前端（用户态）提示 login:/Password:，通过 SYS_LOGIN
 *      让内核校验口令并落地凭据 -> shell 退出后父 getty 重新拉起。
 * 也就是说 `exit` 之后 tty0 回到 login: 提示，而不是让控制台转 idle。
 *
 * 测试模式（-Test，内核参数 autologin=root）不经过这里：bootseq 直接
 * proc_spawn("shell")，行为与引入账号系统之前完全一致（回归不受影响）。
 *
 * 为什么 getty 是内核程序而不是 /bin/getty：当前镜像只有一份签名过的用户态
 * ELF（/bin/shell），新增用户态二进制要再走一遍签名/嵌入/验签链路；getty 的
 * 全部工作只是"置标志 + exec"，放在核内不扩大攻击面。登录前端（提示、读键盘、
 * 失败延迟）仍然在用户态 shell 里，见 docs/accounts.md 的分工说明。 */
void getty_entry(void);

#endif
