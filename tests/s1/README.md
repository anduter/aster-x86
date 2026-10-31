# tests/s1 —— S1 故障注入（WP3-5）

上位口径：`docs/design/architecture.md` 第 3 节（S1：shell/WM 崩溃或卡死时
L3 进程、文档状态、输入不丢；允许视觉中断、焦点/IME 重置、重绘）。
工具入口：`tools/s1-fault.ps1`；证据：`docs/claims/ng-WP3-5.md`。

| 文件 | 作用 |
|---|---|
| `s1_logic.h` / `s1_logic.c` | 纯逻辑：`/run/display.status` 的 s* 字段解析、四个场景的步骤表与断言、证据行格式化。host 单测（`tests/unit-ng/test_s1.c`）和客户机代理共用同一份，所以单测覆盖的是真实断言。 |
| `s1fault.c` | 客户机代理（ring3，L3，无 libc）：`spawn /bin/sessiond` 建真实会话 → 按步骤读状态、kill WM/焦点应用/sessiond → 打 `ng: s1 ...` 证据行。mode d 用 `fault=spin` 让 WM 收到 F12 后进入死循环。 |
| `fat-add.ps1` | 把代理 ELF 追加进 FAT16 磁盘镜像副本（8.3 文件名、空闲根目录项 + 连续簇链、两份 FAT）。内核用 `spawn /mnt/disk/S1FAULT.ELF <mode>` 从 VFS 读它，因此不需要改 build-ng/内核镜像。 |

为什么代理走磁盘而不是编进内核：本 WP 不能改 `build-ng.ps1`/内核镜像里的
7 个用户 ELF；`proc_spawn` 支持任意 VFS 路径，FAT16 的 `/mnt/disk` 是现成的
测试载荷通道。

代理运行期**不退出**（阻塞在 `port_recv` 超时轮询上）：父 shell 保持
`wait` 阻塞，才不会和 displayd 抢 `readkey`；QEMU 由 host 结束后收走。

复跑：

```powershell
powershell -File tools/s1-fault.ps1                # a/b/c/d 全跑（约 52s）
powershell -File tools/s1-fault.ps1 -Scenario a    # 只跑杀 WM
```

证据落在 `obj-ng/s1/last/`（`serial-*.log` + `screen-*.ppm`），可用任意
PPM 查看器打开；像素断言坐标见 `tools/s1-fault.ps1` 的 `Test-ShotPixels`。
