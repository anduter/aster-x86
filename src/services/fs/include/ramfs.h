/*
 * ng WP2-1: ramfs（内存文件系统，L1 服务域）。
 *
 * 结构（全部静态池，不用 heap，失败可预测）：
 *   - 节点池：RAMFS_MAX_NODES 个 ramfs_node，节点 0 是根（永远存在）；
 *   - 目录项池：RAMFS_MAX_ENTS 条 {parent, node, name}，按 parent 线性扫描；
 *     改名只动目录项，删文件先摘目录项、再等最后一个 open 关闭才回收节点；
 *   - 数据页池：RAMFS_MAX_DATA_PAGES 个 4 KiB 页，每个文件用 **页链**
 *     （first_page / next，索引+1 为有效值，0 表示空），不浪费页指针数组；
 *   - 单文件上限 RAMFS_MAX_FILE_BYTES = 1 MiB（256 页），整池上限 2 MiB。
 *
 * 页来源由调用方注入 struct fs_env（内核绑 PMM，host 单测绑 fake 页池）。
 * 约定：page_alloc 返回的页**已清零**，且返回的指针可直接读写（ng 的恒等
 * 映射让 PMM 物理地址可以直接当指针用）；page_free 只接受 page_alloc 返回
 * 过的指针。ramfs 自己不做 heap 分配，所以没有 OOM 的半成品状态。
 *
 * 不实现：目录树深度限制（用节点池自然封顶）、硬链接、rmdir、时间戳、
 * 块对齐/延迟分配；这些属于后续 WP（FAT16/fsd）或不做。
 */
#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

#define RAMFS_MAX_NODES 32u
#define RAMFS_MAX_ENTS 64u
#define RAMFS_MAX_DATA_PAGES 512u
#define RAMFS_MAX_FILE_PAGES 256u
#define RAMFS_MAX_FILE_BYTES (RAMFS_MAX_FILE_PAGES * 4096ULL) /* 1 MiB */
#define RAMFS_ROOT_PARENT 0xFFFFFFFFu
#define RAMFS_NO_ENT 0xFFFFFFFFu

/* 数据页分配器（由调用方注入；内核用 PMM，host 单测用 fake 页池）。 */
struct fs_env {
    void *(*page_alloc)(void *ctx);
    void (*page_free)(void *ctx, void *page);
    void *ctx;
};

struct ramfs_stats {
    u32 nodes_used;
    u32 ents_used;
    u32 pages_used;
    u32 node_peak;
    u32 page_peak;
    u32 enospc;   /* 文件超 1 MiB / 页池耗尽 */
    u32 rejects;  /* 名字非法 / 类型不对 / 目标存在 */
    u64 creates;
    u64 deletes;
    u64 reads;
    u64 writes;
    u64 truncates;
};

struct ramfs_node {
    u32 used;
    u32 type; /* VFS_NODE_* */
    u32 mode; /* 权限位（& 0777） */
    u32 parent;
    u32 first_page; /* 页链头（索引+1；0=无） */
    u32 page_count;
    u32 open_refs;
    u32 deleted;
    u32 ino;
    u64 size;
    char target[VFS_PATH_MAX]; /* 符号链接目标 */
    struct vfs_node vnode;     /* 给 VFS 的视图，随节点内嵌 */
};

struct ramfs_ent {
    u32 used;
    u32 parent;
    u32 node;
    char name[VFS_NAME_MAX];
};

struct ramfs_page {
    void *mem;
    u32 next; /* 链上下一页（索引+1；0=尾） */
    u32 used;
};

struct ramfs {
    struct fs_env env;
    struct ramfs_node nodes[RAMFS_MAX_NODES];
    struct ramfs_ent ents[RAMFS_MAX_ENTS];
    struct ramfs_page pages[RAMFS_MAX_DATA_PAGES];
    u32 ino_seq;
    struct ramfs_stats st;
};

/* 初始化/重挂：清空池、建根目录（mode 0755, ino 1）。env 必须非空。 */
void ramfs_init(struct ramfs *fs, const struct fs_env *env);

/* ops 表（静态常量）/ 根节点 / 统计。 */
const struct vfs_fs_ops *ramfs_ops(void);
struct vfs_node *ramfs_root(struct ramfs *fs);
const struct ramfs_stats *ramfs_stat(const struct ramfs *fs);

#endif /* RAMFS_H */
