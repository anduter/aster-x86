# Aster SDK alpha（0.1.0）

> **状态：alpha。ABI 1.0 未冻结。** 这套头文件目前只覆盖 ng 分支已有的
> syscall / IPC / VFS / display 协议 v2；调用号、结构布局、错误码都可能在
> WP4-1 冻结前调整。跨内核版本请重新编译，不要假设二进制兼容。

这是 Aster ng 的**用户态 SDK 骨架**：不依赖 libc、不 include 任何内核头，
只用 `syscall` 指令进入内核。目标是把现在散在 `src/apps/shell`、
`src/apps/demo`、`src/desktop/shell` 里的手写封装收敛成一套可读、可扩展的
公开 API。

## 目录

```text
sdk/
├── README.md                     # 本文件
├── include/aster/
│   ├── types.h                   # u8..u64、size_t/ssize_t、bool、NULL、版本号
│   ├── syscall.h                 # 调用号镜像 + 内联 syscall + 进程/内存/键盘
│   ├── ipc.h                     # port/ticket/send/recv/reply/close + 16B 消息头
│   ├── fs.h                      # open/read/write/close/stat/listdir/mkdir/unlink
│   ├── display.h                 # display 协议 v2：CREATE/MAP/COMMIT/FOCUS/LIST
│   ├── draw.h                    # ARGB surface 的纯色/矩形/8x8 文字绘制
│   └── app.h                     # aster_app_main + 窗口创建/事件循环骨架
└── examples/
    └── hello_gui/main.c          # 320x200 窗口：画色块/文字、COMMIT、收键、退出
```

## 60 秒上手

从仓库根目录：

```powershell
# 1) 语法门禁（不需要 QEMU；clang 在 PATH 或 D:\msys64\mingw64\bin）
powershell -NoProfile -File tools/sdk-check.ps1

# 2) 写一个最小应用：实现 aster_app_main，include <aster/app.h>
#    完整示例：sdk/examples/hello_gui/main.c

# 3) 交叉编译（x86_64-elf，无 libc、无 SSE）
New-Item -ItemType Directory -Force obj\sdk | Out-Null
clang --target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone `
  -fno-stack-protector -mno-sse -mgeneral-regs-only -fno-builtin `
  -fno-pic -fno-pie -std=c11 -Os -Wall -Wextra -Werror `
  -I sdk/include -c sdk/examples/hello_gui/main.c -o obj/sdk/hello_gui.o
clang --target=x86_64-elf -fuse-ld=lld -nostdlib `
  -Wl,-T,src/ng/user/user.ld -Wl,--build-id=none -Wl,--gc-sections `
  -o obj/sdk/hello_gui.elf obj/sdk/hello_gui.o
```

部署目录、manifest、运行命令和 alpha 阶段的限制见
[docs/sdk/getting-started.md](../docs/sdk/getting-started.md)。

## API 一览

| 头文件 | 主要 API | 对应内核/协议 |
| --- | --- | --- |
| `types.h` | `u8..u64`、`i8..i64`、`usize`、`size_t`、`bool`、`NULL`、`ASTER_SDK_VERSION` | `src/ng/types.h` |
| `syscall.h` | `aster_syscall0..4`、`aster_puts`、`aster_exit`、`aster_getpid`、`aster_spawn/wait/kill`、`aster_shm_alloc/map`、`aster_readkey` | `src/ng/syscall.h` 0/1、30、40–47 |
| `ipc.h` | `aster_port_create/send/recv/reply/close`、`aster_ipc_send/recv`、`struct aster_ipc_hdr` | `src/ng/ipc.h` 10–14 |
| `fs.h` | `aster_open/read/write/close/stat/listdir/mkdir/unlink`、`struct aster_stat/dirent` | `src/services/fs/include/vfs.h` 20–27 |
| `display.h` | `aster_display_connect`、`aster_display_create(_mapped)`、`aster_display_map`、`aster_display_commit`、`aster_display_focus`、`aster_display_list`、`aster_display_destroy`、`aster_display_poll_input`、`aster_display_wait_input` | `src/desktop/include/ds_protocol.h` v2 |
| `draw.h` | `aster_surface_clear`、`aster_draw_pixel/fill/rect/char/text`、`ASTER_RGB` | 纯用户态（8x8 字模同 desktop） |
| `app.h` | `aster_app_main`、`aster_app_open`、`aster_app_poll`、`aster_app_commit`、`aster_app_close`、`_start` 默认入口 | 以上组合 |

## display 协议版本策略

- 本 SDK **发送 v2**：CREATE body 40 字节、FOCUS 的第二个 u32 为 0、
  LIST_REPLY 按 v2 的 44 字节 entry 理解。
- 解码器**接受 version 1..3**：当前 displayd 的编码器写的是它自己的原生
  版本（v3）。v3 只在 CREATE/LIST_REPLY 里追加 `token`，所以 SDK 按消息自带
  的 version 选择 entry stride（44/48），v2 客户端可以跑在 v3 displayd 上。
  `aster_display_entry.token` 在 v2 回复里为 0。
- 所有常量/结构以 `src/desktop/include/ds_protocol.h` 为准；本 SDK 不
  include 它，而是逐项镜像（`_Static_assert` 锁住 body 大小）。

## 与内核 API 的已知差异 / 待办

- **ABI 未冻结**：调用号/结构可能变。`sdk-check` 只验证语法，不做编号比对。
- **header-only**：没有 `.c` 运行时；`aster_memcopy` 等是内联循环。链接时
  不应出现 `memcpy/memset` 依赖（示例的 freestanding 链接已验证无未定义符号）。
- **alpha 限制**：一个进程一个窗口；没有装饰/拖动/缩放（WM 的职责）；没有
  alpha 混合；没有定时器；没有多显示器。
- **surface 生命周期**：没有 `SYS_SHM_FREE`；DESTROY 后映射要等进程退出才
  释放（见 `src/ng/shm.h`）。不要在一个长生命周期进程里反复 CREATE 大 surface。
- **部署**：alpha 还没有用户态安装器。`/System/Packages` 目前由内核启动时
  seed 内置包；真正把新 ELF 落盘要等 WP3-4（或构建期嵌入）。见 getting-started。
- **输入**：只有键盘（PS/2 → displayd INPUT），没有鼠标事件。
- **错误处理**：SDK 返回负 errno；不会 panic。协议消息在 CREATE 等待期间
  到达的 INPUT 会被丢弃（alpha 简化），先 CREATE 再 FOCUS/收键即可。

## 自检

```powershell
powershell -NoProfile -File tools/sdk-check.ps1
```

对 `sdk/include/aster/*.h` 与 `sdk/examples/**/*.c` 逐个跑
`clang -fsyntax-only -std=c11 -I sdk/include -Wall -Wextra -Werror`，
失败返回 1。它不启动 QEMU、不写构建目录，可以和内核构建并行跑。
