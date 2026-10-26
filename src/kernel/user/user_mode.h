#ifndef USER_MODE_H
#define USER_MODE_H

/* 把内核内建的示例映像放进用户页并以 ring3 执行（不返回） */
int user_mode_run(void);
int user_probe_run(void);
int user_fsd_run(void);
int user_blkd_run(void);

/* 从文件系统加载一个程序映像，放进用户页并以 ring3 执行（不返回）。
 * path 形如 "/bin/shell"；映像是裸二进制（链接地址 0x40000000）。 */
int user_exec(const char *path);

/* 同 user_exec，但把 args（空格分隔）放到启动栈上作为 argv。
 * 组件 ELF 用 "svc:<name>" 作为 argv[0] 进入服务模式。 */
int user_exec_args(const char *path, const char *args);

/* 组件 ELF 入口（以 "comprun" 程序注册）：由 user_component_exec 触发。 */
void user_component_entry(void);

/* 在独立进程里加载并执行组件 ELF（_pkgd 用）。
 * 成功返回新进程 pid（已进入 ring3，且 services 由组件自己注册）；
 * 失败返回 -1。父进程会等子进程真正进入用户态后才返回。 */
int user_component_exec(const char *path, const char *args);

/* 用文件系统里的映像替换当前进程（exec）：
 * 换掉用户页，并把系统调用返回现场改成“回到新程序入口”。
 * args 是用空格分隔的参数串（可为 0），内核会在用户栈上构造 argc/argv。 */
struct interrupt_frame;
int user_exec_replace(const char *path, const char *args, struct interrupt_frame *f);

/* 用户栈按需增长：如果缺页地址在栈下方允许范围内，映射新页并返回 true */
#include "../include/types.h"
bool user_try_grow_stack(uint32_t fault_addr);

#endif
