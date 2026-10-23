# x86 OS 架构设计（Linux 式内核，非“一切皆文件”）

## 1. 目标

内核组织方式沿用 Linux 的思路：**单体内核 + 系统调用 + 进程/线程 + 驱动 + 分层子系统**，
但**不采用“一切皆文件”**的抽象：

- **文件只表示持久化的字节数据**，不再兼任设备、进程信息、管道；
- **程序（Program）是独立的一等对象**，不是“磁盘上的可执行文件路径”；
- **设备与进程信息通过类型化系统调用访问**，不伪装成 `/dev`、`/proc` 下的文件；
- **IPC 用消息通道**，不是字节流管道文件。

一句话：Linux 的骨架，但对象模型是“**程序 / 文件 / 设备 / 进程 / 通道**”五类并列，而不是统一成文件描述符。

## 2. 五类一等对象

| 对象 | 标识 | 生命周期 | 访问接口（系统调用） |
|---|---|---|---|
| **Program** 程序 | `program_id`（名 + 版本，如 `shell@1`） | 注册后常驻，只读 | `prog_list` / `prog_info` / `prog_load` |
| **Process** 进程 | `pid` | 创建 → 退出 | `proc_spawn` / `proc_wait` / `proc_exit` / `proc_info` |
| **File** 文件 | `(volume_id, path)` | 打开后持引用 | `file_open` / `file_read` / `file_write` / `file_stat` / `file_close` |
| **Device** 设备 | `(class, instance)`，如 `block:0`、`char:kbd0` | 驱动注册后常驻 | `dev_list` / `dev_open` / 类型化调用（见 §4） |
| **Channel** 通道 | `channel_id` | `chan_create` → `chan_close` | `chan_send` / `chan_recv` / `chan_poll` |

关键约束：

- 文件句柄、设备句柄、通道句柄是**不同类型的句柄**，混用会在系统调用入口被拒绝；
- 进程、程序、设备**没有路径名**，只能通过各自的查询/注册接口访问；
- 只有 **File** 有“路径”概念，路径只存在于卷（volume）命名空间里。

## 3. 命名空间分离

```
文件命名空间    volume_id + "/dir/name"            （只有数据文件）
程序命名空间    program name + version             （如 shell@1、memtest@1）
设备命名空间    device class + instance            （如 block:0、char:kbd0、video:fb0）
进程命名空间    pid                                 （运行期唯一）
```

因此不存在这些“假文件”：

| Linux | 本设计 |
|---|---|
| `/dev/sda` | `dev_open(block:0)` |
| `/dev/tty` | `dev_open(char:kbd0)` + `dev_open(video:fb0)` |
| `/proc/<pid>/stat` | `proc_info(pid, &info)` |
| `/proc/meminfo` | `sys_query(SYS_MEMINFO, &buf)` |
| `pipe()` | `chan_create()` → `chan_send/recv` |
| `execve("/bin/sh")` | `proc_spawn(prog_lookup("shell@1"), args)` |

## 4. 设备模型：类型化接口，而不是 ioctl

设备按类提供函数表，调用方拿到的是具体类型的操作，不需要“打开一个文件再 ioctl”：

```c
typedef struct {
    int  (*init)(void);
    int  (*read_block)(uint32_t lba, void *buf, uint32_t count);
    int  (*write_block)(uint32_t lba, const void *buf, uint32_t count);
    uint32_t block_size;
    uint32_t block_count;
} block_ops_t;

typedef struct {
    int (*putc)(char c);
    int (*getc)(char *out);      /* 返回 0 表示暂无输入 */
} char_ops_t;
```

驱动注册时把 `ops` 挂到设备表；上层通过 `dev_open(class:instance)` 拿到一个**强类型句柄**，
再调用 `block_ops_t` 里的函数。这样：

- 不需要为每种设备发明 ioctl 号；
- 编译期就能发现“拿字符设备当块设备用”的错误；
- 设备不是文件，也就不会出现在文件路径里。

## 5. 程序模型：程序 ≠ 可执行文件

程序是**只读代码映像 + 元数据**的注册项：

```c
typedef struct {
    char      name[32];
    uint32_t  version;
    uint32_t  entry;        /* 映像内入口偏移 */
    uint32_t  image_size;
    const void *image;      /* 只读映像（内建程序时指向内核 .rodata） */
    uint32_t  flags;        /* 是否需要控制台、是否需要块设备…… */
} program_t;

uint32_t prog_register(const program_t *p);
const program_t *prog_lookup(const char *name, uint32_t version);
uint32_t prog_list(uint32_t *ids, uint32_t max);
```

- **启动程序** = 创建进程 + 映射程序映像（多进程可共享只读页）+ 建立初始通道；
- **程序来源**分两阶段演进：
  1. 内建程序表（编译进内核，当前阶段）；
  2. 程序存储区（独立于文件系统的只读区域），由 `prog_load(blob)` 注册；
- 文件系统里的文件**默认不可执行**；即使将来支持从文件装载程序，也要经过
  `prog_load()` 校验并注册成 program 对象，而不是直接 `exec` 一个路径。

**shell 属于这一层，而不是内核**：它是用 `prog_register("shell", …)` 注册的普通程序，
由 init 进程 `proc_spawn` 启动，通过读设备/通道拿到命令行，再调用 `proc_spawn`
执行其它程序。内核只负责“启动第一个程序”，不解释任何命令。
（当前实现：`apps/shell.c` 注册为 `shell@1`，内核用 `prog_run("shell")` 把它当 init 启动；
因为还没有进程/用户态，它暂时运行在内核上下文里。）

## 6. 进程与调度

```
proc_spawn(program_id, argv, env_channel) -> pid
   ├─ 分配地址空间（vmm）
   ├─ 映射程序映像（只读，可与其它进程共享）
   ├─ 分配内核栈、用户栈
   ├─ 建立初始通道（stdin/stdout 由通道表达，不是文件描述符 0/1/2）
   └─ 放入就绪队列
```

- 调度：优先级 + 时间片轮转，时钟中断里做抢占决策；
- 进程退出一律通过 `proc_exit(status)`，父进程用 `proc_wait(pid, &status)` 回收；
- 进程信息通过 `proc_info()` 查询，不提供 `/proc`。

## 7. 系统调用接口（int 0x80）

按对象分组，而不是“一个 read 走天下”：

| 组 | 调用 |
|---|---|
| 程序 | `prog_list` `prog_info` `prog_load` |
| 进程 | `proc_spawn` `proc_wait` `proc_exit` `proc_info` |
| 文件 | `file_open` `file_read` `file_write` `file_stat` `file_unlink` `file_close` |
| 设备 | `dev_list` `dev_open` `dev_block_read` `dev_block_write` `dev_char_putc` `dev_char_getc` |
| 通道 | `chan_create` `chan_send` `chan_recv` `chan_poll` `chan_close` |
| 系统 | `sys_query`（内存/时间/版本）`sys_shutdown` |

约定：**句柄自带类型标签**（高 8 位为类型），系统调用入口统一校验类型，类型不符返回 `-EINVAL`。

## 8. 内核分层（与 Linux 对应）

```
arch/       boot、gdt/idt、上下文切换、系统调用门
mm/         pmm、vmm、堆（已有）
kernel/     对象表、程序表、进程表、调度器、系统调用分发
fs/         卷管理、文件记录、目录（只服务 File 对象）
drivers/    console/fb、键盘、pit、块设备（显式注册，不进文件树）
lib/        kprintf、string（已有）
```

与 Linux 的对应关系：`mm`、`lib`、`arch` 基本同构；差别在 `fs` 只负责文件，
设备与进程信息不再借道文件系统。

## 9. 与 Linux 的差异对照

| 主题 | Linux | 本设计 |
|---|---|---|
| 统一抽象 | 一切皆文件 | 五类对象并列，句柄带类型 |
| 设备访问 | `/dev/*` + ioctl | `dev_open(class:instance)` + 类型化 ops |
| 进程信息 | `/proc` | `proc_info()` / `sys_query()` |
| IPC | pipe / socket（文件） | `channel` 消息对象 |
| 启动程序 | `execve(path)` | `proc_spawn(program_id)`，程序先注册 |
| 文件 | 可执行、设备、管道都是文件 | 只有数据文件，路径只在卷内有效 |

## 10. 落地顺序

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| **P0** | 对象表 + 程序注册表 + 内存文件卷 + 启动自检 | 启动日志里程序表/文件读写/句柄类型校验全部通过 |
| **P1** | 通道（消息 IPC）+ 进程结构（内核线程）+ 简单调度 | `chan send/recv` 往返、两个内核线程交替运行 |
| **P2** | 系统调用门（int 0x80）+ 用户态 + 句柄校验 | 用户态程序通过系统调用打印、退出，非法句柄被拒绝 |
| **P3** | 块设备驱动 + 真实文件系统 + 程序装载 | 从程序存储区加载并运行一个非内建程序 |

当前代码已有：console/kprintf/panic、pmm/vmm/heap、IDT/PIC/PIT/键盘。
P0 可以在现有基础上直接叠加，不需要推翻已有模块。
