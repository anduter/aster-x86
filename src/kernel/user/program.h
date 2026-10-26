#ifndef CORE_PROGRAM_H
#define CORE_PROGRAM_H

#include "../include/types.h"

#define PROG_NAME_MAX 24
#define PROG_MAX      24

/* 程序标志 */
#define PROG_F_PIN_AP   0x1U    /* 亲和到 AP（用于验证 AP 调度/抢占的探针程序） */

/* 一个“程序”就是只读映像 + 元数据。当前阶段程序由内核内建（entry 是函数），
 * 后续会换成“程序存储区里的映像 + 入口偏移”，但对外语义不变：
 * 程序必须先在程序表里注册，才能被 proc_spawn 启动。 */
typedef struct {
    char        name[PROG_NAME_MAX];
    uint32_t    version;
    uint32_t    flags;
    const char *desc;
    void      (*entry)(void);
} program_t;

/* 初始化并注册内建程序 */
void prog_init(void);

/* 注册一个程序；重名返回 E_EXIST，表满返回 E_FULL */
int prog_register(const char *name, uint32_t version, const char *desc,
                  void (*entry)(void));

const program_t *prog_lookup(const char *name);
const program_t *prog_at(uint32_t index);
uint32_t         prog_count(void);

/* 设置程序标志（注册后调用）；成功返回 0 */
int prog_set_flags(const char *name, uint32_t flags);

/* 按名字取一个 OBJ_PROGRAM 句柄；失败返回 0 */
uint32_t prog_open(const char *name);

/* 运行程序（当前直接调用其入口，P1 起改为创建进程） */
int prog_run(const char *name);

#endif
