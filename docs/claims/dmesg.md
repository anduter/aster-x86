# dmesg 会话声明（P2：内核日志环 + SYS_DMESG）

日期：2026-10-31　分支：`main`（基线 `443dba1`，v0.74.2）　会话：`dmesg`

## 1. 改动范围

| 文件 | 内容 |
|---|---|
| `src/kernel/include/klog.h` | 日志环接口：`klog_write/klog_printf/klog_capture/klog_read/klog_read_tail_lines/klog_print_tail/klog_selftest` |
| `src/kernel/core/klog.c` | 64KB 静态环、按行丢弃、关中断短自旋、只读快照、启动自检 |
| `src/kernel/core/misc_syscall.c` | `SYS_DMESG(85)` 的 `owns` + `case`（用户窗口校验 + 锁外 `copy_to_user`） |
| `src/kernel/apps/shell.c` | 内核 shell 新增 `dmesg [-n N]`（默认 24 行，上限 100） |
| `src/kernel/lib/kprintf.c` | **窄例外（已获父会话批准）**：`kprintf` / `kprintln` 各加一次 `klog_capture(buf,len)`，共 2 处；`kprintln` 不经过 `kprintf`，不会重复入环 |
| `docs/claims/dmesg.md` | 本声明 |

未改 `include/abi.h`（85 已预留）、`build.ps1`，未碰 `svc/**`、`net/**`、`fs/**`、`proc/**`。

## 2. 机制

- **容量/结构**：`KLOG_RING_SIZE = 65536` 的静态 BSS 环，前后各一个 `0x4B4C4F47` 金丝雀。
  按字节追加、按 `\n` 计行；溢出时从最旧端**整行**丢弃（没有换行的超长单行整体丢弃），
  因此快照总是从行首开始，不会出现半行拼到别的行上。
- **并发**：写侧只做 `spin_lock_irqsave + memcpy`，临界区上限约 640B，不打印、不分配、
  不睡眠；可在 IRQ/panic 路径调用（panic 的最后输出也会进环）。
- **惰性初始化**：环是静态 BSS，本身零初始化即可用；`klog_init()` 只是显式入口，
  第一次写入会惰性置位，所以 `kprintf` 在 `klog_init` 之前调用是安全的。
- **读侧快照**：`klog_read()` 在锁内把环内字节拷到调用方缓冲，锁外再 `copy_to_user`
  或打印；用户缓冲缺页/坏指针不会影响环。`klog_read_tail_lines()` 供 `dmesg -n N`
  只取最近 N 行（超长时保留最新一段并对齐行首、只输出完整行）。
- **`SYS_DMESG(85)`**：`ebx=用户缓冲, ecx=maxlen`。
  - `maxlen == 0` → `-1`；`misc_user_buf()` 拒绝 `>=3GB`、回绕和 `>64KB` 的缓冲 → `-1`；
  - 正常时返回写入字节数（可能是 0，表示环为空）；
  - `copy_to_user` 中途缺页（窗口内但未映射）→ `-1`，不影响内核。
- **`dmesg [-n N]`**：内核 shell（`apps/shell.c`）解析 `-n`，调用 `klog_print_tail()`；
  输出直接走 `console_write_atomic`（console + 串口），不再经 `kprintf` 入环，
  避免 dmesg 把自己记进下一次输出。
- **启动自检**：`-Test` 构建里，在第一次 `kprintf` 同时满足“`int 0x81` 已走过一次”
  （`syscall_count()>0`）与“时钟中断在推进”（`timer_ticks()>0`）时触发一次
  `klog_selftest()`；用一次性原子 guard 保证只跑一次。不是文本匹配。
  自检先保存启动日志尾部，溢出测试后恢复，避免把真实启动日志顶掉。

## 3. 自检（`-Test` 串口实测）

```text
sys: int 0x81 works
klog: ring detail used=65502 lines=1213 dropped=293
klog: ring 64KB ok
klog: truncate detail asked=128 got=127
klog: truncate ok
dmesg: syscall detail small=46 full=178 bad_hi=-1 bad_map=-1
dmesg: syscall ok
dmesg: last 3 line(s) (4176/65536 bytes, 0 dropped)
dmesg: syscall probe
dmesg: syscall detail small=46 full=178 bad_hi=-1 bad_map=-1
dmesg: syscall ok
dmesg: selftest PASS
```

期望的 4 行都在：

```text
klog: ring 64KB ok
klog: truncate ok
dmesg: syscall ok
dmesg: selftest PASS
```

| 断言 | 含义 |
|---|---|
| `klog: ring detail used=65502 ... dropped=293` | 连续写入 1400 行（≈65.8KB，叠加启动日志后 >64KB）后环基本写满（`65502 <= 65536`），丢掉了 293 行旧日志；`fill line 1399` 仍在、`fill line 0000` 已丢；前后金丝雀完好（否则打印 FAIL）。 |
| `klog: truncate ok` | `klog_read(buf,128)` 返回 127 字节（≤128、以 `\n` 结尾），证明小缓冲按最新尾部截断。 |
| `dmesg: syscall ok` | `int $0x81` 走真实分发：小缓冲 64 → 46 字节；192 字节缓冲 → 178 字节且内容含 `dmesg: syscall probe`；`0xC0000000`（窗口拒绝）与 `0xBFFFF000`（未映射页，copy 失败）都返回 `-1`。 |
| `dmesg: last 3 line(s) ...` + 3 行 | `klog_print_tail(3)`（`dmesg -n 3` 的同一实现）打印最近 3 行；环已恢复启动日志尾部（4176 字节）。 |
| `dmesg: selftest PASS` | 以上全部通过。 |

## 4. `dmesg` 输出样例

`dmesg -n 3`（`klog_print_tail` 的格式，自检里已实际打印）：

```text
dmesg: last 3 line(s) (4176/65536 bytes, 0 dropped)
dmesg: syscall probe
dmesg: syscall detail small=46 full=178 bad_hi=-1 bad_map=-1
dmesg: syscall ok
```

`dmesg`（默认最近 24 行）的头部同格式，随后逐行输出环内日志。内核 shell 入口：

```text
shell> dmesg -n 5
dmesg: last 5 line(s) (...)
...
```

> 说明：`-Test` 构建默认跑 ring3 `ushell`，内核 shell 只在 `/bin/shell` 加载失败时作为
> 回退入口出现。因此自动回归里没有真实敲入 `dmesg` 命令；上面是 `dmesg -n N` 共用实现
> （`klog_print_tail`）在启动自检里实际打印到串口的结果，命令解析层只是多一次 `-n` 参数解析。

## 5. 回归与隔离

- 基线工作区里 `svc/**` 正被 IPC 会话修改（`src/kernel/svc/ipc.h` 当时有未完成的注释，
  主树无法编译）。本会话在 `D:\dmesg-wt`（`git worktree`，基线 `443dba1` + 本会话 5 个
  代码文件）里做构建与回归，不触碰主树的其它会话改动。
- 命令：`tools\session-run.ps1 -Repo D:\dmesg-wt -Name dmesg -Test`
- 结果：`=== TEST PASS (254/254) ===`，耗时 55.7s；工作树干净（只含本会话文件）。
- `build.ps1` 的 checks 未改，新增自检行只打印到串口，不改检查项数（仍 254）。

## 6. 遗留 / 后续

- 用户态 shell（`src/user/user_shell.c`）未加 `dmesg` 命令（任务里“可选，不做也行”）。
  内核侧 `SYS_DMESG` 已就绪，加一行 syscall 包装即可。
- 环里不存时间戳/级别（INFO 无前缀，WARN/ERR 有 `[warn]/[err]` 前缀）；后续可加 tick 前缀。
- `SYS_DMESG` 一次最多 64KB（与 `user_range_ok` 上限一致）；更大的读取需要分页 API。
- `dropped` 是累计值；环在自检恢复后清零重计（避免把自检的 293 次丢弃算进运行期）。
