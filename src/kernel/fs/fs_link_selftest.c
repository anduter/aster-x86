/* =============================================================================
 * fs/fs_link_selftest.c - 硬链接 / 符号链接回归（阶段 4b）
 *
 * 断言（build.ps1 收集 "fs:" 开头的行）：
 *   1. 硬链接：nlink=2，通过任一名字写、另一个名字读得到；
 *   2. 删掉一个名字后数据还在、nlink 回到 1；
 *   3. 目录不能做硬链接（E_INVAL）；
 *   4. 跨卷硬链接拒绝（E_INVAL）；
 *   5. 符号链接：相对目标能被 open 跟随；
 *   6. readlink 读到目标文本；lstat 报 LINK，stat 跟随报 FILE；
 *   7. O_NOFOLLOW 打开符号链接失败；
 *   8. 软链接环有跳数上限（E_INVAL，不会死循环）；
 *   9. FAT16 上的 link/symlink 返回 E_INVAL；
 *  10. 软链接指向 system:/bin 时写操作被 SIP 拦下（防止绕过只读策略）。
 * ============================================================================= */
#include "fs.h"
#include "include/kernel.h"
#include "include/fs.h"
#include "include/dev.h"

#define LK_DIR      "system:/tmp"
#define LK_A        "system:/tmp/lk-a"
#define LK_B        "system:/tmp/lk-b"
#define LK_SL       "system:/tmp/lk-sl"
#define LK_SL2      "system:/tmp/lk-sl2"
#define LK_LOOP_A   "system:/tmp/lk-loop-a"
#define LK_LOOP_B   "system:/tmp/lk-loop-b"
#define LK_NOFOLLOW "system:/tmp/lk-nofollow"
#define LK_BYPASS   "system:/tmp/lk-bypass"

/* XFS1 v6 持久化回归用（独立名字，避免和阶段 4b 的临时文件冲突） */
#define V6_A        "system:/tmp/xfs6-a"
#define V6_B        "system:/tmp/xfs6-b"
#define V6_DATA     "v6-persist"
#define V6_DATA2    "V6-PERSIST"

static int lk_fail(int step)
{
    kprintf("fs: link selftest FAILED at step %d\n", step);
    return -1;
}

static int write_file(const char *path, const void *buf, uint32_t len)
{
    uint32_t h = fs_open_as(path, FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U);
    int n;

    if (h == 0U) return -1;
    n = fs_write(h, buf, len);
    fs_close(h);
    return (n == (int)len) ? 0 : -1;
}

static void unlink_all(void)
{
    (void)fs_unlink_as(LK_A, 0U, 0U);
    (void)fs_unlink_as(LK_B, 0U, 0U);
    (void)fs_unlink_as(LK_SL, 0U, 0U);
    (void)fs_unlink_as(LK_SL2, 0U, 0U);
    (void)fs_unlink_as(LK_LOOP_A, 0U, 0U);
    (void)fs_unlink_as(LK_LOOP_B, 0U, 0U);
    (void)fs_unlink_as(LK_NOFOLLOW, 0U, 0U);
    (void)fs_unlink_as(LK_BYPASS, 0U, 0U);
}

static int read_expect(const char *path, const char *expect, uint32_t len)
{
    char buf[64];
    uint32_t h = fs_open_as(path, FS_OPEN_READ, 0U, 0U);
    int n;

    if (h == 0U) return -1;
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    return (n == (int)len && memcmp(buf, expect, len) == 0) ? 0 : -1;
}

/* v5 兼容：在空闲的 ram0 上写一个 v5 镜像，实际挂载它（证明旧布局可读），
 * 再从持久化盘恢复现场。v5 镜像下次 fs_sync 会写成 v6。
 *
 * 空镜像现在会因"没有卷根"被 mount 拒绝，所以手工写一个 v5 卷根，让兼容性
 * 测试仍然覆盖 "108 字节 inode 记录 + 数据区 LBA 15" 的读取路径。 */
typedef struct {
    uint32_t used;
    uint32_t type;
    uint32_t parent;
    uint32_t size;
    uint32_t data_lba;
    uint32_t volume;
    uint32_t mtime;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    char     name[FS_NAME_MAX];
    uint8_t  pad[3];
} __attribute__((packed)) audit_inode_v5_t;

_Static_assert(sizeof(audit_inode_v5_t) == 108, "v5 audit inode must be 108 bytes");

static int write_v5_root(block_device_t *blk)
{
    uint8_t sec[BLK_SECTOR];
    audit_inode_v5_t in;

    if (blk == 0) return -1;
    memset(&in, 0, sizeof(in));
    in.used = 1U;
    in.type = (uint32_t)FS_TYPE_DIR;
    in.parent = 0xFFFFFFFFU;
    in.mode = FS_MODE_DIR_DEFAULT;
    strncpy(in.name, "system", FS_NAME_MAX - 1U);

    /* v5 super：volumes[0] 在扇区偏移 44（magic 4 + 10 个 u32 字段） */
    if (blk_read(blk, 0U, sec) != E_OK) return -1;
    memset(sec + 44U, 0, FS_DRIVE_MAX);
    memcpy(sec + 44U, "system", 6U);
    if (blk_write(blk, 0U, sec) != E_OK) return -1;

    memset(sec, 0, sizeof(sec));
    memcpy(sec, &in, sizeof(in));
    return (blk_write(blk, 1U, sec) == E_OK) ? 0 : -1;
}

static int v5_mount_compat(void)
{
    const char *backing = fs_backing();
    block_device_t *spare;
    fs_stat_t st;

    if (backing == 0 || backing[0] == '\0') return 0;
    spare = blk_lookup("ram0");
    if (spare == 0 || strcmp(backing, "ram0") == 0) return 0;

    if (fs_sync(backing) != E_OK) {
        kprintln("fs: xfs v5 compat sync failed");
        return -1;
    }
    if (fs_format_disk_version("ram0", 5U) != E_OK) {
        kprintln("fs: xfs v5 format failed");
        return -1;
    }
    if (write_v5_root(spare) != 0) {
        kprintln("fs: xfs v5 root write failed");
        return -1;
    }
    if (fs_mount_disk("ram0") != E_OK) {
        kprintln("fs: xfs v5 mount FAILED");
        (void)fs_mount_disk(backing);
        return -1;
    }
    if (fs_volume_count() != 1U) {
        kprintln("fs: xfs v5 image volume root not restored");
        (void)fs_mount_disk(backing);
        return -1;
    }
    /* 恢复真实卷；v5 -> v6 的升级由后面的 fs_sync 完成 */
    if (fs_mount_disk(backing) != E_OK) {
        kprintln("fs: xfs remount after v5 compat FAILED");
        return -1;
    }
    if (fs_stat("system:/bin/shell", &st) != E_OK || st.size == 0U) {
        kprintln("fs: xfs remount lost system:/bin/shell");
        return -1;
    }
    kprintln("fs: xfs v5 image mount compat ok");
    return 0;
}

/* v6 核心回归：link -> sync -> 清空内存 -> 重新挂载，两个名字仍指向同一
 * inode；删一个名字另一个可读；nlink 正确；fsck 能修正被破坏的 nlink。 */
static int v6_persist(void)
{
    const char *backing = fs_backing();
    fs_stat_t st;
    char report[128];

    if (backing == 0 || backing[0] == '\0') {
        kprintln("fs: xfs v6 hardlink persisted skip (no backing device)");
        return 0;
    }

    (void)fs_unlink_as(V6_A, 0U, 0U);
    (void)fs_unlink_as(V6_B, 0U, 0U);

    if (write_file(V6_A, V6_DATA, (uint32_t)strlen(V6_DATA)) != 0) {
        kprintln("fs: xfs v6 persist write failed");
        return -1;
    }
    if (fs_link_as(V6_A, V6_B, 0U, 0U) != E_OK) {
        kprintln("fs: xfs v6 persist link failed");
        return -1;
    }
    if (fs_stat(V6_B, &st) != E_OK || st.nlink != 2U || st.size != 10U) {
        kprintln("fs: xfs v6 persist nlink before sync FAILED");
        return -1;
    }

    if (fs_sync(backing) != E_OK) {
        kprintln("fs: xfs v6 persist sync failed");
        return -1;
    }

    /* 模拟重启：内存树清空，从盘重建 */
    fs_init();
    if (fs_mount_disk(backing) != E_OK) {
        kprintln("fs: xfs v6 persist mount FAILED");
        return -1;
    }
    if (fs_stat(V6_A, &st) != E_OK || st.nlink != 2U || st.size != 10U ||
        fs_stat(V6_B, &st) != E_OK || st.nlink != 2U || st.size != 10U) {
        kprintln("fs: xfs v6 hardlink relation lost after remount");
        return -1;
    }
    if (read_expect(V6_A, V6_DATA, 10U) != 0 ||
        read_expect(V6_B, V6_DATA, 10U) != 0) {
        kprintln("fs: xfs v6 persisted data mismatch after remount");
        return -1;
    }

    /* 通过一个名字写，另一个名字必须看到（同一 inode，不是两份拷贝） */
    if (write_file(V6_B, V6_DATA2, 10U) != 0 ||
        read_expect(V6_A, V6_DATA2, 10U) != 0) {
        kprintln("fs: xfs v6 shared data via link FAILED");
        return -1;
    }

    /* 删一个名字，另一个仍可读，nlink 回到 1 */
    if (fs_unlink_as(V6_A, 0U, 0U) != E_OK ||
        fs_stat(V6_B, &st) != E_OK || st.nlink != 1U ||
        read_expect(V6_B, V6_DATA2, 10U) != 0) {
        kprintln("fs: xfs v6 unlink one name FAILED");
        return -1;
    }

    /* 再走一次 sync/remount，确认“唯一名字 + nlink=1”也持久化 */
    if (fs_sync(backing) != E_OK) return -1;
    fs_init();
    if (fs_mount_disk(backing) != E_OK) {
        kprintln("fs: xfs v6 second mount FAILED");
        return -1;
    }
    if (fs_stat(V6_B, &st) != E_OK || st.nlink != 1U ||
        read_expect(V6_B, V6_DATA2, 10U) != 0) {
        kprintln("fs: xfs v6 second remount FAILED");
        return -1;
    }

    /* fsck：正常状态 0 错误 */
    if (fs_check(report, sizeof(report)) != 0) {
        kprintln("fs: xfs v6 fsck reports errors on healthy tree");
        return -1;
    }

    /* 注入损坏：nlink 改成 7。fsck 必须发现并按目录项数修回 1。 */
    if (fs_test_corrupt_nlink(V6_B, 7U) != E_OK) {
        kprintln("fs: xfs v6 nlink fault injection failed");
        return -1;
    }
    if (fs_check(report, sizeof(report)) <= 0) {
        kprintln("fs: xfs v6 fsck missed corrupt nlink");
        return -1;
    }
    if (fs_stat(V6_B, &st) != E_OK || st.nlink != 1U) {
        kprintln("fs: xfs v6 fsck did not repair nlink");
        return -1;
    }
    if (fs_check(report, sizeof(report)) != 0) {
        kprintln("fs: xfs v6 fsck not clean after repair");
        return -1;
    }

    (void)fs_unlink_as(V6_B, 0U, 0U);
    kprintln("fs: xfs v6 hardlink persisted ok");
    return 0;
}

void fs_link_selftest(void)
{
    fs_stat_t st;
    char buf[32];
    uint32_t h;
    int n;

    (void)fs_mkdir(LK_DIR);
    unlink_all();

    /* 1. 硬链接 + 通过两个名字看到同一份数据 */
    if (write_file(LK_A, "hello", 5U) != 0) { (void)lk_fail(1); return; }
    if (fs_link_as(LK_A, LK_B, 0U, 0U) != E_OK) { (void)lk_fail(2); return; }
    if (fs_stat(LK_B, &st) != E_OK || st.nlink != 2U) { (void)lk_fail(3); return; }
    if (write_file(LK_B, "HELLO", 5U) != 0) { (void)lk_fail(4); return; }
    h = fs_open_as(LK_A, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) { (void)lk_fail(5); return; }
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    if (n != 5 || memcmp(buf, "HELLO", 5U) != 0) { (void)lk_fail(6); return; }
    kprintln("fs: hard link ok (nlink=2, write via link visible)");

    /* 2. 删一个名字，数据仍在，nlink 回到 1 */
    if (fs_unlink_as(LK_A, 0U, 0U) != E_OK) { (void)lk_fail(7); return; }
    if (fs_stat(LK_B, &st) != E_OK || st.nlink != 1U || st.size != 5U) {
        (void)lk_fail(8);
        return;
    }
    h = fs_open_as(LK_B, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) { (void)lk_fail(9); return; }
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    if (n != 5 || memcmp(buf, "HELLO", 5U) != 0) { (void)lk_fail(10); return; }
    kprintln("fs: hard link unlink keeps data ok");

    /* 3. 目录不能做硬链接 */
    if (fs_link_as(LK_DIR, "system:/tmp/lk-dir-link", 0U, 0U) != E_INVAL) {
        (void)lk_fail(11);
        return;
    }
    kprintln("fs: hard link rejects directory ok");

    /* 4. 跨卷硬链接拒绝（data: 卷在启动时已挂载） */
    if (fs_link_as(LK_B, "data:/lk-cross", 0U, 0U) != E_INVAL) {
        (void)lk_fail(12);
        return;
    }
    kprintln("fs: hard link rejects cross-volume ok");

    /* 5. 软链接：相对目标 + open 跟随 */
    if (fs_symlink_as("lk-b", LK_SL, 0U, 0U) != E_OK) { (void)lk_fail(13); return; }
    h = fs_open_as(LK_SL, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) { (void)lk_fail(14); return; }
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    if (n != 5 || memcmp(buf, "HELLO", 5U) != 0) { (void)lk_fail(15); return; }
    kprintln("fs: symlink follow ok");

    /* 6. readlink / lstat / stat */
    {
        char target[64];

        n = fs_readlink_as(LK_SL, target, sizeof(target), 0U, 0U);
        if (n <= 0 || strcmp(target, "lk-b") != 0) { (void)lk_fail(16); return; }
        if (fs_lstat_as(LK_SL, &st, 0U, 0U) != E_OK || st.type != FS_TYPE_LINK ||
            st.size != 4U) {
            (void)lk_fail(17);
            return;
        }
        if (fs_stat(LK_SL, &st) != E_OK || st.type != FS_TYPE_FILE || st.size != 5U) {
            (void)lk_fail(18);
            return;
        }
    }
    kprintln("fs: readlink/lstat ok");

    /* 7. O_NOFOLLOW 打开软链接必须失败 */
    if (fs_open_as(LK_SL, FS_OPEN_READ | FS_OPEN_NOFOLLOW, 0U, 0U) != 0U) {
        (void)lk_fail(19);
        return;
    }
    kprintln("fs: O_NOFOLLOW rejects symlink ok");

    /* 8. 软链接环：跳数上限，返回 E_INVAL 而不是死循环 */
    if (fs_symlink_as("lk-loop-b", LK_LOOP_A, 0U, 0U) != E_OK ||
        fs_symlink_as("lk-loop-a", LK_LOOP_B, 0U, 0U) != E_OK) {
        (void)lk_fail(20);
        return;
    }
    if (fs_open_as(LK_LOOP_A, FS_OPEN_READ, 0U, 0U) != 0U) {
        (void)lk_fail(21);
        return;
    }
    kprintln("fs: symlink loop limited ok");

    /* 9. FAT16 不支持 link/symlink（返回 E_INVAL） */
    if (fat16_mounted()) {
        if (fs_link_as(LK_B, "fat:/LINK.TXT", 0U, 0U) != E_INVAL ||
            fs_symlink_as("x", "fat:/SLINK.TXT", 0U, 0U) != E_INVAL ||
            fs_readlink_as("fat:/host.txt", buf, sizeof(buf), 0U, 0U) != E_INVAL) {
            (void)lk_fail(22);
            return;
        }
        kprintln("fs: FAT16 link ops E_INVAL ok");
    }

    /* 10. 软链接指向 system:/bin：写操作仍被 SIP 拒绝（防止绕过只读策略） */
    if (sip_enabled()) {
        if (fs_symlink_as("/bin/shell", LK_BYPASS, 0U, 0U) != E_OK) {
            (void)lk_fail(23);
            return;
        }
        if (fs_open_as(LK_BYPASS, FS_OPEN_READ, 0U, 0U) == 0U) {
            (void)lk_fail(24);      /* 读应该被允许（跟随到 shell） */
            return;
        }
        if (fs_open_as(LK_BYPASS, FS_OPEN_WRITE, 0U, 0U) != 0U) {
            (void)lk_fail(25);      /* 写必须被 SIP 拦下 */
            return;
        }
        kprintln("fs: symlink into system path denied ok");
    }

    /* XFS1 v6：先释放前面用过的临时节点，再做 v5 兼容挂载与硬链接持久化 */
    unlink_all();
    if (v5_mount_compat() != 0) { (void)lk_fail(26); return; }
    if (v6_persist() != 0) { (void)lk_fail(27); return; }

    unlink_all();
    /* 不删 /tmp 本身：标准目录，其它自检（SIP）还要用 */
    kprintln("fs: link selftest PASS");
}
