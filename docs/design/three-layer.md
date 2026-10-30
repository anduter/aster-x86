# 三层架构设计（v1，2026-10-30）

> 状态：设计基线。后续所有实现会话以此为准；契约变更必须改本文件并 bump 版本。

## 1. 目标与非目标

**目标**

1. 内核层只保留机制：调度、地址空间、IPC 原语、IRQ→事件、MMIO/DMA 授权、HAL、最小设备核心。
2. 服务/驱动层在 ring3：驱动（blkd/inputd/netd）、fsd、pkgd、authd、logd、supervisor/svcwatch。
3. 程序层只通过 `svc_*` 客户端 API 使用服务，不直接碰硬件。
4. 每阶段保持既有回归全绿（当前 254）并跑 8h 浸泡；保留 ring0 回退。
5. 故障隔离：服务崩溃不拖垮内核；客户端超时/重连。

**非目标（本周期）**

- GUI/Qt、64 位/UEFI、真实硬件。
- 纯微内核（驱动全部 ring3 且删除 ring0 回退）。
- 驱动跑 VM（VT-x）；只做到 ring3 + IOMMU。
- 完整 POSIX/PAM/网络协议族。

## 2. 分层与所有权

```
程序层  ushell / apps / pkg CLI / tests
服务层  supervisor, svcwatch, fsd, blkd, inputd, netd, pkgd, authd, logd
内核层  core, proc, mm, dev(核心), svc(IPC), security(机制), arch, lib
```

| 层 | 可以依赖 | 禁止 |
|---|---|---|
| 程序层 | `ulib` + `svc_*` | 直接 syscall 硬件、直接 MMIO/DMA |
| 服务层 | syscall + `ulib` | 内核符号、直接改内核内存 |
| 内核层 | 自身模块接口 | 业务策略（FS 策略、协议策略逐步上移） |

## 3. IPC 契约

### 3.1 消息头（固定 16 字节，小端）

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `u32 magic` | `0x49504331`（"IPC1"） |
| 4 | `u32 type` | 服务自定义（请求/回复/事件） |
| 8 | `u16 flags` | bit0=NEED_REPLY, bit1=NO_REPLY |
| 10 | `u16 ncaps` | 附带 capability 数（≤4） |
| 12 | `u32 len` | payload 字节数（≤4096） |

payload 紧随头部；capability 数组紧随 payload（每项 8 字节：`u32 type; u32 id;`）。

### 3.2 端口语义

- `SYS_PORT_CREATE(flags)` → port id；`flags` bit0=私有时仅同 uid 可连。
- `SYS_PORT_SEND(port, msg, len, caps, ncaps)`：阻塞直到入队或超时（`-EAGAIN`），返回 `-EINTR` 可被打断。
- `SYS_PORT_RECV(port, buf, maxlen, caps_out)`：阻塞有界；返回消息长度。
- `SYS_PORT_REPLY(ticket, msg, len)`：回复请求（ticket 来自请求消息）。
- 队列深度：默认 16 条；满则发送方阻塞/超时。

### 3.3 capability

| 类型 | 值 | 含义 |
|---|---|---|
| `CAP_PORT` | 1 | 向端口发消息 |
| `CAP_MMIO` | 2 | 映射一段物理 MMIO（base/len/只读） |
| `CAP_IRQ` | 3 | 绑定一个 IRQ 到端口 |
| `CAP_DMA` | 4 | 分配/映射物理连续缓冲（len/对齐） |
| `CAP_FILE` | 5 | 文件/管道 fd |
| `CAP_SOCKET` | 6 | 网络 socket fd |

`SYS_CAP_GRANT(port, cap_type, id, rights)`：把已有 cap 授予某端口（内核校验来源）。
规则：cap 只能由持有者授予；`CAP_MMIO/DMA/IRQ` 仅 root 或 `CAP_SYS_ADMIN`。

## 4. syscall 预留（ABI 1.5）

| 号 | 名称 | 用途 |
|---|---|---|
| 80 | `SYS_PORT_CREATE` | 创建端口 |
| 81 | `SYS_PORT_SEND` | 发消息 |
| 82 | `SYS_PORT_RECV` | 收消息 |
| 83 | `SYS_PORT_REPLY` | 回复 |
| 84 | `SYS_CAP_GRANT` | 授予 capability |
| 85 | `SYS_DMESG` | 读内核日志环 |
| 86 | `SYS_SVC_REGISTER` | 注册服务（name/version/port/manifest） |
| 87 | `SYS_SVC_LOOKUP` | 按名查服务端口 |
| 88 | `SYS_SVC_HEARTBEAT` | 服务心跳 |
| 89 | `SYS_IRQ_WAIT` | 等待已绑定 IRQ 事件 |
| 90 | `SYS_MMIO_MAP` | 映射 MMIO cap |
| 91 | `SYS_DMA_ALLOC` | 分配 DMA cap |
| 92 | `SYS_DMA_FREE` | 释放 DMA cap |
| 93 | `SYS_LOG_WRITE` | 写服务日志（带 tag） |
| 94 | `SYS_SCHED_AFFINITY` | CPU 亲和（设计级） |
| 95 | 保留 | — |

未实现编号必须返回 `-1`，不得 panic。

## 5. 服务清单（manifest）

```ini
name=netd
version=1.0.0
type=service
entry=bin/netd
requires=logd,fsd
capabilities=MMIO:0xFEBC0000:0x20000,IRQ:11,DMA:65536
restart=on-failure
restart_limit=3
uid=0
```

`supervisor` 按 `requires` 拓扑启动；`svcwatch` 按 `restart` 策略重启（指数退避 1s/2s/4s，上限 3 次，之后 quarantine）。

## 6. 驱动模型

- 内核 `dev_ops` 契约保留，两种实现：`ring0`（现状，回退）与 `ring3`（服务）。
- 启动参数 `drivers=ring0|ring3|auto`；`auto` 优先 ring3，失败回退。
- ring3 驱动通过 `CAP_MMIO/IRQ/DMA` 获得硬件；内核不解释设备语义。
- QEMU 验证开 `-device intel-iommu`；无 IOMMU 时 ring3 驱动只允许 `CAP_DMA` 的 bounce buffer，禁止设备任意 DMA（受限模式）。
- 迁移顺序：blkd（块）→ inputd（中断）→ netd（吞吐）。

## 7. 故障与恢复

| 故障 | 行为 |
|---|---|
| 服务崩溃 | 内核回收端口/fd/cap；supervisor 按策略重启 |
| 客户端调用超时 | 返回 `-ETIMEDOUT`；可重连 |
| 驱动设备无响应 | 超时 → 设备 reset → 3 次失败 → quarantine，返回 `E_IO` |
| 内核 panic | 仅机制损坏；设备/服务错误不得 panic |
| 端口满 | 发送方阻塞/超时，不丢消息 |

## 8. 阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 本设计 + ABI 预留 | 文档评审、构建通过 |
| P1 | IPC + capability + ulib | 两个服务互发/回复；cap 传递；254+ 全绿 |
| P2 | dmesg + logd（可选合并） | `dmesg` 可读最近 N 行；服务日志 |
| P3 | 服务注册 + supervisor 拓扑 + 重启 | 依赖启动、杀服务自动重启、客户端重连 |
| P4 | pkgd 迁 ring3 | 安装/卸载全走 ring3；回退 ring0 |
| P5 | blkd / inputd ring3 | 杀驱动内核不死；8h 浸泡 |
| P6 | netd ring3 + vIOMMU | 吞吐 ≥ ring0 70%；故障隔离；8h 浸泡 |
| P7 | 程序层 SDK + 示例 app | 新服务/app 只需 manifest + 入口 |
| P8 | 综合扫描 + 最终浸泡 | 全回归 + 8h + 审计 0 未修 P0 |

## 9. 兼容与回退

- 新 syscall 只追加；旧编号语义不变。
- `drivers=` / `sip=` / `services=` 启动参数控制新路径；关闭后行为与 0.74.2 一致。
- 每阶段结束：`ci -Full` ×2 + 8h 浸泡；失败回退该阶段提交。

## 10. 开放问题（后续设计文档）

- 零拷贝共享环的所有权/回收。
- IRQ 在用户态的 EOI 代理与丢失中断。
- cap 撤销（revoke）语义。
- 服务升级（同版本热替换）。
- 无 IOMMU 时 ring3 驱动的受限 DMA 方案。
