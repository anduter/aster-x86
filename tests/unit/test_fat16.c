/* =============================================================================
 * tests/unit/test_fat16.c - FAT16 根目录/8.3/目录项解析 host 单元测试
 *
 * 被测真实源码：src/kernel/fs/fat16.c（mount / stat / listdir / open / read /
 * write / close / unlink / rename / mkdir）。
 *
 * host shim（tests/unit/shim/include/dev.h）：内存块设备 + blk_read/blk_write/
 * blk_lookup；tests/unit/shim/include/kernel.h 提供自旋锁与字符串声明，
 * 字符串实现链接真实 src/kernel/lib/string.c（-D 改名）。
 *
 * 覆盖点：
 *   - BPB 小端解析（16/32 位域、512 字节扇区、FAT16 簇数范围校验）；
 *   - 8.3 短名转换（大写、空格补位、扩展名边界、非法字符、长名拒绝）；
 *   - 目录项边界（0x00 结束符、0xE5 删除项、LFN/卷标跳过、满 512 项）；
 *   - 目录项 size/cluster 的小端布局；
 *   - 簇链读写、截断链、句柄上限、创建/截断/删除/改名。
 * ========================================================================== */
#include "unit.h"
#include "include/dev.h"
#include "../../src/kernel/fs/fat16.h"

#define SEC_SIZE      512U
#define TOTAL_SEC     16384U          /* 8MB */
#define RESERVED      1U
#define FATS          2U
#define FAT_SECS      64U
#define ROOT_ENTRIES  512U
#define ROOT_SECS     32U
#define ROOT_START    (RESERVED + FATS * FAT_SECS)   /* 129 */
#define DATA_START    (ROOT_START + ROOT_SECS)       /* 161 */
#define CLUSTERS      ((TOTAL_SEC - DATA_START) / 1U) /* 16223, 512B/cluster */

static unsigned char disk[TOTAL_SEC * SEC_SIZE];
static block_device_t dev;

/* ---- 内存块设备 ---- */
static int mem_read(block_device_t *d, uint32_t lba, void *buf)
{
    (void)d;
    if (lba >= TOTAL_SEC) return 1;
    for (uint32_t i = 0; i < SEC_SIZE; i++) {
        ((unsigned char *)buf)[i] = disk[lba * SEC_SIZE + i];
    }
    return 0;
}

static int mem_write(block_device_t *d, uint32_t lba, const void *buf)
{
    (void)d;
    if (lba >= TOTAL_SEC) return 1;
    for (uint32_t i = 0; i < SEC_SIZE; i++) {
        disk[lba * SEC_SIZE + i] = ((const unsigned char *)buf)[i];
    }
    return 0;
}

int blk_read(block_device_t *d, uint32_t lba, void *buf)
{
    if (d == 0 || lba >= d->sector_count) return 1;
    return d->read(d, lba, buf);
}

int blk_write(block_device_t *d, uint32_t lba, const void *buf)
{
    if (d == 0 || lba >= d->sector_count) return 1;
    return d->write(d, lba, buf);
}

block_device_t *blk_lookup(const char *name)
{
    if (name != 0 && name[0] == 'm' && name[1] == '0' && name[2] == '\0') return &dev;
    return 0;
}

void unit_kprintf(const char *fmt, ...) { (void)fmt; }

/* ---- 小工具 ---- */
static void put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFU);
    p[1] = (unsigned char)((v >> 8) & 0xFFU);
}

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFFU);
    p[1] = (unsigned char)((v >> 8) & 0xFFU);
    p[2] = (unsigned char)((v >> 16) & 0xFFU);
    p[3] = (unsigned char)((v >> 24) & 0xFFU);
}

static unsigned char *root_entry(uint32_t idx)
{
    return disk + (ROOT_START + idx / 16U) * SEC_SIZE + (idx % 16U) * 32U;
}

static void mkent(uint32_t idx, const char *name11, unsigned char attr,
                  unsigned int cluster, unsigned int size)
{
    unsigned char *e = root_entry(idx);

    for (uint32_t i = 0; i < 32U; i++) e[i] = 0;
    for (uint32_t i = 0; i < 11U && name11[i] != '\0'; i++) e[i] = (unsigned char)name11[i];
    e[11] = attr;
    put16(e + 26, cluster);        /* cluster_lo，小端 */
    put32(e + 28, size);           /* 文件大小，小端 */
}

static void fat_set_raw(unsigned int cluster, unsigned int value)
{
    unsigned int off = cluster * 2U;

    for (unsigned int f = 0; f < FATS; f++) {
        unsigned char *p = disk + (RESERVED + f * FAT_SECS + off / SEC_SIZE) * SEC_SIZE
                           + (off % SEC_SIZE);
        put16(p, value);
    }
}

static void cluster_data(unsigned int cluster, unsigned int off, unsigned int value)
{
    disk[(DATA_START + (cluster - 2U)) * SEC_SIZE + off] = (unsigned char)value;
}

static void mkfs(void)
{
    unsigned char *sb = disk;

    for (uint32_t i = 0; i < sizeof(disk); i++) disk[i] = 0;
    put16(sb + 11, SEC_SIZE);
    sb[13] = 1;                    /* sectors per cluster */
    put16(sb + 14, RESERVED);
    sb[16] = (unsigned char)FATS;
    put16(sb + 17, ROOT_ENTRIES);
    put16(sb + 19, (unsigned int)TOTAL_SEC);
    put16(sb + 22, FAT_SECS);
    sb[510] = 0x55U;
    sb[511] = 0xAAU;
    fat_set_raw(0, 0xFFF8U);
    fat_set_raw(1, 0xFFFFU);
}

static void mkfs_full_root(void)
{
    char nm[12];

    mkfs();
    for (uint32_t i = 0; i < ROOT_ENTRIES; i++) {
        unsigned int v = i;

        nm[0] = 'F';
        for (int k = 7; k >= 1; k--) {
            nm[k] = (char)('0' + (v % 10U));
            v /= 10U;
        }
        nm[8] = 'T'; nm[9] = 'X'; nm[10] = 'T'; nm[11] = '\0';
        mkent(i, nm, 0x20U, 0, i);
    }
}

static bool str_is(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int mount_ok(void)
{
    return fat16_mount("m0", "fat");
}

int main(void)
{
    unsigned char buf[2048];
    fs_stat_t st;
    char name[32];
    int h;
    int handles[8];
    uint32_t i;

    UNIT_BEGIN("fs/fat16");

    dev.name[0] = 'm'; dev.name[1] = '0'; dev.name[2] = '\0';
    dev.sector_size = SEC_SIZE;
    dev.sector_count = TOTAL_SEC;
    dev.read = mem_read;
    dev.write = mem_write;

    /* ================= 挂载校验 ================= */
    CHECK(fat16_mounted() == false);
    CHECK_INT(fat16_mount("nope", "fat"), -2);       /* E_NOENT */
    CHECK(fat16_mounted() == false);

    mkfs(); disk[510] = 0x00U;                       /* 坏引导签名 */
    CHECK_INT(mount_ok(), -1);
    CHECK(fat16_mounted() == false);

    mkfs(); put16(disk + 11, 256U);                  /* 扇区大小 != 512 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); disk[13] = 0;                            /* sec_per_cluster = 0 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); disk[16] = 0;                            /* fats = 0 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); put16(disk + 22, 0);                     /* fat_sectors = 0 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); put16(disk + 17, 0);                     /* root_entries = 0 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); put16(disk + 19, (unsigned int)DATA_START);  /* 总扇区不够放数据区 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); put16(disk + 19, (unsigned int)(TOTAL_SEC + 1U)); /* 超出设备扇区数 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); put16(disk + 19, 4000U);                 /* 簇数 < 4085 -> FAT12 */
    CHECK_INT(mount_ok(), -1);

    mkfs(); put16(disk + 19, 0);                     /* 32 位总扇区字段 */
    put32(disk + 32, 70000U);
    dev.sector_count = 70000U;                       /* 簇数 > 65524 -> 不是 FAT16 */
    CHECK_INT(mount_ok(), -1);
    dev.sector_count = TOTAL_SEC;

    /* ================= 正常挂载 + 目录项 ================= */
    mkfs();
    mkent(0, "HELLO   TXT", 0x20U, 2, 5);
    mkent(1, "SUB        ", 0x10U, 0, 0);
    {
        unsigned char *e = root_entry(2);            /* 删除项 */
        for (uint32_t k = 0; k < 32U; k++) e[k] = 0;
        e[0] = 0xE5U;
    }
    mkent(3, "LONGFI~1TXT", 0x0FU, 0, 0);           /* LFN：attr 低 4 位 = 0xF */
    mkent(4, "VOLUME     ", 0x08U, 0, 0);           /* 卷标 */
    mkent(5, "TINY    BIN", 0x20U, 0, 0);
    mkent(6, "BIG     DAT", 0x20U, 3, 1024);
    mkent(7, "ENDIAN  RAW", 0x20U, 5, 0x12345678U);
    mkent(8, "TRUNC   DAT", 0x20U, 6, 2048);
    mkent(9, "", 0, 0, 0);                           /* 0x00 结束符 */
    mkent(10, "GHOST   TXT", 0x20U, 0, 7);           /* 结束符之后必须不可见 */

    disk[(DATA_START + 0U) * SEC_SIZE + 0] = 'h';    /* cluster 2 = HELLO 数据 */
    disk[(DATA_START + 0U) * SEC_SIZE + 1] = 'e';
    disk[(DATA_START + 0U) * SEC_SIZE + 2] = 'l';
    disk[(DATA_START + 0U) * SEC_SIZE + 3] = 'l';
    disk[(DATA_START + 0U) * SEC_SIZE + 4] = 'o';

    /* BIG.DAT：cluster 3 -> 4 -> 0xFFFF，两簇各 512 字节 */
    fat_set_raw(3, 4);
    fat_set_raw(4, 0xFFFFU);
    for (i = 0; i < 1024U; i++) cluster_data(3U + i / 512U, i % 512U, (i * 7U + 3U) & 0xFFU);

    /* TRUNC.DAT：声明 2048 字节，但簇链在第一个簇后就结束（FAT=0） */
    fat_set_raw(6, 0);
    for (i = 0; i < 512U; i++) cluster_data(6, i, 0xA5U);

    CHECK_INT(mount_ok(), 0);
    CHECK(fat16_mounted());
    CHECK(str_is(fat16_drive(), "fat"));

    /* 8.3 名字匹配：大小写、前导 '/'、无扩展名、扩展名过长 */
    CHECK_INT(fat16_stat("hello.txt", &st), 0);
    CHECK_U32(st.type, FS_TYPE_FILE);
    CHECK_U32(st.size, 5);
    CHECK_INT(fat16_stat("HELLO.TXT", &st), 0);
    CHECK_INT(fat16_stat("Hello.Txt", &st), 0);
    CHECK_INT(fat16_stat("/hello.txt", &st), 0);
    CHECK_INT(fat16_stat("hello", &st), -2);         /* 8.3 名字必须补空格，不是前缀匹配 */
    CHECK_INT(fat16_stat("hello.txtx", &st), -1);    /* 扩展名超过 3 字符 */
    CHECK_INT(fat16_stat("ghost.txt", &st), -2);     /* 0x00 结束符之后不可见 */
    CHECK_INT(fat16_stat("sub", &st), 0);
    CHECK_U32(st.type, FS_TYPE_DIR);
    CHECK_INT(fat16_stat("", &st), -1);              /* 空名字无法转换 */

    /* 目录遍历：跳过删除项/LFN/卷标，遇到 0x00 停止 */
    CHECK_INT(fat16_listdir("", 0, name, sizeof(name), &st), 0);
    CHECK(str_is(name, "HELLO.TXT"));
    CHECK_U32(st.size, 5);
    CHECK_INT(fat16_listdir("/", 1, name, sizeof(name), &st), 0);
    CHECK(str_is(name, "SUB"));
    CHECK_U32(st.type, FS_TYPE_DIR);
    CHECK_INT(fat16_listdir(0, 2, name, sizeof(name), &st), 0);  /* rel == NULL */
    CHECK(str_is(name, "TINY.BIN"));
    CHECK_U32(st.size, 0);
    CHECK_INT(fat16_listdir("", 3, name, sizeof(name), &st), 0);
    CHECK(str_is(name, "BIG.DAT"));
    CHECK_U32(st.size, 1024);
    CHECK_INT(fat16_listdir("", 4, name, sizeof(name), &st), 0);
    CHECK(str_is(name, "ENDIAN.RAW"));
    CHECK_U32(st.size, 0x12345678U);                 /* 目录项 size 小端 */
    CHECK_INT(fat16_listdir("", 5, name, sizeof(name), &st), 0);
    CHECK(str_is(name, "TRUNC.DAT"));
    CHECK_INT(fat16_listdir("", 6, name, sizeof(name), &st), -2);

    /* from_83 的 name_max 边界：截断但始终 NUL 结尾 */
    name[0] = 'Z'; name[1] = 'Z'; name[2] = 'Z'; name[3] = 'Z'; name[4] = 'Z';
    CHECK_INT(fat16_listdir("", 0, name, 5, 0), 0);
    CHECK(str_is(name, "HELL"));
    name[0] = 'Z';
    CHECK_INT(fat16_listdir("", 0, name, 1, 0), 0);
    CHECK_INT((unsigned char)name[0], 0);
    name[0] = 'Z';
    CHECK_INT(fat16_listdir("", 0, name, 0, 0), 0);
    CHECK_INT((unsigned char)name[0], 'Z');          /* max == 0：不写 */
    CHECK_INT(fat16_listdir("", 0, 0, 0, &st), 0);   /* name == NULL */

    /* 只支持根目录；目录不能当文件打开 */
    CHECK_INT(fat16_listdir("sub", 0, name, sizeof(name), &st), -2);
    CHECK_INT(fat16_open("sub", FS_OPEN_READ), -1);
    CHECK_INT(fat16_open("nope.txt", FS_OPEN_READ), -1);
    CHECK_INT(fat16_read(-1, buf, 1), -1);
    CHECK_INT(fat16_read(99, buf, 1), -1);
    CHECK_INT(fat16_write(-1, buf, 1), -1);
    CHECK_INT(fat16_close(-1), -1);

    /* ================= 读：基本 / 跨簇 / 截断链 ================= */
    h = fat16_open("hello.txt", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 5), 5);
    CHECK_MEM(buf, "hello", 5);
    CHECK_INT(fat16_read(h, buf, 5), 0);              /* 已到 EOF */
    CHECK_INT(fat16_read(h, buf, 0), 0);
    CHECK_INT(fat16_close(h), 0);
    CHECK_INT(fat16_read(h, buf, 1), -1);             /* 关闭后句柄失效 */

    h = fat16_open("big.dat", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 1024), 1024);
    CHECK_U32(buf[0], (0U * 7U + 3U) & 0xFFU);
    CHECK_U32(buf[511], (511U * 7U + 3U) & 0xFFU);
    CHECK_U32(buf[512], (512U * 7U + 3U) & 0xFFU);
    CHECK_U32(buf[1023], (1023U * 7U + 3U) & 0xFFU);
    CHECK_INT(fat16_close(h), 0);

    h = fat16_open("trunc.dat", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 2048), 512);         /* 链在第一个簇后结束 */
    CHECK_U32(buf[0], 0xA5U);
    CHECK_INT(fat16_close(h), 0);

    /* 句柄上限 8：第 9 个打开失败，关闭后可用 */
    for (i = 0; i < 8U; i++) handles[i] = fat16_open("hello.txt", FS_OPEN_READ);
    for (i = 0; i < 8U; i++) CHECK(handles[i] >= 0);
    CHECK_INT(fat16_open("hello.txt", FS_OPEN_READ), -1);
    for (i = 0; i < 8U; i++) CHECK_INT(fat16_close(handles[i]), 0);

    /* ================= 创建 / 写 / 截断 ================= */
    h = fat16_open("new.txt", FS_OPEN_CREAT | FS_OPEN_WRITE);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 16), 0);             /* 新文件 size 0 */
    CHECK_INT(fat16_write(h, "abcdef", 6), 6);
    CHECK_INT(fat16_write(h, "x", 0), 0);
    CHECK_INT(fat16_write(h, 0, 1), -1);              /* NULL + len != 0 */
    CHECK_INT(fat16_close(h), 0);
    CHECK_INT(fat16_stat("new.txt", &st), 0);
    CHECK_U32(st.size, 6);
    {
        unsigned char *e = root_entry(2);             /* 复用了 0xE5 删除槽 */
        CHECK_MEM(e, "NEW     TXT", 11);
        CHECK_U32(e[11], 0x20U);                      /* archive */
    }
    h = fat16_open("new.txt", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 16), 6);
    CHECK_MEM(buf, "abcdef", 6);
    CHECK_INT(fat16_write(h, "z", 1), -1);            /* 只读句柄不能写 */
    CHECK_INT(fat16_close(h), 0);

    /* 对已存在文件用 WRITE 打开 = 截断，旧簇链归还 */
    {
        h = fat16_open("hello.txt", FS_OPEN_READ);
        CHECK(h >= 0);
        CHECK_INT(fat16_close(h), 0);
        h = fat16_open("hello.txt", FS_OPEN_WRITE);
        CHECK(h >= 0);
        CHECK_INT(fat16_write(h, "xy", 2), 2);
        CHECK_INT(fat16_close(h), 0);
    }
    CHECK_INT(fat16_stat("hello.txt", &st), 0);
    CHECK_U32(st.size, 2);
    h = fat16_open("hello.txt", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 8), 2);
    CHECK_MEM(buf, "xy", 2);
    CHECK_INT(fat16_close(h), 0);

    /* 非法名字：长名 / 含 '/' / 含 ':'；目标已存在且是目录 */
    CHECK_INT(fat16_open("toolongname.txt", FS_OPEN_CREAT | FS_OPEN_WRITE), -1);
    CHECK_INT(fat16_open("a/b.txt", FS_OPEN_CREAT | FS_OPEN_WRITE), -1);
    CHECK_INT(fat16_open("a:b.txt", FS_OPEN_CREAT | FS_OPEN_WRITE), -1);
    CHECK_INT(fat16_open("sub", FS_OPEN_CREAT | FS_OPEN_WRITE), -1);
    CHECK_INT(fat16_mkdir("x"), -3);                  /* E_NOSPC：只支持根目录文件 */

    /* ================= 删除 / 改名 ================= */
    h = fat16_open("new.txt", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_unlink("new.txt"), -9);           /* E_BUSY：打开中不能删 */
    CHECK_INT(fat16_close(h), 0);
    CHECK_INT(fat16_unlink("new.txt"), 0);
    CHECK_INT(fat16_stat("new.txt", &st), -2);
    CHECK_INT(fat16_unlink("new.txt"), -2);

    CHECK_INT(fat16_rename("hello.txt", "world.txt"), 0);
    CHECK_INT(fat16_stat("hello.txt", &st), -2);
    CHECK_INT(fat16_stat("world.txt", &st), 0);
    CHECK_U32(st.size, 2);
    CHECK_INT(fat16_rename("nope.txt", "x.txt"), -2);
    CHECK_INT(fat16_rename("world.txt", "BIG.DAT"), -6);   /* E_EXIST */
    CHECK_INT(fat16_rename("world.txt", "WORLD.TXT"), 0);  /* 同一 8.3 名 -> 幂等 */
    CHECK_INT(fat16_rename("world.txt", "sub/world.txt"), -1);
    CHECK_INT(fat16_rename("world.txt", "sub"), -6);        /* 目标目录已存在 */
    CHECK_INT(fat16_rename("sub", "sub2"), -1);             /* 源是目录 */
    CHECK_INT(fat16_rename("world.txt", "verylongname.txt"), -1);
    CHECK_INT(fat16_rename("/world.txt", "WORLD.TXT"), 0); /* 前导 '/' 允许 */

    /* 重新挂载失败不能破坏已有挂载点（几何/句柄保持） */
    {
        unsigned char sig = disk[510];

        disk[510] = 0x00U;
        CHECK_INT(mount_ok(), -1);
        CHECK(fat16_mounted());
        CHECK_INT(fat16_stat("world.txt", &st), 0);
        disk[510] = sig;
    }

    /* ================= 满 512 项根目录边界 ================= */
    mkfs_full_root();
    CHECK_INT(mount_ok(), 0);
    CHECK_U32((unsigned int)CLUSTERS, 16223U);
    CHECK_INT(fat16_listdir("", 0, name, sizeof(name), &st), 0);
    CHECK(str_is(name, "F0000000.TXT"));
    CHECK_U32(st.size, 0);
    name[0] = '\0';
    CHECK_INT(fat16_listdir("", 511, name, sizeof(name), &st), 0);
    CHECK(str_is(name, "F0000511.TXT"));
    CHECK_U32(st.size, 511);
    CHECK_INT(fat16_listdir("", 512, name, sizeof(name), &st), -2);
    CHECK_INT(fat16_stat("F0000511.TXT", &st), 0);
    CHECK_U32(st.size, 511);
    CHECK_INT(fat16_stat("F9999999.TXT", &st), -2);   /* 无 0x00 结束符：扫满 512 项 */

    /* 16 位总扇区字段为 0 时改读 +32 的 32 位字段 */
    mkfs();
    put16(disk + 19, 0);
    put32(disk + 32, (unsigned int)TOTAL_SEC);
    CHECK_INT(mount_ok(), 0);
    CHECK(fat16_mounted());

    UNIT_END();
}
