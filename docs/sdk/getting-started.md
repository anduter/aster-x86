# Aster SDK alpha：从零写一个 GUI 应用（display v3 + 会话通道）

> 适用于 `ng` 分支的多进程 + display 协议 **v3** + session manager（L2c）
> 基线（WP3-4）。
> **alpha：ABI 1.0 未冻结**，调用号/结构可能在 WP4-1 前调整；升级内核后请
> 重新编译并重跑 `tools/sdk-check.ps1`。

本文的命令默认在**仓库根目录**执行（`D:\OS_Project`）。示例完整代码在
`sdk/examples/hello_gui/main.c`，SDK 概览见 `sdk/README.md`。

## 0. 前置条件

- clang（MSYS2 `D:\msys64\mingw64\bin\clang.exe`）与 lld：
  `clang --version` 能跑通；
- 内核侧有 display server（`/bin/displayd`）、WM（`/bin/wm`）和 session
  manager（`/bin/sessiond`）。正常 `tools/run-ng.ps1` 启动的镜像会由 init
  拉起 sessiond，再由它拉起 displayd + wm；
- 想在 QEMU 里验证一个**由会话启动**的 GUI 应用，用
  `tools/sdk-demo.ps1`（见第 5 节），它不需要额外安装器。

## 1. 新建应用目录

```text
sdk/examples/hello_gui/
└── main.c
```

一个应用至少包含：

1. `#include <aster/app.h>` 与（可选）`#include <aster/session.h>`；
2. `int aster_app_main(long argc, char **argv)`（SDK 提供的 `_start` 会调它）；
3. `aster_session_connect(&sess, argc, argv)` 解析会话通道（没有会话返回
   `-ENOENT`，应用仍可直连跑）；
4. `aster_app_open()` 建窗口（`cfg.token` = 会话令牌），往
   `app.surface.pixels` 画；
5. `aster_app_focus()`、`aster_app_poll()`、`aster_app_commit()`；
6. 有会话时 `aster_session_register()`、每 ~200ms
   `aster_session_heartbeat()`、退出前 `aster_session_exit()`；
7. `aster_app_close()` 收尾。

## 2. 最小应用骨架（v3 + 会话）

```c
#include <aster/app.h>
#include <aster/session.h>

ASTER_APP_MAIN
{
    struct aster_app app;
    struct aster_app_config cfg = {
        .width = 320,
        .height = 200,
        .title = "Hello GUI",
        .flags = 0,   /* 0 = displayd 级联放置；或 ASTER_DISPLAY_CREATE_F_POS */
        .x = 0,
        .y = 0,
        .token = 0,   /* 下面由会话令牌覆盖 */
    };
    struct aster_event ev;
    struct aster_session sess;
    int have_session = 0;
    u32 round = 0;
    i32 rc;

    /* sessiond 启动应用时 argv 带 `session=<ticket> token=0x<hex>`。 */
    rc = aster_session_connect(&sess, argc, argv);
    if (rc == 0) {
        have_session = 1;
        cfg.token = sess.token;
    } else if (rc != ASTER_SESSION_ENOENT) {
        (void)aster_puts("hello: bad session args\n");
        return 1;
    }

    rc = aster_app_open(&app, &cfg);
    if (rc < 0) {
        (void)aster_puts("hello: displayd not available\n");
        return 1;
    }
    (void)aster_app_focus(&app);

    aster_surface_clear(&app.surface, ASTER_RGB(0x10, 0x18, 0x20));
    aster_draw_text(&app.surface, 16, 16, "Hello, Aster!",
                    ASTER_RGB(0xF0, 0xF4, 0xFF),
                    ASTER_RGB(0x10, 0x18, 0x20));
    (void)aster_app_commit(&app);

    /* 有会话：登记窗口 + 每 10 轮（~200ms）心跳。 */
    if (have_session) {
        (void)aster_session_register(&sess, app.surface.id, "Hello GUI");
    }

    while (aster_app_running(&app)) {
        rc = aster_app_poll(&app, &ev, ASTER_DISPLAY_POLL_TICKS);
        if (rc < 0) {
            break;                 /* 端口关闭 / 协议错误 */
        }
        round++;
        if (have_session && (round % ASTER_SESSION_HEARTBEAT_ROUNDS) == 0) {
            (void)aster_session_heartbeat(&sess, round);
        }
        if (rc == 0) {
            continue;              /* 20ms 内没有输入 */
        }
        if (ev.type == ASTER_EVENT_KEY_DOWN &&
            (ev.key == 27u || ev.key == (u32)'q')) {
            aster_app_quit(&app);
        }
    }

    if (have_session) {
        (void)aster_session_exit(&sess, 0);
    }
    aster_app_close(&app);
    if (have_session) {
        aster_session_close(&sess);
    }
    return 0;
}
```

### 窗口与绘制要点

- 尺寸上限 1280×800；像素格式 ARGB8888，`pitch_px == width`。
- `aster_draw_*` 不做 alpha 混合，颜色按原值覆盖；实心色用 `ASTER_RGB()`
  （自动 `A=0xFF`）。
- 8×8 字模只覆盖 ASCII `0x20..0x7E`；`aster_draw_text()` 不处理 `\n`。
- 不要自己 `free()` 像素内存：surface 来自 `SYS_SHM_MAP`，进程退出时内核回收。
- 大块重画后只提交脏矩形可减少合成开销：
  `aster_display_commit(&app.display, &app.surface, x, y, w, h)`。
- `aster_app_focus()` 用 `cfg.token` 发 v3 FOCUS；应用自己只能 FOCUS 自己的
  窗口，切别人的窗口需要 WM 令牌（displayd 会拒绝越权请求）。

### 事件要点

- `aster_app_poll()` 返回 `1` 事件 / `0` 超时 / 负 errno。
- `ev.key` 是不可打印为 0 的 ASCII；`ev.scancode` 是 PS/2 扫描码；
  `ev.flags & ASTER_INPUT_F_GLOBAL` 表示这是一份全局热键副本（WM 才会收到，
  普通应用通常没有）。
- `ASTER_DISPLAY_POLL_TICKS` 是 20e6 TSC tick（QEMU TCG 下约 20ms）。
- 没有定时器 syscall；用轮询超时累计实现“空闲 N 轮后退出”。

## 3. 编译

### 3.1 先跑 SDK 门禁

```powershell
powershell -ExecutionPolicy Bypass -File tools/sdk-check.ps1
```

期望输出 `sdk-check: PASS (n files + m SDK host checks)` 且退出码 0。它做
两件事：对头/示例跑 `-fsyntax-only`，再用**宿主** clang 编译运行
`tests/unit-ng/test_sdk.c`（协议尺寸/编解码/argv 解析的纯函数用例）。
不启动 QEMU、不写 `obj-ng/`（测试 exe 只写到系统临时目录）。

### 3.2 交叉编译成 ELF64（无 libc / 无 SSE）

```powershell
New-Item -ItemType Directory -Force .obj-sdk | Out-Null

clang --target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone `
  -fno-stack-protector -fcf-protection=none -mno-sse -mgeneral-regs-only `
  -fno-builtin -fno-pic -fno-pie -std=c11 -Os -Wall -Wextra -Werror `
  -ffunction-sections -fdata-sections `
  -I sdk/include -c sdk/examples/hello_gui/main.c -o .obj-sdk/hello_gui.o

clang --target=x86_64-elf -fuse-ld=lld -nostdlib `
  -Wl,-T,src/ng/user/user.ld -Wl,--build-id=none -Wl,--gc-sections `
  -o .obj-sdk/hello_gui.elf .obj-sdk/hello_gui.o
```

- `src/ng/user/user.ld` 把镜像链到 `0x40000000`，段间 4 KiB 对齐；这是
  ng ELF64 加载器的约定（`docs/design` 里的进程地址空间布局）。
- `-mno-sse -mgeneral-regs-only` 与内核用户程序构建选项一致；SDK 不要求
  FPU/SIMD。
- 如果链接报 `undefined symbol: _start`：确认 include 了 `<aster/app.h>`
  且没有定义 `ASTER_NO_DEFAULT_ENTRY`；如果自己写了 `_start`，请定义该宏。

检查产物：

```powershell
readelf -h .obj-sdk/hello_gui.elf | Select-String 'Type|Machine|Entry'
# 期望：EXEC / Advanced Micro Devices X86-64 / 0x40000000 附近
```

## 4. 部署到 `/System/Packages`（alpha 的现状）

### 4.1 目标布局

内核的包目录约定是 `/System/Packages/<name>/<version>/`，manifest 里的
`entry` 相对包版本目录：

```text
/System/Packages/hello_gui/
└── 1.0.0/
    ├── manifest
    └── bin/
        └── hello_gui      # 就是交叉编译出来的 ELF
```

manifest（与 `src/services/pkg/pkg.c` 的 seed 格式一致）：

```text
name=hello_gui
version=1.0.0
type=app
entry=bin/hello_gui
sha256=<ELF 的 SHA-256 十六进制>
```

staging 树（安装器将来消费的目录结构）：

```powershell
$pkg = '.obj-sdk/pkg/hello_gui/1.0.0'
New-Item -ItemType Directory -Force "$pkg/bin" | Out-Null
Copy-Item .obj-sdk/hello_gui.elf "$pkg/bin/hello_gui"

$sha = (Get-FileHash "$pkg/bin/hello_gui" -Algorithm SHA256).Hash.ToLower()
@"
name=hello_gui
version=1.0.0
type=app
entry=bin/hello_gui
sha256=$sha
"@ | Set-Content -Encoding ascii "$pkg/manifest"
```

### 4.2 alpha 阶段的现实：还没有用户态安装器

- 当前内核只在**启动时**由 `pkg_init()` seed 内置包（模板是
  `/System/Packages/hello/1.0.0`），**没有** `pkg install` 命令把宿主机文件
  拷进 ramfs；
- `/bin` 下的内置程序（shell/demo/wm/...）是构建期 `.incbin` 进内核
  `.bigdata` 再由 `proc_init()` 种到 ramfs 的（见 `tools/build-ng.ps1` 的
  `$desktopPrograms` 表）。

所以目前有三种**真实可用**的落盘路径（都不需要改 SDK 头）：

1. **SdkDemo 构建（WP3-4 的 QEMU 出口，最容易）**：
   `tools/build-ng.ps1 -SdkDemo` 把 `sdk/examples/hello_gui/main.c` 编成
   `/bin/demo` 的 blob（符号 `ng_demo_elf` 不变），于是 init → sessiond →
   WM 的**产品启动链**会自然地由 sessiond 拉起 SDK 应用并签发 role_token。
   `tools/sdk-demo.ps1` 把这条路径完整自动化（见第 5 节）；
2. **构建期 seed 到 `/bin`（正式内置应用）**：按 `proc_init()` 的现有模式把
   ELF 嵌进内核镜像（需要改内核构建/`src/ng/proc.c`，不属于 `sdk/`）；
3. **运行期安装（后续 WP）**：安装器读取 staging 树，校验 `sha256` 后落到
   `/System/Packages/...`，再用 sessiond 的 LAUNCH 启动。

在安装器落地前，不要指望在 shell 里直接 `cp hello_gui /System/Packages/...`：
shell 没有 `cp`，ramfs 也没有宿主目录映射。

## 5. 运行

### 5.1 一条命令在 QEMU 里真跑（推荐）

```powershell
powershell -ExecutionPolicy Bypass -File tools/sdk-demo.ps1
```

它做：

1. `build-ng.ps1 -SdkDemo -DistDir .dist-sdk -ObjDir .obj-sdk`（不碰默认
   `dist-ng/`，也不新增 `/bin` 节点）；
2. 启动 QEMU（stdvga + QMP），按内核自检节奏注入按键；
3. 等 SDK 示例打印
   `ng: sdk session ok (...)` / `ng: sdk token ok (...)` /
   `ng: sdk demo ok (...)`，并等 sessiond 的
   `ng: session window ok (... title=Aster SDK)` 与 WM 任务表的
   `ng: wm task ok (... title=Aster SDK)`；
4. QMP `stop` 冻结画面后 screendump，断言面板 `(4,4)=#202838` 与 SDK 窗口
   内 `(x+40,y+80)=#3070c0`；
5. 打印 `sdk-demo: PASS (...)`，退出码 0。

如果只想复用上次构建：加 `-NoBuild`；保留日志/截图：加 `-KeepLog`。

### 5.2 在镜像的 shell 里手动 spawn

shell 的 `spawn`（syscall 40）从 VFS 读 ELF。如果构建期把 ELF 放进了
`/bin`：

```text
spawn /bin/hello_gui
```

注意：**手动 spawn 没有会话通道**（没有 `session=`/`token=` 参数），应用会
走 `-ENOENT` 分支，token=0。窗口仍会显示，但不进 session manager 的注册表、
也没有 heartbeat 保护；被杀后不会被会话回收。要验证完整的会话生命周期，
请用 5.1 的 SdkDemo 路径（sessiond + WM + LAUNCH）。

### 5.3 SDK 示例的 demo 角色

`sdk/examples/hello_gui` 除了正常的交互角色，还保留两个 **demo harness**
角色（SdkDemo 构建里该二进制占据 `/bin/demo`，要配合内核自检）：

| argv[1] | 行为 |
| --- | --- |
| 无 / 其它 | 交互窗口：320×200、画强调色、q/Esc 退出、空闲 5s 自动退出 |
| `window` | 常驻窗口（~30s 上限）：WM/会话自检用，不自动退出 |
| `hold` | 只建端口、不建 surface/SHM，约 2s 后退出：WP3-1 的“displayd 被杀，客户端自己干净退出”用例 |

普通应用只需要交互角色；`window`/`hold` 的存在是为了让同一个 ELF 能替换
`/bin/demo` 跑完整的内核自检链（见 `docs/claims/ng-WP3-4.md`）。

## 6. 常见错误

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| `aster_session_connect` 返回 `-2` (`ENOENT`) | 没有 `session=` 参数（shell 直连 spawn） | 正常；用 token=0 继续，或改用 SdkDemo/sessiond 路径 |
| `aster_session_connect` 返回 `-22` (`EINVAL`) | `session=`/`token=` 格式错（空值、坏字符、溢出） | 检查启动方传的 argv；SDK 只接受十进制与 `0x` 十六进制 |
| `aster_app_open` 返回 `-2` (`ENOENT`) | displayd 没起，或 `/run/display.port` 不存在 | 用正常镜像启动；`cat /run/display.port` 应看到十进制 ticket |
| 返回 `-22` (`EINVAL`) | 宽/高为 0 或超过 1280×800；协议字段非法 | 检查 `cfg.width/height/flags` |
| 返回 `-12` (`ENOMEM`) | surface 页数超过 1024（4 MiB） | 减小窗口 |
| 返回 `-110` (`ETIMEDOUT`) | CREATE/LIST/ENTRY 等回复超时（displayd/sessiond 忙/死） | 看串口 `ng: displayd ...` / `ng: sessiond ...` 证据行；重启镜像 |
| `aster_session_list/lookup` 返回 `-1` (`EPERM`) | 令牌角色不够（例如 APP 发 LAUNCH/KILL） | 只有 WM 令牌能 LAUNCH/KILL；应用只 REGISTER/HEARTBEAT |
| 提交后屏幕没变化 | 忘了 `aster_app_commit()`，或 COMMIT 前没 MAP | `aster_app_open` 已自动 MAP；检查 commit 返回值 |
| 链接出现 `memcpy`/`memset` 未定义 | 应用自己用了 libc 或结构体大赋值 | SDK 本身是内联循环；用 `aster_memcopy/aster_memzero` |
| 编译报 `syscall`/寄存器错误 | 用了非 x86_64 目标 | 必须 `--target=x86_64-elf` |

## 7. ABI / 兼容性说明（重要）

- 调用号镜像自 `src/ng/syscall.h`；结构布局镜像自
  `src/services/fs/include/vfs.h`、`src/desktop/include/ds_protocol.h`（v3）与
  `src/desktop/include/session_protocol.h`（v1）。SDK **不 include** 这些
  内核头，所以两边是“手工同步”的关系；
- `tools/sdk-check.ps1` 保证 SDK 自身语法/类型/纯函数一致，并用
  `_Static_assert` 锁住 body 大小；**不检查**调用号是否与内核同步。改
  syscall/协议时请同时改 `sdk/include/aster/*.h` 并跑门禁；
- display 协议：SDK 发 v3，解码 1..3；会话协议：v1，解码严格校验版本/长度；
- ABI 1.0 在 WP4-1 冻结前，不保证跨内核版本的二进制兼容。发布应用时请记录
  所依赖的内核提交（`git rev-parse --short HEAD`）与 SDK 版本
  （`ASTER_SDK_VERSION`）。

## 8. 下一步（WP4-1 及之后）

- WP4-1：冻结 syscall 编号、display v3 与会话协议结构，SDK 增加版本协商与
  兼容性矩阵；
- 用户态安装器 / `pkg install`：把第 4 节的 staging 树接上，替换构建期
  seed；
- 后续 SDK 能力：多窗口/装饰 API、鼠标事件、alpha 合成、定时器、共享内存
  显式释放、文本多行/裁剪、包启动（sessiond 的 LAUNCH 直接接
  `/System/Packages`）。
