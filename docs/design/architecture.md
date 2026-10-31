# Aster 架构 v1.0（7 大类）

> 状态：设计基线，2026-10-31。
> 关系：本文件取代 three-layer.md 作为长期架构口径；three-layer.md 保留为 P1–P4 的迁移记录。
> 执行：按 evolution.md 演进，不整体重写；关键取舍见 decisions.md。
> 约束：后续实现会话以此为准；契约变更先改 decisions.md 并 bump 版本。

## 0. 摘要

**5 个运行时域**

1. L0 内核：机制与隔离。
2. L1 系统服务：基础设施。
3. L1D 设备服务：驱动。
4. L2 桌面会话：显示/输入/会话。
5. L3 应用：CLI/GUI。

**2 个横切域**

6. A 平台契约：ABI/SDK/POSIX/包/调试。
7. B 生命周期与运维：身份/沙箱/更新/CI/发布。

分类轴：**信任边界**（能碰什么）、**故障半径**（崩了失去什么）、**变更节奏**（多久变一次）。功能名字不参与分类。

## 1. 目标与非目标

### 目标

1. 通用桌面：64 位、图形会话、可进入应用生态。
2. D0：外部开发者照 SDK 文档写出并打包一个 GUI 应用。
3. 故障隔离：L2 shell 崩溃不影响 L3 应用；L1 服务崩溃可重启；只有 L0 崩溃才全崩。
4. 兼容：D1 提供 POSIX 源码 profile；不做 Linux/Windows 二进制兼容，除非出现明确应用清单。
5. 更新：签名、可回滚。

### 非目标（D0）

- POSIX/二进制兼容（D1 才做 POSIX）。
- 浏览器（B0 并行长线，不作为 D0 出口）。
- 真实硬件、GPU 3D、多显示器、HDR（D1 锁定一款硬件再谈）。
- 驱动 ring3 迁移（P5/P6 冻结到 D1）。
- 微内核纯度（允许 VFS/网络暂留内核，先 facade）。

## 2. 域定义

### 2.1 L0 内核

- 职责：boot/HAL、CPU/arch、PMM/VMM、调度/线程、trap/syscall、IPC 原语（port/cap）、IRQ/MMIO/DMA 授权、计时、panic/watchdog。
- 禁止：文件语义、网络协议、账户策略、桌面策略、manifest 解析。
- 接口：native syscall（ABI 1.0）、trap、cap。
- 失败：fatal；panic + watchdog 重启。
- D0：64 位 long mode、4 级页表、ELF64、syscall、调度、IPC/cap。

### 2.2 L1 系统服务

- 职责：VFS/fsd、网络协议栈、认证/账户、日志、包管理/更新、命名空间/挂载、设备管理、时间/locale。
- 失败：core；快速重启 + 客户端降级（离线/只读/缓存）。
- 接口：port + cap；客户端库属于 A 平台契约。
- D0：VFS 留内核 + auth/log/pkg 最小可用；fsd 后置。

### 2.3 L1D 设备服务

- 职责：块、输入、网卡、GPU、USB、音频。
- 失败：core/optional；设备不可用、可重启；ring0 引导驱动兜底。
- 接口：MMIO/IRQ/DMA cap + 共享缓冲。
- D0：framebuffer 或 virtio-gpu 2D + 键盘鼠标，先 ring0；ring3 迁移 D1 之后。

### 2.4 L2 桌面会话

三个重启域：

- **L2a display server**：合成、扫描输出、surface 生命周期、焦点、输入路由、服务端装饰（SSD）。
- **L2b shell/WM**：面板、启动器、任务切换、通知、窗口策略。
- **L2c session manager**：应用生命周期、窗口注册表、shell 角色令牌。

其他 L2 组件：输入法、剪贴板、设置、无障碍、文件选择器。

- 失败：session/optional；shell 崩溃 L3 保活（S1）；display server 重连属于 S2。
- 接口：surface/buffer/generation/role token；display/input cap。
- D0：S1 完成——杀 shell，L3 应用不死、输入恢复、面板回来。

### 2.5 L3 应用

- 职责：CLI/GUI/浏览器；sandbox；每应用命名空间；包安装。
- 失败：isolated，只死自己。
- 接口：app API + 授予的句柄；文件选择器/网络 broker。
- D0：终端、编辑器、一个 demo app。

### 2.6 A 平台契约（横切）

- 内容：ABI 1.0（native、64 位、syscall、句柄）、SDK/工具链、libc/POSIX Profile、包格式、调试/trace、文档/示例、兼容矩阵。
- 失败：开发/构建受阻，不影响运行时。
- D0：SDK alpha + ABI 草案 + 可编译示例。

### 2.7 B 生命周期与运维（横切）

- 内容：身份/权限策略、沙箱、签名更新/回滚、审计/CVE、CI/测试矩阵、遥测、发布、硬件矩阵。
- 失败：安全/更新能力降级。
- D0：签名包 + CI + 故障注入矩阵。

## 3. 故障等级

| 等级 | 崩了以后 | 典型组件 | 策略 |
|---|---|---|---|
| fatal | 全系统重启 | L0 调度/MM/IPC/IRQ | panic + watchdog |
| core | 大面积功能缺失 | L1 fsd/netd/authd；L1D blkd | 激进重启 + 客户端降级 |
| session | 桌面会话重启，L3 保活 | L2a/L2b/L2c | 重启会话，应用重连 |
| optional | 单一功能消失 | 壁纸/通知/索引/打印 | 退避重启，N 次后禁用，UI 提示 |
| isolated | 只死自己 | L3 应用 | 回收 surface，通知用户 |

**S1 的 L3 存活 SLA**：shell/WM 崩溃或卡死时，L3 进程、文档状态、输入不丢；允许视觉中断、焦点/IME 重置、重绘。display server 崩溃时的保活是 S2；GPU 恢复是 S3。

## 4. 组件模型

manifest 在现有字段（name/version/type/entry/requires/capabilities/restart/restart_limit/uid）上追加：plane、criticality、fallback、health。

示例：

    name=wallpaperd
    version=1.0.0
    type=service
    entry=bin/wallpaperd
    plane=l2
    criticality=optional
    restart=on-failure
    restart_limit=3
    fallback=compositor.solid_color
    requires=compositor
    capabilities=surface
    health=heartbeat:5s

规则：

1. 每个 L1/L2 组件必须声明 plane + criticality + fallback。
2. optional 组件不得被 core 组件同步依赖。
3. shell 不拥有应用；应用生命周期归 session manager。
4. 没有 fallback 的组件不许进 L2。

## 5. 不变量

1. L0 不包含策略代码。
2. L1/L2 服务之间不共享地址空间，只经 IPC + cap。
3. L3 默认无文件系统根、无设备、无网络；一切经 picker/broker 授予。
4. 失败按第 3 节矩阵收敛。
5. 所有更新走签名通道；L0/L1 必须可回滚。
6. 每个域有唯一入口；跨域调用可审计。
7. 控制面走消息，数据面走共享内存/批量通道；4KB 消息只用于控制。
8. 依赖只能向下：L3 → L2 → L1 → L0；L1D 只依赖 L0 与 A/B。
9. ABI 1.0 在 64 位 + SDK 之后冻结。
10. P5/P6 驱动迁移冻结到 D1。

## 6. 现状映射（HEAD 69ab006）

| 模块 / 阶段 | 目标域 | 现状 | 动作 |
|---|---|---|---|
| boot/loader/arch/core/mm/proc/dev core | L0 | 已有，32 位 | 64 位重建 |
| svc/ipc、registry、dmesg（P1–P3） | L0 | 已有 | 句柄化、端口扩容/close |
| hwcap 89–92（MMIO/IRQ/DMA） | L0 | 已提交，305/305 | 保留，驱动隔离地基 |
| VFS/ramfs/FAT16/net stack | L1（临时在 L0） | 内核内 | 先 facade，后 fsd/netd |
| pkgd（P4） | L1 | ring3 已落地 | 保留为样板 |
| supervisor/svcwatch | L1 管理面 | 内核线程 | 后置到 ring3 |
| getty/login | L2 会话入口 | 内核程序 | 归入 session manager |
| shell/apps/tests | L3 | ring3 | CLI Profile |
| framebuffer console | L2（临时显示） | 内核 fb | display server 替换 |
| ATA/e1000/PS2 | L1D | ring0 + asm 原型 | D1 后再迁 |
| 64 位 | L0 | 未开始 | D0 第一优先 |
| POSIX libc/Profile | A | 无 | D1 |
| SDK/工具链 | A | 无（SDK WIP 已丢弃） | D0 |
| 浏览器 | L3 | 无 | B0 长线 |

## 7. 仓库与依赖

目标布局：

    src/kernel/     L0
    src/services/   L1
    src/drivers/    L1D
    src/desktop/    L2
    src/apps/       L3
    sdk/            A
    tools/ci/       B
    docs/design/    architecture / evolution / decisions / domains

规则：

1. 新增工具 check-domains：扫描 include/import，禁止向上依赖；和 check-deps 一样进 CI。
2. 一个工作包（WP）一个会话；按路径所有权，不跨域改文件。
3. build.ps1 保持单一入口，按域拆对象列表。
4. 域边界先以目录和依赖检查落地，再逐步搬代码；不要为了搬目录打断 64 位主线。

## 8. ABI 与兼容性

- **native ABI 1.0**：64 位、syscall 指令、类型化句柄（slot + generation）、port/cap、IPC；只追加，不破坏。
- **0x81 legacy**：冻结为 compat ABI，只修 bug，不加语义。
- **POSIX Profile 1.0**：D1 提供；用户态 libc + syscall shim，内核不实现 POSIX；只做源码兼容。
- **二进制兼容**：不规划；Linux personality 仅在 D2 后出现明确应用清单时评估。
- **动态链接**：D0 保持静态；D1 再评估 dlopen/ld.so。

## 9. 演进策略

一句话：**重写底层，移植中层，新写顶层**。

- 重写：boot、long mode、页表、trap、上下文切换。
- 移植：VFS/ramfs/FAT16、网络、驱动、安全/包，保留语义与测试。
- 新写：桌面会话、SDK、ABI 1.0。

详见 evolution.md。

## 10. D0 / D1 / D2

### D0（开发者桌面）

| 域 | 切片 | 出口 |
|---|---|---|
| L0 | 64 位内核、ring3、syscall、ELF64 | 64 位 shell 跑通 |
| L1 | VFS 留内核 + auth/log/pkg 最小 | 文件/登录/装包可用 |
| L1D | fb 或 virtio-gpu 2D + 键鼠，先 ring0 | 显示与输入 |
| L2 | display server + shell + session manager（S1） | 杀 shell 应用不死 |
| L3 | 终端 + 编辑器 + demo app | 连续用 1 小时 |
| A | ABI 1.0 草案 + SDK alpha + 示例 | 外人一天内写出 GUI app |
| B | 签名包 + CI + 故障注入矩阵 | 一键构建/回滚 |

D0 总出口：外部开发者照文档编译、打包、运行一个 GUI 应用；杀 shell 应用不死；1 小时 smoke 无 panic。

### D1（可用桌面）

- **D1 前置（WP5-0f/0g）**：PIT/TSC/RTC 时间基 + ring3 抢占；内核保持协作，完整内核抢占留 D2。
- POSIX Profile 1.0：zlib/FreeType/FFmpeg/SDL2 能编译并运行。
- 网络：TLS/HTTP、DNS、证书/时间/熵。
- 浏览器 B0 集成（无 JS 的 HTML/CSS 子集）。
- 一款真实硬件目标 + 安装器/更新/回滚。
- 8 小时浸泡。

### D2（通用桌面）

- 应用生态、多用户、GPU/Wi-Fi 驱动、打印/音频、可选的二进制 personality、应用仓库。

## 11. 开放决策

1. D0 启动路径：QEMU multiboot2（默认）还是 UEFI stub；不迁 16 位 bootloader。
2. session manager 崩溃时，已启动应用如何被新实例接管。
3. VFS 长期留内核，还是最终迁 fsd；page cache 归谁。
4. L1 服务用独立 uid 还是纯 cap。
5. 命名空间机制在 L0、策略在 L1/L2 的切分。
6. display server 重连时输入队列语义（S2）。
7. 新组件语言：C only，还是允许 Rust（影响工具链与 ABI）。
