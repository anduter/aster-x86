#ifndef APPS_SHELL_H
#define APPS_SHELL_H

/* shell 是一个“程序”：入口注册到程序表（prog_register），
 * 由内核作为 init 通过 prog_run("shell") 启动。
 * 它自己不做任何内核级初始化，只通过设备接口读键盘、
 * 通过 prog_run 执行别的程序。 */
void shell_entry(void);

#endif
