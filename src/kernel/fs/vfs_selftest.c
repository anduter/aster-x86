/* =============================================================================
 * fs/vfs_selftest.c - 后端一致性自检
 *
 * 目的：VFS 改成 "fs_backend_t ops 表 + probe 路由" 之后，ramfs 与 FAT16
 * 走的是同一条调用链。这里把两个后端放在同一组操作下逐项对比，确保：
 *   - 路由正确（fat: 走 FAT16，其余走 ramfs）；
 *   - ramfs：创建 -> 写 -> stat -> 读回 -> 删除 -> stat 应失败；
 *   - FAT16：stat 的 mode/uid/gid 必须为 0、句柄不可共享（ref 必须失败）、
 *     listdir 越界必须报错；
 *   - 删除后不能再被 stat 到。
 * 任何一步不符合预期就打印具体步骤，方便定位是哪个后端/哪条路径退化。
 * ============================================================================= */
#include "fs.h"
#include "fat16.h"
#include "include/kernel.h"
#include "include/kernel.h"

#define SELFTEST_RAM_PATH "system:/vfs_self.txt"
#define SELFTEST_FAT_PATH "fat:/SELFTEST.TXT"

/* 错误路径自检用的临时路径（跑完都会删掉，不落盘） */
#define EP_FULL_PATH  "system:/vfs_ep_full.txt"
#define EP_SMALL_PATH "system:/vfs_ep_small.txt"
#define EP_OPEN_PATH  "system:/vfs_ep_open.txt"
#define EP_CHUNK      1024U

static int selftest_fail(int step)
{
    kprintf("fs: backend selftest FAILED at step %d\n", step);
    return -1;
}

static int ep_fail(int step)
{
    kprintf("fs: error-path selftest FAILED at step %d\n", step);
    return -1;
}

/* 一次超大写入的源缓冲：验证"放不下时整次拒绝"，而不是写一半 */
static char ep_big[FS_FILE_MAX];

/* 按 1KB 分块写满 total 字节；返回写入字节数，出错返回 -1 */
static int ep_write_chunked(uint8_t be, int h, uint32_t total, uint8_t fill)
{
    char chunk[EP_CHUNK];
    uint32_t done = 0;

    memset(chunk, fill, sizeof(chunk));
    while (done < total) {
        uint32_t n = total - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (fs_write_ex(be, h, chunk, n) != (int)n) return -1;
        done += n;
    }
    return (int)done;
}

/* 9. 单文件上限：边界内写成功；再写必须 E_NOSPC，且大小/内容都不变。 */
static int ep_test_write_limit(void)
{
    uint8_t be = 0xFFU;
    fs_stat_t st;
    char head[4];
    char one = 'Z';
    int h, n;

    h = fs_open_ex(EP_FULL_PATH, FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT,
                   0U, 0U, &be);
    if (h < 0) return ep_fail(1);

    if (ep_write_chunked(be, h, FS_FILE_MAX, 'x') != (int)FS_FILE_MAX) return ep_fail(2);
    if (fs_stat_ex(EP_FULL_PATH, &st, 0) != 0 || st.size != FS_FILE_MAX) return ep_fail(3);

    /* 恰好写满后再写 1 字节：必须报 E_NOSPC（不能返回 0 冒充成功） */
    n = fs_write_ex(be, h, &one, 1U);
    if (n != E_NOSPC) return ep_fail(4);

    /* 失败的写不能改变文件大小 */
    if (fs_stat_ex(EP_FULL_PATH, &st, 0) != 0 || st.size != FS_FILE_MAX) return ep_fail(5);

    /* 也不能覆盖已有内容：重新打开读开头 */
    if (fs_close_ex(be, h) != E_OK) return ep_fail(6);
    h = fs_open_ex(EP_FULL_PATH, FS_OPEN_READ, 0U, 0U, &be);
    if (h < 0) return ep_fail(7);
    n = fs_read_ex(be, h, head, sizeof(head));
    (void)fs_close_ex(be, h);
    if (n != (int)sizeof(head) ||
        head[0] != 'x' || head[1] != 'x' || head[2] != 'x' || head[3] != 'x') {
        return ep_fail(8);
    }

    if (fs_unlink_ex(EP_FULL_PATH, 0U, 0U, 0) != 0) return ep_fail(9);
    return 0;
}

/* 10. 小文件上一记超大写入：整次失败，原内容（3 字节）必须原样保留。 */
static int ep_test_write_atomic(void)
{
    uint8_t be = 0xFFU;
    fs_stat_t st;
    char buf[8];
    int h, n;

    h = fs_open_ex(EP_SMALL_PATH, FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT,
                   0U, 0U, &be);
    if (h < 0) return ep_fail(1);
    if (fs_write_ex(be, h, "abc", 3U) != 3) return ep_fail(2);

    memset(ep_big, 'Q', sizeof(ep_big));
    /* 3 + FS_FILE_MAX 超过上限：必须整次拒绝，不能先覆盖前 32765 字节 */
    n = fs_write_ex(be, h, ep_big, (uint32_t)sizeof(ep_big));
    if (n != E_NOSPC) return ep_fail(3);

    if (fs_stat_ex(EP_SMALL_PATH, &st, 0) != 0 || st.size != 3U) return ep_fail(4);
    (void)fs_close_ex(be, h);

    h = fs_open_ex(EP_SMALL_PATH, FS_OPEN_READ, 0U, 0U, &be);
    if (h < 0) return ep_fail(5);
    n = fs_read_ex(be, h, buf, sizeof(buf));
    (void)fs_close_ex(be, h);
    if (n != 3 || memcmp(buf, "abc", 3U) != 0) return ep_fail(6);

    if (fs_unlink_ex(EP_SMALL_PATH, 0U, 0U, 0) != 0) return ep_fail(7);
    return 0;
}

/* 11. 仍被打开的节点不能删除：否则句柄会指向被复用后的节点。 */
static int ep_test_unlink_open(void)
{
    uint8_t be = 0xFFU;
    fs_stat_t st;
    char buf[8];
    int h, h2, n;

    h = fs_open_ex(EP_OPEN_PATH, FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT,
                   0U, 0U, &be);
    if (h < 0) return ep_fail(1);
    if (fs_write_ex(be, h, "keep", 4U) != 4) return ep_fail(2);

    /* 打开中：unlink 必须失败，文件不能被清空 */
    if (fs_unlink_ex(EP_OPEN_PATH, 0U, 0U, 0) == 0) return ep_fail(3);
    if (fs_stat_ex(EP_OPEN_PATH, &st, 0) != 0 || st.size != 4U) return ep_fail(4);

    /* 旧句柄和新句柄都还能读到原内容 */
    n = fs_read_ex(be, h, buf, sizeof(buf));      /* 写句柄 offset=4，读到 EOF(0) */
    if (n != 0) return ep_fail(5);
    h2 = fs_open_ex(EP_OPEN_PATH, FS_OPEN_READ, 0U, 0U, &be);
    if (h2 < 0) return ep_fail(6);
    n = fs_read_ex(be, h2, buf, sizeof(buf));
    (void)fs_close_ex(be, h2);
    if (n != 4 || memcmp(buf, "keep", 4U) != 0) return ep_fail(7);

    /* 全部关闭后才允许删除 */
    if (fs_close_ex(be, h) != E_OK) return ep_fail(8);
    if (fs_unlink_ex(EP_OPEN_PATH, 0U, 0U, 0) != 0) return ep_fail(9);
    if (fs_stat_ex(EP_OPEN_PATH, &st, 0) == 0) return ep_fail(10);
    return 0;
}

void fs_backend_selftest(void)
{
    fs_stat_t st;
    char buf[32];
    char name[FS_NAME_MAX];
    uint8_t be = 0xFFU;
    int h;
    int n;
    int step = 0;

    /* 1. 路由 */
    if (fs_is_fat(SELFTEST_RAM_PATH) != 0) { selftest_fail(++step); return; }
    if (fs_is_fat(SELFTEST_FAT_PATH) == 0) { selftest_fail(++step); return; }

    /* 2. ramfs：创建 + 写 */
    h = fs_open_ex(SELFTEST_RAM_PATH,
                   FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U, &be);
    if (h < 0 || be != FS_BE_RAMFS) { selftest_fail(++step); return; }
    if (fs_write_ex(be, h, "hello", 5U) != 5) { selftest_fail(++step); return; }
    (void)fs_close_ex(be, h);

    /* 3. 元数据：大小与权限位都应有效（ramfs 有 Unix 权限） */
    if (fs_stat_ex(SELFTEST_RAM_PATH, &st, &be) != 0 ||
        st.size != 5U || st.mode == 0U) { selftest_fail(++step); return; }

    /* 4. 读回内容 */
    h = fs_open_ex(SELFTEST_RAM_PATH, FS_OPEN_READ, 0U, 0U, &be);
    if (h < 0) { selftest_fail(++step); return; }
    n = fs_read_ex(be, h, buf, sizeof(buf));
    if (n != 5 || memcmp(buf, "hello", 5U) != 0) { selftest_fail(++step); return; }
    (void)fs_close_ex(be, h);

    /* 5. 删除后 stat 必须失败 */
    if (fs_unlink_ex(SELFTEST_RAM_PATH, 0U, 0U, &be) != 0) { selftest_fail(++step); return; }
    if (fs_stat_ex(SELFTEST_RAM_PATH, &st, &be) == 0) { selftest_fail(++step); return; }

    /* 6. listdir 越界必须报错 */
    if (fs_listdir_ex("system:", 9999U, name, sizeof(name), &st, &be) == 0) {
        selftest_fail(++step);
        return;
    }

    /* 7. FAT16（没挂载就跳过，不算失败） */
    if (fat16_mounted()) {
        h = fs_open_ex(SELFTEST_FAT_PATH,
                       FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U, &be);
        if (h < 0 || be != FS_BE_FAT16) { selftest_fail(++step); return; }

        /* FAT 句柄没有引用计数，必须拒绝共享 */
        if (fs_ref_ex(be, h) == 0) { selftest_fail(++step); return; }

        if (fs_write_ex(be, h, "fat", 3U) != 3) { selftest_fail(++step); return; }

        /* 打开中的 FAT16 文件不能删除：否则簇链被回收，读句柄会读到已复用簇 */
        if (fs_unlink_ex(SELFTEST_FAT_PATH, 0U, 0U, 0) == 0) {
            selftest_fail(++step);
            return;
        }

        (void)fs_close_ex(be, h);

        /* FAT16 没有 Unix 权限位：mode/uid/gid 必须是 0 */
        if (fs_stat_ex(SELFTEST_FAT_PATH, &st, &be) != 0 ||
            st.mode != 0U || st.uid != 0U || st.gid != 0U) {
            selftest_fail(++step);
            return;
        }

        h = fs_open_ex(SELFTEST_FAT_PATH, FS_OPEN_READ, 0U, 0U, &be);
        if (h < 0) { selftest_fail(++step); return; }
        n = fs_read_ex(be, h, buf, sizeof(buf));
        if (n != 3 || memcmp(buf, "fat", 3U) != 0) { selftest_fail(++step); return; }
        (void)fs_close_ex(be, h);

        if (fs_unlink_ex(SELFTEST_FAT_PATH, 0U, 0U, &be) != 0) { selftest_fail(++step); return; }

        if (fs_listdir_ex("fat:", 9999U, name, sizeof(name), &st, &be) == 0) {
            selftest_fail(++step);
            return;
        }
    }

    /* 8. 错误路径：失败必须"整次失败 + 原状态不变"（详见 docs/error-paths.md） */
    (void)fs_unlink_ex(EP_FULL_PATH, 0U, 0U, 0);
    (void)fs_unlink_ex(EP_SMALL_PATH, 0U, 0U, 0);
    (void)fs_unlink_ex(EP_OPEN_PATH, 0U, 0U, 0);
    if (ep_test_write_limit() != 0) return;
    if (ep_test_write_atomic() != 0) return;
    if (ep_test_unlink_open() != 0) return;
    kprintln("fs: error-path selftest ok");

    kprintln("fs: backend selftest ok");
}
