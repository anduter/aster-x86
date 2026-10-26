# eng 会话 claim（P2/P3 工程化与可观测性）

- 批次：2026-10-27 并行会话（`docs/roadmap-claims.md` 的 `eng`）
- 基线 HEAD：`79f5c66`（v0.73.0 开发线；`version.h` = `0.73.0`）
- 允许改动范围：`tests/**`、`tools/**`、`docs/**`、`README*`、`include/kernel.h`

## 1. 改动文件清单

| 文件 | 改动 |
|---|---|
| `tools/unit.ps1` | 新增 `-Gcc32`；32 位用例自动选 `i686-w64-mingw32-gcc.exe`；新增 path/pmm/fat16 三套；汇总打印 `total checks: N` |
| `tests/unit/test_path.c` | 新增：`make_abs_path` / `path_has_parent` / `copy_user_string` / `user_range_ok` / `fd_alloc*` / `open_rights` |
| `tests/unit/test_pmm.c` | 新增：位图初始化、单页/连续页分配释放、耗尽、非法释放、double-free、引用计数（COW）、`pmm_mark_range` / E820 |
| `tests/unit/test_fat16.c` | 新增：BPB 小端、FAT16 挂载校验、8.3 名字、目录项边界（0x00/0xE5/LFN/卷标/满 512 项）、size 小端、簇链/截断链、句柄上限、创建/截断/删除/改名 |
| `tests/unit/shim/include/dev.h` | 新增：块设备最小 shim（`block_device_t` + `blk_*`） |
| `tests/unit/shim/include/kernel.h` | 补 `memcpy/memcmp/strncmp/strchr` 声明（供 FAT16/PMM 套件链接真实 `lib/string.c`） |
| `tools/session-run.ps1` | 测试锁加固：等待上限 1800s、`{pid,time,session}`、可读持锁者信息、Move 抢占式清理陈旧锁、只删自己的锁 |
| `README.md`、`README.en.md` | v0.73.0、182 项、10 组/520 checks；组件/SIP/link/TCP 能力 |
| `docs/ci.md` | 131→182、7→10 组、520 checks、`-Only`/`-Gcc32` 用法 |
| `docs/modules.md` | 第 9 节「v0.73.0 能力现状（按模块归属）」 |
| `docs/status-2026-10-26.md` | 追加 2026-10-27 并行批次一行（指向本 claim 与 roadmap-claims） |

未改：`build.ps1`、`src/kernel/**`（含 `include/kernel.h` 未动）、`include/abi.h`、`tools/soak*.ps1`、`flake-hunt`。

## 2. host 单测（`tools/unit.ps1`，不启动 QEMU）

新增 3 套，全部直接编译真实内核源文件（不改内核）：

| 套件 | 编译的真实源码 | 编译方式 | checks |
|---|---|---|---|
| `path` | `src/kernel/proc/trap/syscall_util.c` + `src/kernel/lib/string.c` | i686（`sys_getchar_block` 含 32 位 `pushfl/popl`，x64 host gcc 无法汇编） | **79** |
| `pmm` | `src/kernel/mm/pmm.c` + `src/kernel/lib/string.c` | i686 + `-Wl,--disable-dynamicbase` | **92** |
| `fat16` | `src/kernel/fs/fat16.c` + `src/kernel/lib/string.c` | i686 | **162** |

- 原有 7 套 187 checks 全部保持 PASS；合计 **10 套 / 520 checks**。
- `pmm` 套件用 4KB 对齐的静态 arena 充当「位图 + 物理内存起点」，并要求 `-Wl,--disable-dynamicbase` 固定 PE 基址（否则 Windows ASLR 可能把 arena 放到 `HEAP_BASE=8MB` 保留区）。
- `path` 套件用最小 proc shim（`proc_current`/`proc_root_of`/`proc_block`/`keyboard_getchar`/`copy_user_*`）。
- 已知限制（按当前内核实现断言，未改内核）：`make_abs_path` 的 root 前缀匹配不校验段边界（`system:/jailer/x` 会被当作在 `system:/jail` 内）；`out_max == 0` 会越界写 `out[-1]`，测试不调用该边界；相对路径只用 cwd，不重复拼 root。

### 期望断言/汇总行（供 CI/build 集成检查）

```
[PASS] path  (79 checks,
[PASS] pmm  (92 checks,
[PASS] fat16  (162 checks,
total checks: 520
=== UNIT PASS (10/10) ===
```

`tools/ci.ps1` 的 `=== UNIT PASS \((\d+)/\1\) ===` 正则不变，仍能匹配；套件数由 7 变 10。

## 3. 整机回归

- 命令：`tools\session-run.ps1 -Name eng -Test`
- 结果：**182/182 PASS**（2026-10-27，`=== TEST PASS (182/182) ===`；另因其它会话残留 QEMU 占用默认 QMP 4511，验证时显式用 `-QmpPort 4521`）。
- 本批次不改 QEMU 检查项；新增内容只进 host 单测。测试锁 commit：`a3144cc`。
- 注：验证过程中多次遇到**锁外进程**（直接跑 `build.ps1 -Test` 的会话）抢占宿主 12345；`session-run` 现在会先等端口再抢锁，但锁外进程无法被锁约束，`build.ps1` 仍会快速报“12345 被占用”。

## 4. dmesg 环（可选，未做）

评估后放弃，原因是**文件所有权冲突**，不是时间问题：

1. 用户态 `dmesg` shell 命令在 `src/user/user_shell.c`（roadmap 归 `pkg` 会话的 `user/**`）；
2. 读取内核环需要新 syscall，必须改 `include/abi.h`（roadmap 明确只有 `net` 可改）；
3. 内核环本身若放 `src/kernel/core/` 需改 core 目录（本会话只在 `include/kernel.h` 可动）。

因此没有改 `include/kernel.h`、没有新增 core 文件，避免与 `pkg`/`net` 会话冲突。若后续集成会话愿意分配 syscall 号与 shell 文件，可复用现成方案：`core/klog.c`（环形 4KB，`klog_write` 在 `kprintf` 路径追加）+ `SYS_DMESG(n, buf)` + shell `dmesg`。

## 5. 文档同步

- `README.md` / `README.en.md`：v0.73.0、182 项、10 组/520 checks；组件模型、SIP-lite、硬/符号链接、TCP 健壮性；XFS1 v3→v5、单文件 16KB→64KB、8→32 打开文件。
- `docs/ci.md`：182/182、10 套/520 checks、`-Only`/`-Gcc32` 用法与实测时间。
- `docs/modules.md`：新增第 9 节，按 M9/M4/M5/工程化列出组件、SIP、link、TCP 与 host 单测归属。
- `docs/status-2026-10-26.md`：追加 2026-10-27 并行批次一行。

## 6. 遗留

1. dmesg 环（见第 4 节）：需 `abi.h` 与 `user_shell.c` 的所有者配合。
2. `listen/accept`（78/79）由并行 `net` 批次实现；本会话未触碰 ABI。
3. FAT16 测试覆盖根目录 8.3 与 `fat16_*` API；LFN、多级目录、FAT12/32 不在当前后端能力内。
4. 硬链接持久化（XFS1 v6）由 `fs` 批次负责；README 已注明「硬链接关系不落盘」。

## 7. session-run 测试锁加固（并行会话小修）

`tools/session-run.ps1`：

1. **锁路径全机共享**：默认 `$env:TEMP\aster-test.lock`（`-LockPath` 可覆盖），不再用 `<repo>/.session/test.lock`——隔离 worktree 各有 `.session`，原来锁不互斥。
2. **抢锁前先等 12345 端口**：`Get-NetTCPConnection -LocalPort 12345 -State Listen`（不可用时 netstat 回退）有监听就等待/重试（上限 `-WaitSec`，默认 1800s），日志打印持有者进程/PID。
3. **等待上限** `-WaitSec` 默认 600 → **1800s**（7 个会话串行 `-Test` 时 600s 不够）。
4. **锁内容** 写入 `{pid,time,session,repo}`（同时保留 `name/start` 兼容旧读取者）。
5. **可观测** 等待时打印持锁者 `session=... pid=... age=..m` 与剩余秒数（每 30s 或持锁者变化时）；拿不到信息时打印 `actively held`。
6. **原子 stale 检测/抢占**：
   - 主路径仍是 `[IO.File]::Open(..., 'OpenOrCreate', 'ReadWrite', 'Read')`：只有一个写者，但等待者可以读持锁者信息（`FileShare.Read`）；持锁者活着时第二个写者必失败。
   - 打不开时先共享读锁内容；**持锁者 PID 不存在 / 锁龄 > 30 分钟 / 内容不可解析** → 用 `Move-Item` 移到唯一临时名（`test.lock.stale-<pid>-<rand>`）再删。打开中的锁文件没有 `SHARE_DELETE`，Move 必失败；两个等待者同时 Move 也只有一个成功（另一个 source 不存在），因此不会重复删/误删活锁。
   - Move 成功后再核对临时文件里的 PID；若与刚读到的陈旧 PID 不同（说明期间被替换），尽量移回原位并放弃本次抢占。
7. **只删自己的锁**：`finally` 先 `Dispose`，再读锁内容对比 `pid+time` 才 `Remove-Item`；避免释放瞬间把下一个会话刚创建的新锁删掉。
8. `-Name/-Test/-ObjDir/-DistDir/-QmpPort` 语义未变；`-LockPath` 为新增可选参数。

验证（本机）：

```
[session-run] waiting for echo port 12345: session/process=powershell pid=23252; 1800s left
[session-run] waiting for test lock: session=holder-test pid=21104 age=0m; 1800s left
[session-run] test lock acquired by eng (pid 24620)
```

（用单独的监听/持锁进程模拟；端口与锁释放后均正常继续，`%TEMP%\aster-test.lock` 在 `finally` 删除。）

已知并行副作用（非本脚本能完全消除）：`-Name` 非 a/b/c 时 QMP 默认都是 **4511**，若上一个会话的 QEMU 残留会 `build.ps1` 快速失败；锁能避免正常会话互相抢，但锁外直接跑 `build.ps1 -Test` 的进程仍需人工清理。建议各会话在 `-QmpPort` 上使用不同端口（a/b/c 已分别 4481/4491/4501）。
