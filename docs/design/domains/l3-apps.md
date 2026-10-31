# L3 应用（plane=l3）

> 状态：WP0-2 域文档补全，2026-10-31；描述 ng HEAD `c4efabc` 的已提交实现。
> 上位：`../architecture.md` §2.5、§5 不变量 3（默认无根/无设备/无网络）。
> 证据：`../../claims/ng-WP1-7.md`、`ng-WP2-6.md`、`ng-WP3-0.md`、`ng-WP3-1.md`。
> 注意：**L3 的“isolated”目前只对 exit/kill 成立；对 #PF/#GP 不成立**（D06）。

## 职责

| 应用 | 形态 | 代码 | 状态 |
|---|---|---|---|
| `hello` | 汇编 ring3 smoke，ELF64 → `.incbin` 进内核 | `src/ng/user/hello.S` | 已实现（WP1-7） |
| `shell` | 64 位用户态 CLI：15 条命令、行编辑、`>` 重定向、cwd、登录/日志/pkg | `src/apps/shell/shell.c` | 已实现（WP2-6）；WP3-0 加 `spawn/wait/pid` |
| `demo` | GUI demo：读 `/run/display.port` → CREATE → SHM_MAP → 画渐变/矩形/文字 → COMMIT → 等 INPUT；含 `hold/window` 角色 | `src/apps/demo/demo.c` | 已实现（WP3-1/3-2） |
| `proc` | 多角色用户程序：hello / chain / relay / server / hold，演示 3 进程链式 ping | `src/apps/proc/proc.c` | 已实现（WP3-0） |
| 编辑器 / 终端 GUI / 文件管理器 | — | — | D0 设计条目，未实现 |
| 浏览器 | 自有引擎 B0 长线 | — | 未开始（ADR-008） |
| SDK 示例应用 | `sdk/` | — | **不存在**（WP3-4） |

## 不做什么

1. **不直接碰硬件**：没有 MMIO/DMA/端口 IO；显示只能经 displayd 协议 + SHM；
   文件只能经 syscall 20..27；`SYS_FB_MAP`（47）设计上 root-only，当前所有进程
   uid=0，所以普通应用实际也能调用（D14；displayd 是唯一使用者）。
2. **默认没有文件系统根/设备/网络**：这是 architecture 的设计；**当前未实现沙箱**，
   所有进程 uid=0、共享全局 VFS ctx，应用实际能开任何路径（D14）。
3. **不用 libc/POSIX**：静态 ELF64、无 CRT、无动态链接、无 `malloc`/`printf`；
   字符串/格式化在各自文件里。
4. **不做 GUI 工具包/窗口管理**：应用自己建 surface；WM 只切焦点，不装饰。
5. **不做作业控制/管道/信号语义**：shell 无管道、无 `>>`、无引号/通配符/历史/补全；
   `kill` 只记状态码，不做异步信号投递。
6. **不做包安装/启动**：pkg 只列表/校验；WM 启动器只启动 `/bin/*`，包入口未解析。
7. **不做用户态异常处理**：ring3 #PF/#GP → panic（D06）。

## 接口（syscall / IPC / ABI）

### 进程入口 ABI

```text
ELF64 ET_EXEC / EM_X86_64，静态链接，基址 0x40000000
rdi = argc
rsi = argv（用户栈上的 char*[]，NULL 结尾；argv[0] 是程序名）
入口 RSP 16 字节对齐；用户栈 16 KiB @ 0x40100000，守护页 0x400FF000 不映射
退出：syscall 1，code 取低 8 位；被 kill 的状态 = 0x100|sig
```

`spawn` 的限制：ELF ≤256 KiB；path ≤63 字符；argv ≤8 项、单项 <128 字节、
总字符串 ≤1 KiB；用户窗口 `[0x40000000, 0x7FFFFFFFFF]`；段 W^X（不允许 RWX）。

### syscall 使用

| 应用 | 主要调用 | 说明 |
|---|---|---|
| `hello` | 0 write、1 exit、30 readkey | 串口输出；输入轮询超时也通过 |
| `shell` | 0/1、20–27、30、31、32、33、34/35、40/41/42 | 全部经 uaccess；单次 write ≤256、fd read/write ≤512、listdir ≤8 |
| `demo` | 0/1、10–14、20–27、45/46 | 读发现文件、`PORT_CREATE`、`SHM_MAP`、`MAP/COMMIT/FOCUS`、等 `INPUT` |
| `proc` | 0/1、10–14、40–44 | 3 进程链式 ping；ticket 经 argv 与消息 `from` 两跳传递 |
| `wm`（L2b） | 0/1、10–14、20–27、40、45/46 | 面板 surface；LIST/FOCUS/REGISTER_HOTKEY/WM_STATUS；启动器 spawn |

### 应用可见的 ABI 面

- syscall 表/错误码/句柄语义见 [`../../abi/abi-1.0-draft.md`](../../abi/abi-1.0-draft.md)。
- 没有 ABI 版本查询 syscall；没有 `mmap/mprotect/lseek/dup/pipe/socket`；没有 sleep/时钟。
- 没有 libc 头；应用自己声明 syscall 常量。SDK alpha（WP3-4）之前，外部开发者无法
  按“文档 + 头文件”直接编译（D12）。
- 包格式只是目录 + 5 字段 manifest；没有运行时加载器/签名（B 域）。

## 失败等级与恢复

| 场景 | 设计 | 当前行为 | 证据/差距 |
|---|---|---|---|
| 应用 exit | `isolated` | 回收 AS、句柄、端口、共享页；父进程 `wait` 收尸 | WP3-0 |
| 应用被 kill | `isolated` | 摘 IPC 等待者/线程、关端口、销毁 AS；状态 `0x100\|sig` | WP3-0；杀不掉 ring3 死循环（无 tick） |
| 应用 #PF/#GP | `isolated` | **panic 全系统** | D06；WP3-5 改为杀进程 |
| 应用卡死（死循环） | `isolated` | 协作式调度，`kill` 不生效 | 需要 tick IRQ/抢占 |
| 应用 OOM/坏指针 | `isolated` | syscall 返回 `ENOMEM/EFAULT`；不会因坏用户指针 panic（uaccess 先校验） | 已实现 |
| shell 崩溃 | S1 | 当前 shell 是独立进程；杀 shell 不影响已启动应用 | S1 的“输入恢复/面板回来”未验收（D16） |

**当前没有沙箱/命名空间/uid 隔离**：L3 失败隔离主要是地址空间 + 句柄表 +
进程回收；权限面仍是 root（D14）。

## manifest 五元组

设计值（当前无解析器，D04）。L3 应用允许没有运行时 fallback：

| 组件 | plane | criticality | fallback | health | restart |
|---|---|---|---|---|---|
| `shell` | `l3` | `isolated` | `none` | `none` | `never`（由 session manager 拉起） |
| `demo` | `l3` | `isolated` | `none` | `none` | `never` |
| `proc` | `l3`（诊断程序） | `isolated` | `none` | `none` | `never` |
| 普通 GUI app（未来） | `l3` | `isolated` | 应用自定义（可 `none`） | `optional heartbeat` | `on-failure` 由 L2c 决定 |

规则：`isolated` 表示死一个不扩散；`fallback=none` 对 L3 合法。应用的生命周期
归 L2c session manager，不归 shell（ADR-006）。当前由内核自检/调用者 `spawn`，
没有 session manager（WP3-3 进行中）。

## 当前实现状态（ng WP）

| WP | 内容 | 证据 |
|---|---|---|
| WP1-7 | ELF64 hello、ring3、syscall 0/1、用户 AS/uaccess | `../../claims/ng-WP1-7.md` |
| WP2-6 | 用户态 shell（15 命令、行编辑、重定向、cwd、错误码） | `../../claims/ng-WP2-6.md` |
| WP3-0 | `/bin/proc` 多角色、3 进程链式 ping、shell `spawn/wait/pid` | `../../claims/ng-WP3-0.md` |
| WP3-1 | `/bin/demo` GUI 客户端、SHM/LFB、QMP 像素断言 | `../../claims/ng-WP3-1.md` |
| WP3-2 | `/bin/demo window` 常驻角色、title、WM 启动器 | `../../claims/ng-WP3-2.md` |

构建：用户程序用 `clang --target=x86_64-elf`、`-mno-sse -mgeneral-regs-only`、
`-fno-pic -fno-pie`、链接脚本 `src/ng/user/user.ld`，嵌入内核 `.rodata`。
内核只跑静态 ELF64；没有从包目录启动应用的路径。

## 待办

1. **SDK alpha（WP3-4）**：`sdk/` 头文件、syscall 包装、示例 GUI app；ABI 1.0 草案先行。
2. **沙箱/命名空间**：每应用根、picker/broker、网络 broker；先做 `proc->uid` 传进 VFS（D14）。
3. **用户态 fault → 杀进程**：让 isolated 对异常成立（D06，WP3-5）。
4. **session manager 管理生命周期**：应用注册/启动/回收/重启；shell 不拥有应用（WP3-3）。
5. **终端/编辑器**：D0 的 L3 切片；终端要 TTY/display 输出，编辑器要文件选择器。
6. **包启动**：解析 manifest `entry`、版本选择、安装事务（B/L1）。
7. **GUI 工具包**：surface/事件/控件；避免每个 app 重复实现协议。
8. **崩溃报告**：应用死亡原因/状态码/最小转储，供 L2c 重启策略使用。
