# net：网络协议栈崩溃/漏洞审计（audit-net）

- 会话：`audit-net`（独立审计，2026-10-27）
- 范围：`src/kernel/net/**`（`net.c` / `net_boot.c` / `tcp.c` / `net_syscall.c`）
  以及 `src/kernel/dev/net/e1000.c` 交给协议栈的**解析入口以上**部分
- 约束：只改 `net/**`；不改 `e1000.c`、`build.ps1` 检查项、`abi.h`、`fs/**`、
  `proc/**`、`security/**`、`mm/**`
- 结论：**找到并修复 7 个可复现缺陷**（4 个可导致数据注入/劫持、2 个可导致
  拒绝服务、1 个可导致静默数据损坏）；**未发现**帧解析的越界读写/空指针
  （推演范围见 §6）

## 1. 摘要

| # | 严重性 | 缺陷 | 位置 | 回归行（串口） |
|---|---|---|---|---|
| V1 | 高 | IPv4 入口不校验首部校验和、不拒绝分片：坏头/半截分片直接进状态机 | `net.c` `tcp.c` `net_boot.c` | `net: audit tcp ip ingress rejects malformed ok` |
| V2 | 高 | UDP 声明长度与帧长不一致仍交付（截断入库）、非 0 校验和从不校验 | `net.c` `net_boot.c` | `net: audit udp ingress rejects malformed ok` |
| V3 | 高 | 客户端接受任意 SYN（不校验 ACK/ack）：伪造段可重置连接并注入数据 | `tcp.c` `tcp_input` | `net: audit tcp forged SYN ignored ok` |
| V4 | 高 | 服务端握手只校验 ack（且 ISS 可预测）：盲完成三次握手 / 幽灵连接 | `tcp.c` `tcp_listen_complete` | `net: audit tcp handshake seq validated ok` |
| V5 | 中 | 客户端 `rcv_nxt` 按段长推进而非实际交付字节：缓冲满时静默丢数据 | `tcp.c` `tcp_deliver/tcp_input` | `net: audit tcp receive accounting ok` |
| V6 | 中 | `SYS_LISTEN` 兜底监听 socket 永不回收：普通进程可永久占满端口/socket 槽 | `net_syscall.c` `tcp.c` | `net: audit listen socket owned by pid ok` |
| V7 | 中 | ARP reply 只校验 `10.0.x` 两字节：任意主机可改写网关 MAC（流量劫持） | `net.c` `net_parse_arp` | `net: audit arp spoof rejected ok` |

另有两行汇总：`net: audit net ok (udp+arp)`、`net: audit tcp ok`。

## 2. 环境、隔离与复跑

审计期间同一 workspace 有多个并行会话在改 `proc/mm/fs`，共享树的
`build.ps1 -Test` 会被它们的中间状态打断（实测到一次 `proc/sched.c` 半成品导致
构建失败、以及宿主被并发 QEMU 占满时 225 项里 shell/键注入类检查成批失败）。
因此所有结论都在**独立 worktree** 里验证：

```
git worktree add .wt-audit-net 38161af --detach
# tools/i686-elf、tools/w64devkit 用 junction 链到主树；keys/shell_private.pem 拷入
# 只把本会话改的 net/** 复制进去 -> 其它模块保持基线
```

隔离产物：

| 项 | 值 |
|---|---|
| obj 目录 | `.obj-audit-net` |
| dist 目录 | `.dist-audit-net` |
| QMP | `4543` |
| 串口日志 | `obj/audit/net.log` |
| 回显端口 | `12399`（**不占用 12345**） |

复跑：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/audit-net.ps1
# 只断言本会话的回归行（不当 225 项门禁，迭代用）：
powershell -NoProfile -ExecutionPolicy Bypass -File tools/audit-net.ps1 -NoFull
```

`tools/audit-net.ps1` 用上面这套隔离参数跑 `build.ps1 -Test`，再从串口日志断言
§1 的 9 条 `net: audit ...` 行与 11 条既有 net 正向断言，并从 build 日志断言
`225/225`。

## 3. 攻击面与解析入口矩阵

收包只有一条物理入口（e1000 NAPI/RX → 各自轮询），但**解析代码有四份**。
本次把它们统一到同一组校验原语（`net_internal.h`），避免"改了一份漏三份"：

| 入口 | 调用者 | 修复前校验 | 本次新增 |
|---|---|---|---|
| `tcp_parse_frame()` | TCP 全部报文（client/server/listen） | 版本/IHL/帧长/总长/TCP 校验和 | IPv4 首部校验和、分片拒绝、`tlen >= ihl+20` |
| `net_udp_rx_frame()` | 用户态 UDP / 自检 UDP（新拆出的单帧入口） | 版本/IHL/帧长/端口/目的 IP | IP 校验和、分片、UDP 声明长度、UDP 校验和 |
| `nb_parse_udp()` | DHCP / DNS 客户端 | 同上（更宽松，截断而非丢弃） | 同上（共用原语） |
| `net_parse_icmp()` / `nb_ping_one()` | ICMP 自检 / 主动 ping | 版本/IHL/帧内长度 | IP 校验和、分片、ICMP 校验和、源必须是网关 |
| `net_parse_arp()` | 启动期网关 ARP | 操作码 + `10.0.x` 粗匹配 | htype/ptype/hlen/plen、sender IP==gw、target IP==本机、单播 MAC |

发送路径（`net_ip_send` / `tcp_send_seg` / `net_udp_send_from` / `nb_send_udp`）
本次逐条核对了长度上界（§6），未发现溢出。

## 4. 缺陷详情（修复前 → 修复后）

### V1 IPv4 入口不校验首部校验和、不拒绝分片（高）

**复现**：构造 `IPv4/UDP` 并把首部校验和改坏 → 修复前照样入队并交付给应用；
置 `MF=1` 或片偏移≠0 → 本栈**没有重组**，修复前把第一个分片当成完整报文：
UDP 把半截 payload 当数据交付、TCP 把分片字节当段数据处理。

**根因**：四份解析入口各自只做 `版本/IHL/帧长` 宽松检查，`ip[6]` 的 MF/
片偏移与 `ip[10]` 的首部校验和从未被读取。

**修复**：`net.c` 新增 `net_ipv4_validate()`（版本、IHL∈[20,60]、`(flags|frag) &
0x3FFF == 0`（DF 允许）、`tlen∈[ihl,avail]`、首部校验和为 0），
`tcp_parse_frame` / `net_udp_rx_frame` / `nb_parse_udp` / `net_parse_icmp` /
`nb_ping_one` / 旧 `net_tcp_recv` 全部改走它；TCP 额外要求 `tlen >= ihl+20`
（TCP 头必须完整落在 IP 总长内，避免读到帧尾之外）。

**修复前**：无任何串口证据可断言（坏包被静默接受）。
**修复后**：`net: audit tcp ip ingress rejects malformed ok (bad=3)`
（坏 IP 校验和 / MF / 片偏移三种注入都不得进 SYN 队列，合法帧必须进）。

### V2 UDP 声明长度与帧长不一致仍交付、非 0 校验和从不校验（高）

**复现**：UDP 头 `len=300` 而帧里只有 8 字节 payload → 修复前 `plen = min(ulen-8,
avail)` 截断入队，应用收到"短了但合法"的报文；把 payload 改坏但保留非 0
校验和 → 修复前不看校验和，坏数据进应用队列（数据完整性失效）。

**根因**：`udp_drain_locked()` 用 `avail` 夹住 `ulen` 而不是拒绝；UDP 校验和
字段从未参与判断。

**修复**：`net_udp_validate()`：`8 <= ulen <= avail`（声明长度必须整段落在帧内），
校验和非 0 时必须算得 0（0 表示"未计算"，IPv4 允许，保持兼容）；
`udp_drain_locked()` 拆出单帧入口 `net_udp_rx_frame()` 并统一走它，
坏帧计入 `udp_bad_frames`。

**修复前**：坏包静默入队（无证据行）。
**修复后**：`net: audit udp ingress rejects malformed ok (rejected=5)`
（坏 IP 校验和、MF、片偏移、长度越界、坏 UDP 校验和 5 类全拒；两个对照帧必须收下）。

### V3 客户端接受任意 SYN（高）

**复现**：连接 ESTABLISHED（`rcv_nxt=N`）时注入同 4 元组的 `SYN seq=0x7777 ack=0`：
修复前把 `rcv_nxt` 改成 `0x7778`、`snd_una` 改成 0、状态仍 ESTABLISHED。
之后攻击者按自己选的 seq 发数据，`tcp_input` 判定 `seq == rcv_nxt` → 交给应用；
正常对端的数据反而变成"乱序/重复"被丢弃。SYN_SENT 阶段注入
`SYN|ACK ack=<任意值>` 也能让连接"建立"。

**根因**：`tcp_input()` 的 SYN 分支无任何校验，直接改写 `rcv_nxt`/`snd_una`。

**修复**：只有 `state == SYN_SENT` 且带 `ACK` 且 `ack == iss+1` 的 SYN 才推进到
ESTABLISHED；其余 SYN 一律忽略（不改任何状态）。

**修复前**：连接被重置/劫持（无断言行）。
**修复后**：`net: audit tcp forged SYN ignored ok`（两种伪造 SYN 后
`state`/`rcv_nxt` 必须不变）。

### V4 服务端握手只校验 ack + ISS 可预测（高）

**复现**：向监听端口发 `SYN`（得到半开条目），再发一个 **seq 随便写**、
`ack == iss+1` 的裸 ACK：修复前把它放进完成队列，`accept` 成功返回一个
没有任何真实对端的连接（幽灵连接），应用之后写的数据全部发给"攻击者"。
旧 ISS 是 `0x50000 + n*0x1000`，`ack` 可盲猜（客户端 ISS 也是
`0x30000 + ticks&0xFFFF`）。

**根因**：`tcp_listen_complete()` 只比 `ack`，不看 `seq`；ISS 确定性递增。

**修复**：完成握手额外要求 `seq == h->rcv_nxt`（对端 SYN 的下一个字节）；
ISS 改为 `tcp_iss_mix()`（TSC/ticks/栈地址熵 + xorshift，保证每次不同），
客户端 ISS 同样处理。

**修复前**：盲握手可成功（无断言行）。
**修复后**：`net: audit tcp handshake seq validated ok`（seq 错的 ACK 不得进完成
队列；seq/ack 都对的 ACK 必须完成并出现在完成队列）。

### V5 客户端 `rcv_nxt` 越过实际交付字节（中）

**复现**：合成连接把接收缓冲填到 `rlen=2048-16`，再交付一个 100 字节段：
修复前 `tcp_deliver()` 只拷 16 字节，但 `tcp_input()` 仍 `rcv_nxt += 100` 并回 ACK。
对端认为 100 字节已送达 → 不重传 → 应用永久缺 84 字节（流被静默破坏）。

**根因**：`tcp_deliver()` 返回 void，交付量与序号推进量脱钩。

**修复**：`tcp_deliver()` 返回实际拷贝字节数，`rcv_nxt` 只按它推进（缓冲满时回
重复 ACK，让对端按 RTO 重传剩余部分）。

**修复前**：应用侧缺字节且无任何错误返回（无断言行）。
**修复后**：`net: audit tcp receive accounting ok (partial segment not over-ACKed)`
（`rlen == 2048` 且 `rcv_nxt == seq+16`）。

### V6 `SYS_LISTEN` 兜底监听 socket 永不回收（中）

**复现**：普通进程不建 TCP fd，直接 `SYS_LISTEN(port, backlog)`：修复前用
`net_tcp_socket(0)`（`owner_pid=0`）建监听 socket，而 `net_tcp_sock_gc()` 只回收
"有 pid 且 fd 已消失"的槽 → 该 socket **永不释放**。进程退出后端口仍被占，
反复调用可占满 8 个 socket 槽，系统再无法 TCP listen（拒绝服务）。

**根因**：兜底路径的属主信息缺失 + GC 只看 fd。

**修复**：兜底 listener 记在调用者 `pid` 名下（`owner_fd = -1`）；GC 对
`owner_fd < 0` 的槽改用"进程是否还存在"（`proc_find`）判定；并限制每进程最多
1 个这种无 fd listener（`net_tcp_pid_orphan_listeners`）。

**修复前**：端口/socket 槽永久泄漏（无断言行）。
**修复后**：`net: audit listen socket owned by pid ok`（兜底 listener 归当前 pid、
无 fd；同进程第二个端口必须被拒）。

### V7 ARP reply 只校验 `10.0.x` 两字节（中）

**复现**：任意主机（或伪造帧）发一个 ARP reply，sender IP 写成 `10.0.9.9`、
MAC 写成攻击者 MAC：修复前 `net_parse_arp()` 只比 `rd16(f+28) == 0x0A00` 就
`memcpy(gw_mac, f+22, 6)` → 网关 MAC 被改写，之后所有出网流量（含 TCP/UDP
数据）都发给攻击者（同链路流量劫持）。

**根因**：ARP 回复的来源校验过粗，且不校验 target/单播性。

**修复**：校验 htype/ptype/hlen/plen；**sender IP 必须等于 gw_ip**、target IP 必须
等于本机地址；sender MAC 必须是单播且非 0。

**修复前**：任意同链路主机可改写网关 MAC（无断言行）。
**修复后**：`net: audit arp spoof rejected ok`。

## 5. 未修项 / 明确不承诺

以下问题本次**只记录、不修**（跨 `net/**` 之外或需要重构收发模型）：

1. **无 IP 分片重组**：现在明确丢弃所有分片（V1）。这是策略而非缺陷；
   要支持需要重组缓存 + 超时 + 首片/尾片校验。
2. **UDP 单报 64 字节上限**（`NET_UDP_PAYLOAD_MAX`）：超长报文仍截断入队
   （软件队列/`udp_req.data[64]` ABI 冻结所致）。
3. **TCP 广告窗口恒为 2048**：不做零窗口通告；接收缓冲满时靠对端 RTO 重传
   （V5 修好后不再丢数据，但吞吐/时延不优）。
4. **客户端 ACK 伪造**：`conn.snd_una` 只有"正数推进"约束，没有 `snd_max`
   上界；伪造 ACK 可让 `tcp_send_wait` 提前返回（静默丢数据）。修它需要重构
   `snd_nxt` 的推进时机（当前是 `tcp_send_wait` 返回后才推进），本次不动。
5. **服务端无乱序缓存**：`seq != rcv_nxt` 的数据段直接丢弃（只回 ACK）。
6. **`socks[].conn` 是死字段**：监听 socket 关闭不连带已 accept 的连接
   （符合 BSD 语义，但 `net_tcp_sock_conn()` 永远返回 -1，属无用接口）。
7. **DNS 事务 ID/源端口可预测**：`nb_dns_id++`、`nb_dns_sport++` 递增可预测，
   理论上可 off-path 伪造 DNS 响应（缓存投毒）。需要随机源，本次未纳入。
8. **e1000 驱动**：不在本会话修复范围（按要求不改 `dev/net/e1000.c`）；本次只
   确认 `e1000_rx_poll()` 会把帧长夹到 `max_len`，所以解析层拿到的 `len`
   不会超过调用者缓冲区（这是"没有越界"的前提之一）。
9. **tcptest 尾部延迟（既有 flake，已转 audit-smp）**：`net: tcp selftest PASS`
   偶发晚于 60s 窗口；QMP 现场看到 CPU0 停在 `lib/string.c:20` 的 memset 且
   `EFL.IF=0`，失败点在不同用例间漂移（本会话观察到 4 个不同停点），基线
   38161af 同样出现。与 net 解析改动无关；本次审计用例已改为**不依赖调度**
   的纯状态断言，并前置到 `net_tcp_robust_selftest()` 最前面，保证审计证据
   不受该 flake 影响。

## 6. 越界/空指针/死循环：推演范围与结论

对下列每一项都做了"逐帧长度 × 字段组合"的人工推演，并用 §4 的注入用例覆盖边界：

| 检查项 | 结论 |
|---|---|
| 帧缓冲读取越界（`f+12`/`ip+ihl`/`udp+8`/`tcp[12]`/`icmp+6`） | 每个入口先校验 `len >= 14+ihl+8/20` 与 `tlen <= avail`；TCP 额外要求 `tlen >= ihl+20`（本次补）；ICMP 要求 `tlen >= ihl+8`。读 L4 头前不会越过帧尾 |
| 数组下标（`socks[8]`/`srv[4]`/`synq[4]`/`acceptq[4]`/`ooo[4]`/`udp_q[8]`/`arp[8]`/`loop_q[8]`） | 全部有界：`sock_get/srv_get` 范围检查；`acceptq[accept_count]` 由 `accept_count < backlog <= 4` 保证；`ooo`/`udp_q` 定长且满即丢；`marks[seq % 8]` |
| 缓冲区溢出（`rbuf[2048]`、`tcp[20+536]`、`f[700]/f[1600]`、`f[60]`） | 拷贝前统一 `min(len, room)`；发送侧 `len > MSS/MTU` 先夹住；ARP/ICMP 帧固定 42/60 字节且写入偏移 < 60 |
| 空指针 | `sock_get/srv_get/tcp_listen_match/proc_find/fd_get/tcp_srv_alloc` 的返回值都被判 0；`net_syscall` 全经 `copy_*_user`，用户指针不解引用 |
| 整数符号/回绕 | seq/ack 比较统一 `(int32_t)(a-b)`；`tlen/ulen` 是 16 位、`avail` 32 位；先比较后相减，无 `len-ihl` 下溢；UDP 校验和只按声明长度读，且声明长度 ≤ 帧内 |
| 死循环 | `udp_drain`/`tcp_drain` 有帧数上限（8/64）；DHCP/DNS/ping/ARP 等待都有 deadline + spin 上限；`net_tcp_accept/conn_read` 有 waited 上限；`tcp_send_wait/tcp_connect` 有 retries 上限 |

结论：**未发现可利用的越界读写或空指针**。真正的风险集中在"坏包被当合法包"，
即 V1/V2/V3/V4（数据注入、流量劫持、幽灵连接）与 V5（静默丢数据）。

## 7. 验证记录

### 7.1 隔离 worktree（基线 `38161af` + 本会话 `net/**`）

验证前逐文件比对过 worktree 里的 5 个 net 文件与主树（= HEAD）一致，所以下面的
结果就是**已提交代码**的结果。

| 轮次 | 全量断言 | `net: audit` 行 | elapsed | 说明 |
|---|---|---|---|---|
| 终验（干净窗口） | `=== TEST PASS (225/225) ===` | **9/9** | 39.2s | `tools/audit-net.ps1` 首轮即 PASS（未触发重试） |
| 宿主争用窗口（其它会话 QEMU 并发） | 8~26 项 shell/键注入类失败 | **9/9（全部轮次）** | 112~190s | 宿主被占满，键注入/超时类检查成批失败，与本改动无关 |
| 既有 flake 命中轮次 | 只缺 `TCP 健壮性自检通过` + `TCP close 释放端口` | **9/9** | — | 仓库既有 tcptest 尾部延迟（§5-9） |
| 基线对照（未改 `net/**`） | 3/3 通过 | —（无审计行） | ~40s | 既有正向链路正常 |

关键点：**9 条审计行在所有轮次都出现**（包括宿主争用、既有 flake 命中的轮次），
因为它们在 `net_tcp_robust_selftest()` 最前面执行，且是纯状态断言（不 sleep、
不依赖真实对端）。

### 7.2 正向链路未回归

`net: ARP reply gw=…` / `net: ICMP echo reply seq=…` / `net: UDP echo reply` /
`net: TCP echo reply` / `net: dhcp ok` / `net: dns ok` / `net: ping ok` /
`net: tcp handshake ok` / `net: tcp large transfer ok (16384 bytes, retrans=6)` /
`net: tcp listen/accept selftest PASS` / `net: tcp selftest PASS`
全部保持通过（`tools/audit-net.ps1` 逐条断言）。

### 7.3 复跑输出（终验，节选）

```
=== audit-net run 1: build + qemu (obj=.obj-audit-net qmp=4543 echo=12399 serial=obj/audit/net.log) ===
--- net: audit 回归行 ---
  [PASS] net: audit arp spoof rejected ok
  [PASS] net: audit udp ingress rejects malformed ok (rejected=5)
  [PASS] net: audit net ok (udp+arp)
  [PASS] net: audit tcp forged SYN ignored ok
  [PASS] net: audit tcp receive accounting ok (partial segment not over-ACKed)
  [PASS] net: audit tcp handshake seq validated ok
  [PASS] net: audit tcp ip ingress rejects malformed ok (bad=3)
  [PASS] net: audit listen socket owned by pid ok
  [PASS] net: audit tcp ok
--- 既有 net 正向断言 ---
  [PASS] net: ARP reply gw=      ...（11 条全 PASS）
--- tcptest 尾部（已知 flake，单独判定） ---
  [PASS] net: tcp selftest PASS
--- 全量检查：225/225 PASS ---
=== audit-net: PASS（1 轮；修复回归行齐全 + 225/225 + 正向链路未回归） ===
```

## 8. 提交与改动文件

| 提交 | 内容 |
|---|---|
| `e4ef7ea` | `[fix] net: 7 项收包校验/状态机缺陷（audit-net）` |
| `74042d9` | `[test] net: 审计用例前置且不依赖调度；audit-net 门禁读 build 日志 + 尾部 flake 重试` |
| 本提交 | `[docs] audit-net: 网络协议栈审计报告与回归证据`（`docs/audit/net.md`） |

改动文件：

- `src/kernel/net/net_internal.h`（新增：模块内部校验原语声明）
- `src/kernel/net/net.c`（`net_ipv4_validate` / `net_udp_validate` / ARP 严格化 /
  ICMP 校验和 / UDP 单帧入口 / 审计用例）
- `src/kernel/net/net_boot.c`（DHCP/DNS/ping 入口共用校验原语）
- `src/kernel/net/tcp.c`（TCP 入口校验、SYN/握手序号校验、ISS 随机化、
  接收记账、GC 归属、审计用例）
- `src/kernel/net/net_syscall.c`（兜底 listener 归属 + 每进程上限 + 审计用例）
- `tools/audit-net.ps1`（新增：隔离回归入口）
- `tests/audit/net/README.md`（新增：注入用例与帧布局）
- `docs/audit/net.md`（本文）
