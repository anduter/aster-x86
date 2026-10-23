#ifndef CORE_KSYM_H
#define CORE_KSYM_H

/* 内核符号表：构建时从 kernel ELF 生成，用于 oops 把地址翻成“函数+偏移” */

typedef struct {
    unsigned int addr;
    const char  *name;
} ksym_t;

extern const ksym_t   ksym_table[];
extern const unsigned int ksym_count;

/* 查找 addr 落在哪个函数里；返回符号名（找不到返回 0），offset 为偏移 */
const char *ksym_lookup(unsigned int addr, unsigned int *offset);

#endif
