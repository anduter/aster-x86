/*
 * ng WP2-2: FAT16（L1 服务域，src/services/fs/fat16.c）。
 *
 * 定位：VFS 的第二个后端（第一个是 ramfs，WP2-1）。FAT16 只依赖：
 *   - types.h（L0 公共定宽类型）；
 *   - vfs.h（节点 / ops / errno）；
 *   - blk.h 的 struct blk_env（扇区读写，由调用方注入：内核绑 RAM 盘，
 *     host 单测绑 fake 镜像，WP2-4 绑真实块设备）。
 * **不** include src/kernel 或 src/ng 的任何头，也不用内核 heap。
 *
 * 支持范围（WP2-2 出口）：
 *   - BPB 几何校验（magic/bytes-per-sector/sectors-per-cluster/reserved/
 *     FAT 数/root entries/total sectors/FAT size/每簇字节），坏几何 -> EINVAL；
 *   - FAT 表单扇区缓存 + 读写（多份 FAT 同步回写）；
 *   - 簇链遍历（EOC / 坏簇 / 空闲 / 循环 / 越界全部检测，坏链 -> EIO）；
 *   - 空闲簇分配（分配时清零）/ 整链释放；
 *   - 8.3 目录项：0x00 结束、0xE5 删除、卷标与 LFN 跳过；创建/改名 >8.3
 *     -> ENAMETOOLONG；
 *   - 根目录（固定区域）+ 子目录（簇链，"." / ".." 项）；
 *   - mount/read/write/create(mkdir)/unlink/rename/truncate/readdir，
 *     经 struct vfs_fs_ops 与 VFS 对接（VFS 挂到 "/mnt/fat"）。
 *
 * 明确不做（留给后续 WP 或不做）：
 *   - 长文件名（LFN）创建/解析：遇到 LFN 项跳过，不还原长名；
 *   - 目录删除（rmdir）/ 跨目录改名：unlink 目录 -> EISDIR，跨目录 -> EINVAL；
 *   - 时间戳 / 属性编辑 / 权限模型：mode 固定（目录 0755、文件 0644，
 *     read-only 属性 -> 0444），uid/gid 不参与；
 *   - 坏簇重映射、碎片整理、write-back 缓存（数据直接扇区读写）。
 *
 * 并发：与 ramfs 相同，ng 当前单核 + IF=0 + 显式调度点，FAT16 调用全部在
 * 内核态临界区内完成，不加锁；多核/抢占时在公开入口加 spinlock，API 不变。
 */
#ifndef FAT16_H
#define FAT16_H

#include "blk.h"
#include "types.h"
#include "vfs.h"

/* ---- 几何 / 上限 --------------------------------------------------------- */
#define FAT16_SECTOR_SIZE 512u
#define FAT16_MAX_CLUSTERS 65524u /* FAT16 簇号上限（0xFFF4 是最后一个数据簇） */
#define FAT16_MAX_NODES 32u       /* VFS 节点缓存（根节点固定占 0 号） */
#define FAT16_MAX_ROOT_ENTRIES 512u
#define FAT16_MAX_SECTORS_PER_CLUSTER 128u
#define FAT16_MAX_CLUSTER_BYTES 32768u

/* ---- FAT 表项特殊值 ----------------------------------------------------- */
#define FAT16_CLUSTER_FREE 0x0000u
#define FAT16_CLUSTER_BAD 0xFFF7u
#define FAT16_CLUSTER_EOC 0xFFF8u /* 0xFFF8..0xFFFF 都表示链结束 */
#define FAT16_CLUSTER_EOC_MARK 0xFFFFu

/* ---- 目录项属性 --------------------------------------------------------- */
#define FAT16_ATTR_READONLY 0x01u
#define FAT16_ATTR_HIDDEN 0x02u
#define FAT16_ATTR_SYSTEM 0x04u
#define FAT16_ATTR_VOLUME 0x08u
#define FAT16_ATTR_DIR 0x10u
#define FAT16_ATTR_ARCHIVE 0x20u
#define FAT16_ATTR_LFN 0x0Fu /* attr == 0x0F（READONLY|HIDDEN|SYSTEM|VOLUME） */
#define FAT16_ATTR_MASK 0x3Fu

/* 8.3 短名（11 字节，空格填充）在内存里的可显示形式最大 12 + NUL。 */
#define FAT16_NAME83_LEN 11u
#define FAT16_DISPLAY_MAX 13u
#define FAT16_ENTRY_SIZE 32u
#define FAT16_DOT_ENTRY 0x2Eu

/* 0x00 = 目录项所在目录（根目录）；子目录用首簇号标识。 */
#define FAT16_ROOT_DIR 0u

struct fat16_stats {
    u32 mounts;
    u32 mount_fails;
    u32 nodes_used;
    u32 node_peak;
    u32 allocs;      /* 分配过的簇数 */
    u32 frees;       /* 释放过的簇数 */
    u32 enospc;      /* 满盘 / 文件超 4 GiB */
    u32 eio;         /* 块设备错误 / 簇链损坏 */
    u32 chain_loops; /* 检测到的簇链循环 */
    u32 chain_short; /* 簇链比文件大小要求的短 */
    u32 rejects;     /* 名字/类型/几何等参数拒绝 */
    u32 busy;        /* unlink 打开中的文件 */
    u64 reads;
    u64 writes;
    u64 creates;
    u64 deletes;
    u64 renames;
    u64 truncates;
    u64 readdirs;
};

/*
 * FAT16 节点：由 VFS 节点内嵌（vnode.priv 指回这里），VFS 不分配/不释放。
 * 身份 = (parent, entry_off)：父目录标识 + 目录项在目录流里的字节偏移。
 * 改名不改变身份（只改 11 字节名字），所以打开中的节点不会失效。
 */
struct fat16_node {
    u32 used;
    u32 refs; /* open 引用计数；refs>0 的节点不会被回收/复用 */
    u32 is_root;
    u32 parent;     /* 父目录标识：0=根目录，否则父目录首簇 */
    u32 entry_off;  /* 目录项在父目录流中的字节偏移（32 的倍数） */
    u32 first_cluster;
    u32 size;
    u32 ino;
    u8 attr;
    u8 deleted;
    char name[FAT16_DISPLAY_MAX]; /* 8.3 短名（大写），诊断/排序用 */
    struct vfs_node vnode;
};

struct fat16 {
    /* 块设备（由调用方注入；mount 成功后只读使用） */
    struct blk_env blk;
    u32 mounted;

    /* BPB 几何 */
    u32 sector_size;
    u32 sec_per_cluster;
    u32 bytes_per_cluster;
    u32 reserved; /* 保留扇区数（FAT 起点） */
    u32 num_fats;
    u32 root_entries;
    u32 root_dir_sectors;
    u32 fat_sectors;
    u32 total_sectors;
    u32 fat_start; /* = reserved */
    u32 root_start; /* 根目录第一扇区 */
    u32 data_start; /* 数据区第一扇区 */
    u32 cluster_count; /* 数据簇数：合法簇号 2 .. cluster_count+1 */

    u32 last_alloc; /* 下次分配的簇号提示（round-robin） */
    u32 ino_seq;

    /* FAT 表缓存：一扇区（512B），避免链遍历每个表项都读设备。 */
    u8 fat_cache[FAT16_SECTOR_SIZE];
    u32 fat_cache_index; /* 该扇区在 FAT 内的扇区号 */
    u32 fat_cache_valid;
    u32 fat_cache_dirty;

    struct fat16_node nodes[FAT16_MAX_NODES];
    struct fat16_stats st;
};

/* 清空实例（不读盘）。mount 前调用；重复 mount 也从干净状态开始。 */
void fat16_init(struct fat16 *fs);

/*
 * 挂载：读 0 号扇区校验 BPB，成功后才提交几何并建根节点。
 * 失败返回具体负 errno（EINVAL=几何/BPB 非法，EIO=设备读失败，
 * ENOENT=实例/设备为空）；失败不会留下“半挂载”状态。
 */
int fat16_mount(struct fat16 *fs, const struct blk_env *blk);

/* VFS 后端接口：ops 表 / 根节点 / 诊断。 */
const struct vfs_fs_ops *fat16_ops(void);
struct vfs_node *fat16_root(struct fat16 *fs);

u32 fat16_cluster_count(const struct fat16 *fs);
u32 fat16_bytes_per_cluster(const struct fat16 *fs);
u32 fat16_total_sectors(const struct fat16 *fs);
u32 fat16_free_clusters(struct fat16 *fs); /* 扫描 FAT 统计空闲簇（诊断/单测） */
int fat16_fat_entry(struct fat16 *fs, u32 cluster, u32 *value);
const struct fat16_stats *fat16_stat(const struct fat16 *fs);

#endif /* FAT16_H */
