# Aster Native ABI v0（草案）

> 状态：实验性（v0.x 允许破坏性修改；v1.0 冻结后只追加）
>
> 关联文档：[系统结构模型](system-model.md)
>
> 范围：Aster 原生程序的二进制接口。POSIX/Linux profile 不在此文档中定义。

---

## 0. v0 实现状态（2026-10-23）

| 能力 | 状态 |
|---|---|
| `int 0x80` 原生门 + `int 0x81` 旧 ABI 过渡门 | 已实现 |
| `SYS_ABI_VERSION` / `SYS_EXIT` / `SYS_YIELD` | 已实现 |
| `SYS_HANDLE_CLOSE` / `DUP` / `INFO` | 已实现，每进程 16 槽句柄表 |
| 进程 self handle（槽 0） | 已实现，供后续 bootstrap 使用 |
| `SYS_DEBUG_WRITE` | 已实现（串口/控制台） |
| ring3 调用验证 | 已实现（userdemo 执行 ABI 版本查询 + 调试输出 + 原生 exit） |
| `SYS_PROCESS_CREATE` / `SYS_PROCESS_WAIT` | 已实现简化 v0：按注册程序名创建子进程并等待 |
| Port / Channel / Memory | 未实现，编号保留 |
| SealedImage / sandbox | 未实现，feature bits 暂为 0 |

> 命名说明：本表的 `SYS_ABI_VERSION` 指 `int 0x80` 的
> `NATIVE_SYS_ABI_VERSION`（`0x00`，写 `struct native_abi_version`）。
> `int 0x81` 另有一个同名标量查询 `SYS_ABI_VERSION`（编号 59，返回
> `(OS_ABI_VERSION << 16) | feature_bits`），两套门编号空间独立，见第 18 节。

> 注意：当前 `SYS_HANDLE_INFO` 的句柄值尚未通过 bootstrap 交给用户程序；
> 用户态程序暂时只能验证 ABI 版本、调试输出和退出。句柄表已在 ring0 自检和
> 进程生命周期中接通，等 `process_create` + bootstrap 块落地后即可从 ring3 完整使用。

---

## 1. 目标

Native ABI 是 Aster 的 canonical ABI：新程序、系统服务、shell、测试都优先使用它。
它要满足：

1. 不依赖 POSIX 语义：没有 fd、`errno`、`fork`、`execve`、全局 `/`。
2. 所有资源通过带类型和权限的句柄访问。
3. 系统调用只暴露机制；文件系统、控制台、网络通过服务 Channel 提供。
4. 用户指针不可信；所有跨边界访问可失败、可恢复。
5. 能被签名和沙箱约束：启动时只拿到显式授予的句柄。
6. 先支持 32 位 x86，后续 x86-64 另立 ABI 版本。

### 非目标

- 不追求与 Linux/Windows 二进制兼容。
- 不在 v0 定义动态链接、TLS、信号、`fork`、`mmap` 文件后端。
- 不定义完整 TCP/IP 或文件系统协议；这些属于服务协议。
- 不承诺 ABI 1.0 之前的稳定性。

---

## 2. 执行环境

| 项目 | 约定 |
|---|---|
| CPU | i386，保护模式，ring3 |
| 地址空间 | 用户 `0x00000000–0xBFFFFFFF`，内核 `0xC0000000–0xFFFFFFFF` |
| 段 | 平坦用户代码/数据段；不使用分段隔离 |
| 页大小 | 4KB |
| 系统调用门 | `int 0x80`，DPL3 |
| 迁移期旧 ABI | `int 0x81`，只用于现有原型，不承诺长期兼容 |
| 栈 | 用户栈向下增长，至少有 guard page |
| 对齐 | 栈在调用 `main` 前 16 字节对齐；结构体按 4 字节对齐 |
| 字节序 | little-endian |
| 可执行格式 | ELF32，`ET_EXEC`，`EM_386`，静态链接 |
| 异常 | 用户态异常由内核转为故障事件；不直接 panic |

---

## 3. 基本类型

```c
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;
typedef u32      handle_t;
typedef s32      status_t;
```

- `handle_t` 是 32 位不透明值；`0` 永远无效。
- 句柄内部由 `slot + generation + type` 组成，但用户不能解码或伪造。
- 所有结构体字段按自然对齐，保留字段必须清零。

---

## 4. 系统调用约定

### 4.1 入口

```asm
mov eax, syscall_number
mov ebx, arg0
mov ecx, arg1
mov edx, arg2
mov esi, arg3
mov edi, arg4
int 0x80
```

- `eax`：系统调用号。
- `ebx / ecx / edx / esi / edi`：最多 5 个参数；更多参数用指针传结构体。
- 返回值在 `eax`。
- 内核保证 `ebx / esi / edi / ebp` 不被破坏。
- `eax / ecx / edx` 可能被破坏，调用方不能依赖。
- `DF` 必须为 0；内核入口和返回都保证。
- 系统调用可能阻塞；返回时用户寄存器现场恢复。

### 4.2 返回值

- `>= 0`：成功；具体含义由调用定义（长度、句柄、地址等）。
- `< 0`：错误码，见第 5 节。
- 不使用 `errno`，不设置全局错误变量。

### 4.3 指针规则

- 用户指针必须完全落在 `0x00000000–0xBFFFFFFF`。
- 内核用 `copy_from_user` / `copy_to_user` 访问；长度溢出、跨边界、未映射都返回 `E_FAULT`。
- 字符串必须有 NUL 结尾，并有最大长度限制。
- 内核不直接解引用用户指针。

---

## 5. 错误码

```c
#define E_OK         0
#define E_INVAL     -1   /* 参数非法 */
#define E_NOENT     -2   /* 对象/名字不存在 */
#define E_ACCESS    -3   /* 权限不足 */
#define E_TYPE      -4   /* 句柄类型错误 */
#define E_NOSPC     -5   /* 资源耗尽 */
#define E_AGAIN     -6   /* 暂时不可用/队列满 */
#define E_TIMEOUT   -7   /* 超时 */
#define E_BADF      -8   /* 无效句柄 */
#define E_NOSYS     -9   /* 未实现/已移除的系统调用 */
#define E_FAULT     -10  /* 用户内存访问失败 */
#define E_OVERFLOW  -11  /* 缓冲区或计数溢出 */
#define E_BUSY      -12  /* 对象忙 */
#define E_IO        -13  /* 底层 I/O 错误 */
```

错误码是 Native ABI 的一部分，不能随实现随意改变。

---

## 6. 句柄与权限

### 6.1 句柄表

每个进程一张句柄表。表项至少包含：

```c
struct handle_entry {
    void       *object;
    u32         type;
    u32         rights;
    u32         generation;
    handle_t    parent;     /* 授权来源，用于撤销 */
    u32         flags;
};
```

- 关闭句柄时 `generation++`，旧句柄立即失效。
- 句柄值 0 保留为无效。
- 跨进程传递只能通过 IPC 或显式创建接口，不能直接给数值。

### 6.2 权限位

```c
#define R_READ      (1u << 0)
#define R_WRITE     (1u << 1)
#define R_EXEC      (1u << 2)
#define R_MAP       (1u << 3)
#define R_WAIT      (1u << 4)
#define R_SIGNAL    (1u << 5)
#define R_CONNECT   (1u << 6)
#define R_ACCEPT    (1u << 7)
#define R_TRANSFER  (1u << 8)
#define R_MANAGE    (1u << 9)
#define R_ACK       (1u << 10)
```

权限只能缩小，不能通过 `dup` 扩大。IPC 传递时接收方获得发送方持有的权限；v0 不支持逐句柄指定子集。

### 6.3 句柄操作

- `SYS_HANDLE_CLOSE`：关闭句柄；重复关闭返回 `E_BADF`。
- `SYS_HANDLE_DUP`：复制句柄并指定权限子集。
- `SYS_HANDLE_INFO`：查询类型、权限、对象 ID；不泄露内核地址。
- 撤销（revoke）在 v0 只做单句柄关闭；递归撤销留到后续版本。

---

## 7. 对象类型

v0 至少定义以下类型标签：

```c
#define OBJ_PROCESS       1
#define OBJ_THREAD        2
#define OBJ_MEMORY        3
#define OBJ_CHANNEL       4
#define OBJ_PORT          5
#define OBJ_EVENT         6
#define OBJ_NAMESPACE     7
#define OBJ_FILE          8
#define OBJ_DIRECTORY     9
#define OBJ_VOLUME        10
#define OBJ_DEVICE        11
#define OBJ_INTERRUPT     12
#define OBJ_SEALED_IMAGE  13
#define OBJ_TIMER         14
```

对象类型必须稳定；新增类型只能追加。类型不匹配返回 `E_TYPE`。

v0 实际暴露创建/获取接口的类型是 Process、Memory、Channel、Port、SealedImage 和 IRQ；
Event/Timer/File/Directory/Volume/Device 的类型标签先预留，创建或打开接口在后续 minor 版本加入。

---

## 8. 系统调用表 v0

> 编号一旦发布不改变；未实现的编号返回 `E_NOSYS`。

| 编号 | 名称 | 参数 | 返回 |
|---|---|---|---|
| `0x00` | `SYS_ABI_VERSION` | `ebx=struct abi_version*` | `0` |
| `0x01` | `SYS_EXIT` | `ebx=status` | 不返回 |
| `0x02` | `SYS_YIELD` | — | `0` |
| `0x20` | `SYS_PROCESS_CREATE` | `ebx=name, ecx=len, edx=rights` | process handle |
| `0x21` | `SYS_PROCESS_WAIT` | `ebx=process handle, ecx=&status` | pid |
| `0x10` | `SYS_HANDLE_CLOSE` | `ebx=handle` | `0` |
| `0x11` | `SYS_HANDLE_DUP` | `ebx=handle, ecx=rights` | 新句柄 |
| `0x12` | `SYS_HANDLE_INFO` | `ebx=handle, ecx=struct handle_info*` | `0` |
| `0x1e` | `SYS_PORT_CREATE` | `ebx=flags` | Port 句柄 |
| `0x1f` | `SYS_PORT_ACCEPT` | `ebx=Port, ecx=timeout_ms` | Channel 句柄 |
| `0x20` | `SYS_PORT_CONNECT` | `ebx=Port, ecx=timeout_ms` | Channel 句柄 |
| `0x21` | `SYS_CHANNEL_SEND` | `ebx=Channel, ecx=struct ipc_msg*, edx=timeout_ms` | `0` |
| `0x22` | `SYS_CHANNEL_RECV` | `ebx=Channel, ecx=struct ipc_msg*, edx=timeout_ms` | `0` |
| `0x23` | `SYS_WAIT_ANY` | `ebx=wait_item*, ecx=count, edx=timeout_ms, esi=wait_result*` | `0` |
| `0x30` | `SYS_MEM_CREATE` | `ebx=size, ecx=flags` | Memory 句柄 |
| `0x31` | `SYS_MEM_MAP` | `ebx=Memory, ecx=addr_hint, edx=len, esi=prot, edi=flags` | 用户地址 |
| `0x32` | `SYS_MEM_UNMAP` | `ebx=addr, ecx=len` | `0` |
| `0x33` | `SYS_MEM_PROTECT` | `ebx=addr, ecx=len, edx=prot` | `0` |
| `0x40` | `SYS_TIME_NOW` | `ebx=u64* ns` | `0` |
| `0x41` | `SYS_SLEEP` | `ebx=ms` | `0` |
| `0x50` | `SYS_DEBUG_WRITE` | `ebx=buf, ecx=len` | 写入长度 |
| `0x60` | `SYS_IRQ_WAIT` | `ebx=IRQ 句柄, ecx=timeout_ms` | `0` |
| `0x61` | `SYS_IRQ_ACK` | `ebx=IRQ 句柄` | `0` |

保留区间：

- `0x05–0x0f`：进程/线程扩展
- `0x13–0x1d`：句柄/对象扩展
- `0x24–0x2f`：IPC 扩展
- `0x34–0x3f`：内存扩展
- `0x42–0x4f`：时间扩展
- `0x52–0x5f`：调试扩展
- `0x63–0x6f`：设备/DMA 扩展

> 本表是 `int 0x80` 的 Native 编号。`int 0x81` 的 legacy 表（1..58，以及
> v1 新增的 `SYS_ABI_VERSION=59`）定义在 `include/abi.h`，不要与本表混用；
> 两套门由不同的 IDT 向量进入。

---

## 9. 关键结构

### 9.1 ABI 版本

```c
struct abi_version {
    u16 major;
    u16 minor;
    u16 patch;
    u16 reserved;
    u32 features;
};
```

v0 初始值：`major=0, minor=1, patch=0`。

Feature bits：

```c
#define ABI_F_IPC_HANDLES   (1u << 0)
#define ABI_F_SHARED_MEM    (1u << 1)
#define ABI_F_SEALED_IMAGE  (1u << 2)
#define ABI_F_WAIT_ANY      (1u << 3)
#define ABI_F_SANDBOX       (1u << 4)
```

### 9.2 句柄信息

```c
struct handle_info {
    u32 type;
    u32 rights;
    u32 object_id;
    u32 flags;
};
```

`object_id` 仅用于调试/审计，不能用于重新获得权限。

### 9.3 进程创建

```c
#define PROCESS_MAX_HANDLES 8

struct process_create_args {
    u32      size;             /* sizeof(struct process_create_args) */
    handle_t image;            /* SealedImage 句柄 */
    handle_t namespace_root;   /* Namespace 句柄 */
    u32      flags;
    u32      handle_count;     /* <= PROCESS_MAX_HANDLES */
    handle_t handles[PROCESS_MAX_HANDLES];
    u32      argv_count;
    u32      argv_ptr;         /* char *argv[]，调用方地址空间 */
    u32      env_count;
    u32      env_ptr;          /* char *envp[]，可为 0 */
};
```

规则：

- `image` 必须是 `OBJ_SEALED_IMAGE`，普通 File 不能直接执行。
- 内核复制 argv/env 字符串到新进程地址空间。
- `handle_count` 中的句柄在子进程创建时复制；权限不能扩大。
- 内核不额外保留父进程对子进程的引用；返回的进程句柄是调用方唯一的授权来源。
- 调用者必须对 `namespace_root` 持有 `R_MANAGE`，并对 `image` 持有 `R_EXEC` 或 `R_MAP`；
  沙箱可以完全屏蔽该调用，此时返回 `E_ACCESS`。
- v0 只创建单线程进程；线程创建后续加入。

### 9.4 等待

```c
#define WAIT_READABLE   (1u << 0)
#define WAIT_WRITABLE   (1u << 1)
#define WAIT_SIGNALED   (1u << 2)
#define WAIT_EXITED     (1u << 3)

struct wait_item {
    handle_t handle;
    u32      events;
};

struct wait_result {
    u32 index;
    u32 events;
};
```

`SYS_WAIT_ANY` 最多等待 16 个句柄；返回第一个就绪项。`timeout_ms=0` 表示立即返回，`0xFFFFFFFF` 表示无限等待。

---

## 10. IPC 消息格式

```c
#define IPC_MAX_HANDLES 4
#define IPC_MAX_INLINE  64

struct ipc_msg {
    u16      type;
    u16      flags;
    u32      data_len;       /* <= IPC_MAX_INLINE */
    u32      handle_count;   /* <= IPC_MAX_HANDLES */
    handle_t handles[IPC_MAX_HANDLES];
    u8       data[IPC_MAX_INLINE];
};
```

规则：

- 消息是值语义；接收方得到消息副本。
- `handles` 由内核在发送时验证权限并复制到接收方句柄表；发送方保留自己的句柄。
- `data_len` 超过 64 时，发送方应附带一个共享 Memory 句柄，数据区只放描述符。
- 队列深度 v0 固定为 16 条；满时 `SEND` 返回 `E_AGAIN` 或按超时阻塞。
- `RECV` 缓冲区不足返回 `E_OVERFLOW`，消息不丢失。
- Channel 是双向的；请求/回复由服务协议自己定义，v0 不提供内置 `call/reply`。
- 传递句柄需要发送方拥有 `R_TRANSFER`；接收方得到发送方持有的权限集合。

### 10.1 Port 与 Channel

- `PORT_CREATE` 创建监听端点，返回带 `R_ACCEPT | R_MANAGE` 的 Port 句柄。
- 服务端用 `PORT_ACCEPT` 等待客户端；客户端用 `PORT_CONNECT` 连接。
- 连接成功后双方各得到一个 Channel 句柄。
- 客户端只需要 Port 的 `R_CONNECT` 权限。
- 关闭 Channel 一端后，另一端读写得到 `E_NOENT` 或 `E_BADF`。

---

## 11. 内存接口

### 11.1 flags

```c
#define MEM_F_SHARED    (1u << 0)
#define MEM_F_ZERO      (1u << 1)

#define MAP_F_FIXED     (1u << 0)
#define MAP_F_SHARED    (1u << 1)
#define MAP_F_GUARD     (1u << 2)   /* 在映射末尾加 guard page（仅映射时） */

#define PROT_READ       (1u << 0)
#define PROT_WRITE      (1u << 1)
#define PROT_EXEC       (1u << 2)
```

### 11.2 语义

- `SYS_MEM_CREATE(size, flags)` 返回 Memory 句柄；大小按页向上取整。
- `SYS_MEM_MAP(mem, addr_hint, len, prot, flags)` 返回实际用户地址；`addr_hint=0` 表示由内核选择。
- `SYS_MEM_UNMAP(addr, len)` 解除映射；未映射范围返回 `E_NOENT`。
- `SYS_MEM_PROTECT(addr, len, prot)` 修改权限；不允许把内核页改成用户可访问。
- 映射长度按页向上取整；地址必须页对齐。
- 用户页不能同时 `PROT_WRITE | PROT_EXEC`；硬件支持 NX 时强制 W^X，不支持时至少做段权限检查。
- `MAP_F_SHARED` 要求 Memory 对象有 `MEM_F_SHARED`。
- 共享 Memory 可用于 IPC 大数据和进程间共享缓冲区。

---

## 12. 进程启动约定

### 12.1 入口栈

内核把以下内容放在新进程用户栈顶：

```text
esp + 0   : argc
esp + 4   : argv   (char **)
esp + 8   : envp   (char **)
esp + 12  : bootstrap (struct aster_bootstrap *)
```

`_start` 由 `ulib` 提供，读取这些值后调用：

```c
void _start(void);
int main(int argc, char **argv, char **envp);
```

### 12.2 Bootstrap 块

```c
#define ASTER_BOOT_MAGIC 0x42545341u   /* "ASTB" */
#define ASTER_BOOT_MAX_HANDLES 8

struct aster_bootstrap {
    u32      magic;
    u16      abi_major;
    u16      abi_minor;
    u32      features;
    handle_t self_process;
    handle_t stdin_channel;
    handle_t stdout_channel;
    handle_t stderr_channel;
    handle_t namespace_root;
    u32      handle_count;
    handle_t handles[ASTER_BOOT_MAX_HANDLES];
};
```

规则：

- `stdin/stdout/stderr` 是 Channel 句柄，不是 fd 0/1/2。
- 没有全局控制台；程序只能通过显式 Channel 输出。
- 没有句柄的资源不可访问；`namespace_root` 也只授予 `TRAVERSE`/`READ` 所需的权限。
- Bootstrap 块由内核构造，用户不能伪造。

### 12.3 启动流程

```text
supervisor
  → SYS_PROCESS_CREATE(image, namespace_root, handles, argv)
  → 内核创建 AddressSpace、映射 ELF、构造用户栈
  → 新线程在 _start 进入 ring3
  → _start 校验 bootstrap magic
  → 调用 main
  → main 返回后 _start 调用 SYS_EXIT
```

---

## 13. ELF 与程序包

### 13.1 ELF 要求

- `EI_CLASS = ELFCLASS32`
- `e_machine = EM_386`
- `e_type = ET_EXEC`
- 不允许 `PT_INTERP`；v0 只支持静态链接
- `PT_LOAD` 段必须页对齐，文件偏移与虚拟地址同余
- 入口地址必须落在可执行段内
- 栈不可执行；堆/数据按 W^X 规则映射
- v0 不支持 TLS；需要 TLS 的程序等 v1

### 13.2 包与清单

原生程序以 `.aster` 包分发。manifest 至少包含：

```json
{
  "id": "org.example.hello",
  "version": "1.0.0",
  "abi": "aster-native/0.1",
  "entry": "payload/hello",
  "capabilities": ["stdout", "namespace.read"],
  "sandbox": "default",
  "files_hash": "...",
  "signer": "...",
  "anti_rollback": 1
}
```

- 包签名与信任链见后续 `security-model.md`。
- `trustd` 验证签名后创建 `SealedImage`；`SYS_PROCESS_CREATE` 不接受普通 File。
- 沙箱按 manifest 构造初始句柄集合；未申请的权限不会出现。

---

## 14. 与现有原型的迁移

当前原型的 `int 0x80` 调用表是 POSIX 风格（write/getpid/uptime 等），与 Native ABI 不兼容。

迁移策略：

1. Native ABI 使用 `int 0x80`，从 v0 重新编号。
2. 现有调用表暂时迁到 `int 0x81`，只用于运行旧 shell/测试。
3. 新代码全部使用 Native ABI；旧 gate 在 Native shell、FS 服务、签名/沙箱跑通后移除。
4. 不承诺 `int 0x81` 的长期兼容；它只是迁移脚手架。

---

## 15. 版本与稳定性

### 15.1 阶段

- **v0.x（当前）**：Native ABI 实验期，允许破坏性修改；每次修改更新 `minor`。
- **v1.0（冻结条件）**：Native shell + FS 服务 + 签名验证 + 沙箱 + 回归测试全部
  通过后冻结。冻结动作 = 在本文顶部把状态改为“v1.0 已冻结”，并在
  `include/abi.h` 记录 `OS_ABI_VERSION_MAJOR = 1`。

### 15.2 冻结后的规则（新增只追加）

1. 系统调用编号只增不减；删除的调用保留编号，返回 `E_NOSYS`。
2. 结构体只能尾部追加字段，并保留 `size` 字段做版本判断；字段偏移不得改变。
3. 错误码语义/数值不变；新增错误码只能追加。
4. 旧 minor 版本的程序仍可启动；加载器只拒绝不兼容的 major。

### 15.3 变更流程（Native ABI）

1. 先改单一来源（`proc/trap/native.h` 的编号/结构体；用户可见结构同步到
   `include/abi.h`），再改实现；
2. 追加编号/字段，不重排；同步更新 `_Static_assert`；
3. 更新本文的编号表与结构说明，bump `NATIVE_ABI_MINOR`；
4. `tools\session-run.ps1 -Name api -Test` 连续两次通过；
5. 提交信息写清 ABI 变化（前缀 `[api]`）。

### 15.4 用户态头文件 ABI

`include/abi.h` 是 `int 0x81` legacy 表的单一来源，已按 **v1 冻结**
（编号 1..59、结构体布局、错误码值；新增只能追加）。它的版本与 Native ABI
的 `NATIVE_ABI_*` 独立，详见第 18 节。

- 每个程序在 manifest 中声明 `abi`；加载器拒绝不兼容的 major 版本。


---

## 16. 验收测试

### 16.1 ABI 基础

- `SYS_ABI_VERSION` 返回 `0.1.0` 和预期 feature bits。
- 系统调用返回后 `ebx/esi/edi/ebp` 保持不变。
- 错误码与文档一致。

### 16.2 句柄

- 关闭后旧句柄返回 `E_BADF`。
- `dup` 不能扩大权限。
- 用错误类型句柄调用返回 `E_TYPE`。
- 句柄经 IPC 传递后接收方可读，发送方仍可用。
- 伪造句柄/越界槽位返回 `E_BADF`。

### 16.3 IPC

- 两个进程通过 Port/Channel 完成一次请求/回复。
- 队列满返回 `E_AGAIN`；超时返回 `E_TIMEOUT`。
- 消息句柄计数/长度越界返回 `E_OVERFLOW`。
- 传递句柄后接收方获得正确权限。

### 16.4 内存

- `mem_create` + `map` + 写入 + `unmap`。
- 未映射地址访问触发故障并返回 `E_FAULT`，不 panic。
- `PROT_WRITE | PROT_EXEC` 被拒绝。
- 共享 Memory 在两个进程间可见。

### 16.5 进程与启动

- 从签名 `SealedImage` 创建进程，bootstrap magic 正确。
- `stdout` Channel 能收到程序输出。
- 未签名镜像返回 `E_ACCESS`。
- 沙箱中缺少 `PROC_SPAWN` 时 `SYS_PROCESS_CREATE` 返回 `E_ACCESS`。

### 16.6 兼容性

- `SYS_NOSYS` 对保留编号返回正确错误。
- 不同 minor 版本的程序仍可启动。
- 旧 `int 0x81` 路径只用于回归，不影响 Native ABI。

---

## 17. 开放问题

1. `SYS_TIME_NOW` 用 64 位纳秒指针，是否改为 32 位毫秒返回值？
2. `PROCESS_CREATE` 的 argv/env 复制上限是多少？
3. `WAIT_ANY` 是否支持 Event/Timer，还是 v0 只支持 Channel/Process？
4. 32 位 x86 无 NX 时，W^X 如何退化？
5. `SYS_DEBUG_WRITE` 在生产构建中是否保留？
6. 兼容层是否复用 `int 0x80` 编号空间，还是使用独立 gate？
7. 是否在 v0 就引入 `SYS_CHANNEL_CALL` 内置请求/回复，还是由 ulib 组合？

---

## 18. 用户态 ABI 头 `include/abi.h`（v1 冻结）

### 18.1 定位

`include/abi.h` 是 **legacy `int 0x81` 调用表**的单一来源，同时被内核
（经 `proc/trap/syscall.h` 转发）和用户库（经 `src/user/ulib.h`）包含。
它只放跨边界契约，不放实现：

| 内容 | 说明 |
|---|---|
| `SYS_*` 编号 | 1..58 为既有调用（只迁移不改语义），59 为 v1 新增 |
| `OS_ABI_VERSION` / `OS_ABI_MAGIC` | 头文件 ABI 版本与魔数 `0x4F534142`（"OSAB"） |
| `OS_ABI_F_*` | `SYS_ABI_VERSION` 返回的 feature bits |
| `struct udp_req`、`sys_stat_t`、`sys_mouse_t`、`sys_resinfo_t` | 用户可见共享结构体 |
| `E_*` 子集 | legacy 门可能原样返回的错误码 |

`syscall.h` 现在只是兼容转发（`#include "../../include/abi.h"` 加 `sys_call()`），
7 个 `*_syscall.c` 的 `case` 一行未改。

### 18.2 版本与标识

```c
#define OS_ABI_MAGIC          0x4F534142u   /* "OSAB" */
#define OS_ABI_VERSION        1u            /* major */
#define OS_ABI_VERSION_MAJOR  1
#define OS_ABI_VERSION_MINOR  0
```

- 破坏性变化才 +major；追加调用/字段只 +minor。
- 编号一旦发布不改；新增 legacy 调用从 `SYS_NR_MAX + 1`（当前 60）开始。
- 结构体只能尾部追加字段，并同步更新 `abi.h` 末尾的 `_Static_assert`。

### 18.3 `SYS_ABI_VERSION`（int 0x81，编号 59）

```c
/* 无参数 */
return (OS_ABI_VERSION << 16) | OS_ABI_FEATURES;
```

| 位 | 含义 |
|---|---|
| `31..16` | `OS_ABI_VERSION`（major，当前 1） |
| `15..0` | feature bits：`NATIVE_GATE`(bit0)、`VERSIONED_OPS`(bit1)、`DEVICE_API_V1`(bit2)、`VFS_BACKEND_V1`(bit3)、`SERVICE_MANIFEST_V1`(bit4) |

用户态封装：`ulib.c` 的 `abi_version()`。需要完整
`major/minor/patch/features` 结构时仍走 `int 0x80` 的
`NATIVE_SYS_ABI_VERSION`（0x00），两者不冲突。

### 18.4 公共结构体（布局冻结）

| 结构体 | 大小 | 使用者 | 断言 |
|---|---|---|---|
| `struct udp_req` | 72 | `SYS_UDP_SEND/RECV`、`SYS_SENDTO/RECVFROM` | `abi.h` 固定 4 个偏移 |
| `sys_stat_t` | 24 | `SYS_STAT` | 6 × u32 偏移 0/4/8/12/16/20 |
| `sys_mouse_t` | 12 | `SYS_MOUSE` | 3 × s32 |
| `sys_resinfo_t` | 40 | `SYS_RESINFO` | 10 × u32，末字段偏移 36 |

`_Static_assert` 在 `-Wall -Wextra -Werror` 下必须通过；布局改动会直接
编译失败，这是 v1 冻结的机械保证。错误码同理：`E_OK..E_BUSY` 的数值被
`_Static_assert` 钉死。

### 18.5 冻结流程（新增只追加）

1. 在 `abi.h` 追加新编号（`SYS_NR_MAX + 1`）或结构体尾部字段；
2. 更新 `OS_ABI_VERSION_MINOR` 与 `OS_ABI_F_*`（如果新增能力需要 feature bit）；
3. 更新 `_Static_assert`（新结构体/新字段偏移）；
4. 在对应 `*_syscall.c` 组实现（不新建编号空间、不改旧编号）；
5. 更新本文与 `docs/api.md` 的版本表/示例；
6. `tools\session-run.ps1 -Name api -Test` 连续两次通过；
7. 路径限定提交，前缀 `[api]`（涉及文档用 `[docs]`）。

### 18.6 启动自检

`core/abi_selftest.c` 在启动 `selftests` 阶段执行（只打印，不新增回归检查项）：

```text
abi: version ok magic=4f534142 abi=1.0 features=1f structs=udp_req:72,stat:24,mouse:12,resinfo:40 api=dev1/fs1/svc1
```

它同时用纯判定函数核对三套 experimental API 的版本规则：
`fs_backend_version_ok()`、`service_manifest_version_ok()`，以及所有已注册
设备的 `api_version == DEV_API_VERSION`。失败打印 `abi: version FAILED ...`。
