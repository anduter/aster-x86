# ng-WP4-2：外部开发者测试（PASS，带部署缺口）

## 结论

**PASS（受限口径）** —— 以「外部开发者」身份（只读文档 / SDK 头，不改
内核 / 桌面 / 构建系统，只能新增自己的应用）在 `ng` 上（起始基线
`9a6ab48` tag `ng-e3`，提交 parent `f40464c`）从零写出并跑通了一个有实际
内容的 GUI 应用：

- **应用**：`sdk/examples/hello_d0`，480×300、标题 `D0 Demo`；背景 + 标题
  文字 + 3 个彩色卡片（各带柱状图 / 嵌套画框 / 波形）+ 5 位定宽计数器 +
  10 格活动条 + 最后按键 + 页脚空闲进度条；
- **交互**：任意键 `count++` 重绘、`r` 归零、`q`/`Esc` 退出、**400 次
  20ms poll 超时（≈8s）无输入自动退出**（QEMU 实测 400 轮触发）；
- **构建**：`clang --target=x86_64-elf -ffreestanding -nostdlib` 语法 +
  freestanding 链接到 `0x40000000` 通过（23296 B，2 PT_LOAD，W^X）；
- **真跑**：`tools/d0-demo.ps1` 走 `build-ng.ps1 -SdkDemo` 的**同一条通道**
  （不改 build-ng、不改默认构建）在 QEMU 里由内核拉起 `/bin/demo`，QMP
  注入空格 + `q`，串口证据行 + 3 个 screendump 像素断言全部通过
  （15.6s）；空闲自动退出在同一镜像里另有实测证据；
- **只读边界**：未改 `sdk/include/**`、`sdk/examples/hello_gui/**`、`src/**`、
  `build.ps1`、`tools/ci*.ps1`、`abi.h`；新增只有 hello_d0、本 claims 和
  `tools/d0-demo.ps1`。

**不宣称**（诚实边界）：

1. **没有走 sessiond/WM 的 LAUNCH 路径**：hello_d0 是普通应用（不带
   hello_gui 的 `window`/`hold` demo harness 角色），`-SdkDemo` 把它放在
   `/bin/demo` 后由内核 WP3-1 自检直接 spawn，`session=0 token=0`；
   `aster_session_*` 代码路径被编译但**未在 QEMU 里执行**。要真跑会话链，
   需要安装器或 build-ng 的「demo 源文件」参数（见阻塞点 1/3）。
2. **没有要求内核 WP3-1/3-2/3-3 selftest PASS**：它们会因为 `/bin/demo`
   不再是带 harness 角色的 hello_gui 而按设计失败（日志里
   `ng: WP3-1 display status FAIL` 是预期；`d0-demo.ps1` 只断言 hello_d0
   自己的行为与像素）。
3. **ABI 状态在本次工作中变化**：开始时 ABI 1.0 仍是草案（必读清单给的是
   `docs/abi/abi-1.0-draft.md`）；并行 WP4-1 于 18:06 落地 `f40464c`
   （`abi-1.0.md` 冻结 + `docs/abi/abi-1.0.h` + `tools/abi-check.ps1`），
   它也是本提交的 parent。`sdk/include/**` 在 `9a6ab48..f40464c` **无 diff**，
   应用用到的调用号/布局未变；冻结版明确 1.0 **没有运行时 `abi_version()`**
   （延后到后续 major），应用仍按编译期宏假设，并把
   `sdk=0.1.0-alpha abi=0x00010000` 打进 boot 行方便对账。

**给 D1 的一句话**：写 GUI 应用本身「一天内」可以完成；**把外部应用装进
镜像并让产品启动链（sessiond/LAUNCH）拉起它，目前没有一条不改仓库的路**，
这是 WP4-2 暴露出的最大阻塞点。

## 环境 / 基线

| 项 | 值 |
|---|---|
| 分支 / 基线 | `ng`，起始 `9a6ab48`（tag `ng-e3`）；提交 parent `f40464c`（并行 WP4-1 ABI 冻结，18:06:02 落地）；工作区另有未跟踪 `obj-ng/`（WP4-2 未碰） |
| 工具链 | clang / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；目标 `x86_64-elf` |
| QEMU | QEMU 11.0.0，`-m 256 -cpu max -vga std -drive …if=ide -display none -no-reboot` |
| 宿主 | Windows（PowerShell）；`tools/d0-demo.ps1` 与仓库其它 tools 一样是 Windows 脚本 |
| 角色约束 | 只读文档/SDK/头文件；只能新增 `sdk/examples/hello_d0/**`、本 claims、可选 `tools/d0-demo.ps1` |
| SDK 版本 | `ASTER_SDK_VERSION "0.1.0-alpha"`、`ASTER_ABI_VERSION 0x00010000` |

## 交付物

| 路径 | 内容 | 状态 |
|---|---|---|
| `sdk/examples/hello_d0/main.c` | 普通 GUI 应用：窗口/绘制/事件循环/会话/8s 空闲退出；约 550 行（含注释） | **新** |
| `sdk/examples/hello_d0/README.md` | 从零到运行、问题记录、SDK 缺口 | **新** |
| `sdk/examples/hello_d0/screen.png` | QEMU QMP screendump 裁出的 480×300 窗口 | **新** |
| `tools/d0-demo.ps1` | 编译/链接校验 + overlay 走 `build-ng -SdkDemo` 的 QEMU 真跑与断言 | **新** |
| `docs/claims/ng-WP4-2.md` | 本文件（外部开发者视角的文档评估/耗时/阻塞点） | **新** |

未改：`sdk/include/**`、`sdk/examples/hello_gui/**`、`src/**`、`build.ps1`、
`tools/ci-ng.ps1`、`tools/ci.ps1`、`abi.h`、`tools/build-ng.ps1`
（`d0-demo.ps1` 用 junction + 三行代理文件在**系统临时目录**搭 overlay，
仓库里的 build-ng 原封不动）。

## 时间账（实测 wall clock，2026-10-31）

| 阶段 | 时间点 / 耗时 | 说明 |
|---|---|---|
| 会话开始（读文档/头） | 17:56:47 | `sdk/README`、getting-started、8 个头、ABI 草案、display 设计 |
| `main.c` 首次落盘 | 18:00:25（+3m38s） | 读文档 + 设计 + 写完代码 |
| 语法门禁通过 | 18:01 | 只踩了 2 个 `-Werror`（嵌套注释、未用 helper） |
| freestanding 链接通过 | 18:01 | 23296 B / 2 PT_LOAD / entry 0x40000000 |
| 首次 QEMU 真跑（手工） | 18:02（+25s 启动） | 看到 `ng: d0 demo up` + **8s 空闲自动退出**证据 |
| `tools/d0-demo.ps1` 首次 PASS | 18:03:41（+6m54s） | 含 overlay 构建 15.6s |
| README 完成 | 18:05:07（+8m20s） | — |
| 并行 WP4-1 ABI 冻结落地 | 18:06:02（共享工作区 `f40464c`） | 本 WP 未参与；`sdk/include` 无 diff；提交前 `abi-check` 复核 PASS |
| 本 claims 完成 | 约 18:10（总计约 **13 分钟**） | 含截图、冻结后复核与证据整理 |

**解读**：本会话是 agent 速度，读文档 4 分钟内完成；人类外部开发者预计
「读文档 + 写类似应用 + 跑通」约 **2–4 小时**（不含产品部署），远低于
WP4-2 的「一天内」出口。**但**如果要求「装进镜像、被 sessiond/WM 拉起、
走 token 链」，本基线没有可行路径，时间会不可控（见阻塞点 1）——这是
比「写 GUI 代码」更重要的 E4 风险。

## 文档够用 / 不够用（逐份）

| 文档 | 够用的部分 | 不够 / 易踩 |
|---|---|---|
| `sdk/README.md` | API 一览表、60 秒骨架、v3/token、会话通道、alpha 限制、错误处理语义 | 目录树和 API 表只列 `hello_gui`；**没有「普通应用 vs demo harness 角色」说明**（第一次会以为 `/bin/demo` 就是普通应用）；没提格式化/缩放缺失 |
| `docs/sdk/getting-started.md` | §1–§3 从目录到编译命令完整可照抄；§4 部署现实（三种路径）讲得清楚；§6 常见错误表准确（`-ENOENT`/`-EINVAL` 等都对上） | §3.1 没说明 `sdk-check` **只做宿主 `-fsyntax-only` + 纯函数**，不覆盖 `x86_64-elf` 链接；§3.2 的 `-Wl,-T,...` 在 PowerShell 里直接抄会因逗号数组分隔而报「缺少参数」；没有「外部开发者不改仓库怎么真跑」的路径；没讲会话路径在 QEMU 里怎么才能被触发 |
| `sdk/include/aster/app.h` | 入口 ABI、事件循环建议、`aster_app_config`/`aster_app` 字段、`_start` 覆盖方式都清楚 | `aster_app_poll` 对 KEY_DOWN/KEY_UP 都返回 1（注释是空泛的「有事件」）；没有格式/缩放/定时器说明 |
| `sdk/include/aster/session.h` | 流程图、argv 格式、角色令牌、错误码、超时单位、`connect` 的 `-ENOENT` 语义都很完整 | 没有「普通应用拿不到会话」的显式警告（README 有，但头文件更该有）；`parse_u32` 的严格溢出行为只在注释里 |
| `sdk/include/aster/display.h` | v3 CREATE/FOCUS/LIST body、版本策略、尺寸上限、`_Static_assert` 锁布局 | `aster_display_destroy()` 无 reply（注释只说「没有 SHM_FREE」）；`ASTER_DISPLAY_TITLE_MAX=16`（15 字符）只在宏名旁，`aster_app_config.title` 没提截断 |
| `sdk/include/aster/draw.h` | 像素格式、裁剪语义、字模范围 `0x20..0x7E`、不处理 `\n`、不混合 alpha 都写清 | 没有缩放/换行/宽度查询；只有 pixel/fill/rect/char/text，没有 line/circle；想画大字只能自己用 `aster_font_glyph` |
| `sdk/include/aster/ipc.h` / `fs.h` / `syscall.h` / `types.h` | 调用号表、结构布局、句柄语义、错误码、`aster_memcopy/memzero` 都有；`shm_alloc` 的 8 字节 `va` 坑有显式注释 | 没有 `snprintf` 类格式化；没有 `sleep/timer/uptime`；`abi_version()` 不存在 |
| `docs/abi/abi-1.0-draft.md`（提交时已被冻结版 `abi-1.0.md` 取代） | 调用约定、寄存器/返回值、结构布局、错误码、长度上限、§10 的 13 个冻结前缺口都很实；明确「不是稳定 ABI」 | 草案不是 SDK 的「使用手册」：外部开发者不会从它学到「写 GUI 要哪个头」；§10 的缺口没有按「对 GUI 应用的影响」排序；ABI 版本查询缺失。冻结版 §10 对 13+2 项给出结论，但仍不是使用手册，且 `abi_version()` 明确延后 |
| `docs/design/display-server-ng.md` | displayd 的协议/合成/焦点/输入/z-order/像素格式/探针位置、WP3-3 token 规则都清楚；解释了为什么不能劫持 shm | 没有「普通应用必须做什么/不必做什么」；`(25,25)=#3070c0` 的探针契约只散在 claims；session 细节在 `session-ng.md`（本次必读清单没列，但 README/头文件够用） |
| `docs/claims/ng-WP3-4.md` | 是唯一能解释「`-SdkDemo` 把 hello_gui 放到 /bin/demo、断言 title=Aster SDK / 探针」的地方 | 外部开发者不在必读清单里；README 只链 getting-started，导致「怎么真跑自己的应用」必须靠读 claims + build-ng 源码 |

**结论**：写应用所需的核心文档（README + getting-started + 8 个头）**够用**，
API 设计（`aster_app_*` 骨架、v3 token、会话 argv、脏矩形提交）对新手友好，
没有出现「文档说 A、实现是 B」的硬矛盾。不够的主要是**「最后一公里」**：
自己的 ELF 怎么进镜像/怎么被产品链拉起、`sdk-check` 的能力边界、以及
格式化/缩放/定时器这些「写真实 UI 马上需要」的小 API。

## 阻塞点清单（按严重度）

### P0（不解决，外部应用无法产品化）

1. **没有「新增应用」的部署/启动通道**：
   - alpha 没有用户态安装器，`/System/Packages` 只在构建期 seed；
   - `build-ng.ps1 -SdkDemo` **硬编码** `sdk/examples/hello_gui/main.c`；
   - 内核 shell 的 `spawn /bin/hello_d0` 需要 ELF 先构建期 `.incbin` 进镜像
     （要改内核构建），且直连 spawn **没有** `session=`/`token=`；
   - 结果：外部开发者只能像本 WP 一样用 overlay/临时补丁绕过，或者改
     `build-ng.ps1`（被本 WP 的边界禁止）。
   **绕过**：`tools/d0-demo.ps1` 在系统临时目录用 junction + 三行代理文件
   跑原版 build-ng；仅测试可用，不是产品路径。
2. **`sdk-check` 不是交叉构建门禁**：它只对头/示例做宿主
   `-fsyntax-only`，不链接 `x86_64-elf`、不校验 ELF 入口/段。外部开发者
   很容易「sdk-check 绿了，镜像里跑不起来」。本 WP 因此额外加了
   `d0-demo.ps1 -NoQemu`（compile+link+ELF 校验）。
3. **普通应用无法验证会话链**：只有 WM `LAUNCH`（或未来安装器）才给
   `session=`/`token=`；`-SdkDemo` 的 `/bin/demo` 被内核自检直接 spawn，
   所以本应用 QEMU 里 `session=0 token=0`。`aster_session_*` 只验证了
   编译/语法，没有真跑 REGISTER/HEARTBEAT/EXIT。

### P1（写真实 UI 立刻遇到）

4. **没有定时器/`sleep`**：8s 空闲退出只能数 400 次 poll 超时；动画/倒计时
   同理。`ASTER_DISPLAY_POLL_TICKS` 是 TSC tick 近似，文档已提醒不可假设
   毫秒，但除它之外没有时间 API。
5. **没有格式化**：数字/十六进制/拼接要自己写（本应用 3 个 helper +
   证据行拼装）。外部开发者会重复造轮子。
6. **没有文字缩放/换行/裁剪**：本应用用 `aster_font_glyph` + 2×2 `fill`
   自己放大标题和计数器；绘制 API 只有 8×8 单行。

### P2（体验/健壮性）

7. `aster_display_destroy()` 发完即返回（无 reply），无法确认服务端回收；
   `SHM` 无 free。
8. 窗口标题 15 字符截断没有 API 查询/报错。
9. 每次脏矩形 COMMIT 都让 displayd 打一行 `composite ok`（本应用页脚
   ~17 行日志），证据行会被噪音淹没。
10. `PowerShell` 链接参数的逗号陷阱（`-Wl,-T,...`）没有任何文档提示；
    本 WP 在 README 里写了数组写法。
11. 没有鼠标/焦点事件/按键重复；`readkey` 只有 scancode + ascii。
12. `tools/d0-demo.ps1` 依赖 Windows junction（`New-Item -ItemType
    Junction`），只能在 Windows 跑；仓库 tools 目前也都这样，但可移植性
    是 D1 的事。

## 文档缺口 → 建议（只记录，不改；作为 D1 输入）

| 缺口 | 建议（归 WP4-3/D1） |
|---|---|
| `sdk/README.md` 只列 `hello_gui` | 加 `hello_d0`；加一段「普通应用」vs「demo harness（window/hold）」；注明 `-SdkDemo` 的真实用途 |
| getting-started 没有「外部应用怎么真跑」 | 要么给 build-ng 加 `-DemoSrc <path>`（最小改动、最有用），要么把 `tools/d0-demo.ps1` 的 overlay 方案写成正式文档；同时把 `sdk-check` 的能力边界写清 |
| getting-started 的 PowerShell 链接示例 | 改成数组/引号写法，或加一句「PS 会把 `-Wl,...` 的逗号当数组分隔符」 |
| 头文件没有格式化/缩放/定时器 | D1 的 `aster_text`/`aster_time` 小模块；至少先在 `sdk/README` 的「SDK 缺什么」列出来（现在 README 只列了 alpha 限制，没列这些） |
| ABI 冻结版 §10 的 15 项结论 | 加一栏「对 GUI/外部应用的影响」，把 `abi_version()`、SHM free、权限空壳、`EACCES vs EPERM` 标出来 |
| `-SdkDemo` 的断言/探针契约只在 claims | getting-started §5 补一句「示例要画 `(25,25)=#3070c0`、title 由 sdk-demo 断言」；否则外部开发者不知道为什么自己的应用一进 `/bin/demo` 就让 WP3-1 FAIL |
| 会话链的验证入口 | 文档明确：当前只有 WM LAUNCH 能给 token；普通应用在 QEMU 里无法自测会话；给出「D1 安装器落地后怎么验证」的占位 |

## 验证证据（2026-10-31）

### 语法 / 链接 / SDK 门禁

```text
$ clang -fsyntax-only -std=c11 -ffreestanding -fno-builtin -Wall -Wextra -Werror -I sdk/include sdk/examples/hello_d0/main.c
-> exit 0

$ powershell -ExecutionPolicy Bypass -File tools/sdk-check.ps1
[ ok ] sdk/examples/hello_d0/main.c
       sdk-test: 237 checks, 0 failures
sdk-check: PASS (10 files + 237 SDK host checks)

# 提交前在冻结后的 ABI 树上复核（WP4-1 / f40464c）
$ powershell -ExecutionPolicy Bypass -File tools/abi-check.ps1
abi-check: PASS (29 syscalls, 28 errors, 64 constants, 4 struct sizes)

$ powershell -ExecutionPolicy Bypass -File tools/d0-demo.ps1 -NoQemu
d0-demo: link ok (23296 bytes, 2 PT_LOAD, entry 0x40000000, end 0x4000468e)
d0-demo: PASS (link-only, 0.5s; QEMU skipped by -NoQemu)
```

### QEMU 真跑（`-SdkDemo` overlay，不改 build-ng）

```text
$ powershell -ExecutionPolicy Bypass -File tools/d0-demo.ps1
d0-demo: app up (pid=9 id=1 x=160 y=120 w=480 h=300 token=0x00000000 session=0)
d0-demo: key ok (space -> count=1)
d0-demo: screendump card=(190,180)#c0504a panel=(200,290)#16202b header=(620,150)#18222e
d0-demo: q quit ok (reason=q/esc)
d0-demo: PASS (480x300 D0 Demo, 3 cards, key+redraw, q quit; 15.6s, cpu max)

# 在 ABI 冻结后的 parent（f40464c）上重跑一次，同样 PASS（15.2s）：
# d0-demo: PASS (480x300 D0 Demo, 3 cards, key+redraw, q quit; 15.2s, cpu max)
```

串口关键行（同一轮）：

```text
ng: d0 boot ok (sdk=0.1.0-alpha abi=0x00010000)
ng: surface title ok (id=1 title=D0 Demo)
ng: d0 demo up (pid=9 id=1 x=160 y=120 w=480 h=300 token=0x00000000 session=0)
ng: d0 draw ok (cards=3 count=0)
ng: d0 key ok (key=0x20 count=1)
ng: d0 quit ok (reason=q/esc rounds=24 idle=0)
ng: d0 demo bye (id=1 count=1 rounds=24 reason=q/esc)
```

8s 空闲自动退出（同一镜像、不注入按键的另一次启动）：

```text
ng: d0 quit ok (reason=idle rounds=400 idle=400)
ng: d0 demo bye (id=1 count=0 rounds=400 reason=idle)
ng: demo exit ok (status=0)
```

预期内的内核自检失败（**不是 hello_d0 的缺陷**，是它占用了 `/bin/demo`
但不带 hello_gui 的 harness 角色）：

```text
ng: WP3-1 display status FAIL
ng: WP3-1 selftest FAIL (1)
ng: WP3-2 selftest skipped (earlier failure)
ng: WP3-3 selftest skipped (earlier failure)
```

### 截图

`sdk/examples/hello_d0/screen.png`（480×300，`tools/d0-demo.ps1` 的 QMP
screendump 裁切；注入空格后 `count=1`）。

## 限制 / 未验证

1. **QEMU 是 kernel-selftest spawn 路径**（`/bin/demo`），不是
   sessiond → WM → LAUNCH；`session=0 token=0`。会话代码只编译、未运行。
2. **没有跑 `unit-ng`/`parity`/默认 `build-ng`/`ci-ng`**：本 WP 不修改
   内核/桌面/构建路径，改动仅是新增 `sdk/examples` 示例 + 新 tools 脚本 +
   claims；相关门禁是 `sdk-check`（已 PASS）与 `d0-demo`（已 PASS）。没有
   把 QEMU 里预期失败的 WP3-1/3-2/3-3 当作本 WP 的验收项。
3. **像素断言绑定 stdvga 1280×800 + QMP PPM**；`-vga none` 没有可截图画面。
4. **overlay 依赖 Windows junction**；临时目录在系统 temp，正常退出会
   先摘 junction 再删目录（已在两次运行后确认 `src/ng/main.c`、
   `sdk/include/aster/app.h`、`sdk/examples/hello_gui/main.c` 仍在）。
5. **8s 空闲按 poll 超时计**：连续按键时 idle 不增长（符合「无输入」语义），
   但若按键间隔 <20ms，墙钟时间不会被计入。
6. **没有鼠标/焦点事件**；窗口位置来自 displayd 级联（本机 `(160,120)`），
   脚本从证据行解析 x/y，不硬编码。
7. **角色边界说明**：应用代码只依据必读文档 + `sdk/include/aster/**` 写成；
   为了做可选的 QEMU harness（`tools/d0-demo.ps1`）并解释预期失败，额外读了
   `tools/build-ng.ps1`、`tools/sdk-demo.ps1`、`src/desktop/display/displayd.c`
   和 `src/ng/wp31_selftest.c`。这本身就是文档缺口：如果文档把「普通应用 vs
   `/bin/demo` demo harness」和「外部应用怎么跑」写清，就不需要读这些实现。
8. **并行工作区**：WP4-1 在本 WP 写作期间提交了 `f40464c`（ABI 冻结），
   工作区同时还有 `docs/release/`、`tools/release-ng.ps1`、`docs/design/*`
   等**其它会话的未提交改动**。本 WP 只 stage/提交自己的 5 个文件，没有
   触碰这些路径；commit hook 只检查了暂存区。

## 出口

```text
powershell -ExecutionPolicy Bypass -File tools/sdk-check.ps1        # PASS (10 files + 237)
powershell -ExecutionPolicy Bypass -File tools/d0-demo.ps1 -NoQemu  # PASS (link-only)
powershell -ExecutionPolicy Bypass -File tools/d0-demo.ps1          # PASS (QEMU, 15.6s)
```
