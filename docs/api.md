# 用户态系统调用 ABI（v1）

> 状态：2026-10-24，ABI v1 冻结（新增只追加）
>
> 范围：**用户态系统调用 ABI 是唯一对外 API**（`int 0x81` legacy 表，单一来源
> `src/kernel/include/abi.h`），以及它与 `int 0x80` Native ABI 的关系。
>
> 非对外 API（internal only）：设备 ops、VFS 后端、服务 manifest 的版本字段
> 只是内核内部一致性检查——**不是对外驱动/服务 API、不承诺兼容、不做驱动
> 服务化**。本文只在第 0 节记录它们的存在，不为其定义稳定契约。

---

## 0. 总览

| 项 | 单一来源 | 版本 | 稳定级别 | 校验方式 |
|---|---|---|---|---|
| 用户态 syscall ABI（`int 0x81`） | `src/kernel/include/abi.h` | `OS_ABI_VERSION = 1` | **stable**（编号 1..59、结构体布局、错误码冻结） | 编译期 `_Static_assert` + `SYS_ABI_VERSION` 运行时查询 |
| Native ABI（`int 0x80`） | `src/kernel/proc/trap/native.h` | `NATIVE_ABI_MAJOR/MINOR` = 0.1 | experimental（见 `native-abi.md`） | 独立编号表/`NATIVE_SYS_ABI_VERSION` |
| 内部版本检查（**非 API**） | `dev/device.h`、`fs/fs.h`、`svc/service.h` | `DEV/FS_BACKEND/SERVICE_MANIFEST = 1` | **internal only** | 编译期断言 + 注册入口校验 |

三条原则：

1. **单一来源**：编号、结构体、版本宏只在 `include/abi.h` 定义一次。
2. **版本化**：布局变化 bump 版本；已发布的编号/字段只追加。
3. **编译期优先**：能静态固定的布局不拖到运行期。

---

## 1. 单一来源与包含关系

```text
src/kernel/include/abi.h          ← 唯一来源：SYS_*、OS_ABI_*、公共结构体、错误码
        ↑                    ↑
src/kernel/proc/trap/syscall.h   src/user/ulib.h
（兼容转发，内核旧包含路径）       （用户库，经相对路径引入，不依赖 -Isrc/kernel）
```

- `syscall.h` 现在只有 `#include "../../include/abi.h"`、`sys_call()` 和
  `syscall_count()`；7 个 `*_syscall.c` 的 `case` 一行未改。
- `ulib.c` 的 `syscall3()` / `abi_version()` 直接使用 `abi.h` 的编号，
  不再手写任何 `SYS_*`。
- `user_shell.c` 删除了重复的编号块和 `struct udp_req`，统一从 `abi.h` 取。

---

## 2. 编号空间

| 门 | 头文件 | 编号 | 说明 |
|---|---|---|---|
| `int 0x81` legacy | `include/abi.h` | `SYS_WRITE`(1) .. `SYS_RENAME`(58)、`SYS_ABI_VERSION`(59) | 迁移期门，现有 shell/测试使用 |
| `int 0x80` native | `proc/trap/native.h` | `NATIVE_SYS_*`（0x00 起） | Native ABI v0，实验性 |

两套门编号空间互不相干，不要混用宏。`SYS_NR_MAX`（当前 59）记录 legacy
已分配的最大编号；新增调用必须从 `SYS_NR_MAX + 1` 开始，并 bump
`OS_ABI_VERSION_MINOR`。

---

## 3. `SYS_ABI_VERSION`（编号 59）

```c
/* int 0x81，无参数 */
return (OS_ABI_VERSION << 16) | OS_ABI_FEATURES;
```

| 位 | 含义 |
|---|---|
| `31..16` | `OS_ABI_VERSION`（major，当前 `1`） |
| `15..0` | feature bits（只增不减） |

feature bits：

| 位 | 宏 | 含义 | 状态 |
|---|---|---|---|
| 0 | `OS_ABI_F_NATIVE_GATE` | `int 0x80` Native ABI 门已安装 | 已启用 |
| 1 | `OS_ABI_F_VERSIONED_OPS` | 版本化 API 基础设施 | 已启用 |
| 2 | `OS_ABI_F_DEVICE_API_V1` | 设备 ops v1 | **internal only（非对外 API）** |
| 3 | `OS_ABI_F_VFS_BACKEND_V1` | VFS 后端 v1 | **internal only（非对外 API）** |
| 4 | `OS_ABI_F_SERVICE_MANIFEST_V1` | 服务 manifest v1 | **internal only（非对外 API）** |

> 当前内核 `OS_ABI_FEATURES = 0x1f` 包含 bit2-4；它们只是内部实现标记，
> 不构成对外 API，也不承诺兼容。保留与否不影响 legacy 编号/结构体契约。

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

---

## 4. 公共结构体（布局冻结）

| 结构体 | 大小 | 使用者 | 说明 |
|---|---|---|---|
| `struct udp_req` | 72 | `SYS_UDP_SEND/RECV`、`SYS_SENDTO/RECVFROM` | `ip[4] + port + len + data[64]`；`len <= 64` |
| `sys_stat_t` | 24 | `SYS_STAT` | 6 × u32：`type/size/mtime/mode/uid/gid` |
| `sys_mouse_t` | 12 | `SYS_MOUSE` | 3 × s32：`x/y/buttons` |
| `sys_resinfo_t` | 40 | `SYS_RESINFO` | 10 × u32，顺序见 `abi.h` |

`abi.h` 末尾用 `_Static_assert` + `__builtin_offsetof` 固定了每个结构体的
大小和字段偏移；改布局会**编译失败**（`-Wall -Wextra -Werror` 下）。

> `struct sockaddr` 在本快照中不存在：socket 相关调用用 `SYS_SOCKET`
> 的 `domain/type/proto` 整数和 `struct udp_req`，没有独立的地址结构体。

---

## 5. 错误码

- **legacy 门**：多数失败返回 `-1`；文件系统路径可能把 `core/error.h` 的
  `E_*`（`E_OK`/`E_INVAL`/`E_NOENT`/`E_NOSPC`/`E_TYPE`/`E_BADH`/`E_EXIST`/
  `E_FULL`/`E_ACCES`/`E_BUSY`）原样带到用户态。`abi.h` 对这些数值做了
  `_Static_assert`，值已冻结。
- **Native 门**：使用 `NATIVE_E_*`（`NATIVE_E_ACCESS=-3` 等），**数值与
  `E_*` 不同**，两套错误码不要混用。Native 错误码见 `native-abi.md` 第 5 节。

---

## 6. 版本策略与稳定级别

| 级别 | 含义 | 本仓库对象 |
|---|---|---|
| **stable** | 编号/布局/错误码不可变；新增只追加 | legacy `SYS_*` 1..59、`abi.h` 公共结构体、`E_*` 冻结值 |
| **experimental** | 可改，但必须 bump 版本并保留旧格式兼容 | Native ABI v0 |
| **internal** | 内核内部一致性检查，不是对外 API；不承诺兼容 | 设备 ops、VFS 后端、服务 manifest 的版本字段 |

版本号规则：

- `OS_ABI_VERSION` 是 major：破坏性变化才 +1；追加调用/字段只改
  `OS_ABI_VERSION_MINOR`，major 不变。
- 删除的调用保留编号：legacy 返回 `-1`，native 返回 `E_NOSYS`。
- 每个程序在 manifest 中声明 `abi`；加载器拒绝不兼容的 major 版本
  （该 manifest 是程序包格式，与服务注册 manifest 不是同一件事）。

---

## 7. 变更流程（checklist）

新增/修改一个用户态 syscall 时按顺序做：

1. **改单一来源**：只改 `abi.h`（编号/结构体/版本宏），其他头文件只做转发。
2. **保兼容**：已有编号/字段/错误码不动；新调用用 `SYS_NR_MAX + 1`；
   新字段加在结构体尾部。
3. **加编译期断言**：结构体大小/偏移、错误码值；`-Werror` 下必须通过。
4. **实现**：在对应 `*_syscall.c` 组加 `case` 和 `owns` 分支，不动分发器。
5. **更新文档**：本文版本表 + `native-abi.md`（涉及 `int 0x80/0x81` 时）。
6. **验证**：`tools\session-run.ps1 -Name api -Test` 连续两次通过；
   启动日志应出现 `abi: version ok ...`。
7. **提交**：路径限定 `git commit -- <files>`，前缀 `[api]` 或 `[docs]`。

---

## 8. 验证入口

| 检查 | 位置/命令 | 期望 |
|---|---|---|
| 编译期断言 | 任意构建 | `-Wall -Wextra -Werror` 通过 |
| ABI 启动自检 | `serial-*.log` | `abi: version ok magic=4f534142 abi=1.0 features=... structs=udp_req:72,stat:24,mouse:12,resinfo:40` |
| 全量回归 | `tools\session-run.ps1 -Name api -Test` | `=== TEST PASS (n/n) ===` |

> `abi_selftest()` 只打印，不新增 `build.ps1` 检查项；回归项数量由
> `build.ps1` 维护者统一接入。日志里的 `api=dev1/fs1/svc1` 是上述内部
> 版本检查，不属于对外 ABI 契约。
