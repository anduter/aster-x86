#ifndef CORE_SUPERVISOR_H
#define CORE_SUPERVISOR_H

/* 启动内建 supervisor 服务：创建 "supervisor" port 并启动服务线程。 */
void supervisor_init(void);

/* 由程序表调用的服务循环（kernel-safe，阻塞 accept）。 */
void supervisor_entry(void);

#endif
