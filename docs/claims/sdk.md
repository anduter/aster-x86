# SDK WIP 交接记录（2026-10-31，sdk-wip 分支）

## 状态

- 分支：`sdk-wip`（基于 main `69ab006`，305 项检查基线）
- WIP 提交：`b54ccaf`（libsvc.h、ulib svc_*、demoapp、build 装载）
- **当前问题：boot 停在 `ata ready` 之后，无法进入用户态；`-Test` 结果 292/311 失败。**
- 未修完，**不要合入 main**。

## 已做

- `src/kernel/include/libsvc.h`：程序层 SDK 头（svc_connect/rpc/reconnect/错误码）。
- `src/user/ulib.c`：svc_* 封装。
- `src/user/demoapp.c|h`：示例客户端。
- `build.ps1`：demoapp 编译/装载 + 检查项调整（WIP）。
- `docs/design/four-plane.md`、`docs/handoff-2026-10-31.md`：设计/交接草稿。

## 症状

`
boot ... ata ready
（之后无 ushell: ready / 无 demoapp 输出）
=== TEST FAIL (292 of 311 checks failed) ===
`

- 失败集中在“用户态未起来”之后的连锁断言；
- 怀疑：build.ps1 的 demoapp 装载/镜像改动、或 libsvc/ulib 改动影响 shell 启动；
- 未做最小化定位。

## 下一步（恢复工作时）

1. 在 sdk-wip 上 `git diff main` 逐文件回退定位（先回退 build.ps1 的装载改动，只留库代码）；
2. 确认 `ata ready` 后卡住是 build 装载问题还是 ulib 初始化死循环；
3. 修好后跑 `-Test` 到 305+ 全绿，再合并 main；
4. 不要直接 push main。