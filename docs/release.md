# 发布与基准

发版与性能回归的固定流程。目标：**一条命令产出可校验的发布产物**，且不伪造任何
"CI 已通过 / 已在 GitHub 执行"的结论。

| 工具 | 作用 |
|---|---|
| `tools/release.ps1` | bump `version.h` → changelog（git log）→ 构建/签名 → SHA256SUMS → 打 tag（**不 push**） |
| `tools/bench.ps1` | 记录内核字节数、全量/增量构建耗时、`-Test` 耗时、启动到 `ushell: ready` → `logs/bench.csv` |
| `tools/gen-key.ps1` | 生成/轮换签名密钥对（私钥 gitignored） |
| `tools/ci.ps1 -Full` | 发版前必须通过：构建 + 连续 2 次 `build.ps1 -Test` 全 PASS |

## 1. 版本号

`src/kernel/core/version.h` 是唯一来源：

```c
#define OS_NAME     "x86 OS"
#define OS_VERSION  "0.71.0"
```

- 版本号用 `x.y.z`；`release.ps1` 只改 `OS_VERSION`；
- 标题栏、日志、README 都引用它，不要在别处硬编码版本；
- 版本号提交后打 tag：`v<version>`（附注 tag，带 release 消息）。

## 2. 发版

**前置**（缺一不可）：

1. 工作区干净（`git status --porcelain` 为空）；并行会话期间先协调，发版脚本默认拒绝脏工作区；
2. 签名私钥就位：`keys/shell_private.pem`（`tools/gen-key.ps1` 生成，gitignored）；
3. `tools/i686-elf`、`nasm`、`qemu-system-i386`、`openssl` 就位；
4. 先在**无 QEMU 冲突**时跑一次完整回归：`tools/ci.ps1 -Full`（2×`-Test`）。

```powershell
# 完整发版：bump + changelog + commit + build/sign + checksums + tag
powershell -NoProfile -File tools/release.ps1 -Version 0.72.0

# 只看计划，不动文件
powershell -NoProfile -File tools/release.ps1 -Version 0.72.0 -DryRun

# 分步
powershell -NoProfile -File tools/release.ps1 -Version 0.72.0 -NoBuild   # 只 bump/commit/tag
powershell -NoProfile -File tools/release.ps1 -Version 0.72.0 -NoCommit -NoTag   # 只改文件+构建
```

脚本步骤与产物：

| 步骤 | 行为 |
|---|---|
| 1 | 把 `version.h` 的 `OS_VERSION` 改成 `-Version` |
| 2 | 生成 `docs/changelog/v<ver>.md`（`git log <上一个 tag>..HEAD`，无 tag 时取最近 200 条） |
| 3 | `git commit --only` 只提交 `version.h` + changelog（不会扫入别的会话 staged 的文件） |
| 4 | `build.ps1 -ObjDir obj/release -DistDir dist/release`（签名 shell + kernel） |
| 5 | 产物复制到 `dist/release/`，生成 `SHA256SUMS`、`RELEASE.md`（含模数指纹） |
| 6 | `git tag -a v<ver>`；已存在则失败（发版不覆盖） |

产物（`dist/` 已 gitignore）：

```
dist/release/floppy.img  kernel.bin  user_shell.elf  shell.sig  kernel.sig
dist/release/SHA256SUMS  RELEASE.md
docs/changelog/v<ver>.md        # 跟踪
```

**脚本不会 push。** 确认后手动执行：

```powershell
git push origin main
git push origin v0.72.0
```

## 3. 校验发布产物

```powershell
# Windows
Get-FileHash dist/release/floppy.img -Algorithm SHA256
# Git Bash / Linux
cd dist/release && sha256sum -c SHA256SUMS
```

模数指纹（`RELEASE.md` 里也有）：

```powershell
openssl rsa -pubin -in keys/shell_public.pem -noout -modulus
# 对模数 hex 做 SHA-256，与 RELEASE.md 的 sha256:<hex> 对比
```

指纹不一致 = 拒绝该产物（签名密钥被换过或产物被替换）。

## 4. 基准（bench）

```powershell
powershell -NoProfile -File tools/bench.ps1                # 1 轮：构建×2 + -Test
powershell -NoProfile -File tools/bench.ps1 -Runs 3        # 3 轮
powershell -NoProfile -File tools/bench.ps1 -SkipTest      # 只测构建（有浸泡/回归在跑时用）
```

输出追加到 `logs/bench.csv`（`logs/` 已 gitignore），列含义：

| 列 | 含义 |
|---|---|
| `time` / `git` / `version` | 采样时间、HEAD 短哈希、`version.h` 版本 |
| `kernel_bytes` / `image_bytes` | `kernel.bin` 与 `floppy.img` 字节数 |
| `full_build_s` | 删除隔离产物目录后的全量构建耗时（秒） |
| `incremental_build_s` | 紧接着再构建一次的耗时（秒） |
| `test_s` | `build.ps1 -Test` 总耗时（秒；含增量构建 + QEMU 回归） |
| `checks` | `-Test` 通过项数（`N/N`） |
| `boot_ready_s` | QEMU 启动 → `ushell: ready`（`[t before qemu]` 到 `[t boot ready]` 的差值） |
| `note` | `clean`/`dirty`（工作区状态）、`no-test` 等 |

实测样例（2026-10-26，Windows 10 + PS 5.1；完整模式，工作区 `clean`）：

```
time,git,version,kernel_bytes,image_bytes,full_build_s,incremental_build_s,test_s,checks,boot_ready_s,note
2026-10-26T03:03:53,ab59c48,0.71.0,260308,1474560,12.83,2.14,28.67,131,8.60,clean
```

只测构建（`-SkipTest`，例如浸泡/回归正在跑）：

```
2026-10-26T02:30:00,fef1a32,0.71.0,260308,1474560,13.03,2.14,,,,no-test,dirty
```

注意：

- 有 `qemu-system-i386` 进程时脚本自动跳过 `-Test`（不杀别人的 QEMU、不抢 12345 端口）；
- 只比较**同一台机器、同一 `note`（clean）**下的数字；`dirty` 行会混入其他会话的改动；
- `boot_ready_s` 是启动耗时，不含构建；`test_s` 才是整轮回归墙钟时间。

## 5. CI 的真实状态

- 本仓库当前**没有 GitHub 远端**，也没有绑定自托管 runner；`.github/workflows/*` 是
  "已写好、未在 GitHub 执行"的状态。
- 能写进发布说明的只有：**本地已验证**（`ci.ps1 -Quick -StrictBom` + `build.ps1 -Test` ×2 全 PASS）。
- 推送后：`static.yml` 会在 push/PR 自动跑；`full.yml` 需要自托管 Windows runner 且手动触发。
  以 Actions 页面结果为准，不要用本地结果替代 CI 结论。
- 私钥：CI 用仓库 secret `SIGNING_KEY_PEM` 注入，或在 job 内生成一次性密钥；仓库永不提交私钥。

### 本地验证记录（2026-10-26 工程收尾）

| 命令 | 结果 |
|---|---|
| `tools/unit.ps1` | **7/7** 用例 PASS（abi 30 / string 51 / trust 17 / pipe 46 / service 36 / fuzz-trust 2 / fuzz-pipe 5 checks） |
| `tools/ci.ps1 -Quick -StrictBom` | PASS（2.1s；28 个脚本 BOM/解析、264 个被跟踪文件无产物、check-deps 0 违规） |
| `tools/ci.ps1 -Full -StrictBom`（`5bcad98`） | **PASS**：构建 13.0s，`-Test` **131/131 ×2**（30.2s / 19.1s），结束后 worktree 干净 |
| `tools/bench.ps1 -Runs 1` | 全量 12.83s、增量 2.14s、`-Test` 28.67s、启动→ready 8.60s、131 项 |
| `tools/release.ps1 -Version 0.71.0 -NoCommit -NoTag` | 构建/签名/SHA256SUMS/RELEASE.md 生成成功；模数指纹 `sha256:eaff7f61…`（与 `keys/shell_public.pem` 一致） |

**未执行**（如实记录，不算通过）：GitHub Actions（无远端/无 runner）、`cppcheck`、`clang-tidy`（未安装）。
