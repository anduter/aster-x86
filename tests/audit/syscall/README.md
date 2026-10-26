# tests/audit/syscall - 用户→内核边界的 ring3 探测

本目录是 `audit-syscall` 会话的最小复现/回归驱动说明；可执行逻辑在
`tools/audit-syscall.ps1`，ring3 入口是 shell 的调试命令 `sys`。

## 为什么不用独立 ELF

`user_load_program()` 对任何 `exec` 的镜像都调用 `trust_verify_shell()`
（`security/trust.c`），而内核里嵌的是**构建时 shell ELF 的签名/模数**，
不是通用公钥。因此自己编译的 ring3 ELF 无法被 `exec`（会被
`trust: rejected unsigned image` 拒绝）。本会话按任务允许的方式，把探针做成
`src/user/user_shell.c` 的内置命令：

```
sys <nr> [a] [b] [c]      # 发一条 legacy int 0x81，参数为十进制
```

它打印 `sys: nr=.. a=.. b=.. c=.. ret=..`，不新增内核入口、不绕过沙箱
（shell 本来就有 ring3 权限）。

## 用例

驱动脚本把多行命令拼成一次 `-Keys` 注入，然后按出现顺序解析串口里的
`sys:` 行。地址一律用十进制（QMP `sendkey` 对大写十六进制字符不可靠）：

| # | 命令 | 期望（修复后） | 覆盖 |
|---|------|----------------|------|
| 1 | `sys 48 4096 3 0` | ret=1077936128（0x40400000） | SYS_MMAP 正常路径 |
| 2 | `sys 49 1077936128 4096 0` | ret=0 | SYS_MUNMAP 正常路径 |
| 3 | `sys 22 0 0 0` | 0x40200000..0x40300000 | SYS_BRK 查询 |
| 4 | `sys 22 3221225472 0 0` | ret=-1 | SYS_BRK 32 位回绕 |
| 5 | `sys 1 1048576 8 0` | ret=-1 | SYS_WRITE 内核源 |
| 6 | `sys 40 2097152 0 0` | ret=-1 | SYS_RESINFO 内核目标 |
| 7 | `sys 37 8388608 0 0` | ret=-1 | SYS_MOUSE 内核目标 |
| 8 | `sys 35 0 2097152 0` | ret=-1 | SYS_PCI 内核目标 |
| 9 | `sys 39 0 2097152 0` | ret=-1 | SYS_ETH 内核目标 |
| 10 | `sys 57 0 2097152 0` | ret=-1 | SYS_BLK_READ 内核目标 |
| 11 | `res` | `res: mem free=...` | 校验器不误伤合法用户缓冲 |
| 12 | `mouse` | `mouse: x=... y=...` | 同上 |

特权用例（`-Suite priv`）：

```
drop 1000          # 非 root
sys 28 0 0 0       # SYS_PANIC   -> -1（修复前：KERNEL PANIC）
sys 27 0 0 0       # SYS_REBOOT  -> -1
sys 36 0 0 0       # SYS_POWEROFF-> -1
sys 38 0 0 0       # SYS_HANG    -> -1
```

## 运行

```powershell
# 隔离端口/日志；12345 必须空闲
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite guard
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite priv
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite full
# 修复前的复现（信息性；在 vulnerable 内核上跑）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1 -Suite probe
```

驱动在共享工作树被并行会话的未提交改动打断时，可用 `-BuildRoot <HEAD 快照>`
在干净快照上构建。串口日志写到 `obj/audit/syscall-<suite>.log`。
