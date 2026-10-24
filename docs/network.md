# 网络子系统：DHCP / DNS / 主动 ICMP

版本 0.71.0（2026-10）。本文描述启动期网络自检的实现、报文格式、状态机与限制。

相关文件：

| 文件 | 作用 |
| --- | --- |
| `src/kernel/net/net.c` | ARP/ICMP/UDP/TCP 自检、UDP 软件队列、可变的 IP/掩码/网关/DNS 配置 |
| `src/kernel/net/net_boot.c` | DHCP 客户端、DNS A 记录解析、主动 ping（启动期一次） |
| `src/kernel/net/net_boot.h` | 自检入口与 net.c 内部配置接口 |
| `src/kernel/net/net_syscall.c` | 用户态 UDP socket 的系统调用封装（未改动） |
| `src/kernel/dev/net/e1000.c` | 轮询式 RX 环 / TX 环（无中断） |
| `tools/net-probe.ps1` | 从串口日志断言新增自检 |

## 1. 启动顺序

`core/bootseq.c` 在 `e1000_init()` 成功后调用 `net_init()`。`net_init()` 依次：

1. ARP 请求网关，得到网关 MAC（`net: ARP reply gw=...`）；
2. 发 ICMP echo request 给网关，收 reply（`net: ICMP echo reply seq=1`）；
3. UDP 回显（`net: UDP echo reply`，宿主无回显端口时超时是预期行为）；
4. `net_boot_selftest()`：DHCP → DNS → 主动 ping。

这一步发生在 `interrupt_init()` **之前**，所以：

- 没有时钟中断，`timer_ticks()` 恒为 0，不能用它做超时；
- 时间基准用 TSC，并用 PIT 通道 2 的一次性计数（约 20ms）校准出 cycles/ms；
- 校准失败时按约 2.5GHz 兜底，所有等待仍有毫秒上界 + 迭代上界两层保护。

整个 `net_boot_selftest()` 最坏情况下增加约 3s（DHCP）+ 1.6s（DNS）+ 0.9s（ping）；
QEMU slirp 正常时全部在几十毫秒内完成，不会影响 `ushell: ready`。

## 2. 报文格式（本内核实际构造/解析的部分）

### 2.1 以太网 II（14 字节）

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 6 | 目的 MAC（广播 `ff:ff:ff:ff:ff:ff`） |
| 6 | 6 | 源 MAC（e1000 RAL/RAH） |
| 12 | 2 | EtherType：`0x0800` IPv4 / `0x0806` ARP |

`e1000_send_frame()` 负责补零到 60 字节最小帧（不含 FCS）。

### 2.2 IPv4（20 字节，无选项）

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 1 | Version/IHL | `0x45` |
| 2 | 2 | Total Length | 20 + UDP/ICMP 长度 |
| 4 | 2 | Identification | 每个发送路径各自递增 |
| 6 | 2 | Flags/Fragment | `0x4000`（DF），不分片 |
| 8 | 1 | TTL | 64 |
| 9 | 1 | Protocol | 1=ICMP，17=UDP |
| 10 | 2 | Header Checksum | 标准反码和 |
| 12 | 4 | Source | DHCP DISCOVER/REQUEST 用 `0.0.0.0` |
| 16 | 4 | Destination | DHCP 用 `255.255.255.255` |

接收侧校验：版本=4、协议号、IHL>=20、`14+IHL+载荷 <= 帧长`。

### 2.3 UDP（8 字节）

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 2 | 源端口 |
| 2 | 2 | 目的端口 |
| 4 | 2 | 长度（>=8，且按帧长截断） |
| 6 | 2 | 校验和（IPv4 下写 0，不校验） |

接收侧按 UDP 长度截断，避免把以太网填充当载荷。

## 3. ARP 缓存

- 表项：IP、MAC、过期时间（`used` 位）。
- 条目 8 个，TTL 60s；查询时用 `(int32_t)(now - expire) < 0` 判断有效。
- 启动时先用 `net_init()` 的 ARP 结果预热网关条目，正常情况下 DHCP/DNS/ping
  都不需要额外 ARP 往返；解析其它地址（如 slirp DNS `10.0.2.3`）时按需
  广播 ARP 请求，300ms 超时。
- 表满时覆盖第 0 项（最小的 FIFO 近似），不做完整 LRU。

## 4. DHCP 客户端

### 4.1 状态机

```
INIT ──广播 DISCOVER（xid，0.0.0.0:68 -> 255.255.255.255:67）──> SELECTING
SELECTING ──收到 OFFER（xid 匹配，记录 yiaddr/server-id）──> REQUESTING
REQUESTING ──广播 REQUEST（option 50/54）──> 等 ACK
   ACK   -> 应用 IP/掩码/网关/DNS，打印 net: dhcp ok ...，结束
   NAK   -> 失败
   超时  -> 失败，打印 net: dhcp timeout, keep static
```

- `xid` 由 TSC 与 MAC 混合生成，全程校验。
- 广播标志 `flags=0x8000`，允许服务器把 OFFER/ACK 广播回来。
- DISCOVER 每 500ms 重发一次，REQUEST 同样 500ms 重发一次；
  **总预算 3000ms**，到点立即退出。ACK 之后重新 ARP 网关只使用预算里
  剩下的时间（最多 300ms），因此整个 DHCP 阶段不超过 3s。
- 只解析最小选项集：53（消息类型）、1（掩码）、3（路由器）、6（DNS）、
  54（服务器标识）；`50` 只在 REQUEST 里发送。
- 应用配置时若网关变化，`net_cfg_set()` 会清空旧网关 MAC；随后重新 ARP，
  ARP 失败则用 DHCP 响应的源 MAC 兜底（slirp 中网关与 DHCP 服务器同一台）。
- 失败时**不修改** `net.c` 的静态配置（10.0.2.15 / 255.255.255.0 /
  10.0.2.2 / 10.0.2.3），后续 DNS/ping 仍可尝试。

### 4.2 DHCP 报文（BOOTP 300 字节）

| 偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 1 | op：1=REQUEST，2=REPLY |
| 1/2/3 | 1/1/1 | htype=1、hlen=6、hops=0 |
| 4 | 4 | xid |
| 8 | 2 | secs=0 |
| 10 | 2 | flags=0x8000 |
| 12 | 4 | ciaddr=0 |
| 16 | 4 | yiaddr（OFFER/ACK 分配地址） |
| 20 | 4 | siaddr（server identifier 兜底） |
| 24 | 4 | giaddr=0 |
| 28 | 16 | chaddr（MAC + 10 字节 0） |
| 44/108 | 64/128 | sname/file 全 0 |
| 236 | 4 | magic cookie `63 82 53 63` |
| 240 | 变长 | options（以 255 结束） |

## 5. DNS 解析（A 记录）

- 服务器：DHCP 给的 option 6；没有则用网关；静态兜底是 slirp 的 `10.0.2.3`。
- 查询：UDP 源端口 53000 起递增，目的 53；`RD=1`，QDCOUNT=1，QTYPE=A，
  QCLASS=IN。
- 候选域名两个：`example.com`、`www.example.com`，每个预算 800ms；
  服务器明确返回非 0 RCODE 时立即换下一个，不白等超时。
- 应答校验（任一不满足就丢弃）：
  - UDP 源端口 53、目的端口等于本次查询端口；
  - DNS ID 匹配；
  - `QR=1`、`opcode=0`；
  - `RCODE=0`，`QDCOUNT>=1`、`ANCOUNT>=1`；
  - 逐条跳过问题区与答案名（支持压缩指针 `11xxxxxx`），
    答案必须是 `TYPE=A`、`CLASS=IN`、`RDLENGTH=4`。
- 成功打印 `net: dns ok <name>=<a.b.c.d>`；全部失败打印 `net: dns timeout`。

## 6. 主动 ICMP echo

- 目标：DHCP 后的网关（默认 10.0.2.2）。
- 每次用固定 ID `0x4E45`（与 `net.c` 自检的 `0x1234` 区分）和 seq=1..3。
- 报文：20 字节 IP + 8 字节 ICMP + 32 字节载荷（`a`..`z` 循环）。
- 匹配：源 IP 等于网关、ICMP type=0、code=0、ID/seq 都与本次发送一致。
- 每个 seq 预算 300ms，最多 3 个；全部失败打印 `net: ping timeout`。
- RTT 由 TSC 差值换算，以 0.1ms 为单位向上取整，输出
  `net: ping ok seq=N rtt=X.Yms`。

> 说明：`net.c` 原有的 `net: ICMP echo reply seq=1` 是"发一次、等一次"的
> 链路自检；新增 ping 额外验证 3 个 seq 的 TX/收包匹配与 RTT，输出格式独立，
> 两者不互相影响。

## 7. 日志与断言

启动成功时新增三行（QEMU slirp 环境）：

```
net: dhcp ok ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3
net: dns ok example.com=104.20.23.154
net: ping ok seq=1 rtt=0.4ms
...
```

降级时：

```
net: dhcp timeout, keep static
net: dns timeout
net: ping timeout
```

`tools/net-probe.ps1` 从串口日志断言上述行：

```powershell
powershell -File tools/net-probe.ps1                 # 跑 -Test 再断言
powershell -File tools/net-probe.ps1 -ReuseLog       # 只分析 serial-net.log
powershell -File tools/net-probe.ps1 -ReuseLog -AllowDegraded -Log serial-a.log
```

`-AllowDegraded` 把 `dhcp timeout, keep static` / `dns timeout` / `ping timeout`
视为可接受（用于离线环境）。

## 8. 限制

- 单 RX 环、单消费者轮询；没有中断/NAPI，重负载下会丢包。
- 不支持 IPv4 分片重组；发送一律 DF。
- UDP 发送不计算校验和（IPv4 允许），接收不校验校验和。
- DHCP 只做一次租约获取：没有 RENEW/REBIND、没有 lease 时间处理，
  重启后重新 DISCOVER。
- DNS 只支持 A/IN、单问题、单次查询；不做 CNAME 链、不做 IPv6(AAAA)、
  不做缓存（每次启动最多两次查询）。
- TCP 仍是"最小 echo 客户端"：无窗口缩放、无拥塞控制、无重传定时器，
  `net_tcp_echo_test()` 仍用固定循环次数（未参数化 connect 超时）。
- ARP 缓存无完整 LRU、无 gratuitous ARP；TTL 只在查询时惰性判断。
- 所有地址均为 IPv4；默认值与 QEMU slirp 绑定（10.0.2.0/24）。

## 9. 验证

见仓库根目录的提交说明与 `logs-session-net.log` / `serial-net.log`。
回归要求：连续两次 `tools\session-run.ps1 -Name net -Test` 均为
`=== TEST PASS (97/97) ===`。
