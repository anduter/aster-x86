# 用户态驱动服务模型（Driver Service v0）

> 状态：设计稿（P3.6 起实施），描述"驱动搬出内核"的目标形态与最小落地路径。
>
> 关联文档：[system-model.md](system-model.md) · [native-abi.md](native-abi.md) · [security-model.md](security-model.md)

---

## 1. 目标与非目标

**目标**

1. 驱动是一个普通用户进程（service），崩溃不拖垮内核；
2. 客户端通过**句柄**访问设备能力，没有句柄就碰不到硬件；
3. 请求/响应走 Channel，协议版本化、可审计；
4. 驱动重启后客户端可重新连接，不需要重启系统。

**非目标（v0 明确不做）**

- 不做零拷贝共享内存环；
- 不做驱动热插拔/电源管理；
- 不追求 IPC 性能指标（先正确，后快）；
- 不在这阶段做 POSIX 兼容层。

---

## 2. 现有地基（已实现）

| 能力 | 位置 | 状态 |
|---|---|---|
| 类型化对象 + 引用计数 | `core/object.c` | ✅ 已加锁 |
| 通用 capability 表（install/transfer/restrict/revoke） | `proc/cap.c`、`intr/native.c` | ✅ |
| native 句柄 restrict / transfer（含 generation 防旧句柄） | `intr/native.c` | ✅ |
| 成对 Channel endpoint（4 条消息 × 64B，非阻塞） | `ipc/channel.c` | ✅ v0 |
| 匿名内存映射（`mmap`/`munmap`） | `intr/syscall.c` | ✅ |
| 进程创建/等待 | `intr/native.c` | ✅ |

**缺的关键一块**：Channel 目前只传字节，**不能附带句柄**。驱动服务模型要求
"把设备能力随请求传过去"，所以 v0 之后必须先给 Channel 消息加句柄槽。

---

## 3. 模型

```
        ┌──────────────┐   chan_send(req)    ┌──────────────────┐
        │   client     │ ──────────────────► │  driver service  │
        │  (user proc) │                     │   (user proc)    │
        │              │ ◄────────────────── │                  │
        └──────────────┘   chan_send(resp)   └────────┬─────────┘
                                                      │ 只有它有 device 句柄
                                                      ▼
                                               ┌──────────────┐
                                               │  硬件 / MMIO │
                                               └──────────────┘
```

**Service** = 一个进程 + 一个监听 endpoint + 一张能力表。
**Client** 拿到的不是"设备路径"，而是一个已连接的 endpoint 句柄。

命名空间解析（`system:/dev/...` 之类）留给 device manager 服务，v0 先由内核
在启动时把 endpoint 句柄直接交给测试客户端，避免过早引入命名空间。

---

## 4. 消息格式

固定 16 字节头 + 变长负载，小端：

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | `magic` (u32) | `0x44525631`（"DRV1"），协议版本校验 |
| 4 | `op` (u16) | 操作码，驱动自定义 |
| 6 | `flags` (u16) | bit0 = 请求，bit1 = 响应，bit2 = 错误 |
| 8 | `req_id` (u32) | 客户端自增，响应原样带回（支持多请求并发） |
| 12 | `arg` (u32) | 短参数（寄存器偏移、长度等） |
| 16 | payload | 变长，最多 `CHAN_MSG_LEN - 16` |

**句柄附件**（v1）：头部之后紧跟 `u32 handle_count` + `handle_count` 个句柄槽，
接收方在 `chan_recv` 时把这些槽装进自己的 capability 表，返回新的句柄号。

---

## 5. 生命周期

1. **start**：driver manager 用程序名创建进程，把设备句柄 transfer 给它；
2. **ready**：服务初始化完成后向 manager 的 endpoint 发 `OP_READY`；
3. **serve**：循环 `recv → 处理 → send`，无请求时阻塞（Channel v1 需要阻塞语义）；
4. **crash**：进程退出 → 内核回收其句柄 → manager 收到 `wait` 状态 → 重启服务，
   客户端重连；
5. **stop**：manager 发 `OP_STOP`，服务清理后退出。

**验收**：杀掉驱动进程后，内核不 panic、其他进程不受影响、manager 能重新拉起它。

---

## 6. 分阶段计划

| 阶段 | 内容 | 验收 |
|---|---|---|
| **D0（已完成）** | 成对 Channel + 非阻塞收发 + 引用计数 | `channel` 自检；native `OBJ_CHANNEL` 可创建/关闭 |
| **D1** | Channel 阻塞语义（满/空阻塞 + 唤醒） | 两个进程互发 100 条消息，无丢包；空读阻塞后能被写方唤醒 |
| **D2** | 消息附带句柄（transfer 内嵌到消息） | 客户端把文件句柄发给服务，服务读同一文件 |
| **D3** | 第一个用户态驱动：把 `ram0` 块设备服务化 | shell 通过服务读写 `ram0`，与应用直连内核的路径结果一致 |
| **D4** | 驱动崩溃自动重启 | 注入崩溃后 1 秒内服务恢复，客户端重连成功 |
| **D5** | 网络驱动（`e1000`）服务化 | RX/TX 全部经服务，内核不再直接碰 MMIO |

D3 是分水岭：它证明"驱动可以不在内核里"这件事在本系统成立；
D5 依赖网络 RX 先在内核里跑通。

---

## 7. 与参考系统的差异

- **QNX**：资源管理器把路径当唯一入口；Aster 保留句柄优先，命名空间是可选服务。
- **seL4**：能力即一切，IPC 是唯一同步原语；Aster 允许内核内快速路径（启动关键驱动），
  但接口按能力设计，便于后续搬迁。
- **Fuchsia**：组件框架 + FIDL；Aster v0 只做二进制协议，不引入 IDL 编译器。

---

## 8. 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| 阻塞式 IPC 与抢占调度交互复杂 | 死锁/优先级反转 | D1 先做单核语义，再在双核上验证 |
| 句柄随消息传递需要原子性 | 泄漏或重复关闭 | 句柄槽在 recv 时一次性装入，失败整条消息回滚 |
| 驱动服务化后启动链变长 | 启动变慢/失败点多 | 启动关键路径（磁盘）最后搬，先搬 `ram0` |
| 没有 DMA 隔离 | 驱动 bug 仍能写坏内存 | 记录为限制；后续靠 IOMMU 或 bounce buffer |

---

## 附录：e1000 RX 当前诊断（2026-10-24）

在 `-nic user,model=e1000` 下复现 `net: ARP reply timeout`，抓到的现场：

```
e1000: MAC=52:54:00:12:34:56 link=up
e1000: TX test frame sent (60 bytes, TDH==TDT)
net: ARP reply timeout (RDH/RDT check)
dbg: RCTL=801e RDBAL=0x18d8000 RDLEN=0x80 RDH=0 RDT=7 TPT=0 GPRC=0
dbg: rx[0] addr=0x18d9000 status=0 len=0 err=0
```

可以排除的：

- MAC/链路正常（`link=up`，读回 52:54:00:12:34:56）；
- RX 环参数看着正确：RDLEN=128（8×16）、RDBAL 4KB 对齐、RDT=7 已交给硬件、
  RCTL=0x801e（EN|SBP|UPE|MPE|BAM），缓冲区 2KB 对齐；
- 描述符布局是标准 legacy RX（addr/len/csum/status/errors/special = 16B）。

可疑点：

1. **TX 的"成功"判据太弱**：`TDH==TDT` 只说明描述符被取走，不代表帧真上了线；
   `TPT`（发送包计数）读出来是 0。需要确认 QEMU 是否实现该计数器，或换
   `-netdev socket` 后端在宿主侧抓包；
2. **GPRC=0 且 RDH 恒为 0**：连一个广播帧都没进 RX 环，问题更可能在
   "帧没出去"而不是"环没配好"；
3. 未验证项：`TCTL.EN` 是否真的生效、PCI 命令寄存器的 bus master 位
   （`pci_init` 目前只读配置空间，没有写 CMD 寄存器）。

下一步建议：用 `-netdev socket,listen=...` 起一个可抓包的宿主端口，先确认
ARP 请求是否真的发出；如果发出了，再查 slirp 为什么不应答。

