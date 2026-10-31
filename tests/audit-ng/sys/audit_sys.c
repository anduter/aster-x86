/*
 * ng audit-sys: ring3 user->kernel 边界探针（syscall 0/1, 10..14, 20..27,
 * 30..35, 40..47）。
 *
 * 这是一个独立的 ring3 ELF，不编进内核：host 用 tools/audit-ng-sys.ps1 编译，
 * 经 tests/s1/fat-add.ps1 写进 FAT16 磁盘镜像的副本，再由 ring3 shell
 * `spawn /mnt/disk/audit.elf` 拉起。每个用例打印一行
 *   AUDIT: PASS <name>
 *   AUDIT: FAIL <name> got=<n> want=<n>
 * 最后打印 `AUDIT: summary pass=N fail=M total=T`，host 断言 fail=0。
 *
 * 覆盖目标（只碰 QEMU 里的客户机）：
 *   - 非法用户指针：0、内核低地址 0x1000、未映射 0x41000000、跨页边界；
 *   - len 0 / 超大 / 截断（IPC msg 头、FS IO 上限、SVC 缓冲上限）；
 *   - spawn 超长 path/argv、坏 argv、8 项无 NUL 终结的 argv、坏 &pid、
 *     wait/kill 非法 pid、并发 spawn/kill/wait（storm）；
 *   - IPC 坏 ticket、depth 越界、队列满/关闭/超时、ncaps 语义边界；
 *   - FS 长路径、坏 fd、listdir 小 max / 超大 max（canary 检查栈缓冲写穿）、
 *     stat 坏指针；
 *   - 显示/会话机制面：SYS_SHM_ALLOC/MAP 越界、SYS_FB_MAP 坏指针；
 *   - 崩溃对照 mode `crash`：ring3 空指针解引用 —— 已知限制（trap.c 对
 *     用户异常仍 panic），host 期望看到 PANIC: vec=0xe，作为设计级记录。
 *
 * 子模式（由 storm 用例 spawn 自己）：
 *   audit.elf child <id> block   建端口后阻塞在 recv（等 kill）
 *   audit.elf child <id> quick   立刻 exit(0)
 */
#include "types.h"

/* ---- ABI 常量（docs/abi/abi-1.0.md；用户程序不 include 内核头）---------- */
#define SYS_WRITE 0
#define SYS_EXIT 1
#define SYS_PORT_CREATE 10
#define SYS_PORT_SEND 11
#define SYS_PORT_RECV 12
#define SYS_PORT_REPLY 13
#define SYS_PORT_CLOSE 14
#define SYS_OPEN 20
#define SYS_READ 21
#define SYS_WRITE_FD 22
#define SYS_CLOSE 23
#define SYS_STAT 24
#define SYS_LISTDIR 25
#define SYS_MKDIR 26
#define SYS_UNLINK 27
#define SYS_LOGIN 31
#define SYS_DMESG 32
#define SYS_LOG_WRITE 33
#define SYS_PKG_LIST 34
#define SYS_PKG_VERIFY 35
#define SYS_SPAWN 40
#define SYS_WAIT 41
#define SYS_GETPID 42
#define SYS_KILL 43
#define SYS_GETPPID 44
#define SYS_SHM_ALLOC 45
#define SYS_SHM_MAP 46
#define SYS_FB_MAP 47

/* 与内核一致的错误码（负 errno）。 */
#define E_PERM (-1)
#define E_NOENT (-2)
#define E_SRCH (-3)
#define E_CHILD (-10)
#define E_AGAIN (-11)
#define E_NOMEM (-12)
#define E_ACCES (-13)
#define E_FAULT (-14)
#define E_BUSY (-16)
#define E_EXIST (-17)
#define E_ISDIR (-21)
#define E_INVAL (-22)
#define E_NOSPC (-28)
#define E_NAMETOOLONG (-36)
#define E_MSGSIZE (-90)
#define E_TIMEDOUT (-110)

/* IPC / FS / SHM 上限（ABI 1.0）。 */
#define IPC_HDR 16u
#define IPC_MSG_MAX 4112u
#define FS_IO_MAX 512u
#define SHM_PAGES_MAX 1024u

#define O_RDONLY 0x000u
#define O_WRONLY 0x001u
#define O_RDWR 0x002u
#define O_CREAT 0x040u
#define O_TRUNC 0x200u

#define ARRAY_N(a) (sizeof(a) / sizeof((a)[0]))

struct audit_hdr {
    u32 type;
    u32 len;
    u32 from;
    u32 flags;
};

struct audit_stat {
    u32 type;
    u32 mode;
    u64 size;
    u64 ino;
};

struct audit_dirent {
    u32 type;
    u32 name_len;
    u64 size;
    char name[64];
};

struct audit_fb_info {
    u64 base;
    u64 phys;
    u64 size;
    u32 pitch;
    u32 bpp;
    u32 width;
    u32 height;
    u32 flags;
    u32 reserved;
};

_Static_assert(sizeof(struct audit_hdr) == 16, "ipc hdr");
_Static_assert(sizeof(struct audit_stat) == 24, "vfs_stat");
_Static_assert(sizeof(struct audit_dirent) == 80, "vfs_dirent");
_Static_assert(sizeof(struct audit_fb_info) == 48, "ds_fb_info");

/* ---- 静态缓冲（.bss；16 KiB 用户栈之外）-------------------------------- */
static u8 g_buf[64];
static u8 g_big[8192] __attribute__((aligned(4096)));
static char g_path[300];
static char g_path2[300];
static char g_arg127[128];
static u8 g_ents_raw[8 * 80 + 16];
static struct audit_stat g_st;
static struct audit_fb_info g_fb;
static u32 g_pass;
static u32 g_fail;
static u32 g_sleep_port;

static const char k_ro_msg[] = "audit-ro\n";

/* ------------------------------------------------------------------------- */
/* 基础工具                                                                   */
/* ------------------------------------------------------------------------- */
static i64 sys4(i64 nr, i64 a1, i64 a2, i64 a3, i64 a4)
{
    register i64 r10 __asm__("r10") = a4;
    i64 ret;

    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static i64 sys3(i64 nr, i64 a1, i64 a2, i64 a3)
{
    return sys4(nr, a1, a2, a3, 0);
}

static i64 sys2(i64 nr, i64 a1, i64 a2)
{
    return sys4(nr, a1, a2, 0, 0);
}

static u32 str_len(const char *s)
{
    u32 n = 0;

    while (s != NULL && s[n] != '\0') {
        n++;
    }
    return n;
}

static int str_eq(const char *a, const char *b)
{
    u32 i;

    for (i = 0;; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
}

static void put_str(const char *s)
{
    u32 n = str_len(s);

    if (n != 0) {
        (void)sys3(SYS_WRITE, (i64)(usize)s, (i64)n, 0);
    }
}

static int put_s64(char *dst, u32 cap, i64 v)
{
    char tmp[24];
    u32 n = 0;
    u32 i;
    u32 pos = 0;
    int neg = (v < 0);
    u64 u = neg ? (u64)(-(v + 1)) + 1u : (u64)v;

    if (u == 0) {
        tmp[n++] = '0';
    } else {
        while (u != 0 && n < (u32)sizeof(tmp)) {
            tmp[n++] = (char)('0' + (int)(u % 10u));
            u /= 10u;
        }
    }
    if (neg) {
        tmp[n++] = '-';
    }
    for (i = n; i > 0; i--) {
        if (pos + 1u < cap) {
            dst[pos++] = tmp[i - 1u];
        }
    }
    dst[pos] = '\0';
    return (int)pos;
}

static void result(const char *name, int ok, i64 got, i64 want)
{
    char num[28];

    if (ok) {
        g_pass++;
        put_str("AUDIT: PASS ");
        put_str(name);
        put_str("\n");
        return;
    }
    g_fail++;
    put_str("AUDIT: FAIL ");
    put_str(name);
    put_str(" got=");
    (void)put_s64(num, (u32)sizeof(num), got);
    put_str(num);
    put_str(" want=");
    (void)put_s64(num, (u32)sizeof(num), want);
    put_str(num);
    put_str("\n");
}

static void expect(const char *name, i64 got, i64 want)
{
    result(name, got == want, got, want);
}

static void expect_true(const char *name, int cond, i64 got)
{
    result(name, cond != 0, got, 1);
}

static void section(const char *name)
{
    put_str("AUDIT: section ");
    put_str(name);
    put_str("\n");
}

/* 阻塞一小段（用 recv 超时让出 CPU；没有 nanosleep 系统调用）。 */
static void sleep_ticks(u64 ticks)
{
    u8 tmp[8];

    if (g_sleep_port != 0) {
        (void)sys4(SYS_PORT_RECV, (i64)g_sleep_port, (i64)(usize)tmp,
                   (i64)sizeof(tmp), (i64)ticks);
    }
}

/* ------------------------------------------------------------------------- */
/* 打开/关闭辅助                                                              */
/* ------------------------------------------------------------------------- */
static i64 open_file(const char *path, u32 flags, u32 mode)
{
    return sys3(SYS_OPEN, (i64)(usize)path, (i64)flags, (i64)mode);
}

/* ------------------------------------------------------------------------- */
/* child 子模式                                                               */
/* ------------------------------------------------------------------------- */
static u32 parse_u32(const char *s)
{
    u32 v = 0;

    while (s != NULL && *s >= '0' && *s <= '9') {
        v = v * 10u + (u32)(*s - '0');
        s++;
    }
    return v;
}

static u64 parse_u64(const char *s)
{
    u64 v = 0;

    while (s != NULL && *s >= '0' && *s <= '9') {
        v = v * 10u + (u64)(*s - '0');
        s++;
    }
    return v;
}

static int child_main(long argc, char **argv)
{
    u32 id = (argc >= 3) ? parse_u32(argv[2]) : 0;
    const char *mode = (argc >= 4) ? argv[3] : "quick";
    int block = (str_eq(mode, "block") || str_eq(mode, "shm") ||
                 str_eq(mode, "fb"));
    char num[24];

    put_str("AUDIT: child up id=");
    (void)put_s64(num, (u32)sizeof(num), (i64)id);
    put_str(num);
    put_str(" pid=");
    (void)put_s64(num, (u32)sizeof(num), sys3(SYS_GETPID, 0, 0, 0));
    put_str(num);
    put_str(block ? " block\n" : " quick\n");

    if (str_eq(mode, "shm") && argc >= 5) {
        u64 handle = parse_u64(argv[4]);
        u64 va = 0;
        i64 rc = sys3(SYS_SHM_MAP, (i64)handle, (i64)(usize)&va, 0);

        if (rc == 0) {
            *(volatile u32 *)(usize)va = 0x5A5A5A5Au;
            put_str("AUDIT: child shm id=");
            (void)put_s64(num, (u32)sizeof(num), (i64)id);
            put_str(num);
            put_str(" va=0x5a5a5a5a\n");
        } else {
            put_str("AUDIT: child shm map failed\n");
        }
    } else if (str_eq(mode, "fb")) {
        u64 va = 0;
        struct audit_fb_info info;
        i64 rc = sys3(SYS_FB_MAP, (i64)(usize)&va, (i64)(usize)&info, 0);

        if (rc == 0) {
            put_str("AUDIT: child fb id=");
            (void)put_s64(num, (u32)sizeof(num), (i64)id);
            put_str(num);
            put_str(" mapped\n");
        } else {
            put_str("AUDIT: child fb map failed\n");
        }
    }

    if (block) {
        i64 t = sys3(SYS_PORT_CREATE, 4, 0, 0);
        u8 tmp[8];

        if (t <= 0) {
            return 1;
        }
        /* 永久阻塞在 recv：等父进程 kill，用于验证 waiter 取消路径。 */
        (void)sys4(SYS_PORT_RECV, t, (i64)(usize)tmp, (i64)sizeof(tmp),
                   (i64)~0ULL);
        return 2;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* syscall 0 / 1                                                              */
/* ------------------------------------------------------------------------- */
static void test_write(void)
{
    section("write");

    g_buf[0] = 'o';
    g_buf[1] = 'k';
    g_buf[2] = '\n';
    g_buf[3] = 0;

    expect("write.valid", sys3(SYS_WRITE, (i64)(usize)g_buf, 3, 0), 3);
    expect("write.unaligned", sys3(SYS_WRITE, (i64)(usize)(g_buf + 1), 2, 0), 2);
    expect("write.rodata_readable",
           sys3(SYS_WRITE, (i64)(usize)k_ro_msg, (i64)str_len(k_ro_msg), 0),
           (i64)str_len(k_ro_msg));
    /* rdi=0 是 WP1-3 的诊断模式（打印一行，返回 0）。 */
    expect("write.diag_rdi0", sys3(SYS_WRITE, 0, 4, 0), 0);
    expect("write.len0", sys3(SYS_WRITE, (i64)(usize)g_buf, 0, 0), E_INVAL);
    expect("write.len257", sys3(SYS_WRITE, (i64)(usize)g_buf, 257, 0), E_INVAL);
    expect("write.len_u64max",
           sys3(SYS_WRITE, (i64)(usize)g_buf, (i64)~0ULL, 0), E_INVAL);
    expect("write.kernel_ptr", sys3(SYS_WRITE, 0x1000, 4, 0), E_FAULT);
    expect("write.unmapped", sys3(SYS_WRITE, 0x41000000, 4, 0), E_FAULT);
    /* 0x40103FFC 在用户栈最后一页内，跨到 0x40104000（未映射）。 */
    expect("write.cross_stack_top",
           sys3(SYS_WRITE, 0x40103FFC, 8, 0), E_FAULT);
    expect("write.bad_syscall", sys3(999, 0, 0, 0), -1);
}

/* ------------------------------------------------------------------------- */
/* IPC 10..14                                                                 */
/* ------------------------------------------------------------------------- */
static void hdr_init(struct audit_hdr *h, u32 type, u32 len, u32 from, u32 flags)
{
    h->type = type;
    h->len = len;
    h->from = from;
    h->flags = flags;
}

static void test_ipc(void)
{
    struct audit_hdr h;
    struct audit_hdr *got;
    i64 t1;
    i64 t2;
    i64 t3;
    i64 t4;
    i64 held[32];
    u32 i;
    u32 made;
    i64 rc;

    section("ipc");

    /* create 参数边界 */
    t1 = sys3(SYS_PORT_CREATE, 0, 0, 0); /* 0 = 默认 depth */
    expect_true("ipc.create_default", t1 > 0 && t1 <= 0xFFFFFFFFLL, t1);
    expect("ipc.create_depth65", sys3(SYS_PORT_CREATE, 65, 0, 0), E_INVAL);
    expect("ipc.create_depth_u32max",
           sys3(SYS_PORT_CREATE, 0xFFFFFFFFLL, 0, 0), E_INVAL);
    expect("ipc.create_depth_u64max",
           sys3(SYS_PORT_CREATE, (i64)~0ULL, 0, 0), E_INVAL);
    t2 = sys3(SYS_PORT_CREATE, 64, 0, 0);
    expect_true("ipc.create_depth64", t2 > 0, t2);

    /* 坏 ticket / 坏指针 / 坏长度：先做指针校验，再查 ticket。 */
    hdr_init(&h, 1, 0, 0, 0);
    expect("ipc.send_bad_ticket",
           sys4(SYS_PORT_SEND, 0xDEADBEEFLL, (i64)(usize)&h, IPC_HDR, 0), -9);
    expect("ipc.send_bad_ptr",
           sys4(SYS_PORT_SEND, t1, 0x1000, IPC_HDR, 0), E_FAULT);
    expect("ipc.send_null_ptr",
           sys4(SYS_PORT_SEND, t1, 0, IPC_HDR, 0), E_FAULT);
    expect("ipc.send_len15",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, 15, 0), E_INVAL);
    expect("ipc.send_len0",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, 0, 0), E_INVAL);
    expect("ipc.send_len4113",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, 4113, 0), E_MSGSIZE);
    expect("ipc.send_len_max",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, (i64)~0ULL, 0), E_INVAL);
    expect("ipc.send_bad_handle_high",
           sys4(SYS_PORT_SEND, 0x100000000LL, (i64)(usize)&h, IPC_HDR, 0), -9);

    /* 消息头校验。 */
    hdr_init(&h, 1, 1, 0, 0); /* hdr.len+16 != 16 */
    expect("ipc.send_hdr_len_mismatch",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), E_INVAL);
    hdr_init(&h, 1, 0, 0, 1); /* flags 必须为 0 */
    expect("ipc.send_hdr_flags",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), E_INVAL);
    hdr_init(&h, 1, 0, 0xDEADBEEF, 0); /* from 不是存活 ticket */
    expect("ipc.send_hdr_bad_from",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), -9);

    /* 正常 send/recv（recv 返回消息总长，不是 payload 长度）。 */
    hdr_init(&h, 0x55, 0, 0, 0);
    expect("ipc.send_ok", sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.recv_ok", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);
    got = (struct audit_hdr *)g_big;
    expect_true("ipc.recv_payload", got->type == 0x55 && got->len == 0,
                (i64)got->type);

    /* recv 参数边界。 */
    expect("ipc.recv_max0", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, 0, 0),
           E_INVAL);
    expect("ipc.recv_bad_ptr",
           sys4(SYS_PORT_RECV, t1, 0x1000, IPC_HDR, 0), E_FAULT);
    /* 超大 max 会被钳到 IPC_MSG_MAX；空队列 + timeout=0 -> EAGAIN。 */
    expect("ipc.recv_max_huge",
           sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, 0xFFFFFFFFLL, 0), E_AGAIN);

    /* depth=1：队列满、close 之后的行为。 */
    t3 = sys3(SYS_PORT_CREATE, 1, 0, 0);
    expect_true("ipc.create_depth1", t3 > 0, t3);
    hdr_init(&h, 2, 0, 0, 0);
    expect("ipc.fill", sys4(SYS_PORT_SEND, t3, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.full_eagain",
           sys4(SYS_PORT_SEND, t3, (i64)(usize)&h, IPC_HDR, 0), E_AGAIN);
    /* 阻塞 send 超时：必须让出 CPU 后按 deadline 返回 ETIMEDOUT。 */
    expect("ipc.full_timeout",
           sys4(SYS_PORT_SEND, t3, (i64)(usize)&h, IPC_HDR, 20000000), E_TIMEDOUT);
    expect("ipc.drain", sys4(SYS_PORT_RECV, t3, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);
    expect("ipc.close", sys3(SYS_PORT_CLOSE, t3, 0, 0), 0);
    expect("ipc.recv_after_close",
           sys4(SYS_PORT_RECV, t3, (i64)(usize)g_big, IPC_HDR, 0), -9);
    expect("ipc.close_twice", sys3(SYS_PORT_CLOSE, t3, 0, 0), -9);

    /* 空队列 recv 超时。 */
    t4 = sys3(SYS_PORT_CREATE, 1, 0, 0);
    expect_true("ipc.create_empty", t4 > 0, t4);
    expect("ipc.recv_timeout",
           sys4(SYS_PORT_RECV, t4, (i64)(usize)g_big, IPC_HDR, 20000000),
           E_TIMEDOUT);
    expect("ipc.close_empty", sys3(SYS_PORT_CLOSE, t4, 0, 0), 0);

    /* reply：坏 ticket / 正常入队。 */
    expect("ipc.reply_bad_ticket",
           sys4(SYS_PORT_REPLY, 0xDEADBEEFLL, (i64)(usize)&h, IPC_HDR, 0), -9);
    hdr_init(&h, 3, 0, 0, 0);
    expect("ipc.reply_ok", sys4(SYS_PORT_REPLY, t1, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.reply_recv", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);

    /* 端口池上限：持有所有能创建的端口（不 close），下一个必须是 ENOSPC。 */
    made = 0;
    rc = 0;
    for (i = 0; i < 32u; i++) {
        rc = sys3(SYS_PORT_CREATE, 1, 0, 0);
        if (rc <= 0) {
            break;
        }
        held[made] = rc;
        made++;
    }
    expect_true("ipc.port_pool_cycle", made >= 1 && rc != 0, rc);
    expect("ipc.port_pool_enospc", rc, E_NOSPC);
    for (i = 0; i < made; i++) {
        (void)sys3(SYS_PORT_CLOSE, held[i], 0, 0);
    }

    /* 从 ticket（hdr.from）跨端口发送：from 必须是存活 PORT 句柄。 */
    hdr_init(&h, 4, 0, (u32)t2, 0);
    expect("ipc.send_from_ticket",
           sys4(SYS_PORT_SEND, t1, (i64)(usize)&h, IPC_HDR, 0), 0);
    expect("ipc.send_from_recv", sys4(SYS_PORT_RECV, t1, (i64)(usize)g_big, IPC_HDR, 0),
           IPC_HDR);

    /* 最大消息（16 头 + 4096 payload = 4112）与短 recv 缓冲。 */
    {
        struct audit_hdr *big = (struct audit_hdr *)(void *)g_big;
        u32 k;

        for (k = 0; k < 4112u; k++) {
            g_big[k] = 0;
        }
        big->type = 0x66;
        big->len = 4096;
        big->from = 0;
        big->flags = 0;
        expect("ipc.send_max_msg", sys4(SYS_PORT_SEND, t2, (i64)(usize)g_big, 4112, 0),
               0);
        expect("ipc.recv_max_msg", sys4(SYS_PORT_RECV, t2, (i64)(usize)g_big, 4112, 0),
               4112);
        expect_true("ipc.recv_max_msg_hdr",
                    big->type == 0x66 && big->len == 4096, (i64)big->type);

        /* 短缓冲：消息总长 24，max=16 时返回总长、只拷前 16 字节。 */
        big->type = 0x77;
        big->len = 8;
        big->from = 0;
        big->flags = 0;
        g_big[16] = 0xAA;
        g_big[17] = 0xBB;
        expect("ipc.send_short_msg", sys4(SYS_PORT_SEND, t2, (i64)(usize)g_big, 24, 0),
               0);
        expect("ipc.recv_short_buf", sys4(SYS_PORT_RECV, t2, (i64)(usize)g_big, 16, 0),
               24);
        expect_true("ipc.recv_short_hdr",
                    ((struct audit_hdr *)(void *)g_big)->type == 0x77,
                    (i64)((struct audit_hdr *)(void *)g_big)->type);
    }

    /* close 时有排队消息：ring 里的消息必须被释放，recv 变 EBADF。 */
    {
        i64 t5 = sys3(SYS_PORT_CREATE, 2, 0, 0);

        expect_true("ipc.close_queued_port", t5 > 0, t5);
        if (t5 > 0) {
            hdr_init(&h, 9, 0, 0, 0);
            expect("ipc.close_queued_send1",
                   sys4(SYS_PORT_SEND, t5, (i64)(usize)&h, IPC_HDR, 0), 0);
            expect("ipc.close_queued_send2",
                   sys4(SYS_PORT_SEND, t5, (i64)(usize)&h, IPC_HDR, 0), 0);
            expect("ipc.close_queued_close", sys3(SYS_PORT_CLOSE, t5, 0, 0), 0);
            expect("ipc.close_queued_recv",
                   sys4(SYS_PORT_RECV, t5, (i64)(usize)g_big, IPC_HDR, 0), -9);
        }
    }

    (void)sys3(SYS_PORT_CLOSE, t1, 0, 0);
    (void)sys3(SYS_PORT_CLOSE, t2, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* FS 20..27                                                                  */
/* ------------------------------------------------------------------------- */
static void test_fs(void)
{
    i64 fd;
    i64 rc;
    u32 i;
    u32 *canary;
    char num[24];

    section("fs");

    /* 路径指针/长度。 */
    expect("fs.open_bad_ptr", open_file((const char *)0x1000, O_RDONLY, 0),
           E_FAULT);
    expect("fs.open_null_ptr", open_file((const char *)0, O_RDONLY, 0), E_FAULT);
    expect("fs.open_unmapped", open_file((const char *)0x41000000, O_RDONLY, 0),
           E_FAULT);
    expect("fs.open_bad_flags",
           open_file("/bin/shell", 0x80000000u, 0), E_INVAL);
    expect("fs.open_missing", open_file("/bin/nosuch", O_RDONLY, 0), E_NOENT);

    /* 251 字节单组件路径（> VFS_NAME_MAX）：copyin_str 能装下，VFS 拒绝。 */
    g_path[0] = '/';
    for (i = 1; i < 251u; i++) {
        g_path[i] = 'a';
    }
    g_path[251] = '\0';
    rc = open_file(g_path, O_RDONLY, 0);
    expect("fs.open_long_component", rc, E_NAMETOOLONG);

    /* 300 字节路径：copyin_str 在 255 字节截断 -> ENAMETOOLONG。 */
    for (i = 0; i < 299u; i++) {
        g_path2[i] = 'b';
    }
    g_path2[299] = '\0';
    expect("fs.open_overlong_path", open_file(g_path2, O_RDONLY, 0),
           E_NAMETOOLONG);

    /* 正常 open/read/close + 坏 fd/坏指针/坏长度。 */
    fd = open_file("/bin/shell", O_RDONLY, 0);
    expect_true("fs.open_shell", fd >= 3, fd);
    if (fd >= 3) {
        expect("fs.read_ok", sys3(SYS_READ, fd, (i64)(usize)g_big, 64), 64);
        expect("fs.read_bad_ptr", sys3(SYS_READ, fd, 0x1000, 16), E_FAULT);
        expect("fs.read_len0", sys3(SYS_READ, fd, (i64)(usize)g_big, 0), E_INVAL);
        expect("fs.read_len513", sys3(SYS_READ, fd, (i64)(usize)g_big, 513),
               E_INVAL);
        expect("fs.read_len_huge",
               sys3(SYS_READ, fd, (i64)(usize)g_big, (i64)~0ULL), E_INVAL);
        expect("fs.read_cross_stack_top",
               sys3(SYS_READ, fd, 0x40103FFC, 8), E_FAULT);
        expect("fs.close_ok", sys3(SYS_CLOSE, fd, 0, 0), 0);
        expect("fs.close_twice", sys3(SYS_CLOSE, fd, 0, 0), -9);
        expect("fs.read_closed", sys3(SYS_READ, fd, (i64)(usize)g_big, 16), -9);
    }
    expect("fs.read_bad_fd", sys3(SYS_READ, 0xFFFFFFFFLL, (i64)(usize)g_big, 16),
           -9);
    expect("fs.write_bad_fd", sys3(SYS_WRITE_FD, 0xFFFFFFFFLL, (i64)(usize)g_buf, 4),
           -9);

    /* 创建/写入/回读/stat/listdir/unlink。 */
    fd = open_file("/tmp/audit.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    expect_true("fs.create", fd >= 3, fd);
    if (fd >= 3) {
        g_buf[0] = 'h';
        g_buf[1] = 'e';
        g_buf[2] = 'l';
        g_buf[3] = 'l';
        g_buf[4] = 'o';
        expect("fs.write_ok", sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 5), 5);
        expect("fs.write_bad_ptr", sys3(SYS_WRITE_FD, fd, 0x1000, 5), E_FAULT);
        expect("fs.write_len0", sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 0),
               E_INVAL);
        expect("fs.write_len513", sys3(SYS_WRITE_FD, fd, (i64)(usize)g_buf, 513),
               E_INVAL);
        expect("fs.write_cross_stack_top",
               sys3(SYS_WRITE_FD, fd, 0x40103FFC, 8), E_FAULT);
        expect("fs.close_created", sys3(SYS_CLOSE, fd, 0, 0), 0);
    }

    fd = open_file("/tmp/audit.txt", O_RDONLY, 0);
    expect_true("fs.reopen", fd >= 3, fd);
    if (fd >= 3) {
        for (i = 0; i < 16u; i++) {
            g_big[i] = 0;
        }
        expect("fs.read_back", sys3(SYS_READ, fd, (i64)(usize)g_big, 5), 5);
        expect_true("fs.read_content",
                    g_big[0] == 'h' && g_big[1] == 'e' && g_big[2] == 'l' &&
                        g_big[3] == 'l' && g_big[4] == 'o',
                    (i64)g_big[0]);
        expect("fs.read_eof", sys3(SYS_READ, fd, (i64)(usize)g_big, 5), 0);
        (void)sys3(SYS_CLOSE, fd, 0, 0);
    }

    /* stat 坏指针 + 正常。 */
    expect("fs.stat_ok", sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt",
                               (i64)(usize)&g_st, 0), 0);
    expect_true("fs.stat_size", g_st.size == 5, (i64)g_st.size);
    expect("fs.stat_bad_out",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt", 0x1000, 0), E_FAULT);
    expect("fs.stat_null_out",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt", 0, 0), E_FAULT);
    expect("fs.stat_bad_path", sys3(SYS_STAT, 0x1000, (i64)(usize)&g_st, 0),
           E_FAULT);
    expect("fs.stat_cross_stack_top",
           sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt", 0x40103FFE, 0), E_FAULT);
    expect("fs.stat_missing", sys3(SYS_STAT, (i64)(usize)"/nosuch",
                                   (i64)(usize)&g_st, 0), E_NOENT);
    expect("fs.stat_missing_out_null",
           sys3(SYS_STAT, (i64)(usize)"/nosuch", 0, 0), E_FAULT);

    /* listdir：max=1 只写一条；max=2^32-1 被钳到 8，不能写穿栈缓冲。 */
    for (i = 0; i < (u32)sizeof(g_ents_raw); i++) {
        g_ents_raw[i] = 0xA5;
    }
    expect("fs.listdir_one", sys3(SYS_LISTDIR, (i64)(usize)"/",
                                  (i64)(usize)g_ents_raw, 1), 1);
    canary = (u32 *)(void *)(g_ents_raw + 8 * 80);
    expect_true("fs.listdir_one_canary", *canary == 0xA5A5A5A5u, (i64)*canary);
    rc = sys3(SYS_LISTDIR, (i64)(usize)"/", (i64)(usize)g_ents_raw,
              0xFFFFFFFFLL);
    expect_true("fs.listdir_huge", rc >= 0 && rc <= 8, rc);
    expect_true("fs.listdir_huge_canary", *canary == 0xA5A5A5A5u, (i64)*canary);
    expect("fs.listdir_bad_path",
           sys3(SYS_LISTDIR, 0x1000, (i64)(usize)g_ents_raw, 1), E_FAULT);
    expect("fs.listdir_bad_buf",
           sys3(SYS_LISTDIR, (i64)(usize)"/", 0x1000, 1), E_FAULT);
    expect("fs.listdir_cross_stack_top",
           sys3(SYS_LISTDIR, (i64)(usize)"/", 0x40103FF0, 2), E_FAULT);
    expect("fs.listdir_null_buf", sys3(SYS_LISTDIR, (i64)(usize)"/", 0, 1),
           E_FAULT);
    /* max=0 目前走 EFAULT 而不是 EINVAL：记录为错误码 nit（不崩溃）。 */
    rc = sys3(SYS_LISTDIR, (i64)(usize)"/", (i64)(usize)g_ents_raw, 0);
    expect_true("fs.listdir_max0_nonpanic", rc < 0, rc);

    /* mkdir / unlink。 */
    expect("fs.mkdir_ok", sys3(SYS_MKDIR, (i64)(usize)"/tmp/audit-dir", 0755, 0),
           0);
    expect("fs.mkdir_exists", sys3(SYS_MKDIR, (i64)(usize)"/tmp/audit-dir", 0755, 0),
           E_EXIST);
    expect("fs.unlink_dir", sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-dir", 0, 0),
           E_ISDIR);
    expect("fs.mkdir_bad_path", sys3(SYS_MKDIR, 0x1000, 0755, 0), E_FAULT);
    expect("fs.unlink_missing",
           sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit-nosuch", 0, 0), E_NOENT);
    expect("fs.unlink_bad_path", sys3(SYS_UNLINK, 0x1000, 0, 0), E_FAULT);
    expect("fs.unlink_ok", sys3(SYS_UNLINK, (i64)(usize)"/tmp/audit.txt", 0, 0),
           0);
    expect("fs.stat_after_unlink", sys3(SYS_STAT, (i64)(usize)"/tmp/audit.txt",
                                        (i64)(usize)&g_st, 0), E_NOENT);

    put_str("AUDIT: info fs.stat_size=");
    (void)put_s64(num, (u32)sizeof(num), (i64)g_st.size);
    put_str(num);
    put_str("\n");
}

/* ------------------------------------------------------------------------- */
/* SVC 31..35                                                                 */
/* ------------------------------------------------------------------------- */
static void test_svc(void)
{
    i64 rc;

    section("svc");

    expect("svc.login_bad_name_ptr",
           sys2(SYS_LOGIN, 0x1000, (i64)(usize)"x"), E_FAULT);
    expect("svc.login_bad_pass_ptr",
           sys2(SYS_LOGIN, (i64)(usize)"root", 0x1000), E_FAULT);
    expect("svc.login_empty", sys2(SYS_LOGIN, (i64)(usize)"", (i64)(usize)""),
           E_INVAL);
    expect("svc.login_wrong_pass",
           sys2(SYS_LOGIN, (i64)(usize)"root", (i64)(usize)"wrong"), E_ACCES);
    for (u32 i = 0; i < (u32)sizeof(g_arg127) - 1u; i++) {
        g_arg127[i] = 'n';
    }
    g_arg127[(u32)sizeof(g_arg127) - 1u] = '\0';
    expect("svc.login_long_name",
           sys2(SYS_LOGIN, (i64)(usize)g_arg127, (i64)(usize)"x"),
           E_NAMETOOLONG);

    expect("svc.dmesg_bad_ptr", sys2(SYS_DMESG, 0x1000, 16), E_FAULT);
    expect("svc.dmesg_null_ptr", sys2(SYS_DMESG, 0, 16), E_FAULT);
    expect("svc.dmesg_max0", sys2(SYS_DMESG, (i64)(usize)g_big, 0), E_INVAL);
    expect("svc.dmesg_max4097", sys2(SYS_DMESG, (i64)(usize)g_big, 4097), E_INVAL);
    rc = sys2(SYS_DMESG, (i64)(usize)g_big, 64);
    expect_true("svc.dmesg_ok", rc >= 0 && rc <= 64, rc);

    expect("svc.log_bad_tag", sys2(SYS_LOG_WRITE, 0x1000, (i64)(usize)"x"),
           E_FAULT);
    expect("svc.log_bad_msg",
           sys2(SYS_LOG_WRITE, (i64)(usize)"audit", 0x1000), E_FAULT);
    expect("svc.log_empty_tag",
           sys2(SYS_LOG_WRITE, (i64)(usize)"", (i64)(usize)"x"), E_INVAL);
    expect("svc.log_ok",
           sys2(SYS_LOG_WRITE, (i64)(usize)"audit", (i64)(usize)"syscall"), 0);

    expect("svc.pkg_list_bad_ptr", sys2(SYS_PKG_LIST, 0x1000, 16), E_FAULT);
    expect("svc.pkg_list_max0", sys2(SYS_PKG_LIST, (i64)(usize)g_big, 0), E_INVAL);
    expect("svc.pkg_list_max600",
           sys2(SYS_PKG_LIST, (i64)(usize)g_big, 600), E_INVAL);
    rc = sys2(SYS_PKG_LIST, (i64)(usize)g_big, 512);
    expect_true("svc.pkg_list_ok", rc >= 0, rc);

    expect("svc.pkg_verify_bad_ptr", sys3(SYS_PKG_VERIFY, 0x1000, 0, 0), E_FAULT);
    expect("svc.pkg_verify_ok", sys3(SYS_PKG_VERIFY, (i64)(usize)"hello", 0, 0),
           0);
    expect("svc.pkg_verify_missing",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"nosuch", 0, 0), E_NOENT);
    expect("svc.pkg_verify_traversal",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"../etc", 0, 0), E_INVAL);
    expect("svc.pkg_verify_slash",
           sys3(SYS_PKG_VERIFY, (i64)(usize)"hello/../x", 0, 0), E_INVAL);
}

/* ------------------------------------------------------------------------- */
/* PROC 40..44                                                                */
/* ------------------------------------------------------------------------- */
static void test_proc(char **argv)
{
    char *self = (argv != NULL && argv[0] != NULL) ? argv[0] : "/mnt/disk/audit.elf";
    char *av[9];
    char *av8[8];
    char *av_bad[4];
    char hstr[24];
    char num[24];
    u32 pid = 0;
    int status = 0;
    i64 rc;
    u32 pids[4];
    u32 i;

    section("proc");

    rc = sys3(SYS_GETPID, 0, 0, 0);
    expect_true("proc.getpid", rc > 0, rc);
    rc = sys3(SYS_GETPPID, 0, 0, 0);
    expect_true("proc.getppid", rc > 0, rc);

    expect("proc.wait_pid0", sys3(SYS_WAIT, 0, (i64)(usize)&status, 0), E_INVAL);
    expect("proc.wait_pid_high",
           sys3(SYS_WAIT, 0x100000000LL, (i64)(usize)&status, 0), E_INVAL);
    expect("proc.wait_unknown",
           sys3(SYS_WAIT, 999999, (i64)(usize)&status, 0), E_CHILD);
    expect("proc.wait_self",
           sys3(SYS_WAIT, (i64)sys3(SYS_GETPID, 0, 0, 0),
                (i64)(usize)&status, 0), E_CHILD);
    expect("proc.wait_parent",
           sys3(SYS_WAIT, (i64)sys3(SYS_GETPPID, 0, 0, 0),
                (i64)(usize)&status, 0), E_CHILD);

    expect("proc.kill_pid0", sys3(SYS_KILL, 0, 0, 0), E_INVAL);
    expect("proc.kill_pid_high", sys3(SYS_KILL, 0x100000000LL, 0, 0), E_INVAL);
    expect("proc.kill_unknown", sys3(SYS_KILL, 999999, 0, 0), E_SRCH);
    expect("proc.kill_sig256",
           sys3(SYS_KILL, (i64)sys3(SYS_GETPID, 0, 0, 0), 256, 0), E_INVAL);

    /* spawn 参数边界（全部应在创建进程之前被拒）。 */
    av[0] = self;
    av[1] = (char *)"child";
    av[2] = (char *)"1";
    av[3] = NULL;
    expect("proc.spawn_null_path", sys3(SYS_SPAWN, 0, (i64)(usize)av,
                                        (i64)(usize)&pid), E_FAULT);
    expect("proc.spawn_bad_path", sys3(SYS_SPAWN, 0x1000, (i64)(usize)av,
                                       (i64)(usize)&pid), E_FAULT);
    expect("proc.spawn_empty_path",
           sys3(SYS_SPAWN, (i64)(usize)"", (i64)(usize)av, (i64)(usize)&pid),
           E_INVAL);
    expect("proc.spawn_missing",
           sys3(SYS_SPAWN, (i64)(usize)"/bin/nosuch", (i64)(usize)av,
                (i64)(usize)&pid), E_NOENT);
    /* 64 字节 path 上限：63 个字符就会触发 ENAMETOOLONG。 */
    for (i = 0; i < 100u; i++) {
        g_path[i] = 'c';
    }
    g_path[0] = '/';
    g_path[100] = '\0';
    expect("proc.spawn_long_path",
           sys3(SYS_SPAWN, (i64)(usize)g_path, (i64)(usize)av, (i64)(usize)&pid),
           E_NAMETOOLONG);
    expect("proc.spawn_bad_upid",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, 0), E_FAULT);
    expect("proc.spawn_bad_argv_ptr",
           sys3(SYS_SPAWN, (i64)(usize)self, 0x1000, (i64)(usize)&pid), E_FAULT);
    av_bad[0] = self;
    av_bad[1] = (char *)"child";
    av_bad[2] = (char *)0x1000; /* 坏 argv 元素指针 */
    av_bad[3] = NULL;
    expect("proc.spawn_bad_argv_elem",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av_bad,
                (i64)(usize)&pid), E_FAULT);
    for (i = 0; i < 127u; i++) {
        g_arg127[i] = 'z';
    }
    g_arg127[127] = '\0';
    av_bad[2] = g_arg127; /* 127 字符：>= PROC_ARG_MAX-1 -> 拒绝 */
    av_bad[3] = NULL;
    expect("proc.spawn_arg_too_long",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av_bad,
                (i64)(usize)&pid), E_NAMETOOLONG);

    /*
     * upid 坏指针：必须在 spawn 之前校验。修复前这里会先拉起子进程再
     * copyout 失败 —— 子进程会打印 id=badsid（host 断言该行不存在）。
     */
    av[2] = (char *)"7777";
    expect("proc.spawn_bad_out_ptr",
           sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, 0x1000), E_FAULT);
    av[2] = (char *)"1";

    /* 正常 spawn/wait。 */
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
    expect_true("proc.spawn_ok", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        status = 0x7FFFFFFF;
        expect("proc.wait_ok", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
        expect("proc.wait_status", status, 0);
        expect("proc.wait_again", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0),
               E_CHILD);
    }

    /* 8 项 argv、无 NULL 终结符：线性边界，不能读未初始化栈槽。 */
    av8[0] = self;
    av8[1] = (char *)"child";
    av8[2] = (char *)"8";
    av8[3] = (char *)"a";
    av8[4] = (char *)"b";
    av8[5] = (char *)"c";
    av8[6] = (char *)"d";
    av8[7] = (char *)"e";
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av8, (i64)(usize)&pid);
    expect_true("proc.spawn_argv8_no_null", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        status = 0x7FFFFFFF;
        expect("proc.wait_argv8", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
        expect("proc.wait_argv8_status", status, 0);
    }

    /* wait 的 status 坏指针必须在阻塞之前拒绝：先起一个 block 子进程。 */
    av[2] = (char *)"2";
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
    expect_true("proc.spawn_block", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        expect("proc.wait_bad_status",
               sys3(SYS_WAIT, pid, 0x1000, 0), E_FAULT);
        expect("proc.kill_block", sys3(SYS_KILL, pid, 9, 0), 0);
        status = 0;
        expect("proc.wait_killed", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
        expect("proc.killed_status", status, 0x100 | 9);
    }

    /*
     * storm：4 个 block 子进程 -> sleep -> 全部 kill -> wait；
     * 再 4 个 quick 子进程 -> 立即 wait（同时覆盖"未跑过就被 kill"和
     * "跑完再 wait"两条路径）。
     */
    for (i = 0; i < 4u; i++) {
        av[2] = (char *)"10";
        av[3] = (char *)"block";
        av[4] = NULL;
        pid = 0;
        rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
        pids[i] = pid;
        expect_true("proc.storm_spawn_block", rc == 0 && pid > 0, rc);
    }
    sleep_ticks(40000000); /* ~40ms：让 4 个 block 子进程跑到 recv */
    for (i = 0; i < 4u; i++) {
        if (pids[i] != 0) {
            (void)sys3(SYS_KILL, pids[i], 9, 0);
        }
    }
    for (i = 0; i < 4u; i++) {
        if (pids[i] != 0) {
            status = 0;
            rc = sys3(SYS_WAIT, pids[i], (i64)(usize)&status, 0);
            expect_true("proc.storm_wait_block", rc == 0 && status == (0x100 | 9),
                        rc);
        }
    }
    put_str("AUDIT: info proc.storm_block pids=");
    for (i = 0; i < 4u; i++) {
        (void)put_s64(num, (u32)sizeof(num), (i64)pids[i]);
        put_str(num);
        put_str(i + 1u < 4u ? "," : "\n");
    }

    av[3] = (char *)"quick";
    av[4] = NULL;
    for (i = 0; i < 4u; i++) {
        av[2] = (char *)"20";
        pid = 0;
        rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
        expect_true("proc.storm_spawn_quick", rc == 0 && pid > 0, rc);
        if (rc == 0 && pid != 0) {
            if (i == 0u) {
                (void)sys3(SYS_KILL, pid, 15, 0);
            }
            status = 0;
            rc = sys3(SYS_WAIT, pid, (i64)(usize)&status, 0);
            expect_true("proc.storm_wait_quick",
                        rc == 0 && (status == 0 || status == (0x100 | 15)), rc);
        }
    }

    /*
     * 跨进程共享页 + kill 回收顺序：子进程映射父进程的 shm 并写同一物理页，
     * 父进程验证内容；kill 子进程后，父进程的页必须仍然可读写（shm_release_proc
     * 先把共享页从子 AS 摘掉，再 as_destroy —— 顺序错会 double free/内容丢失）。
     */
    {
        u64 shmva = 0;
        u32 shmh = 0;
        i64 src = sys3(SYS_SHM_ALLOC, 4, (i64)(usize)&shmva,
                       (i64)(usize)&shmh);

        expect_true("proc.shm_alloc_for_child", src == 0 && shmh != 0, src);
        if (src == 0) {
            *(volatile u32 *)(usize)shmva = 0x11111111u;
            (void)put_s64(hstr, (u32)sizeof(hstr), (i64)shmh);
            av[2] = (char *)"30";
            av[3] = (char *)"shm";
            av[4] = hstr;
            av[5] = NULL;
            pid = 0;
            rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av,
                      (i64)(usize)&pid);
            expect_true("proc.shm_child_spawn", rc == 0 && pid > 0, rc);
            if (rc == 0 && pid != 0) {
                sleep_ticks(40000000);
                expect_true("proc.shm_child_wrote",
                            *(volatile u32 *)(usize)shmva == 0x5A5A5A5Au,
                            (i64)*(volatile u32 *)(usize)shmva);
                expect("proc.shm_child_kill", sys3(SYS_KILL, pid, 9, 0), 0);
                status = 0;
                expect("proc.shm_child_wait",
                       sys3(SYS_WAIT, pid, (i64)(usize)&status, 0), 0);
                expect("proc.shm_child_status", status, 0x100 | 9);
                expect_true("proc.shm_parent_survives",
                            *(volatile u32 *)(usize)shmva == 0x5A5A5A5Au,
                            (i64)*(volatile u32 *)(usize)shmva);
                *(volatile u32 *)(usize)shmva = 0x22222222u;
            }
        }
    }

    /* 子进程映射 LFB 后被杀：fbmap_release_proc 必须先摘 PTE 再 as_destroy。 */
    av[2] = (char *)"31";
    av[3] = (char *)"fb";
    av[4] = NULL;
    pid = 0;
    rc = sys3(SYS_SPAWN, (i64)(usize)self, (i64)(usize)av, (i64)(usize)&pid);
    expect_true("proc.fb_child_spawn", rc == 0 && pid > 0, rc);
    if (rc == 0 && pid != 0) {
        sleep_ticks(40000000);
        expect("proc.fb_child_kill", sys3(SYS_KILL, pid, 9, 0), 0);
        status = 0;
        expect("proc.fb_child_wait", sys3(SYS_WAIT, pid, (i64)(usize)&status, 0),
               0);
        expect("proc.fb_child_status", status, 0x100 | 9);
    }
}

/* ------------------------------------------------------------------------- */
/* SHM 45/46 + FB 47                                                          */
/* ------------------------------------------------------------------------- */
static void test_shm_fb(void)
{
    u64 va = 0;
    u64 va2 = 0;
    u32 handle = 0;
    i64 rc;

    section("shmfb");

    expect("shm.alloc_pages0",
           sys3(SYS_SHM_ALLOC, 0, (i64)(usize)&va, (i64)(usize)&handle),
           -22);
    expect("shm.alloc_pages1025",
           sys3(SYS_SHM_ALLOC, 1025, (i64)(usize)&va, (i64)(usize)&handle),
           -22);
    expect("shm.alloc_bad_va_out",
           sys3(SYS_SHM_ALLOC, 1, 0x1000, (i64)(usize)&handle), -14);
    expect("shm.alloc_bad_handle_out",
           sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, 0x1000), -14);

    va = 0;
    handle = 0;
    rc = sys3(SYS_SHM_ALLOC, 1, (i64)(usize)&va, (i64)(usize)&handle);
    expect_true("shm.alloc_ok", rc == 0 && va >= 0x50000000ULL && handle != 0, rc);
    if (rc == 0) {
        *(volatile u32 *)(usize)va = 0x12345678u;
        expect_true("shm.alloc_writable",
                    *(volatile u32 *)(usize)va == 0x12345678u,
                    (i64)*(volatile u32 *)(usize)va);
        expect_true("shm.map_ok",
                    sys3(SYS_SHM_MAP, (i64)handle, (i64)(usize)&va2, 0) == 0 &&
                        va2 >= 0x50000000ULL,
                    (i64)va2);
        expect("shm.map_bad_handle",
               sys3(SYS_SHM_MAP, 0xFFFFFFFFLL, (i64)(usize)&va2, 0), -9);
        expect("shm.map_zero_handle",
               sys3(SYS_SHM_MAP, 0, (i64)(usize)&va2, 0), -9);
        expect("shm.map_bad_out",
               sys3(SYS_SHM_MAP, (i64)handle, 0x1000, 0), -14);
    }

    /* SHM_PAGES_MAX=1024：4 MiB，验证按上限分配不越界。 */
    va = 0;
    handle = 0;
    rc = sys3(SYS_SHM_ALLOC, 1024, (i64)(usize)&va, (i64)(usize)&handle);
    expect_true("shm.alloc_max_pages", rc == 0 && handle != 0, rc);

    /* FB_MAP：输出指针、正常映射、重复映射 EBUSY。 */
    va = 0;
    expect("fb.bad_va_out", sys3(SYS_FB_MAP, 0x1000, (i64)(usize)&g_fb, 0), -14);
    expect("fb.bad_info_out", sys3(SYS_FB_MAP, (i64)(usize)&va, 0x1000, 0), -14);
    rc = sys3(SYS_FB_MAP, (i64)(usize)&va, (i64)(usize)&g_fb, 0);
    expect_true("fb.map_ok", rc == 0 && va == 0x60000000ULL, rc);
    if (rc == 0) {
        expect_true("fb.info_sane",
                    g_fb.base == va && g_fb.width > 0 && g_fb.height > 0 &&
                        g_fb.bpp == 32 && g_fb.pitch >= g_fb.width * 4u,
                    (i64)g_fb.width);
        expect("fb.map_twice_busy",
               sys3(SYS_FB_MAP, (i64)(usize)&va2, (i64)(usize)&g_fb, 0), -16);
    }
}

/* ------------------------------------------------------------------------- */
/* 入口                                                                       */
/* ------------------------------------------------------------------------- */
static int crash_main(void)
{
    volatile u64 *p = (volatile u64 *)(usize)0;

    put_str("AUDIT: crash mode: dereference NULL in ring3\n");
    *p = 0xA5A5A5A5u; /* CPL3 #PF -> trap.c 目前 panic（已知限制） */
    put_str("AUDIT: crash mode survived (unexpected)\n");
    return 1;
}

int audit_main(long argc, char **argv)
{
    char num[24];

    if (argc >= 2 && str_eq(argv[1], "child")) {
        return child_main(argc, argv);
    }
    if (argc >= 2 && str_eq(argv[1], "crash")) {
        return crash_main();
    }

    put_str("AUDIT: boot syscall-boundary probe\n");
    put_str("AUDIT: pid=");
    (void)put_s64(num, (u32)sizeof(num), sys3(SYS_GETPID, 0, 0, 0));
    put_str(num);
    put_str(" argc=");
    (void)put_s64(num, (u32)sizeof(num), (i64)argc);
    put_str(num);
    put_str("\n");

    /* 一个专用端口，用于 sleep_ticks() 让出 CPU。 */
    g_sleep_port = (u32)sys3(SYS_PORT_CREATE, 1, 0, 0);

    test_write();
    test_ipc();
    test_fs();
    test_svc();
    test_proc(argv);
    test_shm_fb();

    put_str("AUDIT: summary pass=");
    (void)put_s64(num, (u32)sizeof(num), (i64)g_pass);
    put_str(num);
    put_str(" fail=");
    (void)put_s64(num, (u32)sizeof(num), (i64)g_fail);
    put_str(num);
    put_str(" total=");
    (void)put_s64(num, (u32)sizeof(num), (i64)(g_pass + g_fail));
    put_str(num);
    put_str("\n");
    return (g_fail == 0) ? 0 : 1;
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("andq $-16, %rsp\n\t"
                     "call audit_main\n\t"
                     "movl %eax, %edi\n\t"
                     "movl $1, %eax\n\t"
                     "syscall\n\t"
                     "1: jmp 1b\n\t");
}
