/* =============================================================================
 * tests/unit/test_abi.c - 用户态 ABI 布局与版本常量（host 编译，不启动 QEMU）
 *
 * 只包含 include/abi.h（内核与用户态共享的单一来源），断言冻结的
 * sizeof / offsetof / 系统调用编号。host 与 i686 对这些纯 u32 结构体布局一致。
 * ========================================================================== */
#include "unit.h"
#include "../../src/kernel/include/abi.h"

int main(void)
{
    UNIT_BEGIN("abi");

    /* 版本与魔数（major 冻结；minor 可增大，不在这里钉死） */
    CHECK_U32(OS_ABI_MAGIC, 0x4F534142u);
    CHECK_U32(OS_ABI_VERSION, 1u);
    CHECK_U32(OS_ABI_VERSION_MAJOR, 1);
    CHECK(OS_ABI_VERSION_MINOR >= 3);
    CHECK_U32(OS_ABI_FEATURES,
              OS_ABI_F_NATIVE_GATE | OS_ABI_F_VERSIONED_OPS |
              OS_ABI_F_DEVICE_API_V1 | OS_ABI_F_VFS_BACKEND_V1 |
              OS_ABI_F_SERVICE_MANIFEST_V1);

    /* 冻结的 legacy 系统调用编号（只增不改） */
    CHECK_U32(SYS_WRITE, 1);
    CHECK_U32(SYS_OPEN, 6);
    CHECK_U32(SYS_FORK, 18);
    CHECK_U32(SYS_GETUID, 41);

    /* 冻结的错误码 */
    CHECK_INT(E_OK, 0);
    CHECK_INT(E_INVAL, -1);
    CHECK_INT(E_NOENT, -2);
    CHECK_INT(E_ACCES, -8);
    CHECK_INT(E_BUSY, -9);

    /* 用户可见结构体：大小 + 关键偏移（与 abi.h 内 _Static_assert 双保险） */
    CHECK_U32(sizeof(sys_stat_t), 24);
    CHECK_U32(__builtin_offsetof(sys_stat_t, type), 0);
    CHECK_U32(__builtin_offsetof(sys_stat_t, size), 4);
    CHECK_U32(__builtin_offsetof(sys_stat_t, mtime), 8);
    CHECK_U32(__builtin_offsetof(sys_stat_t, mode), 12);
    CHECK_U32(__builtin_offsetof(sys_stat_t, uid), 16);
    CHECK_U32(__builtin_offsetof(sys_stat_t, gid), 20);

    CHECK_U32(sizeof(struct udp_req), 72);
    CHECK_U32(__builtin_offsetof(struct udp_req, ip), 0);
    CHECK_U32(__builtin_offsetof(struct udp_req, port), 4);
    CHECK_U32(__builtin_offsetof(struct udp_req, len), 6);
    CHECK_U32(__builtin_offsetof(struct udp_req, data), 8);

    CHECK_U32(sizeof(sys_mouse_t), 12);
    CHECK_U32(__builtin_offsetof(sys_mouse_t, buttons), 8);

    CHECK_U32(sizeof(sys_resinfo_t), 40);
    CHECK_U32(__builtin_offsetof(sys_resinfo_t, pci_devices), 36);

    UNIT_END();
}
