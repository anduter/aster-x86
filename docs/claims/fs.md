# fs 会话声明（P0：XFS1 v6 硬链接持久化）

测试：`tools\session-run.ps1 -Name fs -Test`。以下行应在串口输出中出现（供后续
集成会话加进 `build.ps1` 检查项）：

```text
fs: xfs v5 image mount compat ok
fs: XFS1 v5 mounted, file limit 65536 bytes
fs: xfs v6 hardlink persisted ok
fs: XFS1 v6 mounted, file limit 65536 bytes (nlink persisted)
```

## 断言含义

| 行 | 含义 |
|---|---|
| `fs: xfs v5 image mount compat ok` | 在空闲块设备上生成一个 v5 镜像并实际挂载成功，随后恢复真实卷；证明旧镜像可读。 |
| `fs: XFS1 v5 mounted, file limit 65536 bytes` | v5 兼容挂载路径打印的版本行（保留给现有 182 项检查）。 |
| `fs: xfs v6 hardlink persisted ok` | `link a b` → `fs_sync` → `fs_init` → `fs_mount_disk` 后 a/b 的 nlink=2 且写一个名字另一个可见；删 a 后 b 可读且 nlink=1；再次 sync/remount 仍正确；fsck 对健康树报 0，对注入的 nlink=7 能发现并修回。 |
| `fs: XFS1 v6 mounted, file limit 65536 bytes (nlink persisted)` | 当前格式的挂载行。 |

## v6 磁盘格式要点

- 超级块不变（仍 512 字节），`version = 6`；`inode_size = 116`，inode 表从
  LBA 1 起占 15 个扇区，数据区从 LBA 16 开始。
- `disk_inode_t` 在 v5 的 108 字节布局尾部增加 `inode_id` 与 `nlink`（各 u32），
  变为 116 字节。同一 inode 的多个目录项各有一条记录：`inode_id` 相同、
  `nlink` 相同、数据只写一份（第一条目录项的 `data_lba`，其余复用）。
- 挂载时按 `inode_id` 分组：第一条记录建立数据宿主，后续记录重建 `canon`
  硬链接，`fs_stat_t.nlink` 来自宿主；组内目录项数会与 `nlink` 复核，不一致
  时按目录项数修正并打印 `fs: XFS1 v6 nlink repaired on mount ...`。

## 兼容策略

- v5（108 字节 inode 记录、数据区 LBA 15）与 v4（同布局、无 max_file 字段）
  都可挂载；每个目录项恢复为独立 inode（nlink=1），与 v5 的旧行为一致。
- 任意一次 `fs_sync` 一律写 v6：旧 v5/v4 镜像在下次 sync 时自动升级，无需
  迁移数据（数据区整体重写）。
- 挂载前校验几何字段、inode 类型/大小/数据区范围；结构性损坏（坏类型、
  孤儿、重复卷根、硬链接组元数据不一致）整次拒绝，nlink 计数不一致可修。

## fsck

`fs_check()` / `SYS_FSCK`：校验类型、大小、父链、`canon` 合法性与 nlink 计数；
nlink 与目录项数不符时按实际目录项数修复（返回发现的问题数，报告里带
`fixed=`），结构损坏只报告不改指针。`fs_sync` 落盘前也会先按目录项数修 nlink。

## 已知残留

- 目录 nlink 仍固定为 1（不模拟 `.` / `..`），与 v5 行为一致。
- v5/v4 镜像中的历史硬链接关系已经退化，升级 v6 后不会自动找回；新建立的
  硬链接才会持久化。
