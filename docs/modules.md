# 内核模块划分（九模块）

本文是模块边界契约。新代码按「归属决策树」定位；跨模块只能 include `src/kernel/include/<mod>.h`。

## 1. 分层与模块

### L0 底座（机制，不含策略）

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M0 启动与内核骨架** | 启动链、init 注册表、日志/panic/符号、公共 libc | 能进内核并跑完 init 的镜像 | 子系统初始化细节、自检、演示、对象表 |

### L1 资源（按资源所有权）

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M1 进程管理** | CPU 时间、进程/线程、调度、trap 入口、信号 | 可运行/切换/回收的执行体 | 具体设备、文件、网络 |
| **M2 内存管理** | 物理页、地址空间、页表、堆、对象生命周期 | 可分配/映射/回收的地址空间 | 调度策略、文件语义 |
| **M3 设备管理** | PCI/ACPI、块设备、字符设备、中断控制器 | 统一设备模型 + 驱动注册 | 文件系统、协议栈、服务编排 |
| **M4 文件系统** | 命名空间、持久化、权限执行 | 路径 → 文件的稳定视图 | 块设备驱动、用户程序 |
| **M5 网络管理** | ARP/ICMP/UDP/TCP、socket | 可收发数据的 socket | 网卡寄存器细节（归 M3） |

### L2 策略（跨资源协调）

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M6 服务管理** | 服务注册/发现/生命周期、channel/port | 可注册、可重启的服务运行时 | 具体服务业务逻辑 |
| **M7 安全管理** | 身份、capability、验签、沙箱、W^X 策略 | 统一权限判定入口 | 文件系统本身、加载器 |

### L3 用户态

| 模块 | 拥有什么 | 产出物 | 拒绝 |
|---|---|---|---|
| **M8 用户态环境** | ulib、ABI 头、ELF 加载、程序注册、shell | ring3 可运行的程序环境 | GDT/页表实现、服务编排、内核 syscall 实现 |

## 2. 归属决策树

```
1. 是启动/日志/panic/libc/init 注册表？        → M0
2. 管 CPU 时间、进程、陷入、调度？              → M1
3. 管页、地址空间、对象生命周期、堆？            → M2
4. 直接访问硬件寄存器、总线、中断控制器？        → M3
5. 管路径、文件、目录、持久化、权限执行？        → M4
6. 管协议、报文、socket？                        → M5
7. 管服务注册、发现、重启、IPC 端口？            → M6
8. 管身份、权限判定、验签、隔离策略？            → M7
9. 在 ring3 跑，或负责加载 ring3？               → M8
10. 都不是 → 不是新模块，是某模块的子功能；禁止新建 misc/core/common
```

## 3. 目录布局

```
src/
├─ boot/  loader/                  M0 启动链
├─ kernel/
│  ├─ include/                     九个对外头（唯一跨模块入口）
│  ├─ core/                        M0：kernel.c / panic / ksym / version / splash
│  ├─ lib/                         M0：kprintf、string
│  ├─ proc/                        M1：进程、调度、switch、gdt、smp、trap/
│  ├─ mm/                          M2：pmm、vmm、heap、object、user_layout
│  ├─ dev/                         M3：device/pci/acpi/rtc、block/、char/、intc/
│  ├─ fs/                          M4：vfs、ramfs、fat16、svcfs、blkfs、pipe
│  ├─ net/                         M5：net 协议栈（e1000 在 dev/net）
│  ├─ svc/                         M6：service、supervisor、channel、port
│  ├─ security/                    M7：trust、cap、cpu features
│  ├─ user/                        M8 内核侧：loader、entry、program registry
│  └─ apps/                        内核启动的服务程序
└─ user/                           M8 用户态：ulib、programs/、services/
```

依赖方向（单向）：

```
M0 → M2 → M1 → M3 → M4
              ↘ M5
M6 →（M1/M2/M4/M5）
M7 →（M1/M2/M4）
M8 → 只依赖 include/abi.h
```

## 4. 硬指标

| 指标 | 阈值 | 检查方式 |
|---|---|---|
| `core/kernel.c` | ≤ 120 行，只含注册表 + init 调用 | 人工 + 行数 |
| `core/` 内容 | 只允许 kernel.c / panic / ksym / version / splash | 目录清单 |
| 跨模块 include | 只能走 `include/<mod>.h` | `tools/check-deps.ps1` |
| 每模块文档 | `docs/modules/<mod>.md`（职责/接口/依赖/不变量/拒绝） | 目录检查 |
| ring3 程序位置 | `src/user/programs/` 或 `src/user/services/` | 目录检查 |
| 自检代码 | 各模块 `*_selftest.c`，不得进 `kernel.c` | 人工 |

## 5. 迁移清单（按风险从低到高）

| 步 | 动作 | 状态 |
|---|---|---|
| 0 | 本文 + `tools/check-deps.ps1` 基线 | ✅ |
| 1 | M2：`heap`、`object`、`user_layout` 迁入 `mm/` | 进行中 |
| 2 | M7：`trust`、`cap`、`cpu` 迁入 `security/` | 待做 |
| 3 | M3：`block/`、`device/pci/acpi/rtc`、`fb/console`、`intc/` 迁入 `dev/` | 待做 |
| 4 | M6：`service`、`channel` 迁入 `svc/` | 待做 |
| 5 | M5：`net` 迁入 `net/`，`e1000` 迁入 `dev/net/` | 待做（`net.c` 在别的会话工作区） |
| 6 | M1：`smp/percpu`、`idl/fault/trap`、`gdt` 迁入 `proc/` | 待做 |
| 7 | M8：三个程序目录合并到 `src/user/`，ABI 头唯一化 | 待做（`user_mode`/`user_demo` 在别的会话工作区） |
| 8 | 拆 `syscall.c` / `native.c` 到各模块 `*_syscall.c` | 待做 |

> 规则：**处于其它会话未提交状态（`git status` 显示 M/??）的文件，等归属会话提交后再搬**，避免路径冲突。
