# tests/audit/net：畸形包注入用例（audit-net）

本目录记录 `audit-net` 会话用于找/验证网络栈缺陷的注入用例。**注入点在客户机
内部**（`net_loopback_push()` + 各解析入口），因此可以精确构造 IP/UDP/TCP 头，
而不受 QEMU slirp 只转发合法报文头的限制；真实链路（slirp + 宿主 12399 回显）
用来保证"修完还能通"。

## 1. 为什么不用宿主 socket 直接发畸形包

- QEMU user 网络（slirp）会替客户机重写/生成 IP/UDP/TCP 头：从宿主只能改
  payload 与部分 TCP 字段，改不了 IHL/校验和/分片位，而本次缺陷恰好都在
  头字段校验上；
- `net_loopback_push()` 把帧塞进内核软件 RX 队列，`net_rx_frame()` 优先取它，
  帧随后走**完整**的 `tcp_parse_frame()` / `net_udp_rx_frame()` 解析、校验和、
  分发路径 —— 与真网卡收到的帧同一条代码路径。

## 2. 用例与期望（对应 `net: audit ...` 串口行）

| 用例 | 构造 | 期望 | 回归行 |
|---|---|---|---|
| IP 校验和坏 | `IPv4` 首部 `checksum` 字段故意写错 | 丢弃，不进 SYN 队列 | `net: audit tcp ip ingress rejects malformed ok` |
| MF 分片 | `ip[6] |= 0x2000`（DF 保留） | 丢弃 | 同上 |
| 片偏移非 0 | `ip[6] = 0x0003` | 丢弃 | 同上 |
| UDP 长度越界 | UDP `len=300`，实际 payload 8 | 丢弃 | `net: audit udp ingress rejects malformed ok` |
| UDP 校验和错 | 非 0 但算错 | 丢弃（0 仍允许） | 同上 |
| 伪造 SYN | `SYN seq=0x7777 ack=0`（已建立连接上） | `state`/`rcv_nxt` 不变 | `net: audit tcp forged SYN ignored ok` |
| SYN 带错 ack | `SYN|ACK ack != iss+1` | 不建立连接 | 同上 |
| 半开握手错 seq | `ACK seq = syn_seq+1+7` | 不进完成队列 | `net: audit tcp handshake seq validated ok` |
| 接收缓冲满 | `rlen=2048-16` 时交付 100B | `rcv_nxt` 只 +16 | `net: audit tcp receive accounting ok` |
| ARP 欺骗 | reply 的 sender IP = 10.0.9.9 | `gw_mac` 不变 | `net: audit arp spoof rejected ok` |
| 兜底 listener 归属 | 无 TCP fd 时 `SYS_LISTEN` | 槽记在 pid 名下、每进程 ≤1 | `net: audit listen socket owned by pid ok` |

每个用例都配一个**正向对照**（合法帧必须被接受），避免"全部丢弃"式的假通过。

## 3. 帧布局（构造时直接用）

```
Ethernet: [0..5] dst mac, [6..11] src mac, [12..13] 0x0800
IPv4:     [14] 0x45, [16..17] total_len, [18..19] id, [20..21] flags|frag,
          [22] ttl, [23] proto, [24..25] hdr_csum, [26..29] src, [30..33] dst
UDP:      [34..35] sport, [36..37] dport, [38..39] len, [40..41] csum, [42..] payload
TCP:      [34..35] sport, [36..37] dport, [38..41] seq, [42..45] ack,
          [46] data_off<<4, [47] flags, [48..49] window, [50..51] csum
```

校验和一律按 16 位反码和计算；伪首部 = src(4) + dst(4) + proto + L4 长度。

## 4. 如何扩展

在 `src/kernel/net/*.c` 的 `*_audit_selftest()` 里加用例（它们与解析代码同文件，
能直接调 static 函数），并在 `docs/audit/net.md` 的表格里登记新的串口行；
`tools/audit-net.ps1` 会断言这些行。
