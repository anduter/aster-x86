# 设计文档索引

本目录是内核工程的设计与工程记录入口。三会话并行开发期间，改动前请先读
[`parallel-sessions.md`](parallel-sessions.md) 的文件所有权与构建串行约定。

## 1. 设计文档

| 文档 | 范围 | 状态 |
|---|---|---|
| [`system-model.md`](system-model.md) | 总体模型：设计公理、五类对象、句柄与权限、数据流、迁移路线 | 现行 |
| [`architecture.md`](architecture.md) | 分层与命名空间、程序/设备/文件模型、与 Linux 的差异 | 现行 |
| [`native-abi.md`](native-abi.md) | `int 0x80` 原生 ABI：类型、句柄、错误码、IPC 消息、内存接口；`include/abi.h` v1 冻结流程 | v0 已实现 / 头文件 ABI v1 |
| [`api.md`](api.md) | 用户态 syscall ABI、设备 ops、VFS 后端、服务 manifest 的版本策略、校验落点与变更流程 | ABI v1 冻结 |
| [`security-model.md`](security-model.md) | 威胁模型、信任边界、已实现控制、限制、安全测试矩阵 | 现行（回归 94/94） |
| [`driver-service.md`](driver-service.md) | 驱动服务化设计：消息格式、生命周期、分阶段计划 | 规划中（e1000 仍在核内） |
| [`modules.md`](modules.md) | 九模块划分契约：归属决策树、依赖规则、硬指标、迁移进展 | 现行（迁移进行中） |

## 2. 工程记录

| 文档 | 内容 |
|---|---|
| [`parallel-sessions.md`](parallel-sessions.md) | 多会话协作约定、文件所有权、构建串行、登记表 |
| [`changes-fs.md`](changes-fs.md) | 文件系统/VFS 会话的提交清单、验证证据、已知问题 |
| [`status-2026-10-24.md`](status-2026-10-24.md) | 工程监理报告：进度快照、完整度、日志归档、变更记录 |

## 3. 验证入口

| 命令 | 作用 |
|---|---|
| `.\build.ps1` | 构建 `dist\floppy.img`（含内核签名、shell 签名、启动验证器） |
| `.\build.ps1 -Test` | 全量回归（当前 **97 项**）。默认占用 `obj/`、`dist/`、QMP 4444、`serial.log`；并行会话用 `-ObjDir/-DistDir/-QmpPort/-SerialLog` 隔离（宿主 12345 回显端口仍只能单跑） |
| `.\build.ps1 -Run` | 交互运行（挂 `dist\disk.img` + `dist\fat.img`） |
| `.\tools\watch-changes.ps1` | 事件驱动变更监视（提交/版本/文件集合变化时触发） |

回归项清单维护在 [`../README.md`](../README.md) 的“自动回归”章节；安全断言清单在
[`security-model.md`](security-model.md) 第 6 节。

## 4. 维护约定

- 设计文档随功能提交一起更新（新增系统调用、后端、安全控制时）；
- 回归数字以最近一次 `build.ps1 -Test` 的实测结果为准，不写“计划值”；
- 工程记录由对应会话更新；监理会话只改 `status-*.md`、本索引与 `.gitignore`。
