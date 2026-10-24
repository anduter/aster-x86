# 用户态 / 驱动 / 服务 API（v1）

> 状态：2026-10-24，ABI v1 冻结（新增只追加）
>
> 关联文档：[native-abi.md](native-abi.md)（`int 0x80` Native ABI）、
> [driver-service.md](driver-service.md)（驱动服务化规划）、
> [modules.md](modules.md)（模块边界）
>
> 本文是**跨边界契约**的索引：单一来源、版本号、校验落点、变更流程。
> 实现细节仍以头文件和对应设计文档为准。

---

## 0. 总览

| API 类 | 单一来源 | 版本宏 | 当前版本 | 稳定级别 | 校验落点 |
|---|---|---|---|---|---|
| 用户态系统调用 ABI（`int 0x81` legacy + `int 0x80` native） | `src/kernel/include/abi.h`（native 编号在 `proc/trap/native.h`） | `OS_ABI_VERSION` / `OS_ABI_MAGIC` | 1.0 | **stable**（1..59 编号与结构体布局冻结） | 编译期 `_Static_assert` + `SYS_ABI_VERSION` 标量查询 |
| 设备 ops | `src/kernel/dev/device.h` | `DEV_API_VERSION` | 1 | experimental | 编译期布局断言 + `dev_register()` 写入 `api_version` |
| 文件系统后端 | `src/kernel/fs/fs.h` | `FS_BACKEND_API_VERSION` | 1 | experimental | `fs/vfs.c` 注册表校验（不匹配 `E_INVAL` + 打印一行） |
| 服务 manifest | `src/kernel/svc/service.h` | `SERVICE_MANIFEST_VERSION` | 1 | experimental | `service_register_manifest()` 校验（不匹配 `E_INVAL` + 打印一行） |

三条原则：

1. **单一来源**：编号、结构体、版本宏只在一个头文件里定义；其他位置只转发或引用。
2. **版本化**：布局变化必须 bump 版本；旧格式用 `0` 表示，按 v1 解释，保证旧调用方零改动。
3. **编译期 + 运行期双校验**：能用 `_Static_assert` 固定的布局不拖到运行期；
   运行期只校验无法在编译期知道的东西（后端/注册者声明的版本）。

---

## 1. 用户态系统调用 ABI

### 1.1 单一来源与包含关系

```text
src/kernel/include/abi.h          ← 唯一来源：SYS_*、OS_ABI_*、公共结构体、错误码
        ↑                    ↑
src/kernel/proc/trap/syscall.h   src/user/ulib.h
（兼容转发，内核侧旧包含路径）     （用户库，经相对路径引入，不依赖 -Isrc/kernel）
```

- `syscall.h` 现在只有 `#include "../../include/abi.h"`、`sys_call()` 和
  `syscall_count()`；7 个 `*_syscall.c` 一行未改。
- `ulib.c` 的 `syscall3()` 与 `abi_version()` 直接用 `abi.h` 的编号，
  不再手写任何 `SYS_*`。
- `user_shell.c` 删掉了重复的编号块和 `struct udp_req`，统一用 `abi.h`。

### 1.2 编号空间

| 门 | 头文件 | 编号 | 说明 |
|---|---|---|---|
| `int 0x81` legacy | `include/abi.h` | `SYS_WRITE`(1) .. `SYS_RENAME`(58)、`SYS_ABI_VERSION`(59) | 迁移期门，现有 shell/测试使用 |
| `int 0x80` native | `proc/trap/native.h` | `NATIVE_SYS_*`（0x00 起） | Native ABI v0，实验性 |

两套门编号空间互不相干，不要混用宏。`SYS_NR_MAX` 记录 legacy 已分配的最大编号；
新增 legacy 调用必须从 `SYS_NR_MAX + 1` 开始，并 bump `OS_ABI_VERSION_MINOR`。

### 1.3 `SYS_ABI_VERSION`（59）

```c
/* 无参数，返回一个 int： */
(OS_ABI_VERSION << 16) | OS_ABI_FEATURES
```

| 位 | 含义 |
|---|---|
| `31..16` | `OS_ABI_VERSION`（major，当前 `1`） |
| `15..0` | feature bits（只增不减） |

feature bits（低 16 位）：

| 位 | 宏 | 含义 |
|---|---|---|
| 0 | `OS_ABI_F_NATIVE_GATE` | `int 0x80` Native ABI 门已安装 |
| 1 | `OS_ABI_F_VERSIONED_OPS` | 三类 API 带版本校验 |
| 2 | `OS_ABI_F_DEVICE_API_V1` | 设备 ops v1 |
| 3 | `OS_ABI_F_VFS_BACKEND_V1` | VFS 后端 v1 |
| 4 | `OS_ABI_F_SERVICE_MANIFEST_V1` | 服务 manifest v1 |

示例（用户态）：

```c
#include "ulib.h"        /* 已包含 include/abi.h */

int v = abi_version();
if ((v >> 16) != OS_ABI_VERSION_MAJOR) {
    /* 程序声明的 ABI 与内核不兼容 */
}
```

需要 `major/minor/patch/features` 完整结构时，走 `int 0x80` 的
`NATIVE_SYS_ABI_VERSION`（`0x00`，写 `struct native_abi_version`）。

### 1.4 公共结构体（布局冻结）

| 结构体 | 大小 | 使用者 | 说明 |
|---|---|---|---|
| `struct udp_req` | 72 | `SYS_UDP_SEND/RECV`、`SYS_SENDTO/RECVFROM` | `ip[4] + port + len + data[64]`；`len <= 64` |
| `sys_stat_t` | 24 | `SYS_STAT` | 6 × u32：`type/size/mtime/mode/uid/gid` |
| `sys_mouse_t` | 12 | `SYS_MOUSE` | 3 × s32：`x/y/buttons` |
| `sys_resinfo_t` | 40 | `SYS_RESINFO` | 10 × u32，顺序见 `abi.h` |

`abi.h` 末尾用 `_Static_assert` + `__builtin_offsetof` 固定了每个结构体的大小和
字段偏移；改动布局会**编译失败**，这正是版本控制的入口。

### 1.5 错误码

- **legacy 门**：多数失败返回 `-1`；文件系统路径可能把 `core/error.h` 的
  `E_*`（`E_OK`/`E_INVAL`/`E_NOENT`/`E_NOSPC`/`E_TYPE`/`E_BADH`/`E_EXIST`/
  `E_FULL`/`E_ACCES`/`E_BUSY`）原样带到用户态。`abi.h` 对这批数值做了
  `_Static_assert`，值已冻结。
- **Native 门**：使用 `NATIVE_E_*`（`NATIVE_E_ACCESS=-3` 等），**数值与
  `E_*` 不同**，两套错误码不要混用。Native 错误码见 `native-abi.md` 第 5 节。

---

## 2. 设备 ops API（experimental）

头文件：`src/kernel/dev/device.h`；版本：`DEV_API_VERSION = 1`。

```c
typedef struct {
    char        name[DEV_NAME_MAX];  /* 16，NUL 结尾 */
    dev_class_t cls;                 /* DEV_CHAR/BLOCK/VIDEO/TIMER */
    int       (*putc)(char c);       /* 成功 E_OK，失败负 E_*；不睡眠 */
    int       (*getc)(char *out);    /* 有字符 E_OK，暂无 E_NOENT；不忙等 */
    uint32_t    api_version;         /* dev_register 自动写 DEV_API_VERSION */
} device_t;
```

返回值与所有权（v1 约定）：

| 操作 | 成功 | 失败 | 所有权 |
|---|---|---|---|
| `dev_register` | `E_OK` | `E_INVAL`/`E_EXIST`/`E_FULL` | `name` 被复制；`putc/getc` 函数指针由设备实现方保证生命周期 |
| `dev_lookup`/`dev_at` | 只读借用指针 | `0` | 归注册表；不得 `free`/写入，生命周期到下一次 `dev_init()` |
| `putc` | `E_OK` | 负 `E_*` | 只消费字符值，不保留指针；可能在持锁/关中断上下文调用 |
| `getc` | `E_OK`，写 1 字节 | `E_NOENT`（无输入） | `*out` 缓冲区归调用方 |
| `dev_open` | `OBJ_DEVICE` 句柄 | `0` | 句柄归调用方（对象/close 释放）；`device_t` 仍归注册表 |

布局断言：`sizeof(device_t) == 32`，`name/cls/putc/getc/api_version` 偏移 0/16/20/24/28。
增删字段必须 bump `DEV_API_VERSION` 并同步改断言。

> v1 没有通用 `open/read/write/ioctl`：字符/显示设备用 `putc/getc`，
> 块设备走 `block_device_t` 的 `block_read/block_write`。这是有意的强类型设计。

---

## 3. 文件系统后端 API（experimental）

头文件：`src/kernel/fs/fs.h`；版本：`FS_BACKEND_API_VERSION = 1`。

`fs_backend_t` 是一张 ops 表（`name/probe/open/read/write/close/ref/stat/
listdir/mkdir/unlink/rename`），实现放在 `fs/ramfs.c`、`fs/fat16.c`、
`fs/svcfs.c`、`fs/blkfs.c`，注册表在 `fs/vfs.c`：

```c
typedef struct {
    const fs_backend_t *backend;
    uint32_t            api_version;  /* 后端编写时对应的 FS_BACKEND_API_VERSION */
} fs_backend_reg_t;

static const fs_backend_reg_t backends[] = {
    { &fat16_backend, FS_BACKEND_API_VERSION },
    ...
};
```

校验行为：

- 首次路由前统一校验注册表；`api_version` 为 `0`（旧格式）或等于
  `FS_BACKEND_API_VERSION` 才接受；
- 不匹配：`kprintf("vfs: backend %s api_version=%u, expected %u: rejected\n", ...)`
  并让所有 `fs_*_ex` 返回 `-1`（`E_INVAL` 语义），不会用错布局的 ops 表继续跑；
- 判定逻辑拆成 `fs_backend_version_ok()`（纯函数，不打印）和
  `fs_backend_api_check()`（打印 + 返回 `E_INVAL`），便于自检复用。

> 版本号放在注册表而不是 `fs_backend_t` 字段：现有后端用位置初始化器，
> 给结构体尾部加字段会触发 `-Wmissing-field-initializers`（`-Wextra -Werror`
> 直接失败）。注册表方式同样能在版本升级时拒绝旧后端，且不需要改各后端文件。

新增后端步骤：

1. 实现 `const fs_backend_t xxx_backend = { ... }`（字段顺序不可变）；
2. 在 `fs/vfs.c` 声明 `extern` 并在 `backends[]` 加一行，版本写
   `FS_BACKEND_API_VERSION`；
3. 版本升级时先 bump `FS_BACKEND_API_VERSION`，再逐个更新注册行；
4. 跑 `tools\session-run.ps1 -Name api -Test` 两次。

---

## 4. 服务 manifest API（experimental）

头文件：`src/kernel/svc/service.h`；版本：`SERVICE_MANIFEST_VERSION = 1`。

```c
typedef struct service_manifest {
    uint32_t    version;   /* SERVICE_MANIFEST_VERSION；0 = 旧格式（按 v1） */
    const char *name;      /* NUL 结尾，长度 < SERVICE_NAME(32) */
    uint32_t    port;      /* 已创建的 port id */
    uint32_t    flags;     /* 保留，v1 必须为 0 */
} service_manifest_t;

int service_register(const char *name, uint32_t port);           /* 旧接口 = v1 */
int service_register_manifest(const service_manifest_t *m);     /* 带版本 */
```

校验行为：

- `version == 0` 或 `SERVICE_MANIFEST_VERSION` 接受；
- 其它版本：打印
  `svc: manifest '<name>' version <v> rejected (expected 1)`，
  返回 `E_INVAL`，不写入注册表；
- `flags != 0`、空名字返回 `E_INVAL`；
- 旧接口 `service_register()` 等价于 `version = 0`，行为与之前完全一致；
- 注册表内部记录实际生效的 `manifest_version`，为后续按版本分派留位。

示例：

```c
service_manifest_t m = {
    SERVICE_MANIFEST_VERSION,
    "echosvc",
    (uint32_t)port,
    0
};
if (service_register_manifest(&m) != E_OK) {
    /* 版本不兼容/重名/表满 */
}
```

---

## 5. 版本策略与稳定级别

| 级别 | 含义 | 本仓库对象 |
|---|---|---|
| **stable** | 编号/布局/错误码不可变；新增只追加 | legacy `SYS_*` 1..59、`abi.h` 公共结构体、`E_*` 冻结值 |
| **experimental** | 可改，但必须 bump 版本并保留旧格式兼容 | `device_t`、`fs_backend_t`、`service_manifest_t`、Native ABI v0 |
| **deprecated** | 保留编号/入口，文档标注替代方案 | 暂无 |

版本号规则：

- `OS_ABI_VERSION` 是 major：破坏性变化才 +1；minor 变化（追加调用/字段）
  只改 `OS_ABI_VERSION_MINOR`，major 不变。
- 三类 experimental API 的版本宏是独立小整数：`1` → `2` 表示布局变化；
  注册方声明 `0` 永远按 v1 解释，保证旧代码不改也能跑。
- 删除的调用保留编号：legacy 返回 `-1`，native 返回 `E_NOSYS`。

---

## 6. 变更流程（ checklist）

新增/修改一个跨边界 API 时按顺序做：

1. **改单一来源**：只改 `abi.h` / `device.h` / `fs.h` / `service.h` 中的一处；
   其他头文件只做转发。
2. **保兼容**：已有编号/字段/错误码不动；新调用用新编号，新字段加在结构体尾部；
   旧格式用 `0` 表示。
3. **加编译期断言**：结构体大小/偏移、错误码值、ops 表布局；
   `-Werror` 下必须通过。
4. **加运行期校验**：后端/注册者声明版本，不匹配 `E_INVAL` + 打印一行；
   不要静默接受。
5. **更新文档**：本文的版本表 + `native-abi.md`（涉及 `int 0x80/0x81` 时）；
   在提交信息里写清版本变化。
6. **验证**：`tools\session-run.ps1 -Name api -Test` 连续两次通过；
   启动日志应出现 `abi: version ok ...`。
7. **提交**：路径限定 `git commit -- <files>`，前缀 `[api]` 或 `[docs]`。

---

## 7. 验证入口

| 检查 | 位置/命令 | 期望 |
|---|---|---|
| 编译期断言 | 任意构建 | `-Wall -Wextra -Werror` 通过 |
| ABI 启动自检 | `serial-*.log` | `abi: version ok magic=4f534142 abi=1.0 features=1f structs=udp_req:72,stat:24,mouse:12,resinfo:40 api=dev1/fs1/svc1` |
| 全量回归 | `tools\session-run.ps1 -Name api -Test` | `=== TEST PASS (n/n) ===` |
| 后端版本拒绝 | 人为把 `vfs.c` 注册行的版本改成 `2` | 打印 `vfs: backend ... rejected`，`fs_*_ex` 返回 `-1` |
| manifest 版本拒绝 | 用 `version=99` 调 `service_register_manifest` | 打印 `svc: manifest ... rejected`，返回 `E_INVAL` |

> `abi_selftest()` 只打印，不新增 `build.ps1` 检查项；回归项数量由
> `build.ps1` 维护者统一接入。
