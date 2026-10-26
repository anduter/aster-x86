/* =============================================================================
 * fs/fs_audit_selftest.c - 损坏镜像 / 路径边界 / 硬链接 nlink 审计自检
 *
 * 覆盖（全部在内存里的 ram0 上构造，不依赖宿主注入）：
 *   1. 非法盘符前缀 ":..." / 64+ 字符盘符：fs_listdir 不能读未初始化指针；
 *   2. XFS1 v6 坏镜像：坏类型（type=0）、孤儿、size 越界、data_lba 越界、
 *      空卷根都必须 E_INVAL；
 *   3. mount 失败后必须保留可用的默认卷 system:（否则启动安装器没有落点，
 *      整机降级/丢盘）；
 *   4. 合法镜像仍能挂载，nlink 与目录项数不符时按目录项数修复；
 *   5. 硬链接删宿主后 nlink/数据仍正确；
 *   6. 深路径、64 字符名字、`..` 到卷根、重复 '/' 不崩。
 *
 * 打印 "fs: audit selftest PASS" 作为回归证据行。
 * ============================================================================= */
#include "fs.h"
#include "fs_syscall.h"
#include "abi.h"
#include "include/kernel.h"
#include "include/dev.h"

#define AUD_SECTOR 512U
#define AUD_INODE_MAX 64U
#define AUD_INODE_SECT 15U
#define AUD_DATA_LBA 16U
#define AUD_TAG "fs: audit"

/* 与 ramfs.c 的 disk_super_t / disk_inode_t 同布局（审计侧独立定义，
 * 用 _Static_assert 钉住大小，格式改动会在这里编译失败）。 */
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t inode_count;
    uint32_t inode_size;
    uint32_t inode_lba;
    uint32_t data_lba;
    uint32_t volume_count;
    uint32_t max_file;
    uint32_t data_bytes;
    char     volumes[FS_MAX_VOLUMES][FS_DRIVE_MAX];
    uint8_t  pad[340];
} audit_super_t;

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
    uint32_t inode_id;
    uint32_t nlink;
    char     name[FS_NAME_MAX];
    uint8_t  pad[3];
} audit_inode_t;

_Static_assert(sizeof(audit_super_t) == AUD_SECTOR, "audit super must be 512 bytes");
_Static_assert(sizeof(audit_inode_t) == 116, "audit v6 inode must be 116 bytes");

/* 构造场景 */
enum {
    AUD_GOOD = 0,
    AUD_TYPE_NONE,
    AUD_ORPHAN,
    AUD_BAD_SIZE,
    AUD_BAD_DATA_LBA,
    AUD_EMPTY,
    AUD_BAD_VOLNAME,
    AUD_NLINK_MISMATCH
};

static int aud_fail(int step)
{
    kprintf("fs: audit selftest FAILED at step %d\n", step);
    return -1;
}

static void aud_inode(audit_inode_t *in, uint32_t type, uint32_t parent,
                      uint32_t size, uint32_t data_lba, uint32_t inode_id,
                      uint32_t nlink, const char *name)
{
    memset(in, 0, sizeof(*in));
    in->used = 1U;
    in->type = type;
    in->parent = parent;
    in->size = size;
    in->data_lba = data_lba;
    in->mode = (type == (uint32_t)FS_TYPE_DIR) ? FS_MODE_DIR_DEFAULT
                                               : FS_MODE_FILE_DEFAULT;
    in->inode_id = inode_id;
    in->nlink = nlink;
    strncpy(in->name, name, FS_NAME_MAX - 1U);
}

/* 在 ram0 上写一个 XFS1 v6 镜像；返回 0 成功。 */
static int aud_write_image(int scenario)
{
    block_device_t *blk = blk_lookup("ram0");
    audit_super_t super;
    audit_inode_t inodes[AUD_INODE_MAX];
    uint8_t table[AUD_INODE_SECT * AUD_SECTOR];
    uint8_t sec[AUD_SECTOR];
    uint32_t count = 1U;

    if (blk == 0) return -1;

    memset(&super, 0, sizeof(super));
    super.magic[0] = 'X'; super.magic[1] = 'F';
    super.magic[2] = 'S'; super.magic[3] = '1';
    super.version = 6U;
    super.sector_size = AUD_SECTOR;
    super.sector_count = blk->sector_count;
    super.inode_count = AUD_INODE_MAX;
    super.inode_size = (uint32_t)sizeof(audit_inode_t);
    super.inode_lba = 1U;
    super.data_lba = AUD_DATA_LBA;
    super.max_file = FS_FILE_MAX;
    super.data_bytes = (blk->sector_count - AUD_DATA_LBA) * AUD_SECTOR;

    memset(inodes, 0, sizeof(inodes));
    aud_inode(&inodes[0], (uint32_t)FS_TYPE_DIR, 0xFFFFFFFFU, 0U, 0U, 1U, 1U,
              "system");
    super.volumes[0][0] = 's'; super.volumes[0][1] = 'y';
    super.volumes[0][2] = 's'; super.volumes[0][3] = 't';
    super.volumes[0][4] = 'e'; super.volumes[0][5] = 'm';
    super.volume_count = 1U;

    switch (scenario) {
    case AUD_GOOD:
        break;
    case AUD_TYPE_NONE:
        aud_inode(&inodes[1], 0U, 0U, 0U, 0U, 2U, 1U, "ghost");
        count = 2U;
        break;
    case AUD_ORPHAN:
        aud_inode(&inodes[1], (uint32_t)FS_TYPE_FILE, 9U, 4U, AUD_DATA_LBA,
                  2U, 1U, "lost");
        count = 2U;
        break;
    case AUD_BAD_SIZE:
        aud_inode(&inodes[1], (uint32_t)FS_TYPE_FILE, 0U, FS_FILE_MAX + 1U,
                  AUD_DATA_LBA, 2U, 1U, "huge");
        count = 2U;
        break;
    case AUD_BAD_DATA_LBA:
        aud_inode(&inodes[1], (uint32_t)FS_TYPE_FILE, 0U, 4096U,
                  blk->sector_count - 1U, 2U, 1U, "tail");
        count = 2U;
        break;
    case AUD_EMPTY:
        memset(&inodes[0], 0, sizeof(inodes[0]));
        count = 0U;
        super.volume_count = 0U;
        break;
    case AUD_BAD_VOLNAME:
        memset(super.volumes[0], 0, FS_DRIVE_MAX);
        count = 1U;
        break;
    case AUD_NLINK_MISMATCH:
        aud_inode(&inodes[1], (uint32_t)FS_TYPE_FILE, 0U, 4U, AUD_DATA_LBA,
                  2U, 7U, "auditdata");
        count = 2U;
        break;
    default:
        return -1;
    }

    memset(sec, 0, sizeof(sec));
    memcpy(sec, &super, sizeof(super));
    if (blk_write(blk, 0U, sec) != E_OK) return -1;

    memset(table, 0, sizeof(table));
    if (count > 0U) memcpy(table, inodes, count * sizeof(audit_inode_t));
    for (uint32_t s = 0; s < AUD_INODE_SECT; s++) {
        if (blk_write(blk, 1U + s, table + s * AUD_SECTOR) != E_OK) return -1;
    }

    if (scenario == AUD_NLINK_MISMATCH) {
        memset(sec, 0, sizeof(sec));
        memcpy(sec, "DATA", 4U);
        if (blk_write(blk, AUD_DATA_LBA, sec) != E_OK) return -1;
    }
    return 0;
}

/* 把内存树恢复到持久化盘（ata0）上的真实文件系统。 */
static int aud_restore(void)
{
    const char *backing = fs_backing();

    if (backing == 0 || backing[0] == '\0') return -1;
    fs_init();
    return (fs_mount_disk(backing) == E_OK) ? 0 : -1;
}

/* 默认卷必须可用：mount_disk_fail() 不能把 fs_init() 的 system: 清掉。 */
static int aud_default_volume_ok(void)
{
    fs_stat_t st;

    return (fs_stat("system:/", &st) == E_OK && st.type == FS_TYPE_DIR) ? 1 : 0;
}

/* 1. 非法盘符/超长盘符：不能让 fs_listdir 读未初始化的 rel 指针。 */
static int aud_test_listdir_paths(void)
{
    char name[FS_NAME_MAX];
    fs_stat_t st;
    const char *bad[] = {
        ":/x",
        ":", 
        "0123456789012345678901234567890123:/x",
        "nosuch:/x"
    };

    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (fs_listdir(bad[i], 0U, name, sizeof(name), &st) == E_OK) return -1;
    }
    kprintf("%s listdir bad volume prefixes rejected ok\n", AUD_TAG);
    return 0;
}

/* 2/3. 坏镜像必须整次拒绝；失败后默认卷仍可用。 */
static int aud_test_corrupt_images(void)
{
    static const struct {
        int scenario;
        const char *name;
    } cases[] = {
        { AUD_TYPE_NONE,    "type=0 ghost inode" },
        { AUD_ORPHAN,       "orphan parent" },
        { AUD_BAD_SIZE,     "oversized inode" },
        { AUD_BAD_DATA_LBA, "data_lba past device" },
        { AUD_EMPTY,        "no volume root" },
        { AUD_BAD_VOLNAME,  "empty volume name" }
    };

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (aud_write_image(cases[i].scenario) != 0) return -1;
        if (fs_mount_disk("ram0") == E_OK) {
            kprintf("fs: audit mount accepted corrupt image (%s)\n", cases[i].name);
            (void)aud_restore();
            return -1;
        }
        if (!aud_default_volume_ok()) {
            kprintf("fs: audit mount failure lost default volume (%s)\n",
                    cases[i].name);
            (void)aud_restore();
            return -1;
        }
    }
    kprintf("%s corrupt image mounts rejected, default volume kept ok\n", AUD_TAG);
    return 0;
}

/* 4. 合法镜像 + nlink 修复 */
static int aud_test_valid_mount(void)
{
    fs_stat_t st;

    if (aud_write_image(AUD_GOOD) != 0) return -1;
    if (fs_mount_disk("ram0") != E_OK) { (void)aud_restore(); return -1; }
    if (fs_volume_count() == 0U) { (void)aud_restore(); return -1; }
    if (fs_stat("system:/", &st) != E_OK || st.type != FS_TYPE_DIR) {
        (void)aud_restore();
        return -1;
    }

    if (aud_write_image(AUD_NLINK_MISMATCH) != 0) return -1;
    if (fs_mount_disk("ram0") != E_OK) { (void)aud_restore(); return -1; }
    if (fs_stat("system:/auditdata", &st) != E_OK || st.nlink != 1U ||
        st.size != 4U) {
        (void)aud_restore();
        return -1;
    }
    if (aud_restore() != 0) return -1;
    kprintf("%s valid image mount + nlink repair ok\n", AUD_TAG);
    return 0;
}

/* 5. 硬链接：删宿主后数据/计数正确（ino_promote 路径）。 */
static int aud_test_hardlink(void)
{
    const char *a = "system:/audit-hl-a";
    const char *b = "system:/audit-hl-b";
    fs_stat_t st;
    char buf[8];
    uint32_t h;
    int n;

    (void)fs_unlink(a);
    (void)fs_unlink(b);

    h = fs_open_as(a, FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U);
    if (h == 0U) return -1;
    if (fs_write(h, "audit", 5U) != 5) { fs_close(h); return -1; }
    fs_close(h);

    if (fs_link_as(a, b, 0U, 0U) != E_OK) return -1;
    if (fs_stat(b, &st) != E_OK || st.nlink != 2U) return -1;

    /* 删宿主 a：b 必须成为新宿主，数据不丢，nlink=1 */
    if (fs_unlink(a) != E_OK) return -1;
    if (fs_stat(b, &st) != E_OK || st.nlink != 1U || st.size != 5U) return -1;
    h = fs_open_as(b, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) return -1;
    n = fs_read(h, buf, sizeof(buf));
    fs_close(h);
    if (n != 5 || memcmp(buf, "audit", 5U) != 0) return -1;

    /* 再建链接并删链接（非宿主）路径 */
    if (fs_link_as(b, a, 0U, 0U) != E_OK) return -1;
    if (fs_unlink(a) != E_OK) return -1;
    if (fs_stat(b, &st) != E_OK || st.nlink != 1U) return -1;
    (void)fs_unlink(b);
    kprintf("%s hardlink nlink / ino_promote ok\n", AUD_TAG);
    return 0;
}

/* 去掉路径最后一段（"a/b/c" -> "a/b"，"system:" 不变） */
static void aud_path_parent(char *p)
{
    uint32_t n = (uint32_t)strlen(p);

    while (n > 0U && p[n - 1U] != '/') n--;
    if (n > 0U) n--;                 /* 去掉分隔符 */
    p[n] = '\0';
}

/* 6. 路径边界：深目录、64 字符名字、重复 '/'、".." 到卷根。 */
static int aud_test_path_edges(void)
{
    char path[FS_PATH_MAX];
    char name[FS_NAME_MAX + 4];
    fs_stat_t st;
    uint32_t used = 0;
    uint32_t h;

    /* 12 层目录 */
    strcpy(path, "system:");
    for (uint32_t i = 0; i < 12U; i++) {
        uint32_t n = (uint32_t)strlen(path);
        ksnprintf(path + n, sizeof(path) - n, "/audit-d%u", (unsigned int)i);
        if (fs_mkdir(path) != E_OK && fs_stat(path, &st) != E_OK) {
            kprintf("fs: audit deep mkdir failed at %u\n", (unsigned int)i);
            break;
        }
        used = i + 1U;
    }
    if (used == 12U) {
        if (fs_stat(path, &st) != E_OK || st.type != FS_TYPE_DIR) return -1;
        /* 从最深往上删 */
        while (used > 0U) {
            if (fs_unlink(path) != E_OK) return -1;
            used--;
            aud_path_parent(path);
        }
    } else {
        return -1;
    }

    /* 64 字符名字（FS_NAME_MAX-1） */
    memset(name, 'n', 64U);
    name[64] = '\0';
    ksnprintf(path, sizeof(path), "system:/%s", name);
    h = fs_open_as(path, FS_OPEN_WRITE | FS_OPEN_CREAT, 0U, 0U);
    if (h == 0U) return -1;
    fs_close(h);
    h = fs_open_as(path, FS_OPEN_READ, 0U, 0U);
    if (h == 0U) return -1;
    fs_close(h);
    if (fs_stat(path, &st) != E_OK || st.size != 0U) return -1;
    if (fs_unlink(path) != E_OK) return -1;

    /* 重复 '/'、"."、".." 到卷根 */
    if (fs_stat("system:///bin//./../", &st) != E_OK || st.type != FS_TYPE_DIR) {
        return -1;
    }
    if (fs_stat("system:/../../../../..", &st) != E_OK ||
        st.type != FS_TYPE_DIR) {
        return -1;
    }
    kprintf("%s deep/long/.. path edges ok\n", AUD_TAG);
    return 0;
}

/* 7. legacy fs 系统调用的缓冲边界：
 *    - SYS_CHROOT 的路径 -> cwd(192B) 复制：超长必须 -1 且 canary 不变；
 *    - SYS_TIME 遵守 ecx=max：4B 缓冲只写 4B（末字节 NUL），其余 canary 不变；
 *      max=0 返回 -1 且不写；
 *    - SYS_FSCK 坏用户指针返回 -1（旧实现忽略 copy_to_user 返回值）。 */
static int aud_test_syscall_guards(void)
{
    static proc_t fake;
    char longpath[220];
    char tbuf[32];
    interrupt_frame_t f;
    uint32_t i;
    int r;

    /* --- SYS_CHROOT 写 cwd 的入口：超长路径必须 -1 且 cwd/后续字段不变 --- */
    memset(&fake, 0xAA, sizeof(fake));
    fake.user_brk = 0xA5A5A5A5U;     /* cwd 后面的字段，作为越界 canary */
    memset(longpath, 'a', sizeof(longpath) - 1U);
    longpath[sizeof(longpath) - 1U] = '\0';
    if (fs_chroot_set_cwd(&fake, longpath) != E_INVAL) return -1;
    if (fake.user_brk != 0xA5A5A5A5U) return -1;
    for (i = 0; i < sizeof(fake.cwd); i++) {
        if ((uint8_t)fake.cwd[i] != 0xAAU) return -1;
    }
    if (fs_chroot_set_cwd(&fake, "system:/tmp") != E_OK) return -1;
    if (strcmp(fake.cwd, "system:/tmp") != 0) return -1;
    if (fake.user_brk != 0xA5A5A5A5U) return -1;

    /* --- SYS_TIME：小缓冲截断 + NUL，不写穿 --- */
    memset(tbuf, 0xAA, sizeof(tbuf));
    memset(&f, 0, sizeof(f));
    f.eax = SYS_TIME;
    f.ebx = (uint32_t)(uintptr_t)tbuf;
    f.ecx = 4U;
    if (fs_syscall(&f) != 0) return -1;
    if (tbuf[3] != '\0') return -1;
    for (i = 4U; i < sizeof(tbuf); i++) {
        if ((uint8_t)tbuf[i] != 0xAAU) return -1;
    }

    memset(tbuf, 0xAA, sizeof(tbuf));
    f.ecx = 0U;
    if (fs_syscall(&f) != -1) return -1;
    for (i = 0; i < sizeof(tbuf); i++) {
        if ((uint8_t)tbuf[i] != 0xAAU) return -1;
    }

    /* --- SYS_FSCK：坏用户指针必须 -1 --- */
    memset(&f, 0, sizeof(f));
    f.eax = SYS_FSCK;
    f.ebx = 0xBFFFFF00U;             /* < USER_ADDR_LIMIT 但未映射 */
    f.ecx = 96U;
    r = fs_syscall(&f);
    if (r != -1) return -1;

    kprintf("%s syscall chroot/time/fsck buffer guards ok\n", AUD_TAG);
    return 0;
}

void fs_audit_selftest(void)
{
    const char *backing = fs_backing();

    if (backing == 0 || strcmp(backing, "ram0") == 0) {
        kprintln("fs: audit selftest SKIPPED (no spare block device)");
        return;
    }
    if (aud_test_listdir_paths() != 0) { (void)aud_fail(1); return; }
    if (aud_test_corrupt_images() != 0) { (void)aud_fail(2); return; }
    if (aud_test_valid_mount() != 0) { (void)aud_fail(3); return; }
    if (aud_test_hardlink() != 0) { (void)aud_fail(4); return; }
    if (aud_test_path_edges() != 0) { (void)aud_fail(5); return; }
    if (aud_test_syscall_guards() != 0) { (void)aud_fail(6); return; }
    kprintln("fs: audit selftest PASS");
}
