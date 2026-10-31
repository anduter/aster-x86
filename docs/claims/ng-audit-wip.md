# ng 审计（进行中，partial 存档）

## 状态

两个审计会话在运行中因会话异常中断，但工作区留下了**已通过门禁**的修复/测试：

- host 单测：`unit-ng` **5221 checks / 0 failures**（+38）；
- `build-ng` PASS；
- `run-ng` **159/159 + 75 patterns PASS**（含 display/WM/session 截图像素）。

## 已落盘内容

| 路径 | 说明 |
|---|---|
| `src/desktop/display/displayd.c` | 协议/状态机加固（待补 claims 明细） |
| `src/desktop/include|src/ds_protocol.*`、`ds_surface.*` | 协议字段/边界校验 |
| `src/desktop/session/sessiond.c`、`session_table.c` | 会话表/令牌校验 |
| `src/desktop/shell/wm.c` | 小修 |
| `src/ng/proc.c` | syscall 边界加固（+18 行） |
| `tests/unit-ng/test_desktop.c`、`test_session.c` | +130 行回归 |
| `tests/audit-ng/**`、`tools/audit-ng-sys.ps1` | syscall 审计工具（未完成） |

## 未完成（恢复时继续）

- 两个审计的**逐项 bug 清单/复现/根因**未写入；需重新跑 `tools/audit-ng-sys.ps1` 与 UI 协议畸形用例，把结果补进 claims；
- 8h 浸泡需重新起跑（上一轮 QEMU 随会话中断）；
- 未跑 `ci-ng` 全量（本轮只跑 unit/build/run-ng）。

## 恢复命令

```
powershell -File tools/unit-ng.ps1
powershell -File tools/build-ng.ps1
powershell -File tools/run-ng.ps1
powershell -File tools/audit-ng-sys.ps1   # 需补完
```