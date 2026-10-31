# Aster SDK alpha（0.1.0）

> **状态：alpha。ABI 1.0 未冻结。** 这套头文件覆盖 ng 分支已有的
> syscall / IPC / VFS / display 协议 **v3** / session 通道（会话协议 v1）；
> 调用号、结构布局、错误码都可能在 WP4-1 冻结前调整。跨内核版本请重新编译，
> 不要假设二进制兼容。

这是 Aster ng 的**用户态 SDK**：不依赖 libc、不 include 任何内核头，只用
`syscall` 指令进入内核。目标是把散在 `src/apps/shell`、`src/apps/demo`、
`src/desktop/shell` 里的手写封装收敛成一套可读、可扩展的公开 API。

## 目录

```text
sdk/
├── README.md                     # 本文件
├── include/aster/
│   ├── types.h                   # u8..u64、size_t/ssize_t、bool、NULL、版本号
│   ├── syscall.h                 # 调用号镜像 + 内联 syscall + 进程/内存/键盘
│   ├── ipc.h                     # port/ticket/send/recv/reply/close + 16B 消息头
│   ├── fs.h                      # open/read/write/close/stat/listdir/mkdir/unlink
│   ├── display.h                 # display 协议 v3：CREATE/LIST/FOCUS 带 token
│   ├── session.h                 # 会话通道：argv session=/token=、REGISTER/LAUNCH/LOOKUP/LIST
│   ├── draw.h                    # ARGB surface 的纯色/矩形/8x8 文字绘制
│   └── app.h                     # aster_app_main + 窗口创建/事件循环骨架
└── examples/
    └── hello_gui/main.c          # v3 + 会话通道：320x200 窗口、token 回读、常驻/交互两角色
```

## 60 秒上手

从仓库根目录：

```powershell
# 1) 语法 + SDK 宿主单测（不需要 QEMU；clang 在 PATH 或 D:\msys64\mingw64\bin）
powershell -ExecutionPolicy Bypass -File tools/sdk-check.ps1

# 2) 在 QEMU 里真跑示例（SdkDemo 内核：sessiond/WM 拉起 SDK hello_gui）
powershell -ExecutionPolicy Bypass -File tools/sdk-demo.ps1

# 3) 写一个最小应用：实现 aster_app_main，include <aster/app.h> + <aster/session.h>
#    完整示例：sdk/examples/hello_gui/main.c
```

会话通道的最小用法（应用被 sessiond 启动时 argv 会带
`session=<ticket> token=0x<role_token>`）：

```c
#include <aster/app.h>
#include <aster/session.h>

ASTER_APP_MAIN
{
    struct aster_app app;
    struct aster_app_config cfg = {
        .width = 320, .height = 200, .title = "Hello",
    };
    struct aster_session sess;
    i32 rc = aster_session_connect(&sess, argc, argv);

    if (rc == 0) {
        cfg.token = sess.token;            /* CREATE/FOCUS 都用它 */
    } else if (rc != ASTER_SESSION_ENOENT) {
        return 1;                          /* argv 格式错/端口失败 */
    }
    if (aster_app_open(&app, &cfg) < 0) return 1;
    (void)aster_app_focus(&app);
    if (rc == 0) {
        (void)aster_session_register(&sess, app.surface.id, "Hello");
    }
    /* ... 画 + commit + 事件循环，每 ~200ms heartbeat ... */
    aster_app_close(&app);
    if (rc == 0) { (void)aster_session_exit(&sess, 0); aster_session_close(&sess); }
    return 0;
}
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
| `display.h` | `aster_display_connect`、`aster_display_create(_mapped)`、`aster_display_map`、`aster_display_commit`、`aster_display_focus(_ex)`、`aster_display_list`、`aster_display_destroy`、`aster_display_poll_input`、`aster_display_wait_input` | `src/desktop/include/ds_protocol.h` **v3** |
| `session.h` | `aster_session_connect`、`session_register/heartbeat/exit`、`session_launch/kill`、`session_lookup/list`、`aster_session_token_*`、`aster_session_parse_args` | `src/desktop/include/session_protocol.h` v1 / L2c |
| `draw.h` | `aster_surface_clear`、`aster_draw_pixel/fill/rect/char/text`、`ASTER_RGB` | 纯用户态（8x8 字模同 desktop） |
| `app.h` | `aster_app_main`、`aster_app_open`、`aster_app_focus`、`aster_app_poll`、`aster_app_commit`、`aster_app_close`、`_start` 默认入口 | 以上组合 |

## display 协议版本策略（v3）

- 本 SDK **发送 v3**：CREATE body 44（末尾 `token`）、FOCUS 的第二个 u32 是
  `token`、LIST_REPLY 按 v3 的 48 字节 entry 编码。
- 解码器**接受 version 1..3**：v1 CREATE 16 字节、v2 CREATE 40 字节（token=0），
  v2 LIST_REPLY 仍按 44 字节 entry 解析（`aster_display_entry.token` 为 0）。
  SDK 按消息自带的 version 选择 body/stride。
- `token` 来自 session manager：sessiond 经 argv 发给应用，应用填进
  CREATE / FOCUS；displayd 在 v3 下校验 FOCUS 的令牌（surface 自己的令牌或
  WM 令牌），越权 FOCUS 会被拒绝。没有会话通道时 token=0，displayd 走
  v1/v2 兼容路径（不校验归属）。
- 所有常量/结构以 `src/desktop/include/ds_protocol.h` 与
  `src/desktop/include/session_protocol.h` 为准；SDK 不 include 它们，而是
  逐项镜像（`_Static_assert` 锁住 body 大小）。

## 会话通道（v1）要点

| 消息 | 方向 | body | 用途 |
| --- | --- | --- | --- |
| `REGISTER` | app/wm→sessiond | 28 | 上报窗口（token/port/surface/title） |
| `HEARTBEAT` | app/wm→sessiond | 8 | ~200ms 存活心跳（800ms 过期回收） |
| `LAUNCH` | **wm→sessiond** | 84 | 启动应用（应用令牌会被 EPERM 拒绝） |
| `EXIT` / `KILL` | app/wm→sessiond | 8 / 12 | 正常退出 / 会话内终止 |
| `LOOKUP` / `ENTRY` | app↔sessiond | 8 / 48 | 按 surface/pid 查注册表 |
| `LIST` / `LIST_REPLY` | app↔sessiond | 4 / 336 | 注册表快照 |

`aster_session_connect(&s, argc, argv)` 解析 `session=<十进制>` /
`token=<0x十六进制|十进制>`，并为会话**单独建一个回复端口**（与 display 的
客户端端口分开，INPUT 不会和 ENTRY/LIST_REPLY 串线）。没有 `session=` 时
返回 `-ENOENT`（`-2`），应用可以按无会话模式继续跑。

## 与内核 API 的已知差异 / 待办

- **ABI 未冻结**：调用号/结构可能变。`sdk-check` 只验证 SDK 自身语法与纯
  函数，不做编号比对；升级内核后请重读本文件并重跑门禁。
- **header-only**：没有 `.c` 运行时；`aster_memcopy` 等是内联循环。链接时
  不应出现 `memcpy/memset` 依赖（示例的 freestanding 链接已验证）。
- **alpha 限制**：一个进程一个窗口；没有装饰/拖动/缩放（WM 的职责）；没有
  alpha 混合；没有定时器；没有多显示器；没有鼠标事件。
- **surface 生命周期**：没有 `SYS_SHM_FREE`；DESTROY 后映射要等进程退出才
  释放（见 `src/ng/shm.h`）。不要在一个长生命周期进程里反复 CREATE 大 surface。
- **部署**：alpha 还没有用户态安装器。`/System/Packages` 目前由内核启动时
  seed 内置包；把新 ELF 放进镜像仍要构建期 `.incbin`（见 getting-started
  第 4 节与 `tools/sdk-demo.ps1` 的做法）。
- **会话发现只走 argv**：手动从 shell `spawn` 的 GUI 应用拿不到会话通道
  （token=0）；被 sessiond/WM 启动的应用才能拿到 role_token。
- **错误处理**：SDK 返回负 errno；不会 panic。协议消息在等待回复期间到达的
  其它消息（例如 INPUT）会被丢弃（alpha 简化）。

## 自检

```powershell
powershell -ExecutionPolicy Bypass -File tools/sdk-check.ps1
```

对 `sdk/include/aster/*.h` 与 `sdk/examples/**/*.c` 逐个跑
`clang -fsyntax-only -std=c11 -I sdk/include -Wall -Wextra -Werror`，然后用
宿主 clang 编译运行 `tests/unit-ng/test_sdk.c`（display v3 / session 协议 /
argv 解析的纯函数用例，要求 ≥30 checks）。失败返回 1。

另外 `tools/sdk-demo.ps1` 会构建 `-SdkDemo` 内核并在 QEMU 里由
sessiond/WM 拉起 `sdk/examples/hello_gui`，断言会话/窗口/令牌证据行与
screendump 像素（详见脚本头注释与 `docs/claims/ng-WP3-4.md`）。
