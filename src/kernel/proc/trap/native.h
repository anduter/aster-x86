#ifndef INTR_NATIVE_H
#define INTR_NATIVE_H

#include "../../types.h"

/* Native ABI v0：原生程序入口是 int 0x80。
 * 旧原型的 POSIX 风格调用表在迁移期走 int 0x81，不作为长期 ABI。 */

#define NATIVE_ABI_MAJOR 0
#define NATIVE_ABI_MINOR 1
#define NATIVE_ABI_PATCH 0

/* Native ABI 错误码（与 core/error.h 的内核内部错误码分开） */
#define NATIVE_E_OK       0
#define NATIVE_E_INVAL   (-1)
#define NATIVE_E_NOENT   (-2)
#define NATIVE_E_ACCESS  (-3)
#define NATIVE_E_TYPE    (-4)
#define NATIVE_E_NOSPC   (-5)
#define NATIVE_E_AGAIN   (-6)
#define NATIVE_E_TIMEOUT (-7)
#define NATIVE_E_BADF    (-8)
#define NATIVE_E_NOSYS   (-9)
#define NATIVE_E_FAULT   (-10)
#define NATIVE_E_OVERFLOW (-11)
#define NATIVE_E_BUSY    (-12)
#define NATIVE_E_IO      (-13)

/* 原生对象类型（与旧 obj_type_t 的命名空间分开） */
#define NATIVE_OBJ_PROCESS      1
#define NATIVE_OBJ_THREAD       2
#define NATIVE_OBJ_MEMORY       3
#define NATIVE_OBJ_CHANNEL      4
#define NATIVE_OBJ_PORT         5
#define NATIVE_OBJ_EVENT        6
#define NATIVE_OBJ_NAMESPACE    7
#define NATIVE_OBJ_FILE         8
#define NATIVE_OBJ_DIRECTORY    9
#define NATIVE_OBJ_VOLUME       10
#define NATIVE_OBJ_DEVICE       11
#define NATIVE_OBJ_INTERRUPT    12
#define NATIVE_OBJ_SEALED_IMAGE 13
#define NATIVE_OBJ_TIMER        14

/* 句柄权限位 */
#define NATIVE_R_READ     (1U << 0)
#define NATIVE_R_WRITE    (1U << 1)
#define NATIVE_R_EXEC     (1U << 2)
#define NATIVE_R_MAP      (1U << 3)
#define NATIVE_R_WAIT     (1U << 4)
#define NATIVE_R_SIGNAL   (1U << 5)
#define NATIVE_R_CONNECT  (1U << 6)
#define NATIVE_R_ACCEPT   (1U << 7)
#define NATIVE_R_TRANSFER (1U << 8)
#define NATIVE_R_MANAGE   (1U << 9)
#define NATIVE_R_ACK      (1U << 10)

/* 系统调用号（v0 只实现最小集合） */
#define NATIVE_SYS_ABI_VERSION  0x00
#define NATIVE_SYS_EXIT         0x01
#define NATIVE_SYS_YIELD        0x02
#define NATIVE_SYS_HANDLE_CLOSE 0x10
#define NATIVE_SYS_HANDLE_DUP   0x11
#define NATIVE_SYS_HANDLE_INFO  0x12
#define NATIVE_SYS_HANDLE_RESTRICT 0x13 /* ebx=handle, ecx=drop rights */
#define NATIVE_SYS_HANDLE_TRANSFER 0x14 /* ebx=pid, ecx=handle, edx=rights */
#define NATIVE_SYS_HANDLE_REVOKE 0x15   /* ebx=handle */
#define NATIVE_SYS_PROCESS_CREATE 0x20   /* ebx=name, ecx=len, edx=rights -> proc handle */
#define NATIVE_SYS_PROCESS_WAIT   0x21   /* ebx=proc handle, ecx=&status -> pid */
#define NATIVE_SYS_CHANNEL_CREATE 0x30   /* ebx=u32[2] -> 0 */
#define NATIVE_SYS_CHANNEL_SEND   0x31   /* ebx=handle, ecx=buf, edx=len */
#define NATIVE_SYS_CHANNEL_RECV   0x32   /* ebx=handle, ecx=buf, edx=max */
#define NATIVE_SYS_PORT_CREATE    0x33   /* -> port handle */
#define NATIVE_SYS_PORT_CONNECT   0x34   /* ebx=port handle -> client channel */
#define NATIVE_SYS_PORT_ACCEPT    0x35   /* ebx=port handle -> server channel */
#define NATIVE_SYS_CHANNEL_RECV_BLOCK 0x36 /* ebx=handle, ecx=buf, edx=max */
#define NATIVE_SYS_PORT_ACCEPT_BLOCK 0x37  /* ebx=port handle -> server channel */
#define NATIVE_SYS_SERVICE_REGISTER 0x38 /* ebx=name, ecx=len, edx=port handle */
#define NATIVE_SYS_SERVICE_CONNECT  0x39 /* ebx=name, ecx=len -> client channel */
#define NATIVE_SYS_DEBUG_WRITE  0x50

/* ABI feature bits */
#define NATIVE_F_IPC_HANDLES  (1U << 0)
#define NATIVE_F_SHARED_MEM   (1U << 1)
#define NATIVE_F_SEALED_IMAGE (1U << 2)
#define NATIVE_F_WAIT_ANY     (1U << 3)
#define NATIVE_F_SANDBOX      (1U << 4)

struct native_abi_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t reserved;
    uint32_t features;
};

struct native_handle_info {
    uint32_t type;
    uint32_t rights;
    uint32_t object_id;
    uint32_t flags;
};

/* 进程生命周期钩子：由 process.c 调用 */
struct process;
void native_proc_init(struct process *p);
void native_proc_copy(struct process *parent, struct process *child);
void native_proc_free(struct process *p);

/* 系统调用分发（向量 0x80）与启动自检 */
struct interrupt_frame;
void native_syscall_dispatch(struct interrupt_frame *f);
void native_selftest(void);

#endif
