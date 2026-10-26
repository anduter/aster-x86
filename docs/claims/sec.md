# sec 会话 claims（P0：shadow PBKDF2 / 审计环 / getty 重启）

提交范围：`src/kernel/security/**`、`src/kernel/include/security.h`（未改动，仅确认接口）、
`src/kernel/apps/getty.*`、`src/kernel/core/bootseq.c`（仅注释级确认，无功能改动）。
未改 `build.ps1`，未新增 syscall。

## 新增 selftest 期望断言行（建议加进 build.ps1 检查项）

```text
acct: pbkdf2 ok
acct: shadow migrate ok
audit: ring 128 ok
audit: append flush ok
```

说明：

| 行 | 覆盖内容 |
|---|---|
| `acct: pbkdf2 ok` | PBKDF2-HMAC-SHA512 已知向量（"password"/"salt"/1/32 与 hashlib 一致）、`pbkdf2$sha512$10000$...` 格式与长度、正确口令通过、错口令拒绝、随机盐两次摘要不同 |
| `acct: shadow migrate ok` | 把 root 的 shadow 行种回旧格式（单次 sha256），走真正的 `password_check()` 登录校验，成功后自动改写为 PBKDF2，且新摘要可校验/错口令被拒 |
| `audit: ring 128 ok` | 审计环容量确为 128 行：压入 130 条后 `last` 仍为 128 行，最旧 2 条被覆盖，最新一条保留（测完恢复原环） |
| `audit: append flush ok` | `acct_audit_flush()` 为追加语义：两次 flush 后文件变大，且历史记录与新增记录都在 `/var/log/audit.log` 里 |

既有断言行保持不变（新格式仍固定比较 64 个十六进制字符）：

```text
acct: password verify ok ct-steps=64
acct: wrong password rejected ct-steps=64
acct: constant-time compare equal iterations ok
acct: sha256 hex length=64 stable=1 ok
acct: selftest PASS
```

## shadow 存储格式

```text
name:pbkdf2$sha512$10000$<32 hex salt>$<64 hex digest>:0:0:99999:7:::
```

- 盐：16 字节随机（rdtsc/时钟/pid/计数器混合），32 个十六进制字符；
- 迭代：10000；输出 32 字节（64 个十六进制字符）；
- 整串长度 117 字符；
- 旧格式 `name:<64 hex sha256>:...` 仍可登录，**成功登录后自动迁移**（`audit: SHADOW_MIGRATE ...`）；
- `passwd` 与 `useradd` 的写回：`passwd` 直接写 PBKDF2；`useradd` 仍先写 `*`（锁定），
  之后 `passwd <user>` 写入 PBKDF2（与迁移共用 `shadow_write_hash()`，0600 root）。

示例（salt 为示例值，实际每次随机）：

```text
root:pbkdf2$sha512$10000$0123456789abcdef0123456789abcdef$<64 hex PBKDF2 output>:0:0:99999:7:::
```

## 审计

- 环容量 24 → **128** 行（`ACCT_AUDIT_LINES`），串口行格式不变：`audit: TYPE detail`。
- `sync` 刷盘改为**追加**：只追加自上次成功刷盘后的新事件；文件超过
  `ACCT_AUDIT_FILE_MAX`（16 KB）时截断旧文件重建（轮转），避免无限增长。

## getty 重启（生产路径，-Test 不走）

`bootseq -> proc_spawn("getty")` 现在启动一个 **getty supervisor**：

- 父 getty `spawn` 子 getty 并 `proc_wait`；
- 子 getty 置 `login_required` 后 `exec /bin/shell`；
- shell `exit` 后子进程结束，父 getty 立即重新 `spawn`，tty0 回到 `login:`，
  不再转 idle。

该行为只在生产启动（无 `autologin=root`）可见；`-Test` 仍直接 `proc_spawn("shell")`，
因此不新增可断言行，但生产日志会打印：

```text
getty: tty0 supervisor pid=<n> (respawn login after exit)
getty: tty0 session pid=<n> ended status=<n> -> respawn login
```

## 测试

共享树（`tools\session-run.ps1 -Name sec -Test`，2026-10-27 02:00 左右）：
本会话相关行全部 PASS：

```text
acct: pbkdf2 ok
acct: shadow migrate ok
audit: ring 128 ok
audit: append flush ok
acct: password verify ok ct-steps=64
acct: wrong password rejected ct-steps=64
acct: constant-time compare equal iterations ok
acct: sha256 hex length=64 stable=1 ok
acct: selftest PASS
```

同轮 182 项里 19 项 FAIL 全部来自当时其它会话在飞的改动（pkg 11 项组件 ELF/pkgcli、
net 5 项 listen/accept/TCP、1 项 ELF argv），与 security/ 路径无关。

隔离 worktree 验证（`D:\OS_Project_sec_iso`，HEAD `8767724` + 本会话补丁，
`tools\session-run.ps1 -Repo ... -Name seciso -Test`）：
- 本会话新增/既有安全行同样全部 PASS（日志 `logs-session-seciso.log`）；
- 额外做了 `/bin/shell` 完整性探针：`after-demo / after-disk / after-acct /
  after-backend / after-link / after-sip` 六处 `trust=1`（镜像未被账号/审计自检破坏）；
- 该 HEAD 的完整 182 未全绿：ring3 shell 在 **pkgtest 并发安装组件之后**读取
  `/bin/shell` 被信任校验拒绝（`trust: rejected unsigned image`），属 pkg/fs 基线
  在飞问题；同一工作树换到包含 fs 修复与 pkg 工作区改动的共享树后 shell 正常启动。
  等父会话集成 pkg/fs 后统一复跑 182。

命令：

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools\session-run.ps1 -Name sec -Test
```
