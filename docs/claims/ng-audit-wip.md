# ng 审计（进行中，partial 存档）

> 2026-10-31 更新：两个审计会话的成果已分别落到
> `docs/claims/ng-audit-syscall.md`（syscall 边界，本轮完成）与
> `docs/claims/ng-audit-ui.md`（UI 协议）。本文保留为历史快照。

## 状态

两个审计会话在运行中因会话异常中断，但工作区留下了**已通过门禁**的修复/测试：

- host 单测：`unit-ng` **5221 checks / 0 failures**（+38）；
- `build-ng` PASS；
- `run-ng` **159/159 + 75 patterns PASS**（含 display/WM/session 截图像素）。

## 已落盘内容

| 路径 | 说明 |
|---|---|
| `src/desktop/display/displayd.c` | 协议/状态机加固（明细见 `ng-audit-ui.md`） |
| `src/desktop/include|src/ds_protocol.*`、`ds_surface.*` | 协议字段/边界校验 |
| `src/desktop/session/sessiond.c`、`session_table.c` | 会话表/令牌校验 |
| `src/desktop/shell/wm.c` | 小修 |
| `src/ng/proc.c` | syscall 边界加固（+18 行；argv8 哨兵 / spawn &pid 预校验） |
| `tests/unit-ng/test_desktop.c`、`test_session.c` | +130 行回归 |
| `tests/audit-ng/**`、`tools/audit-ng-sys.ps1` | syscall 审计工具（本轮补完，531 用例） |

## 后续（更新）

- `audit-sys`：已补完并跑通（531/531，max 与 qemu64 双 CPU），发现并修复
  `vfs_open` 失败副作用与 ramfs 节点池耗尽；已知限制/未修项见
  `ng-audit-syscall.md` §6。
- `audit-ui`：见 `ng-audit-ui.md`。
- 8h 浸泡：按最终 HEAD 另起（本轮按要求不跑长浸泡）。
- 全量 `ci-ng`：本轮只跑 unit/build/run-ng/audit-ng-sys。

## 恢复命令

```
powershell -File tools/unit-ng.ps1
powershell -File tools/build-ng.ps1
powershell -File tools/run-ng.ps1
powershell -File tools/audit-ng-sys.ps1            # 531 用例
powershell -File tools/audit-ng-sys.ps1 -Cpu qemu64
```
