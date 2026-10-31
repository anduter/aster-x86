# ng-WP3-5：S1 故障注入（杀 WM / 输入 / sessiond + WM 死循环）

## 结论

**PASS（S1 出口）** —— 按 `docs/design/architecture.md` §3 的 S1 SLA 断言：
**shell/WM 崩溃或卡死时，L3 进程、文档状态、输入不丢**（允许视觉中断、
焦点/IME 重置、重绘）。`tools/s1-fault.ps1` 一条命令起 4 个独立 QEMU，
3 个场景 PASS、1 个（WM 死循环）按已知限制记 KNOWN，退出码 0：

| 场景 | 注入 | 结果 | 关键证据 |
|---|---|---|---|
| a 杀 WM | `kill(wm, 9)` | **PASS** | Demo pid 25 存活；sessiond 重启 WM 24→26；新面板截图；连按 `1` 两次 → 两条 `via=key` LAUNCH（pid 27/28） |
| b 杀输入/焦点应用 | `kill(demo, 9)` | **PASS** | sessiond 心跳回收（reaps=1）、WM 不重启；按 `1` 启动新 Demo（pid 26） |
| c 杀 sessiond | `kill(spid, 9)` | **PASS**（记录行为，不要求恢复） | sessiond 22 死亡后无接管；WM 24/Demo 25/面板/输入仍在（按 `1` 仍发出 LAUNCH 请求），`srestarts` 不变 |
| d WM 死循环 | WM `fault=spin` + QMP F12 | **KNOWN** | 协作式调度无抢占：整机冻结、sessiond 无法检测/重启；冻结最后一帧仍有面板+窗口 → S1 SLA **不满足**，如实记录 |

一句话：**S1 的"杀 WM 应用不死 + 面板回来 + 输入可用"达成；"WM 卡死"在
D0 的协作式调度下无法救活，作为已知限制留给内核抢占/watchdog（不在 WP3-5
范围）。**

## 交付物

| 路径 | 内容 |
|---|---|
| `tools/s1-fault.ps1` | **新**：host 侧故障注入工具。编译客户机代理 → 造 FAT16 副本 → 每场景独立 QEMU（复用 run-ng 的 ELF/磁盘/`-vga std`/`-cpu` 参数、独立随机 QMP 端口、`-display none` + serial file）→ 按串口证据注入 QMP 按键、`screendump` 像素断言 → 每场景 PASS/FAIL/KNOWN 与退出码 0/1。不进 ci-ng（4 个 QEMU，约 52s）。 |
| `tests/s1/s1_logic.h` / `.c` | **新**：S1 纯逻辑 —— `/run/display.status` 的 s* 字段解析（0 与缺失分开、十六进制/坏数字/重复键/前缀诱饵）、四场景步骤表与每步断言（WAIT/OK/FAIL）、证据行格式化。**客户机代理与 host 单测共用同一份源码**。 |
| `tests/s1/s1fault.c` | **新**：客户机代理（ring3 L3，无 libc）。`spawn /bin/sessiond` 建真实会话；按步骤读状态、kill WM/焦点应用/sessiond、按需打印 `input req`；mode d 带 `fault=spin`。结束/失败后阻塞在 port_recv 上不退出（避免和 displayd 抢 readkey）。 |
| `tests/s1/fat-add.ps1` | **新**：往 FAT16 镜像追加 8.3 文件（解析 BPB、根目录空项、连续空闲簇链、双 FAT、内容自校验）；用于把代理 ELF 放进 `/mnt/disk/S1FAULT.ELF`。 |
| `tests/s1/README.md` | **新**：注入通道/复跑/证据位置说明。 |
| `tests/unit-ng/test_s1.c` | **新**：**+149 checks**（5183 total）；状态解析/模式与步骤表/每步断言正反例/格式化。 |
| `src/desktop/session/sessiond.c` | **改**：`s_main(argc, argv)` 解析 `fault=spin`，只在该参数存在时透传给 WM（S1 场景 d）。正式启动路径无参数，行为不变。 |
| `src/desktop/shell/wm.c` | **改**：`fault=spin` 模式收到 F12（scancode 0x58）打印证据后死循环（模拟 WM 卡死）；普通 WM 对该键照旧忽略。 |
| `tools/unit-ng.ps1`、`tests/unit-ng/unit_main.c` | **改**：注册 `tests/s1/s1_logic.c`、`test_s1.c`（不改变既有测试行为）。 |
| `docs/design/session-ng.md` | **改**：新增 §12 S1 故障注入（口径/通道/场景/钩子/证据/已知限制）。 |
| `docs/claims/ng-WP3-5.md` | **新**：本文件。 |

未修改 `src/kernel/**`、`build-ng.ps1`、`build.ps1`、`tools/run-ng.ps1`、
`tools/ci.ps1`、`abi.h`、`sdk/**`、`docs/sdk/**`（除与并行 WP3-4 已提交内容
共存外，未触碰其路径）。

## 环境

- 仓库 `D:\OS_Project`，分支 `ng`。实现起点 HEAD `36d350d`（WP3-3）；
  期间并行会话提交 `b8b985e`（spawn 路径/argv 截断加固，unit-ng 5034）与
  `3455eed`（WP3-4 SDK，display v3 同一协议）已并入工作区，最终门禁在
  `3455eed` + 本 WP 变更上跑。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；
  客户机代理：`--target=x86_64-elf -ffreestanding -nostdlib -mno-sse
  -mgeneral-regs-only -Os -Werror`，链接 `src/ng/user/user.ld`，
  `--gc-sections`，输出 17,976 字节（3 PT_LOAD，0x40000000）。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive <disk.img>,format=raw,if=ide -display none -no-reboot -serial
  file:<log> -qmp tcp:127.0.0.1:<随机端口>,server=on,wait=off`。
- 磁盘：`tools/mkfat-ng.ps1 -SizeMB 8` 生成 8 MB FAT16，`fat-add.ps1`
  追加 `S1FAULT.ELF`（18 个簇，首簇 5）；`dist-ng/disk.img` 本身不改，
  每次运行用副本。

## 注入通道（为什么不用内核自检）

`wp33_selftest` 在启动时杀 WM/应用并**清理整个会话**（释放 2 个 ramfs 节点，
给后续 ring3 shell 留空间），所以 S1 不能复用它的时序，也不应该把故障注入
塞进产品启动路径。做法是：

```text
host:
  clang tests/s1/s1fault.c tests/s1/s1_logic.c -> obj-ng/s1/last/s1fault.elf
  mkfat-ng.ps1 -> disk-s1.img（副本）; fat-add.ps1 -> /S1FAULT.ELF
  QEMU（独立 QMP）
  serial 看到 `ng: shell ready` 后 QMP 逐字符输入：
    spawn /mnt/disk/S1FAULT.ELF a
    wait <agent pid>          # 阻塞 shell，避免它和 displayd 抢按键
guest:
  代理 spawn /bin/sessiond（真实路径 sessiond -> displayd + wm）
  按步骤读 /run/display.status、kill 目标、打印 ng: s1 证据
  host 需要的时刻打印 `ng: s1 input req (scenario=..)`
host:
  按证据 QMP 注入 `1`（WM 启动器）或 `f12`（WM 故障钩子）
  QMP screendump -> PPM 像素断言（面板 + Demo 窗口）
```

关键取舍：

1. **代理从 FAT16 加载**：`proc_spawn` 从任意 VFS 路径读 ELF，`/mnt/disk`
   是已挂载的 FAT16；这样不必改 `build-ng.ps1`（内核镜像里没有这个 ELF），
   也不占产品 ramfs 节点。
2. **代理不提前退出**：shell 的 `wait` 返回后会重新 `readkey` 忙等；代理在
   结束/失败/d-arm 后都阻塞在 `port_recv` 20ms 超时轮询上，让 displayd 独占
   键盘轮询。QEMU 由 host 结束。
3. **一个场景一个 QEMU**：c 杀 sessiond、d 冻结整机，都会破坏后续场景的
   前提；独立启动保证互不影响，也方便单独复跑。

## 场景与证据

### 构建 / 镜像

```text
s1-fault: building guest agent (tests/s1/s1fault.c)
s1-fault: guest agent ok (17976 bytes)
=== mkfat-ng (WP2-2/2-4) ===
mode: disk 8MB
geometry: 16384 sectors x 512, spc=2, FATS=2x32, root=512 (32 sec)
          data.start=97, clusters=8143, bytes/cluster=1024
mkfat-ng: PASS -> D:\OS_Project\obj-ng\s1\last\disk-s1.img (8388608 bytes)
=== fat-add (WP3-5) ===
file:   ...\s1fault.elf -> S1FAULT.ELF (17976 bytes, 18 clusters, first=5)
fat-add: PASS -> ...\disk-s1.img
s1-fault: disk image ok (8388608 bytes, /mnt/disk/S1FAULT.ELF)
```

### 场景 a：杀 WM（PASS，12.9s）

```text
s1-fault: QEMU up (scenario a, qmp ..., serial serial-a.log)
s1-fault: guest agent spawned (pid=21)
s1-fault: [a] WM restarted (24 -> 26, restarts=1)
s1-fault: screendump ok (screen-a-wm-restart-1.ppm) panel(4,4)=#202838 demo(217,177)=#3070c0
s1-fault: [a] QMP sendkey 1 x2 -> new WM launcher -> sessiond LAUNCH
s1-fault: screendump ok (screen-a-final-1.ppm) panel(4,4)=#202838 demo(217,177)=#3070c0
```

串口（同一场景）：

```text
ng: s1 agent up (pid=21 mode=a)
ng: s1 sessiond spawn ok (pid=22)
ng: sessiond up (pid=22 displayd=23 wm=24 port=655361)
ng: s1 session up (sessiond=22 displayd=23 wm=24 app=25 launches=1 windows=2)
ng: s1 wm killed ok (pid=24)
ng: session wm dead ok (pid=24)
ng: session wm restart sched ok (old=24 restarts=0 backoff_ticks=20000000)
ng: session wm restart ok (old=0 new=26 restarts=1)
ng: session wm restart ready ok (wm=26 id=3)
ng: s1 wm restart ok (old=24 new=26 restarts=1)
ng: s1 demo alive ok (app=25 apps=1 windows=2 reaps=0)
ng: s1 input req (scenario=a)
ng: hotkey registered ok (surface=3 key=any)
ng: displayd hotkey ok (key=0x31 scancode=0x2)
ng: wm launch req ok (app=/bin/demo via=key)
ng: session app launch ok (pid=27 app=/bin/demo token=0x8fbb0205)
ng: displayd hotkey ok (key=0x31 scancode=0x2)
ng: wm launch req ok (app=/bin/demo via=key)
ng: session app launch ok (pid=28 app=/bin/demo token=0x4e940206)
ng: s1 input ok (launches=3 app=28)
ng: s1 scenario a PASS (step=4)
```

要点：

- `s1 demo alive ok (app=25 ...)`：**应用不归 WM**，WM 死亡/重启不回收应用；
  `sreaps=0` 证明不是"应用跟着 WM 一起死"。
- `swmpid` 由 24 → 26：sessiond 按 `restart_limit=3` + 20ms 退避真的重启了 WM。
- 新 WM 重新 CREATE 面板（id=3）→ sessiond `restart ready` → 截图 `(4,4)` 仍有
  面板；Demo 窗口仍在 `(192,152)`（`(217,177)=#3070c0`）。
- 连按两次 `1` 的两条 `via=key` LAUNCH 证明**输入路径在新 WM 上可用**；为什么
  按两次：新 WM 的 WP3-2 自检 fallback 可能自动 LAUNCH 一次，agent 要求
  `slaunches >= launches0+2`，host 再单独断言 `via=key` 行，排除"auto 冒充按键"。

### 场景 b：杀输入/焦点应用（PASS，12.3s）

```text
s1-fault: [b] focused app reaped (pid=25, reaps=1)
s1-fault: screendump ok (screen-b-app-reaped-1.ppm) panel(4,4)=#202838
s1-fault: [b] QMP sendkey 1 -> WM launcher -> sessiond LAUNCH (input recovered)
s1-fault: screendump ok (screen-b-final-1.ppm) panel(4,4)=#202838 demo(217,177)=#3070c0
```

```text
ng: s1 session up (sessiond=22 displayd=23 wm=24 app=25 launches=1 windows=2)
ng: s1 app killed ok (pid=25)
ng: session app reap ok (pid=25 why=stale)
ng: s1 app reaped ok (pid=25 reaps=1 wm=24)
ng: s1 input req (scenario=b)
ng: displayd hotkey ok (key=0x31 scancode=0x2)
ng: wm launch req ok (app=/bin/demo via=key)
ng: session app launch ok (pid=26 app=/bin/demo token=0xac820204)
ng: s1 input recovery ok (launches=2 app=26 windows=1)
ng: s1 scenario b PASS (step=3)
```

要点：焦点应用被 kill 后 sessiond 心跳回收（`why=stale`），WM 没有跟着重启
（`srestarts` 不变）；按 `1` 仍能启动新 Demo，说明 displayd 的输入路由和 WM
热键在焦点应用死亡后仍工作。第三张截图（`b-final`）同时断言面板与新窗口。

### 场景 c：杀 sessiond（PASS，12.5s，记录 session 域行为）

```text
s1-fault: [c] sessiond killed (pid=22), no session takeover expected (D0)
s1-fault: screendump ok (screen-c-after-kill-1.ppm) panel(4,4)=#202838 demo(217,177)=#3070c0
s1-fault: [c] QMP sendkey 1 -> WM launcher still alive (input path intact)
s1-fault: screendump ok (screen-c-final-1.ppm) panel(4,4)=#202838 demo(217,177)=#3070c0
```

```text
ng: s1 session up (sessiond=22 displayd=23 wm=24 app=25 launches=1 windows=2)
ng: s1 sessiond killed ok (pid=22)
ng: s1 sessiond dead ok (pid=22 stale=1 restarts=0 wm=24 app=25)
ng: s1 input req (scenario=c)
ng: displayd hotkey ok (key=0x31 scancode=0x2)
ng: wm launch req ok (app=/bin/demo via=key)
```

要点：sessiond 死亡后状态文件停在最后一次 SESSION_STATUS（`spid=22` 不变、
`srestarts=0`），没有"会话重启"——这与 WP3-3 遗留/architecture 开放决策 2
一致；WM/Demo/displayd 作为孤儿继续跑，输入路径仍把按键送到 WM（`via=key`），
面板+窗口像素仍在。**按 WP3-5 要求只记录行为，不要求恢复**；session 接管/
displayd 重连分别属后续 WP 与 S2。

### 场景 d：WM 死循环（KNOWN，13.4s）

```text
s1-fault: screendump ok (screen-d-before-spin-1.ppm) panel(4,4)=#202838 demo(217,177)=#3070c0
s1-fault: [d] QMP sendkey f12 -> WM fault spin (fault=spin only)
s1-fault: [d] frozen-frame screendump ok panel(4,4)=#202838 demo(217,177)=#3070c0
```

```text
ng: s1 session up (sessiond=22 displayd=23 wm=24 app=25 launches=1 windows=2)
ng: s1 spin armed (wm=24 scancode=0x58)
ng: s1 input req (scenario=d)
ng: displayd hotkey ok (key=0x0 scancode=0x58)
ng: wm fault spin (scancode=0x58)
<serial 冻结：1.5s 内长度不变，没有 session wm dead/restart、没有 wm exit>
```

工具判定：QEMU 仍在、serial 不再增长、冻结帧仍有面板+窗口 → **KNOWN**：
`WM spin froze the whole VM (cooperative scheduler, no preemption):
sessiond cannot detect/restart; frozen frame still has panel + window - S1 SLA
not met`。这不是本 WP 能修的问题（需要内核抢占/watchdog），按任务要求
"记录为已知限制"；退出码仍 0（`KNOWN` 不算 FAIL，但 summary 单独列出）。

## 截图证据

每次场景运行把 `serial-<mode>.log` 与 `screen-*.ppm`（1280x800 P6）留在
`obj-ng/s1/last/`（默认 `-WorkDir`）；本次运行 8 张：

| 文件 | 断言 |
|---|---|
| `screen-a-wm-restart-1.ppm` | 新 WM 面板 `(4,4)=#202838` + Demo 窗口 `(217,177)=#3070c0` |
| `screen-a-final-1.ppm` | 按键 LAUNCH 后同一断言（新窗口 `(224,184)` 也在画面里） |
| `screen-b-app-reaped-1.ppm` | 应用回收后面板仍在 `(4,4)=#202838` |
| `screen-b-final-1.ppm` | 新应用窗口 `(217,177)=#3070c0` + 面板 |
| `screen-c-after-kill-1.ppm` | sessiond 死后 WM 面板 + 原 Demo 窗口 |
| `screen-c-final-1.ppm` | 按键后同一视觉（WM 还活着） |
| `screen-d-before-spin-1.ppm` | 自旋前最后一帧 |
| `screen-d-after-spin.ppm` | 冻结后的同一帧（面板 + 窗口仍在） |

像素坐标来源：面板背景 `#202838`，1280x32 贴在 `(0,0)`；Demo 400x300 在
`(192,152)`，实心矩形 `(20,20)-(160,90)` 颜色 `#3070c0`，探针取
`(192+25,152+25)`。详细推导见 `docs/design/session-ng.md` §12.5。

## host 单测

```text
powershell -File tools/unit-ng.ps1
parity(host): 95/95 PASS (5 display/input cases are kernel-only)
5183 checks, 0 failures
unit-ng: PASS (5183 checks, 0 failures)
```

WP3-3 为 5025；并行输入加固（`b8b985e`）到 5034；本 WP **+149** →
**5183**。`tests/unit-ng/test_s1.c` 直接编译 `tests/s1/s1_logic.c`，覆盖：

- 状态解析：完整 12 字段、前缀诱饵（`pid/apps/windows/restarts/sessions`、
  `xsess`、`sapppidx`）、0 与缺失分开、十六进制/前导 0/坏数字/负号/空值、
  重复键、tab/CRLF 分隔、无 `=` token、值里再出现 `=`、NULL/空文本；
- 模式/步骤/动作表：a/b/c/d 解析、非法模式、步骤数与步骤名、每步动作
  （含越界）；
- 断言：session-up（缺字段/非 running/缺 sapppid）、wm-restart（新 pid/
  restarts/state）、demo-alive（reaps/app/windows）、input-launch（+2 阈值）、
  app-reap（reaps 增/restarts 不变）、input-recovery、sessiond-dead
  （kill_ok、spid 不变、sess、restarts、wmstate）、SPIN、越界/非法/NULL；
- 基线抓取（wm/app/sessiond/launches/reaps/restarts/kill_ok）与证据行格式化
  （PASS/FAIL/截断/NULL）、u32 十进制边界。

## 门禁（不回归）

```text
tools/ci-ng.ps1
  PASS check-domains    0 violations
  PASS check-deps       0 violations
  PASS unit-ng          5183 checks
  PASS build-ng         2082328 bytes, user-elf 15872 bytes
  PASS run-ng           159/159 lines + 75 patterns
  PASS parity-ng        100/100 items, 10 domains
  PASS legacy-32bit     305/305 parity checks
  7 passed, 0 failed, 0 skipped, 71.1s
=== NG CI PASS ===
```

`tools/s1-fault.ps1` **不进 ci-ng**：4 个 QEMU 约 51s，比 run-ng 重；S1 是
阶段出口证据而不是每次提交门禁。

## 已知限制 / 未做

1. **WM 死循环无法救活（d KNOWN）**：调度器是协作式（`src/ng/thread.h`：
   只在 `sched_yield/thread_sleep/thread_exit` 切换），WM 在 ring3 自旋后
   sessiond/displayd 拿不到 CPU，也没有 watchdog。修复需要内核抢占或
   watchdog（D0 之外）。因此 S1 的"崩溃或卡死"中**卡死这一半没有满足**，
   只有"崩溃"（a/b/c）满足。
2. **sessiond 自身崩溃无接管（c）**：应用/displayd/WM 变孤儿继续跑，但没有
   注册表/重启策略的接管；按设计只记录（WP3-3 遗留、architecture §11 开放
   决策 2）。
3. **displayd 崩溃（S2）没测**：杀 displayd 的重连/保活属 S2；本 WP 只在
   场景主线上依赖 displayd，不注入它。
4. **输入只测键盘**：PS/2 + QMP `input-send-event`；鼠标/IME 未实现（E3/D1）。
5. **"文档状态"用进程/窗口/注册表代替**：D0 还没有文档型 L3 应用；等编辑器
   接入后要补"内容不丢"的断言。
6. **像素断言依赖 stdvga 1280x800 + QMP screendump**：`-vga none` 没有 LFB，
   S1 场景不适用（与 run-ng `-Fallback` 同一约束）。
7. **d 的 screendump 在自旋时可能被 QEMU main loop 饿死**（3s 无响应）：
   工具同时接受"抓到冻结帧"和"QMP 无响应"两种证据，都算 KNOWN 证据；
   不是产品行为断言。
8. **FAT16 代理是测试通道**：产品路径不加载；E4 外部开发者测试仍走 SDK/包。
9. **故障钩子留在产品二进制里但只有 argv 主动开启**：`sessiond` 的
   `fault=spin` 透传与 WM 的 F12 自旋不会在正式启动（init → sessiond → wm）
   出现；`run-ng`/`ci-ng` 的 WP3-3/3-2 路径对此无感知。

## 复跑命令

```powershell
# 一次性构建（内核 + 用户 ELF；未构建时 s1-fault 会自动调 build-ng）
powershell -File tools/build-ng.ps1

# S1 全部场景（约 52s，3 PASS + 1 KNOWN，退出码 0）
powershell -File tools/s1-fault.ps1

# 单独复跑某个场景
powershell -File tools/s1-fault.ps1 -Scenario a
powershell -File tools/s1-fault.ps1 -Scenario b
powershell -File tools/s1-fault.ps1 -Scenario c
powershell -File tools/s1-fault.ps1 -Scenario d

# 其它 CPU（不依赖 SMEP/SMAP；S1 只在 max 上取过证据）
powershell -File tools/s1-fault.ps1 -Cpu qemu64
```

## 出口

```text
powershell -File tools/s1-fault.ps1               # PASS，3 PASS + 1 KNOWN，51.1s
powershell -File tools/unit-ng.ps1                # PASS，5183 checks, 0 failures
powershell -File tools/ci-ng.ps1                  # 7/7 PASS（含 run-ng 159/159+75）
powershell -File tools/parity-ng.ps1              # PASS，100/100（host 95/95）
powershell -File build.ps1 -Test                  # PASS，305/305（旧 32 位，ci-ng 第 7 步覆盖）
```
