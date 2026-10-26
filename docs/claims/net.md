# net：TCP 服务端 listen/accept（P0 收尾）

ABI v1.4 预留的 `SYS_LISTEN(78)` / `SYS_ACCEPT(79)` 已实现；编号与结构未改
（`include/abi.h` 未改），只在 `net/**` + `include/net.h` 内完成。

## 1. ABI 语义（已实现）

| 调用 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `SYS_SOCKET(52)` | ebx=2(AF_INET), ecx=1(SOCK_STREAM) | fd | 分配 TCP socket（`FD_KIND_TCP=6`）；`ecx=2/其它` 仍走原来的 UDP 分支 |
| `SYS_LISTEN(78)` | ebx=port, ecx=backlog | 0 / -1 | 绑定当前进程最近分配且未绑定的 TCP socket；没有就建一个内核持有的监听 socket（此时用端口号也能 accept）。backlog 截到 [1,4] |
| `SYS_ACCEPT(79)` | ebx=sock(fd 或监听端口), ecx=&addr | 连接 fd / -1 | 有界阻塞 2s；只返回**三次握手已完成**的连接；`addr` = `{ uint8_t ip[4]; uint16_t port; }`（6 字节） |
| `SYS_SENDTO(53)` | ebx=连接 fd, ecx=&udp_req | 写入字节数 / -1 | 对 TCP fd 复用 `udp_req.data/len`（单次 ≤64B，ABI 结构冻结不改） |
| `SYS_RECVFROM(54)` | ebx=连接 fd, ecx=&udp_req | 读出字节数 / -1 | 有界等待 1s；超时/对端已关 -> -1 |
| `SYS_CLOSE(9)` | ebx=fd | 0 | 仍由 fs 组实现（只清 fd 表项）；网络侧在下一个网络 syscall 入口 `net_tcp_sock_gc()` 里回收 socket/连接（进程退出同理） |

内核 API（`include/net.h`）：`net_tcp_socket/listen/listen_port/accept/accept_port/
conn_read/conn_write/conn_close/conn_port/sock_close/sock_of_port/sock_gc`。

fd->handle 编码（内部）：监听 socket = `0x1000|sock`，已接受连接 = `0x2000|conn`。

## 2. 端口复用策略

- **重复 bind 拒绝**：端口已被监听 socket、已建立连接、或 TIME_WAIT 连接占用
  -> `SYS_LISTEN` 返回 -1（legacy 门的 E_ADDRINUSE 语义）。
- **TIME_WAIT**：服务端主动 `close` 先发 FIN，进 `FIN_WAIT`；收到 ACK（或 1s
  有界等待超时）后进 `TIME_WAIT`，**200ms（2*MSL 简化）内端口仍被占用**，
  期满由 `tcp_reap()` 释放；客户端侧沿用既有 500ms 的 `TCP_TIME_WAIT`。
- **关闭监听 socket**：端口立即释放，未 `accept` 的完成队列条目回 RST 丢弃。
- **关闭连接 fd**：连接被 GC 关闭（FIN -> TIME_WAIT），期满端口可再 bind。
- **半开/完成队列**：`SYN` 进 backlog 队列；超过 backlog 的 SYN 丢弃并计数
  （`overflow`，自检断言用）；半开连接**不会**被 `accept` 返回；条目 3s 超时清理。

## 3. 新增 selftest 行（父会话加进 `build.ps1` 检查项）

期望串口输出（顺序即执行顺序）：

```
net: tcp listen ok (port=15060 backlog=2, duplicate bind rejected)
net: tcp loopback connect ok (SYN/SYN-ACK/ACK to 10.0.2.15)
net: tcp accept data ok (client->server 20 bytes)
net: tcp accept reply ok (server->client 12 bytes)
net: tcp TIME_WAIT holds port ok (rebind rejected)
net: tcp port reuse after close ok (TIME_WAIT expired)
net: tcp backlog overflow ok (backlog=1, surplus SYN dropped)
net: tcp accept timeout ok (half-open not accepted, bounded wait)
net: tcp backlog handshake ok (completed conn accepted)
net: tcp listen/accept selftest PASS
net: tcp listen/accept syscall ok (fd + addr + read/write)
net: tcp socket fd close releases port ok
```

建议 `build.ps1` 断言（12 行，全部 `-match`）：

| 断言串 | 覆盖 |
|---|---|
| `net: tcp listen ok \(port=15060 backlog=2, duplicate bind rejected\)` | listen + 重复 bind 拒绝 |
| `net: tcp loopback connect ok` | 客户端 connect（本机回环三次握手） |
| `net: tcp accept data ok` | accept 句柄 + 客户端->服务端数据 |
| `net: tcp accept reply ok` | 服务端->客户端数据（双向） |
| `net: tcp TIME_WAIT holds port ok` | TIME_WAIT 期间重复 bind 被拒 |
| `net: tcp port reuse after close ok` | TIME_WAIT 过期后端口释放 |
| `net: tcp backlog overflow ok` | backlog 溢出负向用例 |
| `net: tcp accept timeout ok` | 半开连接不被 accept + 有界超时 |
| `net: tcp backlog handshake ok` | 完成握手后 accept 拿到连接 |
| `net: tcp listen/accept selftest PASS` | 内核侧 listen/accept 自检总结 |
| `net: tcp listen/accept syscall ok` | SYS_SOCKET/SYS_LISTEN/SYS_ACCEPT/SYS_SENDTO/SYS_RECVFROM 全链路 |
| `net: tcp socket fd close releases port ok` | fd 关闭 -> GC -> TIME_WAIT -> 端口回收 |

## 4. 测试方案（QEMU slirp 下可行）

slirp 不转发 guest 的 127.0.0.1，所以回环放在内核里：`net_ip_send()` 目的 IP =
本机地址时把帧塞进软件 RX 队列，`net_rx_frame()` 优先取该队列 —— TCP 仍走完整
的 IPv4/TCP 解析与校验和，只是不经过 e1000。负向用例（backlog 溢出）用同一
路径注入“外部对端”的 SYN 帧（`tcp_test_inject`），走真实 parse/分发代码。

链路：
1. `SYS_SOCKET(SOCK_STREAM)` -> `SYS_LISTEN(15064, 2)`；
2. 内核客户端 `tcp_connect(10.0.2.15:15064)`（本机回环）；
3. `SYS_ACCEPT` 返回连接 fd 并填 `{ip,port}`；
4. `SYS_SENDTO`/`SYS_RECVFROM` 双向收发；客户端侧校验字节；
5. 客户端 close；模拟 `SYS_CLOSE` 清 fd 表项 -> 下一个网络 syscall 入口 GC
   关闭连接（TIME_WAIT）-> 过期后同端口重新 `SYS_LISTEN` 成功。

## 5. 验证记录

- `tools\session-run.ps1 -Name net -Test`：**182/182 PASS**，连跑两次均通过
  （42.1s / 54.4s，串口 12 行新证据齐全）。既有 8 项 TCP 健壮性断言全部保持：
  握手、乱序重组、坏校验和、RST、16KB 大传输 + 丢包重传、RTT/RTO、
  FIN/TIME_WAIT、`net: tcp selftest PASS`。
- `tools\ci.ps1 -Quick`：静态项全 PASS（BOM/parse 31 文件、禁用文件、
  `check-deps` 0 违规）；唯一失败项是 "worktree clean"（本会话尚未提交）。
- `tools\check-deps.ps1`：`OK: no cross-module include violations`（183 文件）。

## 6. 遗留 / 限制

- 服务端是“单段同步收发”：`conn_write` 一次最多排一个 MSS 段且不等待 ACK
  （窗口/cwnd 只在客户端侧建模）；作为 P0 的 listen/accept 语义够用，不是
  高吞吐实现。
- `SYS_CLOSE` 的清理是延迟的（下一个网络 syscall 入口 GC），因为 fd 表归
  proc/fs 模块所有；语义上等价于 close 后立即关闭连接（TIME_WAIT 仍然生效）。
  连接 socket 关闭后必须再发生一次网络 syscall 才会真正回收端口。
- fd 属主只记录“创建它的进程 + fd 槽”；fork 继承的 TCP fd 不做额外引用计数，
  P0 不承诺父子共享同一条连接。
- 服务端连接表 `TCP_SRV_MAX=4`、socket 槽 `TCP_SOCK_MAX=8`、backlog 上限 4，
  超限返回 -1（无排队）。
- `SYS_SENDTO/SYS_RECVFROM` 对 TCP 单次 <= 64 字节（复用 `struct udp_req`，
  ABI 冻结不改）；大块数据由用户态分多次调用。
- 未做真实硬件验证（按规矩不做）；未做 TCP 选项（SO_REUSEADDR 等）。
