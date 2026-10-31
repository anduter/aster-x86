# ng-WP4-3：D0 本地发布（PASS）

## 结论

**PASS** —— E4 / WP4-3 出口达成：`tools/release-ng.ps1` 一条命令完成
**ABI 门禁 + 构建 + D0 发布目录 + SHA-256 自检**，产物在
`dist-ng/release/d0/`（只本地；不 push、不签名、不打 tag）。

- **发布目录 24 个文件**：`aster-ng.elf`、`disk.img`、8 个 SDK 头、
  4 个 SDK 示例文件（`hello_d0` 的 main/README/截图 + `hello_gui` 源码）、
  ABI 1.0 头/规范、D0/架构/演进/设计索引/SDK 上手文档、`LICENSE`、
  `RELEASE-NOTES.md`、`SHA256SUMS`、`.gitignore`。
- **SHA-256**：对 24 个载荷文件逐一计算并回读校验，`[ok] 24 files verified`。
- **ABI**：发布前强制跑 `tools/abi-check.ps1`，PASS（29 syscalls / 28 errors /
  64 constants / 4 struct sizes）；ABI 1.0 冻结版随包。
- **构建**：`build-ng` PASS，ELF **2,082,328** 字节；`disk.img` **8,388,608**
  字节（8 MB FAT16）。
- **文档**：`docs/release/d0.md` 覆盖 D0 范围/出口/已知限制/QEMU 运行/校验；
  `RELEASE-NOTES.md` 记录版本、提交、内容、运行和已知限制。
- **git 保护**：发布目录内 `.gitignore` 为 `*`；根 `.gitignore` 未改，
  `git status` 不显示 `dist-ng/release/**`。

## 基线

- 仓库 `D:\OS_Project`，分支 `ng`；WP4-1 提交 `f40464c`（ABI 1.0 冻结）之上。
- 依赖：`tools/build-ng.ps1`、`tools/mkfat-ng.ps1`、`tools/abi-check.ps1`、
  `docs/abi/abi-1.0.{h,md}`、`docs/release/d0.md`、`sdk/include/aster/*.h`。
- 工具链：clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；
  QEMU 不参与发布打包（运行说明见 D0 文档）。

## 交付物

| 路径 | 内容 |
|---|---|
| `tools/release-ng.ps1` | **新**：本地发布脚本（abi-check → build-ng → 组装 → RELEASE-NOTES → SHA256SUMS → 自校验）；输出目录限定在 `dist-ng/release/**`；BOM |
| `docs/release/d0.md` | **新**：D0 发布说明（范围/出口/内容/安装运行/校验/已知限制/复现/边界） |
| `docs/claims/ng-WP4-3.md` | **新**：本文件 |
| `docs/design/README.md` | **改**：E4 状态、ABI 1.0 冻结、D0 发布、门禁与下一步 |
| `docs/design/evolution.md` | **改**：E4 进度（WP4-1/4-2/4-3 完成；仅 WP4-4 待做，E4 未整体关闭） |
| `dist-ng/release/d0/**` | **产物**（gitignore，不提交）：见上 |

未修改 `src/**`、`sdk/include/**`、`build.ps1`、`tools/ci.ps1`、
`tools/ci-ng.ps1`、`abi.h`；未 push、未打 tag。

## 发布目录

```text
dist-ng/release/d0/
  aster-ng.elf                 2082328 bytes
  disk.img                     8388608 bytes
  abi/abi-1.0.h
  abi/abi-1.0.md
  sdk/include/aster/*.h        app/display/draw/fs/ipc/session/syscall/types
  sdk/examples/hello_d0/       main.c / README.md / screen.png（WP4-2）
  sdk/examples/hello_gui/      main.c
  sdk/README.md
  docs/d0.md
  docs/architecture.md
  docs/evolution.md
  docs/design-readme.md
  docs/sdk-getting-started.md
  LICENSE
  RELEASE-NOTES.md
  SHA256SUMS                   24 行
  .gitignore                   *
```

`RELEASE-NOTES.md` 记录：版本 `1.0.0-d0`、构建时间、分支/提交、ELF/disk
大小、内容表、QEMU 交互/串口命令、SHA256 校验命令、D0 已知限制 8 条。

## 证据（2026-10-31）

### 一条命令（完整构建）

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/release-ng.ps1 -AllowDirty
=== release-ng (D0 local release) ===
  repo    : D:\OS_Project
  branch  : ng
  commit  : f40464c (ng-e3-1-gf40464c)
  version : 1.0.0-d0
  out     : D:\OS_Project\dist-ng\release\d0
  dirty   : yes (-AllowDirty)

--- 1. abi-check (ABI 1.0 freeze) ---
abi-check: PASS (29 syscalls, 28 errors, 64 constants, 4 struct sizes)

--- 2. build-ng ---
=== build-ng (WP3-3) ===
clang: D:\msys64\mingw64\bin\clang.exe
lld:   D:\msys64\mingw64\bin\ld.lld.exe
...
disk image: PASS (8388608 bytes, 16384 sectors, fats=2x32, data.start=97, clusters=8143)
ELF64: PASS (2082328 bytes)
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
  build: 2082328 bytes (ELF), 8388608 bytes (disk)

--- 3. assemble release tree ---
...
--- 4. RELEASE-NOTES.md ---
  write: RELEASE-NOTES.md
--- 5. SHA256SUMS ---
  write: SHA256SUMS (24 files)
--- 6. verify SHA256SUMS ---
[ok] 24 files verified
--- 7. summary ---
  ...
release-ng: PASS -> D:\OS_Project\dist-ng\release\d0
release-ng: 24 files, SHA256SUMS verified; local only, nothing pushed
```

> `-AllowDirty` 只用于提交前预跑；最终在 WP4-3 提交的干净工作区上重跑，
> 默认（不带 `-AllowDirty`）PASS，`RELEASE-NOTES.md` 记录最终提交哈希。

### 独立校验

```text
# 抽查：ABI 头 / ELF / disk 的哈希与 SHA256SUMS 一致
Get-FileHash -Algorithm SHA256 -LiteralPath abi/abi-1.0.h
Get-FileHash -Algorithm SHA256 -LiteralPath aster-ng.elf
Get-FileHash -Algorithm SHA256 -LiteralPath disk.img
# 逐行校验脚本见 docs/release/d0.md §5.1；输出 SHA256SUMS: PASS
```

### sdk-check（不回归）

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/sdk-check.ps1
sdk-check: PASS (10 files + 237 SDK host checks)
```

> `sdk-check` 的 10 个文件包含 WP4-2 的 `sdk/examples/hello_d0/main.c`；本 WP
> 未改 `sdk/**`，发布包复制 SDK 头（8 个）、`sdk/README.md` 与 WP4-2 示例源码。

## D0 发布边界

- **是**：本地开发包/交付物；ABI 1.0 冻结；SDK 头；QEMU 可运行镜像；
  SHA256；D0 文档。
- **不是**：签名正式发行；D0 总出口的“1 小时 smoke”（WP4-4）证据；
  D1 的 POSIX/网络/真机/更新。（WP4-2 外部开发者测试已由另一会话完成，
  本包只分发其示例源码，不替代其 QEMU 证据。）
- **不 push**：脚本不调用 `git push`，不创建远端 release，不打 tag。
- **产物 gitignore**：`dist-ng/release/d0/.gitignore` 内容 `*`；根 `.gitignore`
  不修改。

## 遗留（按 WP 边界）

1. **无签名/无版本清单签名**：D0 只做 SHA256；签名更新/回滚属 B 域 D1。
2. **无安装器**：运行方式是 QEMU `-kernel`/`-drive`；真实硬件/安装器属 D1。
3. **1h smoke 未做**：WP4-4，E4 未整体关闭；WP4-2 已完成（`3328b8e`）。
4. **D0 已知限制**：WM 死循环、用户 fault panic、sessiond 无接管、root-only、
   bearer ticket、无包启动、静态 ELF/无 POSIX、QEMU 矩阵有限；详见
   `docs/release/d0.md` §6。
5. **发布脚本不归档构建日志**：只保留关键行与产物哈希；完整串口证据在
   `docs/claims/` 与 `obj-ng/s1/`（本地产物）。

## 出口

```text
powershell -File tools/abi-check.ps1          # PASS
powershell -File tools/release-ng.ps1         # PASS，24 files + SHA256 verified
powershell -File tools/sdk-check.ps1          # PASS，237 checks
# 产物：dist-ng/release/d0/（本地，不提交）
```
