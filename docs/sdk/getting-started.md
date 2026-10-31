# Aster SDK alpha：从零写一个 GUI 应用

> 适用于 `ng` 分支的多进程 + display 协议 v2 基线。
> **alpha：ABI 1.0 未冻结**，调用号/结构可能在 WP4-1 前调整；升级内核后请
> 重新编译并重跑 `tools/sdk-check.ps1`。

本文的命令默认在**仓库根目录**执行（`D:\OS_Project`）。示例完整代码在
`sdk/examples/hello_gui/main.c`，SDK 概览见 `sdk/README.md`。

## 0. 前置条件

- clang（MSYS2 `D:\msys64\mingw64\bin\clang.exe`）与 lld：
  `clang --version` 能跑通；
- 内核侧已有 display server（`/bin/displayd`）与 WM（`/bin/wm`）。正常
  `tools/run-ng.ps1` 启动的镜像会拉起它们；本文不涉及改内核/构建脚本。

## 1. 新建应用目录

```text
sdk/examples/hello_gui/
└── main.c
```

一个应用至少包含：

1. `#include <aster/app.h>`；
2. `int aster_app_main(long argc, char **argv)`（SDK 提供的 `_start` 会调它）；
3. `aster_app_open()` 建窗口，往 `app.surface.pixels` 画；
4. `aster_app_poll()` 收键盘事件，`aster_app_commit()` 提交；
5. `aster_app_close()` 收尾。

## 2. 最小应用骨架

```c
#include <aster/app.h>

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
    };
    struct aster_event ev;
    i32 rc;

    (void)argc;
    (void)argv;

    rc = aster_app_open(&app, &cfg);
    if (rc < 0) {
        (void)aster_puts("hello: displayd not available\n");
        return 1;
    }

    aster_surface_clear(&app.surface, ASTER_RGB(0x10, 0x18, 0x20));
    aster_draw_text(&app.surface, 16, 16, "Hello, Aster!",
                    ASTER_RGB(0xF0, 0xF4, 0xFF),
                    ASTER_RGB(0x10, 0x18, 0x20));
    (void)aster_app_commit(&app);

    while (aster_app_running(&app)) {
        rc = aster_app_poll(&app, &ev, ASTER_DISPLAY_POLL_TICKS);
        if (rc < 0) {
            break;                 /* 端口关闭 / 协议错误 */
        }
        if (rc == 0) {
            continue;              /* 20ms 内没有输入 */
        }
        if (ev.type == ASTER_EVENT_KEY_DOWN &&
            (ev.key == 27u || ev.key == (u32)'q')) {
            aster_app_quit(&app);
        }
    }

    aster_app_close(&app);
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

### 事件要点

- `aster_app_poll()` 返回 `1` 事件 / `0` 超时 / 负 errno。
- `ev.key` 是不可打印为 0 的 ASCII；`ev.scancode` 是 PS/2 扫描码；
  `ev.flags & ASTER_INPUT_F_GLOBAL` 表示这是一份全局热键副本（WM 才会收到，
  普通应用通常没有）。
- `ASTER_DISPLAY_POLL_TICKS` 是 20e6 TSC tick（QEMU TCG 下约 20ms）。
- 没有定时器 syscall；用轮询超时累计实现“空闲 N 轮后退出”。

## 3. 编译

### 3.1 先跑 SDK 语法门禁

```powershell
powershell -NoProfile -File tools/sdk-check.ps1
```

期望输出 `sdk-check: PASS (n files)` 且退出码 0。它只做 `-fsyntax-only`，
不启动 QEMU、不写 `obj-ng/`。

### 3.2 交叉编译成 ELF64（无 libc / 无 SSE）

```powershell
New-Item -ItemType Directory -Force obj\sdk | Out-Null

clang --target=x86_64-elf -ffreestanding -nostdlib -mno-red-zone `
  -fno-stack-protector -mno-sse -mgeneral-regs-only -fno-builtin `
  -fno-pic -fno-pie -std=c11 -Os -Wall -Wextra -Werror `
  -I sdk/include -c sdk/examples/hello_gui/main.c -o obj/sdk/hello_gui.o

clang --target=x86_64-elf -fuse-ld=lld -nostdlib `
  -Wl,-T,src/ng/user/user.ld -Wl,--build-id=none -Wl,--gc-sections `
  -o obj/sdk/hello_gui.elf obj/sdk/hello_gui.o
```

- `src/ng/user/user.ld` 把镜像链到 `0x40000000`，段间 4 KiB 对齐；这是
  ng ELF64 加载器的约定（`docs/design` 里的进程地址空间布局）。
- `-mno-sse -mgeneral-regs-only` 与内核用户程序构建选项一致；SDK 不要求
  FPU/SIMD。
- 如果链接报 `undefined symbol: _start`：确认 include 了 `<aster/app.h>`
  且没有定义 `ASTER_NO_DEFAULT_ENTRY`；如果自己写了 `_start`，请定义该宏。

检查产物：

```powershell
readelf -h obj\sdk\hello_gui.elf | Select-String 'Type|Machine|Entry'
# 期望：EXEC / Advanced Micro Devices X86-64 / 0x40000000
```

## 4. 部署到 `/System/Packages`

### 4.1 目标布局

内核的包目录约定是 `/System/Packages/<name>/<version>/`，manifest 里的
`entry` 相对包版本目录：

```text
/System/Packages/hello_gui/
└── 1.0.0/
    ├── manifest
    └── bin/
        └── hello_gui      # 就是 obj/sdk/hello_gui.elf
```

manifest（与 `src/services/pkg/pkg.c` 的 seed 格式一致）：

```text
name=hello_gui
version=1.0.0
type=app
entry=bin/hello_gui
sha256=<ELF 的 SHA-256 十六进制>
```

在仓库里先准备一份同样布局的 staging 树并算哈希：

```powershell
$pkg = 'build/sdk-pkg/hello_gui/1.0.0'
New-Item -ItemType Directory -Force "$pkg/bin" | Out-Null
Copy-Item obj/sdk/hello_gui.elf "$pkg/bin/hello_gui"

$sha = (Get-FileHash "$pkg/bin/hello_gui" -Algorithm SHA256).Hash.ToLower()
@"
name=hello_gui
version=1.0.0
type=app
entry=bin/hello_gui
sha256=$sha
"@ | Set-Content -Encoding ascii "$pkg/manifest"

Get-Content "$pkg/manifest"
```

### 4.2 alpha 阶段的现实：还没有用户态安装器

这是必须说清楚的一条边界：

- 当前内核只在**启动时**由 `pkg_init()` seed 内置包（模板是
  `/System/Packages/hello/1.0.0`），**没有** `pkg install` 命令把宿主机文件
  拷进 ramfs；
- `/bin` 下的内置程序（shell/demo/wm/...）是构建期 `.incbin` 进内核
  `.rodata` 再由 `proc_init()` 种到 ramfs 的（见 `tools/build-ng.ps1` 的
  `$userApps` 表）。

所以 alpha 阶段有两条真实可用的落盘路径（都**不在 sdk/ 里改**，属于内核
构建/安装器的工作）：

1. **构建期 seed（现在可用）**：把上面的 staging 树按 `pkg_init()` /
   `proc_init()` 的现有模式嵌进内核镜像，重新构建内核。适合做内置示例；
2. **运行期安装（WP3-4 目标）**：安装器读取 `.pkg` / staging 树，验签+校验
   `sha256` 后落到 `/System/Packages/...`；落地后用下面的 `spawn` 运行。

在 WP3-4 合入前，不要指望在 shell 里直接 `cp hello_gui /System/Packages/...`：
shell 没有 `cp`，ramfs 也没有宿主目录映射。本文的 staging 命令产出的就是
安装器将来要消费的目录结构，方便提前验证 manifest/路径。

## 5. 运行

displayd 必须已经在跑（正常 `run-ng` 镜像会启动它）。在串口/控制台 shell 里：

```text
spawn /System/Packages/hello_gui/1.0.0/bin/hello_gui
```

- `spawn` 从 VFS 读 ELF64 并创建子进程（syscall 40）；
- 应用应在终端打印 `hello_gui: window up`，屏幕上出现 320×200 窗口；
- 按 `q` 或 `Esc` 退出；没有键盘输入（例如 `-NoQmp`）时约 5 秒后自动退出，
  并打印 `hello_gui: bye`。

如果构建期把它嵌到了 `/bin`，则用：

```text
spawn /bin/hello_gui
```

WM 的启动器（数字键 1..9）当前只把 `/bin` 下的文件当可启动项；
`/System/Packages` 下的目录在 WP3-2 里只作为包元数据展示，不直接启动。
所以 alpha 阶段请用 `spawn` 运行包目录里的 ELF。

## 6. 常见错误

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| `aster_app_open` 返回 `-2` (`ENOENT`) | displayd 没起，或 `/run/display.port` 不存在 | 用正常镜像启动；`cat /run/display.port` 应看到十进制 ticket |
| 返回 `-22` (`EINVAL`) | 宽/高为 0 或超过 1280×800；协议字段非法 | 检查 `cfg.width/height/flags` |
| 返回 `-12` (`ENOMEM`) | surface 页数超过 1024（4 MiB） | 减小窗口 |
| 返回 `-110` (`ETIMEDOUT`) | CREATE/LIST 等回复超时（displayd 忙/死） | 看串口 `ng: displayd ...` 证据行；重启镜像 |
| 提交后屏幕没变化 | 忘了 `aster_app_commit()`，或 COMMIT 前没 MAP | `aster_app_open` 已自动 MAP；检查 commit 返回值 |
| 链接出现 `memcpy`/`memset` 未定义 | 应用自己用了 libc 或结构体大赋值 | SDK 本身是内联循环；用 `aster_memcopy/aster_memzero` |
| 编译报 `syscall`/寄存器错误 | 用了非 x86_64 目标 | 必须 `--target=x86_64-elf` |

## 7. ABI / 兼容性说明（重要）

- 调用号镜像自 `src/ng/syscall.h`；结构布局镜像自
  `src/services/fs/include/vfs.h` 与 `src/desktop/include/ds_protocol.h`。
  SDK **不 include** 这些内核头，所以两边是“手工同步”的关系；
- `tools/sdk-check.ps1` 只保证 SDK 自身语法/类型一致，**不检查**编号是否与
  内核同步；改 syscall/协议时请同时改 `sdk/include/aster/*.h` 并跑门禁；
- display 协议：SDK 发 v2，解码 1..3（原因见 `sdk/README.md`）；
- ABI 1.0 在 WP4-1 冻结前，不保证跨内核版本的二进制兼容。发布应用时请记录
  所依赖的内核提交（`git rev-parse --short HEAD`）与 SDK 版本
  （`ASTER_SDK_VERSION`）。

## 8. 下一步（WP3-4 / WP4-1）

- WP3-4：session manager / 包安装器落地后，把本文第 4 节的 staging 树接到
  `pkg install`，替换“构建期 seed”的临时做法；
- WP4-1：冻结 syscall 编号与 display 协议结构，SDK 增加版本协商与兼容性矩阵；
- 后续 SDK 能力：多窗口/装饰 API、鼠标事件、alpha 合成、定时器、共享内存
  显式释放、文本多行/裁剪。
