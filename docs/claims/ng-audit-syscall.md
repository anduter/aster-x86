# ng syscall 边界审计（第二轮，audit-sys）

- 分支：`ng`；起点 HEAD `60ea988`；日期 2026-10-31。
- 工具：`tools/audit-ng-sys.ps1` + ring3 探针 `tests/audit-ng/sys/audit_sys.c`
  （独立 ELF，经 FAT16 注入 `AUDIT.ELF`，由 ring3 shell 拉起；QMP 用随机空闲端口，
  不碰 soak 的 11995/11996）。
- 结论：**531 个边界用例在 `-cpu max` 与 `-cpu qemu64` 下都 PASS，无内核
  panic**。找到并修复 2 个真实缺陷（都不是 panic，但都会破坏文件/耗尽资源）；
  ring3 异常 -> 内核 panic 仍是已知限制（本轮的 `-Scenario crash` 继续按预期
  观察到 `PANIC: vec=0xe`）。

## 1. 结果总览

| 项 | 值 |
|---|---|
| 边界用例 | **531 / 531 PASS**（`AUDIT: summary pass=531 fail=0 total=531`） |
| CPU | `max`（SMEP/SMAP on）与 `qemu64`（SMEP/SMAP off）两轮都 PASS |
| 内核 panic | boundary 场景 0 次；crash 场景按设计 1 次（已知限制） |
| 发现 | 2 个（`vfs_open` 失败副作用、ramfs 节点池耗尽） |
| 修复 | 2 个（提交 `26f1b75`、`6e05740`） |
| 未修 | ring3 fault->panic（已知，需 E3 故障投递）；2 个 kill 时的小泄漏（见 §6） |

用例分组（按 `AUDIT: PASS <group>.*` 统计，另 3 个循环内用例在 total 里）：

| 组 | 数 | 覆盖 |
|---|---|---|
| `write*` | 17 | syscall 0：长度为 0/255/256/257/u64max、坏指针/非 canonical/溢出、页尾可读与跨页、`rdi=0` 诊断模式 |
| `ipc*` | 133 | 10..14：depth 0/1/64/65/u32max/u64max、坏/高位 ticket、坏指针/跨页、len 15/0/4113/u64max、消息头 len/flags/from、stale from、队列满/超时/close 释放、reply 边界、端口池 ENOSPC、跨进程 send/recv 唤醒、两个 blocked sender 的 `wake_one` 握手、kill/close 唤醒 blocked sender、非属主 recv/close=EPERM、depth=64×4112 深队列 close 复用、waiter 池饱和 |
| `fs*` | 127 | 20..27：路径指针/长度 62/63/251/254/300、跨页路径、坏 fd/负 fd、fd 表满 EMFILE、目录 fd read/write=EISDIR、O_CREAT\|O_EXCL、O_TRUNC、unlink-open、页边界 copyin/copyout（write=1 检查）、listdir max 0/1/2/u32max 与 80B 页尾、stat 页尾/跨页、`/mnt/disk`（FAT16）stat/read/listdir/create/write/truncate/unlink/8.3 名字、FAT unlink busy=EBUSY、ramfs 节点余量 |
| `svc*` | 51 | 30..35：readkey（垃圾参数/空闲）、dmesg max 4096/4097/u64max/页跨页、login 名字 30/31、pass 63、跨页指针、log tag 14/15、msg 126/127、pkg_list max 1/512/513/u64max/只读页/页尾、pkg_verify 30/31 字符与 `.`/`..`/空/穿越 |
| `proc*` | 153 | 40..47：spawn 8 项 argv（126/127 单项、正好 8 项、无 NULL 终结、argv=NULL）、path 62/63、坏 argv 元素/跨页、upid 预校验/页尾、spawn 目录、exit code 掩码、self-kill（含带 shm/fb 映射）、kill pid/sig 边界、kill 幂等/已回收、wait status NULL/页尾/跨页、wait 非子进程、线程池/进程表耗尽与回收、kill 阻塞在 send/recv/wait 的进程、并发 spawn/kill/wait storm、shm/fb 映射与 kill/正常退出回收 |
| `shm*`/`fb*` | 43 | 45/46/47：pages 0/1025/u64max、坏/跨页输出指针、对象表 8 满 ENOSPC、同一对象 8 个映射满、跨进程 map（kill 与正常退出两条释放顺序）、FB 重复 map=EBUSY、LFB 子进程 kill/正常退出 |
| `fuzz*` | 4 | 1500 条随机 VFS 路径（stat/open/listdir）、500 条随机 IPC 消息（合法缓冲 + 随机 len/type/flags/from + 隔条合法消息） |

## 2. 复现

```powershell
# 边界（默认 boundary；531 用例，~1 分钟）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-ng-sys.ps1

# 降级 CPU（无 SMEP/SMAP，走 pg_smap_on()=0 的 uaccess 路径）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-ng-sys.ps1 -Cpu qemu64

# 已知限制：ring3 NULL 解引用 -> PANIC: vec=0xe
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-ng-sys.ps1 -Scenario crash
```

产物：`obj-ng/audit-sys/serial-boundary.log`（最近一次）、
`serial-boundary-qemu64.log`（降级 CPU）、`serial-crash.log`，控制台输出
`obj-ng/audit-sys/run*.out`。

两个已修缺陷的探针级复现（修复前失败、修复后为 PASS 用例）：

| 用例 | 修复前 | 修复后 |
|---|---|---|
| `fs2.trunc_emfile` / `fs2.trunc_preserved` | 16 个 fd 占满后 `open("/tmp/audit-trunc", O_RDWR\|O_TRUNC)` 返回 `-EMFILE`，但文件已被截断（size 0） | 返回 `-EMFILE` 且 size 仍是 5 |
| `fs2.create_emfile` / `fs2.create_absent` | fd 满时 `open("/tmp/audit-leak", O_CREAT\|O_RDWR)` 返回 `-EMFILE`，但文件被创建、残留 | 返回 `-EMFILE` 且路径 `ENOENT` |
| `fs2.create_emfile_fat` / `fs2.create_fat_absent` | 同上，FAT16 `/mnt/disk/AUDIT.LEK` 也残留 | 同上（覆盖 FAT16 的 create 回滚） |
| `fs2.ramfs_node_*` / `AUDIT: info ramfs headroom=N` | 启动已用 30/32 节点（`wp33` 证据 `nodes=30`），ring3 只剩 1~2 个 `O_CREAT` 余量 | 池扩到 64/128 后余量 **33**，脚本断言 `headroom >= 8` |

## 3. 修复 1：`vfs_open()` 失败仍有副作用（提交 `26f1b75`）

- 文件：`src/services/fs/vfs.c`、回归 `tests/unit-ng/test_vfs.c`。
- 根因：旧顺序是
  `resolve/create -> O_TRUNC -> retain -> fd_alloc`。
  - fd 表满（`fd_alloc` 返回 `EMFILE`）时，**截断已经发生**——失败的 open
    会销毁文件内容（`/bin/shell` 这类可写文件也在射程内）；
  - `O_CREAT` 已经建好节点，`fd_alloc` 失败后**没有回滚**，失败调用留下
    孤儿文件（ramfs 上还占一个节点）。
- 修复：先 `fd_alloc` 占 fd，再做 `O_TRUNC`/`retain`；
  - `fd_alloc` 失败：若本次是 `O_CREAT` 新建的节点，调用父目录 `unlink` 回滚；
  - 截断/`retain` 失败：新增 `fd_release_slot()` 归还 fd（generation+1，
    旧整数句柄失效）并按需 unlink 新建节点。
- 回归：`tests/unit-ng/test_vfs.c` 新增 `t_open_side_effects()`（fd 满时
  O_TRUNC 不截断、O_CREAT 不留文件；表清空后同样 open 成功）。
  `unit-ng`：**5315 checks / 0 failures**（+94）。
- 探针证据：`fs2.trunc_*`、`fs2.create_emfile*`、`fs2.create_fat_absent` 全 PASS。

## 4. 修复 2：ramfs 节点/目录项池过小（提交 `6e05740`）

- 文件：`src/services/fs/include/ramfs.h`、探针/脚本。
- 根因：`RAMFS_MAX_NODES=32` / `RAMFS_MAX_ENTS=64`，而启动内容
  （`/bin` 7 个 ELF、`/System/Packages`、`/run` 等）已用 **30 个节点**
  （`wp33` 启动行 `ng: session cleanup ok (live=0 nodes=30)`）。用户进程
  只能再建 1~2 个文件，之后 `O_CREAT` 直接 `ENOSPC`——审计里
  `open("/tmp/audit-leak", O_CREAT)` 在修复前就返回 `-ENOSPC` 而不是
  `-EMFILE`，暴露了池子耗尽。
- 修复：节点 32 -> 64、目录项 64 -> 128（静态表 +~20 KiB，可忽略）。
- 证据：探针创建到 `ENOSPC` 并打印余量，修复后
  `AUDIT: info ramfs headroom=33`；`tools/audit-ng-sys.ps1` 断言
  `headroom >= 8`。创建出的临时文件随后全部 unlink，不影响后续用例。
- 兼容性：`run-ng` 对 `nodes=` 只做正则匹配，`run-ng` 仍 PASS 159/159+75。

## 5. 审计工具/探针改动（提交 `b007c04`、`6e05740`）

| 路径 | 说明 |
|---|---|
| `tests/audit-ng/sys/audit_sys.c` | 216 -> **531** 用例；新增 marker 页边界、跨进程 IPC 唤醒/握手/EPERM、kill blocked sender/wait、self-kill（含 shm/fb 映射）、FAT16 读写/listdir/listdir 小缓冲、SVC 30..35、fd/线程/进程表耗尽、深队列、waiter 饱和、VFS/IPC 结构化 fuzz、退出时关端口唤醒 blocked sender、ramfs 余量与 `open` 失败副作用 |
| `tools/audit-ng-sys.ps1` | 探针编译改 `-Os`（`.bss` 必须留在 `0x40010000` 以下：内核在该 VA 映射每进程 marker 页，ELF 段越界会被覆盖，脚本原有 PT_LOAD 断言会在编译后立刻拦下）；新增 13 条证据断言（含 `headroom >= 8`、探针退出后的 panic 复查）；补 UTF-8 BOM（仓库 pre-commit 强制） |

> 说明：脚本里新增注释一律用 ASCII。Windows PowerShell 5.1 无 BOM 时按
> ANSI/GBK 解码，中文注释的尾字节可能吞掉换行，把下一行代码并进注释。

## 6. 未修 / 已知限制（本轮未改内核行为的部分）

1. **ring3 异常仍 panic**（WP1-3 起的已知限制）：用户程序 NULL 解引用 /
   非法指令 -> `PANIC: vec=...`，整机停机。`-Scenario crash` 持续按预期
   验证 `vec=0xe`。修复需要用户态故障投递（kill 进程 + 恢复内核），属 E3
   设计范围，不是低风险改动。
2. **VFS fd 表仍是全局一份**（`docs/claims/ng-WP3-1.md` 遗留）：进程 A 打开
   的 fd 数值可被进程 B close/read；需要 per-process fd 表才根治。审计里
   只用同进程 fd，未做破坏性验证。
3. **kill 阻塞在 IPC 中的进程会泄漏 syscall 里 kmalloc 的缓冲**：
   `ipc_sys_send/reply` 的 kbuf（≤4112 B）、`ipc_sys_recv` 的 kbuf
   （≤4112 B）在进程被 kill 时不会回收（线程不再恢复执行）。我们的
   `ipc2.ksend_*` 用例每 kill 一个 blocked sender 泄漏 4 KiB；没有 panic，
   属小泄漏，修法需要在 TCB 上登记"阻塞中的 syscall 缓冲"再在 kill 路径
   释放，暂不做。
4. **fd 与 ticket 共用 `(gen<<16)|slot` 整数编码**且 fd 表全局：同一个数值
   可能既是 fd 又是端口 ticket；`SYS_CLOSE(ticket)` 会关掉同数值的 fd。
   这是 ABI 语义歧义（不是内存安全问题），ABI 1.0 冻结后未改。
5. **没有 `SYS_SHM_FREE`**：shm 对象活到进程退出；探针用满 8 个对象后
   `SYS_SHM_ALLOC` 只能 `ENOSPC`（已按此语义断言）。
6. **ramfs/FAT16 固有上限**：ramfs 单文件 1 MiB、页池 2 MiB；FAT16 8.3 名字
   （扩展名 >3 字符返回 `ENAMETOOLONG`）。都是返回错误码，不是崩溃。
7. **`SYS_WAIT`/`SYS_SPAWN` 的 copyout 在预校验之后**，正常不可达失败；
   若未来页表可被第三方修改，失败会留下僵尸/孤儿子进程（当前协作式单核
   不会发生），记录备查。

## 7. 门禁证据（本轮结束）

```text
powershell -File tools/unit-ng.ps1     # PASS，5315 checks / 0 failures
powershell -File tools/build-ng.ps1    # PASS -> dist-ng/aster-ng.elf (2083056 B)
powershell -File tools/run-ng.ps1      # PASS，159/159 + 75 patterns
powershell -File tools/audit-ng-sys.ps1              # PASS，531/531，cpu max
powershell -File tools/audit-ng-sys.ps1 -Cpu qemu64  # PASS，531/531
powershell -File tools/audit-ng-sys.ps1 -Scenario crash  # PASS（vec=0xe，已知限制）
```

未修改：`src/desktop/**`（另一审计会话负责）、`sdk/**`、`build.ps1`、
`tools/ci.ps1`、`tools/ci-ng.ps1`。未 push。

## 8. 提交

| 提交 | 内容 |
|---|---|
| `b007c04` | `[ng][audit-sys]` 探针 216 -> 479 用例 + 脚本证据断言（第一段） |
| `26f1b75` | `[ng][audit-sys]` vfs：open 失败不截断/不留文件（含 host 回归） |
| `6e05740` | `[ng][audit-sys]` ramfs 池 32/64 -> 64/128；探针 531 用例 + headroom/副作用断言 |
