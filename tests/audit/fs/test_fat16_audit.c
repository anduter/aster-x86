/* =============================================================================
 * tests/audit/fs/test_fat16_audit.c - FAT16 损坏镜像 / 簇链 / BPB 审计（host）
 *
 * 编译真实 src/kernel/fs/fat16.c（不复制实现），用内存块设备注入：
 *   - 坏 BPB：reserved=0、FAT 表太小（原实现会把根目录当 FAT 读）；
 *   - 目录项首簇非法（0/1/>=cluster_count+2）不得把元数据当文件内容；
 *   - 截断链以 EOC/bad cluster 结尾时按链结束返回短计数，不越界读；
 *   - 打开读句柄时再截断（WRITE|CREAT）必须 E_BUSY，避免旧句柄读到复用簇；
 *   - 随机 BPB / 随机目录项 / 随机 FAT 的快速 fuzz（无崩溃/无死循环）。
 *
 * 运行：tools/audit-fs-unit.ps1（32 位 mingw gcc + tests/unit/shim）。
 * ============================================================================= */
#include "unit.h"
#include "include/kernel.h"
#include "include/dev.h"
#include "../../../src/kernel/fs/fat16.h"

#define SEC_SIZE      512U
#define TOTAL_SEC     16384U
#define RESERVED      1U
#define FATS          2U
#define FAT_SECS      64U
#define ROOT_ENTRIES  512U
#define ROOT_SECS     32U
#define ROOT_START    (RESERVED + FATS * FAT_SECS)
#define DATA_START    (ROOT_START + ROOT_SECS)
#define CLUSTERS      ((TOTAL_SEC - DATA_START) / 1U)

static unsigned char disk[TOTAL_SEC * SEC_SIZE];
static block_device_t dev;

/* ---- 内存块设备 + 内核符号 shim ---- */
static int mem_read(block_device_t *d, uint32_t lba, void *buf)
{
    (void)d;
    if (lba >= TOTAL_SEC) return 1;
    memcpy(buf, disk + (size_t)lba * SEC_SIZE, SEC_SIZE);
    return 0;
}

static int mem_write(block_device_t *d, uint32_t lba, const void *buf)
{
    (void)d;
    if (lba >= TOTAL_SEC) return 1;
    memcpy(disk + (size_t)lba * SEC_SIZE, buf, SEC_SIZE);
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

/* ---- 镜像构造 ---- */
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

    memset(e, 0, 32);
    for (uint32_t i = 0; i < 11U && name11[i] != '\0'; i++) e[i] = (unsigned char)name11[i];
    e[11] = attr;
    put16(e + 26, cluster);
    put32(e + 28, size);
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
    if (cluster < 2U || cluster >= CLUSTERS + 2U) return;
    disk[(DATA_START + (cluster - 2U)) * SEC_SIZE + off] = (unsigned char)value;
}

static void mkfs(void)
{
    unsigned char *sb = disk;

    memset(disk, 0, sizeof(disk));
    put16(sb + 11, SEC_SIZE);
    sb[13] = 1;                    /* 1 sector/cluster */
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

static uint32_t rng_state = 0x12345678U;
static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

int main(void)
{
    unsigned char buf[4096];
    fs_stat_t st;
    char name[32];
    int h;

    dev.name[0] = 'm'; dev.name[1] = '0'; dev.name[2] = '\0';
    dev.sector_size = SEC_SIZE;
    dev.sector_count = TOTAL_SEC;
    dev.read = mem_read;
    dev.write = mem_write;

    UNIT_BEGIN("fs/fat16-audit");

    /* 0. 基线 */
    mkfs();
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    CHECK(fat16_mounted());

    /* 1. reserved = 0：FAT 不能从引导扇区开始 */
    mkfs();
    put16(disk + 14, 0U);
    CHECK_INT(fat16_mount("m0", "fat"), -1);

    /* 2. FAT 表不足以覆盖 cluster_count+2 个表项 */
    mkfs();
    put16(disk + 22, 1U);
    CHECK_INT(fat16_mount("m0", "fat"), -1);

    /* 3. 截断链以 EOC(0xFFFF) 结尾：只返回已读出的 512 字节 */
    mkfs();
    mkent(0, "TRUNC   DAT", 0x20U, 6U, 2048U);
    fat_set_raw(6U, 0xFFFFU);
    cluster_data(6U, 0U, 'A');
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    h = fat16_open("trunc.dat", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 2048U), 512);
    CHECK_INT(buf[0], 'A');
    (void)fat16_close(h);

    /* 4. 非空文件首簇 0（cluster_lba 下溢到 FAT/根目录区）必须拒绝 */
    mkfs();
    mkent(0, "BADCLU  BIN", 0x20U, 0U, 8U);
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    CHECK_INT(fat16_open("badclu.bin", FS_OPEN_READ), -1);

    /* 5. 首簇是保留/EOC 值也必须拒绝 */
    mkfs();
    mkent(0, "BADCL2  BIN", 0x20U, 0xFFF0U, 8U);
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    CHECK_INT(fat16_open("badcl2.bin", FS_OPEN_READ), -1);

    /* 6. 最后一个合法簇 cluster_count+1 仍可读 */
    mkfs();
    mkent(0, "LAST    BIN", 0x20U, CLUSTERS + 1U, 512U);
    fat_set_raw(CLUSTERS + 1U, 0xFFFFU);
    cluster_data(CLUSTERS + 1U, 0U, 0x5AU);
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    h = fat16_open("last.bin", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 512U), 512);
    CHECK_INT(buf[0], 0x5AU);
    (void)fat16_close(h);

    /* 7. 自环链：按 size 读完就停，不挂死 */
    mkfs();
    mkent(0, "LOOP    BIN", 0x20U, 6U, 1024U);
    fat_set_raw(6U, 6U);
    cluster_data(6U, 0U, 0x77U);
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    h = fat16_open("loop.bin", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_read(h, buf, 1024U), 1024);
    (void)fat16_close(h);

    /* 8. 读句柄打开时再 WRITE|CREAT 截断：必须 E_BUSY（防旧句柄读复用簇） */
    mkfs();
    mkent(0, "BUSY    TXT", 0x20U, 2U, 5U);
    cluster_data(2U, 0U, 'x');
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    h = fat16_open("busy.txt", FS_OPEN_READ);
    CHECK(h >= 0);
    CHECK_INT(fat16_open("busy.txt", FS_OPEN_WRITE | FS_OPEN_CREAT), -1);
    CHECK_INT(fat16_close(h), 0);
    h = fat16_open("busy.txt", FS_OPEN_WRITE | FS_OPEN_CREAT);
    CHECK(h >= 0);
    CHECK_INT(fat16_close(h), 0);

    /* 9. 随机 BPB fuzz：挂载成功就做几次只读操作 */
    for (uint32_t iter = 0; iter < 800U; iter++) {
        for (uint32_t i = 0; i < SEC_SIZE; i++) disk[i] = (unsigned char)rnd();
        if ((iter & 1U) != 0U) { disk[510] = 0x55U; disk[511] = 0xAAU; }
        if ((iter & 3U) == 0U) {
            put16(disk + 11, SEC_SIZE);
            disk[13] = (unsigned char)(1U + (rnd() % 16U));
            put16(disk + 14, (unsigned int)(rnd() % 8U));
            disk[16] = (unsigned char)(rnd() % 4U);
            put16(disk + 17, (unsigned int)(rnd() % 1024U));
            put16(disk + 19, (unsigned int)(rnd() % 32768U));
            put16(disk + 22, (unsigned int)(rnd() % 128U));
            disk[510] = 0x55U; disk[511] = 0xAAU;
        }
        if (fat16_mount("m0", "fat") == 0) {
            for (uint32_t k = 0; k < 3U; k++) {
                (void)fat16_listdir("", k, name, sizeof(name), &st);
            }
            h = fat16_open("A.TXT", FS_OPEN_READ);
            if (h >= 0) { (void)fat16_read(h, buf, 64U); (void)fat16_close(h); }
        }
    }
    dev.sector_count = TOTAL_SEC;

    /* 10. 随机目录项 fuzz（固定合法 BPB） */
    mkfs();
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    for (uint32_t iter = 0; iter < 600U; iter++) {
        uint32_t idx = rnd() % ROOT_ENTRIES;
        unsigned char *e = root_entry(idx);

        for (uint32_t i = 0; i < 32U; i++) e[i] = (unsigned char)rnd();
        if (e[0] == 0x00U) e[0] = 0x01U;
        e[11] = 0x20U;
        put16(e + 26, (unsigned int)(rnd() & 0xFFFFU));
        put32(e + 28, rnd());

        /* 固定名字的入口也放随机首簇/size，保证 open/read 路径真的被执行 */
        mkent(0, "FUZZ    BIN", 0x20U,
              (unsigned int)(rnd() & 0xFFFFU), rnd());
        (void)fat16_listdir("", rnd() % 600U, name, sizeof(name), &st);
        h = fat16_open("FUZZ.BIN", FS_OPEN_READ);
        if (h >= 0) {
            (void)fat16_read(h, buf, sizeof(buf));
            (void)fat16_close(h);
        }
    }

    /* 11. 随机 FAT fuzz：合法目录项 + 随机簇链 */
    mkfs();
    CHECK_INT(fat16_mount("m0", "fat"), 0);
    for (uint32_t iter = 0; iter < 300U; iter++) {
        for (uint32_t s = 0; s < FAT_SECS; s++) {
            unsigned char *p = disk + (RESERVED + s) * SEC_SIZE;
            for (uint32_t i = 0; i < SEC_SIZE; i++) p[i] = (unsigned char)rnd();
        }
        mkent(iter % ROOT_ENTRIES, "FATFUZ  BIN", 0x20U,
              (unsigned int)(2U + (rnd() % (CLUSTERS + 1U))), 2048U);
        h = fat16_open("fatfuz.bin", FS_OPEN_READ);
        if (h >= 0) {
            (void)fat16_read(h, buf, sizeof(buf));
            (void)fat16_close(h);
        }
    }

    UNIT_END();
}
