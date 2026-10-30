# Aster 四平面架构 v0.1

> 状态：设计口径草案（2026-10-31）。
> 与 `three-layer.md` 的关系：**三层回答“按什么顺序搬”，四平面回答“搬完以后的信任/故障/更新边界”**。
> 三层继续作为迁移计划；四平面作为长期架构口径。冲突时以本文为准，并回写三层。

## 0. 一句话

内核只给机制，L1 给基础设施，L2 给桌面会话，L3 给应用；SDK、ABI、POSIX Profile、包格式、安全模型是横切契约，不属于任何平面。

## 1. 原则

1. **内核只给机制，不给语义**：地址空间、调度、IPC、IRQ、MMIO/DMA 授权、计时、HAL；不解析 manifest、不做文件语义、不做桌面策略。
2. **一切跨平面访问经句柄 + 能力**：没有 ambient authority。
3. **控制面走消息，数据面走共享内存/批量通道**：4KB IPC 只用于控制；帧、音频、大文件必须零拷贝或分块直传。
4. **每层只信任下一层**：L3 不信任 L2，L2 不信任 L1，L1 不信任 L0 的“善意”。
5. **SDK 不是服务**：它是横切的开发产物（头文件/库/工具链/文档/模拟器），不参与运行时，不占平面。

## 2. 平面定义

```
L3 应用        ls / 编辑器 / 浏览器 / 微信
L2 桌面会话    合成器 / shell / 输入法 / 通知 / 设置 / session manager
L1 系统服务    fsd / netd / authd / logd / pkgd / devd
L0 内核        调度 / 内存 / IPC / IRQ / MMIO-DMA / 计时
横切契约       ABI 1.0 · SDK · POSIX Profile · 包格式 · 安全模型
```

| 平面 | 职责 | 信任级别 | 入口 | 崩溃影响 |
|---|---|---|---|---|
| L0 | 资源与隔离机制 | 最高（TCB） | syscall / trap | 系统死，panic + 重启 |
| L1 | 基础设施服务 | 高，最小 cap | port + cap | 服务不可用，可重启/隔离 |
| L2 | 桌面会话与 UI 服务 | 中，每用户 | session API / display/input cap | 会话可重启，L3 应用默认保活（S1 起） |
| L3 | 用户程序 | 低，沙箱 | app API / 授予的句柄 | 只死自己 |

### 2.1 L2 的三个重启域

L2 不能是一个整体，否则它是单点。拆成三个可独立重启的进程：

```
L3 应用 ── surface/buffer ──┐
                            ▼
                 L2a display server
                 （surface 表、层叠、焦点、输入路由、扫描输出、服务端装饰）
                    ▲                  ▲
       控制/查询     │                  │ 输入事件
                    │                  │
              L2b shell/WM          L1 inputd
              （面板、启动器、任务切换、通知）
                    │
              L2c session manager
              （应用生命周期；应用是它的子进程，不是 shell 的）
```

| 子角色 | 职责 | 崩溃后 |
|---|---|---|
| L2a display server | 合成、扫描输出、surface 生命周期、焦点、输入路由、SSD | L3 进程存活；屏幕冻结/黑；S2 做重连 |
| L2b shell / WM | 面板、启动器、任务切换、通知、窗口策略 | L3 与显示不受影响；新 shell 查询窗口表重建 |
| L2c session manager | 应用生命周期、窗口注册表、shell 角色令牌 | 已启动应用存活；新实例从持久化表重建 |

**L3 存活 SLA（从 S1 起）**：shell 崩溃/卡死不影响 L3 进程、文档状态、输入；允许视觉中断、焦点/IME 重置、重绘。display server 崩溃时的 L3 保活属于 S2，GPU 恢复属于 S3。

## 3. 横切契约

- **ABI 1.0**：syscall 编号、结构体布局、错误码、句柄语义；只追加，不破坏。
- **SDK**：aster 工具链 + sysroot + 头文件 + 示例 + 文档 + GDB stub；验收是“外人一天内写出 GUI 应用”。
- **POSIX Profile 1.0**：用户态 libc + syscall shim；明示支持/不支持矩阵；不追求认证。
- **包格式**：签名、manifest、依赖、安装/卸载/回滚；L1/L2/L3 共用。
- **安全模型**：cap 类型与权利、沙箱、更新信任链、CVE 流程。

## 4. 七条不变量

1. L0 不包含策略代码；策略只出现在 L1 及以上。
2. L1 服务之间不共享地址空间，只经 IPC + cap。
3. L3 默认无文件系统根、无设备、无网络；一切经 picker/broker 授予。
4. L2 崩溃不拖垮 L0/L1；L3 崩溃不拖垮 L2；L2 的 shell 崩溃不拖垮 L3。
5. 所有更新走签名通道；L0/L1 必须可回滚。
6. 每个平面有唯一入口；跨平面调用必须可审计。
7. 数据通道必须支持零拷贝或批量传输；4KB 消息不得承载帧/音频/大 I/O。

## 5. 现状映射（HEAD 69ab006）

| 模块 / 阶段 | 目标平面 | 现状 | 动作 |
|---|---|---|---|
| boot / loader / mm / proc / trap / dev core | L0 | 已有 | 64 位移植（D0 第一优先） |
| svc/ipc + registry + dmesg（P1–P3） | L0 机制 | 已有 | cap 句柄化、端口扩容/close |
| hwcap 89–92（MMIO/IRQ/DMA） | L0 机制 | 已提交，305/305 | 保留，作为驱动隔离地基 |
| pkgd（P4） | L1 | ring3 已落地 | 保留为样板 |
| pkg 验签/落盘原语 | L0 | 内核 | 保留为信任根 |
| VFS / ramfs / FAT16 / net stack | L1（临时在 L0） | 内核内 | 先 facade，后 fsd/netd |
| supervisor / svcwatch | L1 管理面 | 内核线程 | 后置到 ring3 |
| getty / login | L2 会话入口 | 内核程序 | 归入 session manager |
| shell / apps / tests | L3 | ring3 | CLI Profile |
| framebuffer console | L2（临时显示） | 内核 fb | 合成器替换 |
| ATA / e1000 / PS2 | L1 驱动 | L0 + asm 原型 | P5/P6 暂缓，D1 后再迁 |
| 64 位 | L0 | 未开始 | D0 |
| POSIX libc / Profile | 横切 | 无 | D1 |
| SDK / 工具链 | 横切 | sdk WIP（未提交） | D0 |
| 浏览器 | L3 | 无 | B0 长线 |

## 6. 权限矩阵

| 平面 | 运行身份 | 持有 | 可以访问 | 禁止 |
|---|---|---|---|---|
| L0 | ring0 | 全部 | 全部 | — |
| L1 | ring3，每服务独立 uid | 端口 + 特定 MMIO/IRQ/DMA + 文件 cap | 自己的设备/文件范围 | 用户内存、别的服务端口、任意 DMA |
| L2 | 登录用户 | display/input/GPU cap | 显示、输入、启动应用 | 块设备、其他用户数据 |
| L3 | 用户 + 沙箱 | 仅授予的句柄 | picker 选中的文件、经 broker 的网络 | 文件系统根、设备、其他应用内存 |

## 7. 故障与重启矩阵

| 事件 | L3 应用 | 屏幕 | 输入 | 恢复 |
|---|---|---|---|---|
| L2b shell 崩溃/卡死 | 活 | 正常 | 正常 | supervisor 重启 shell；查询窗口表重建面板 |
| L2a display server 崩溃 | 活（进程） | 冻结/黑 | 断 | S2：重连 + 重建 surface |
| L2c session manager 崩溃 | 已启动的活 | 正常 | 正常 | 从持久化窗口表重建 |
| L1 服务崩溃 | 活（客户端重连） | 正常 | 正常 | 退避重启、隔离（P3 已有） |
| L3 应用崩溃 | 只死自己 | 正常 | 正常 | display server 回收 surface |
| GPU hang | 活（可能） | 冻结 | 断 | S3：device-lost 恢复 |
| L0 panic | 死 | 黑 | 断 | 重启 + 看门狗 |

## 8. 测试矩阵

| 平面 | 单元 | 集成 | 故障注入 | 安全 | 性能 |
|---|---|---|---|---|---|
| L0 | host 单测 | 启动矩阵 / 8h 浸泡 | panic/oops、NMI、坏页 | SMEP/SMAP、uaccess、W^X | 调度/内存基准 |
| L1 | 服务自检 | 拓扑启动、协议往返 | kill -9、端口耗尽、队列满、超时 | cap 负向、伪造 ticket、越权 send | IPC 延迟/吞吐 |
| L2 | 工具包单测 | 截图基线、输入回放 | 杀 shell/compositor/inputd | 会话隔离、剪贴板/拖放权限 | 帧率、输入延迟 |
| L3 | POSIX 子集 | 应用兼容矩阵 | 应用崩溃/OOM | 沙箱逃逸、权限提示 | 启动时间、内存 |
| 横切 | ABI 一致性 | 包安装/回滚 | 断电/更新中断 | 签名/证书/更新链 | 构建可复现 |

## 9. 更新与生命周期

| 平面 | 交付物 | 重启/恢复 | 回滚 |
|---|---|---|---|
| L0 | 签名内核镜像 | panic 重启、看门狗 | A/B 或上一版镜像 |
| L1 | 签名服务包 | 退避重启、隔离 | 包版本回滚 |
| L2 | 桌面环境包 | 会话重启，L3 默认保活 | 上一版会话包 |
| L3 | 应用包 | 只重启该应用 | 应用版本回滚 |

## 10. 对路线图的影响

1. **64 位先行**：L0 的地址空间/ABI/工具链是 D0 的第一优先；ABI 1.0 在 64 位 + SDK + POSIX Profile 决策之后再冻结。
2. **P5/P6 暂缓**：驱动迁移不是桌面 D0 的关键路径；hwcap 已把机制备好，等 D1 有硬件目标和崩溃数据再迁。
3. **L2 从 S1 开始**：先拆 display server / shell / session manager，拿到“杀 shell 应用不死”，再谈 compositor 重连（S2）和 GPU 恢复（S3）。
4. **浏览器 B0 长线**：自有引擎按 NetSurf/Ladybird 尺度；先做无 JS 的 CSS/HTML 子集，D0 通过前不启动 B1（JS）。

## 11. 开放问题

1. L2c session manager 崩溃时，已启动应用如何被新实例接管？（pid 扫描 / 持久化注册表）
2. L1 服务用独立 uid 还是纯 cap？对 POSIX 兼容层的影响。
3. L0 是否长期保留 VFS 作为 page cache + L1 fsd 的 facade？
4. 命名空间归 L0 机制还是 L1 策略？（建议：机制在 L0，挂载/策略在 L1/L2。）
5. display server 重启时的输入队列语义：丢弃、排队还是回放？
6. 重连鉴权：session token 的签发/轮换/撤销。
