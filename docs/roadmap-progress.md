# 路线图 1→4 进度

每个阶段完成门禁后追加一段：阶段、HEAD、检查项数、测试证据、遗留。
门禁定义见 `docs/ci.md`：`tools/ci.ps1 -Quick -StrictBom` +
连续 2 次 `build.ps1 -Test` 全 PASS + `check-deps` 0 违规。

## 阶段 0（起点，只记录）

- 起点 HEAD：`07e43d0`（v0.72.0 tag `646f81d`）
- 工作区干净，131 项检查，无 QEMU 运行时冲突
- 提交：无（只写本文档）

## 阶段 1：push + GitHub CI 实跑

- 状态：**blocked**（环境缺失，非代码问题）
- 事实：`git remote -v` 为空；`gh` 不在 PATH（`CommandNotFoundException`）；
  无任何 GitHub 凭据可验证。
- 处置：按任务要求不卡住，写 `docs/github-ci-blocked.md`（缺什么 + 用户需
  执行的两条命令），继续阶段 2。
- 未做：创建公开仓库、推送到未知地址、伪造 CI 结论。
- HEAD：`07e43d0` + 本文档所在提交

## 阶段 2：组件模型 MVP（签名 .pkg + `_pkgd` + echosvc 组件化）

- 状态：**完成**
- 新增：`src/kernel/include/pkg.h`、`src/kernel/pkg/{pkg_verify,pkgd,pkg_selftest}.c`、
  `tools/mkpkg.ps1`、`tools/pkg-verify.ps1`、`packages/{echosvc,needsdep,broken,helloapp}/*.conf`；
  `build.ps1` 打包 + 内建包目录 + 15 项检查；`supervisor` 支持 `start:/stop:`；
  `prog_init` 注册 `pkgd`/`pkgtest`，`bootseq` 起 `_pkgd` 与回归线程。
- 设计文档：`docs/pkg.md`（容器布局、manifest、目录/DB、安装回滚、服务协议、限制）。
- 检查项：**131 → 146**。
- 测试证据：
  - 单次 `build.ps1 -Test`：`=== TEST PASS (146/146) ===`（`serial-pkg3.log`）
  - 8 条要求的用例全部覆盖并 PASS：合法安装、坏签名、缺依赖、失败回滚、
    卸载运行中服务、重复安装、普通用户越权、用户应用装 `~/Apps`；
    另有 7 项（服务注册/落盘/通知 supervisor/校验/list/协议/重装上线）。
- 遗留：
  1. `entry` 仍指向内核内建程序（echosvc 未拆 ring3 ELF）；
  2. 无用户态 `pkg` 命令（只有内核 API + `_pkgd` 协议）；
  3. 组件 DB/目录落盘依赖显式 `sync`（安装发生在 `disk_selftest` 之后）；
  4. 同名组件不支持多版本/升级回滚。
- 门禁：见下方"阶段 2 门禁"小节。

## 阶段 3：SIP-lite（系统路径运行时只读）

- 状态：**完成**
- 实现：`src/kernel/fs/sip.c`；声明进 `fs/fs.h`；ramfs 的 6 个写入口
  （写打开/mkdir/unlink/rename/chmod/chown）统一调 `sip_guard_write`。
- 规则：`system:/System`、`system:/bin`、`system:/drivers` 对**所有人（含 root）**
  只读；判定前规范化 `..` / `.` / 重复 `/`。
- 例外：`_pkgd` 安装/卸载期间进入"签名更新器作用域"，scope 绑定进入时线程 pid；
  启动期安装 `/bin/shell` 走同一入口。
- 开关：内核参数 `sip=0` / 运行时 `sip_set_enabled(0)`；`-Test` 默认仍开启
  （否则"root 写系统目录被拒"没有意义）。
- 检查项：**146 → 156**。
- 测试证据：`build.ps1 -Test` → `=== TEST PASS (156/156) ===`（`serial-sip2.log`）；
  SIP 10 项全 PASS：三目录拒绝、写/删/改名/改权限拒绝、穿越规范化、包仓库
  对 root 只读、更新器放行、`sip=0` 可关闭、自检 PASS。
- 遗留：
  1. 只覆盖 ramfs 写路径（FAT16/svc/blk 另有自己的语义）；
  2. 判定不解析符号链接（阶段 4b 落地后需补"解析后复查"）；
  3. 更新器作用域是内核可信标志，未做成 capability 表项。

## 阶段 4a：FS 单文件 64KB + XFS1 v5

- 状态：**完成**
- `FS_FILE_MAX` 40960 → **65536**（任务卡写的 32768 是更早基线；仓库当时已是
  40960），新增 `FS_FILE_MAX_LEGACY = 40960`。
- `FS_DISK_VERSION = 5`：super 增加 `max_file` / `data_bytes` 并撑满 512B
  （顺手修掉 164B 结构体被按 512B 写的越界读）；挂载接受 v4/v5：v4 按 40960
  限流并打印 legacy 行，下次 `fs_sync` 自动升级为 v5（数据区布局不变）；
  v5 的 `max_file` 必须落在 [40960, 65536]；其它版本拒绝。
- 挂载时新增 inode 校验：`size > 限额` 或数据区超出设备 → 整次挂载失败
  （之前损坏镜像可以打穿 64KB 的节点数据区）。
- `.bss`：`nodes[64]` 从 2.6MB 涨到 4MB，`_kernel_end` 0x83ac64 → 0x9e1f04；
  heap arena 自动抬到 0x9e2000，仍剩 14.4MB（`HEAP_BASE` 不用改）。
- 内核 shell 的 `file write/read` 缓冲移到 `static`（64KB 不能放栈上）。
- 检查项：**156 → 158**。
- 测试证据：`=== TEST PASS (158/158) ===`（`serial-fs1.log`）；
  `fs: XFS1 v5 mounted, file limit 65536 bytes`、
  `fs: 64KB file limit ok (wrote 65536 bytes, one-past rejected)`。
- 遗留：FAT16 单文件上限仍是簇链限制；没有 v4 旧镜像的自动化测试（兼容路径
  靠代码 + 文档，-Test 每次重建 disk.img）。（4b/4c 见下）

## 阶段 4b：硬链接 / 符号链接

- 状态：**完成**
- 内核：ramfs 增加 `canon/nlink`（硬链接）与 `FS_TYPE_LINK`（`data` 存目标）；
  `resolve_path()` 统一展开软链接（跳数上限 8）；`O_NOFOLLOW`/`lstat`/`readlink`；
  `ino_promote()` 处理"删掉宿主但还有链接"；fsck 允许 LINK、校验 canon 越界。
- VFS：`fs_link_cred`/`fs_symlink_cred`/`fs_readlink_cred`/`fs_lstat_cred` 路由，
  跨卷硬链接与 FAT16 的 link 操作一律 `E_INVAL`。
- SIP 交互：写操作对**展开后**的路径复查，堵住 `/tmp` 软链接指向 `/bin` 的绕过。
- 用户态：`SYS_LINK=73` / `SYS_SYMLINK=74` / `SYS_READLINK=75` / `SYS_LSTAT=76`
  （`SYS_NR_MAX=76`，只追加）；ulib 封装；ring3 shell 增加 `ln [-s]`、`readlink`、
  `linktest`，`-Test` 的注入命令里加了 `linktest`。
- 持久化：符号链接进 XFS1（type 3）；**硬链接关系不落盘**（sync/remount 后退化为
  独立副本），要真正持久化需要 XFS1 v6 的 inode `link_to/nlink`。
- 检查项：**158 → 174**。
- 测试证据：`=== TEST PASS (174/174) ===`（`serial-lnk5.log`）；11 条 `fs:` 链接断言
  + 5 条 `ushell:` 链接断言全 PASS。
- 遗留：
  1. 硬链接不跨重启；
  2. `fs_stat_t.nlink` 未暴露到 ABI（stat 结构仍是 24B，避免 ABI 变更）；
  3. 软链接目标不支持跨卷；目录硬链接按 POSIX 禁止。

## 阶段 4c：TCP 健壮性

- 状态：**完成**
- 新文件 `src/kernel/net/tcp.c`：序列号/ACK、MSS/窗口、校验和、RTT/RTO
  （RFC6298 简版）、超时重传 + 指数退避、3 次重复 ACK 快重传、乱序重组、
  FIN/RST/TIME_WAIT、慢启动 cwnd。丢包用 `loss_every=N` 在发送侧注入。
- `net.c` 导出 `net_ip_send` / `net_rx_frame` / IP 指针；`net.h` 声明。
- 回归放在 `tcptest` 程序线程（测试构建 spawn）：syscall 上下文关中断、
  timer 不前进，RTO 会等到 NMI watchdog panic——这一点已写进 docs/network.md。
- 宿主回显服务改成流式（16KB 往返）；build.ps1 的注入等待同时等
  `pkg: selftest done` 与 `net: tcp selftest PASS`。
- 检查项：**174 → 182**。
- 测试证据：`=== TEST PASS (182/182) ===`（`serial-tcp3.log`）：
  握手 / 乱序 / 坏校验和 / RST / 16KB + 6 次丢包重传 / RTO / FIN+TIME_WAIT / PASS。
- 遗留：
  1. 同步收发（在途 1 段），没有流水线/SACK/窗口缩放；
  2. 无 IP 分片重组、无 ARP 解析（目的 MAC 用网关）、只有客户端侧。