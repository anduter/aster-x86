#ifndef APPS_HEARTBEAT_H
#define APPS_HEARTBEAT_H

/* 心跳程序：每秒打印一行，用来证明调度器在多个内核线程之间切换。
 * 它和 shell 一样，是程序表里的一个程序。 */
void heartbeat_entry(void);

#endif
