# ng-WP2-5：auth / log / pkg 移植（PASS）

## 结论

**PASS** —— E2 / WP2-5 出口达成，`src/services/{auth,log,pkg}` 成为继
`src/services/fs` 之后的 L1 系统服务：

- **auth**：用户表 root(uid 0) / alice(uid 1000)；口令用 **16 字节 salt +
  SHA-256 迭代 4096 轮**存储（自实现 `sha256.c`，不依赖内核/OpenSSL/libc）；
  `auth_init()` 种入 `/etc/passwd` 风格记录（ramfs），
  `auth_login(name,pass)` 返回 uid / `-ENOENT` / `-EACCES` / `-EINVAL`；
  连续失败 5 次锁定（正确口令也拒绝，重新 init 清零）。
- **log**：**64 KiB（65536 字节）按行环形缓冲**；`log_write(tag,msg)` /
  `log_read(buf,max)`；满时丢最旧整行；tag 1..15、msg 1..127、仅可打印
  ASCII；内核启动/每个 WP 汇总/panic 写入 log。
- **pkg**：ramfs `/System/Packages/<name>/<ver>/manifest` + entry 文件；
  `pkg_init()` 种入示例包 `hello/1.0.0`；`pkg_list(buf,max)`、
  `pkg_verify(name)`（清单字段 + entry 文件 SHA-256 重算比对）；
  **不做签名/RSA**（D1），差异见下文。
- **syscall**：**31 login / 32 dmesg / 33 log_write / 34 pkg_list /
  35 pkg_verify**；用户指针全部经 `uaccess.c`（copyin_str/copyin/copyout），
  坏指针 `-EFAULT`；内核自检用一页临时 `U=1` 映射跑成功路径，ring3
  `hello.S` 再走一遍（`ng: ring3 auth ok` / `ring3 pkg ok` / `ring3 svc ok`）。
- **门禁**：unit-ng **2791 checks, 0 failures**（+269）；run-ng
  **92/92 期望行 + 36 条正则**（Fallback 91/91 + 30；qemu64 92/92 + 36；
  NoQmp 92/92 + 36）；旧 32 位 **305/305**；check-domains / check-deps
  0 违规；ci-ng **6/6 PASS**。

`src/kernel/**`、`build.ps1`、`tools/ci.ps1`、`abi.h` 未修改；不做 shell
（WP2-6）、不做 fsd 进程化、不做签名/RSA。

## 交付物

| 路径 | 内容 |
|---|---|
| `src/services/auth/include/sha256.h`、`src/services/auth/sha256.c` | FIPS 180-4 SHA-256：流式 API、hex 编解码（**新**） |
| `src/services/auth/include/auth.h`、`src/services/auth/auth.c` | 用户表、salt 派生、迭代哈希、`/etc/passwd` 种子、登录/锁定（**新**） |
| `src/services/log/include/log.h`、`src/services/log/log.c` | 64 KiB 按行环形缓冲、校验、最近行读取（**新**） |
| `src/services/pkg/include/pkg.h`、`src/services/pkg/pkg.c` | 包目录、manifest 解析、列表、SHA-256 校验、hello 种子（**新**） |
| `src/ng/svc_syscall.h`、`src/ng/svc_syscall.c` | syscall 31..35 用户指针胶水（**新**） |
| `src/ng/wp25.h`、`src/ng/wp25_selftest.c` | 内核自检：auth/log/pkg + 临时用户页上的 31..35（**新**） |
| `src/ng/syscall.h`、`src/ng/syscall.c` | 追加 31..35 调用号与分发（**改**） |
| `src/ng/main.c` | `log_init` 最早调用、启动/各 WP 汇总写 log、WP2-5 自检接线（**改**） |
| `src/ng/trap.c` | panic 写 `panic: unrecoverable exception`（**改**） |
| `src/ng/user/hello.S` | ring3 svc demo：31/33/32/34/35（**改**） |
| `tests/unit-ng/test_auth.c` | SHA-256 向量/分块/hex + auth 种子/登录/锁定（**新**） |
| `tests/unit-ng/test_log.c` | 行格式/读取语义/校验/精确 64 KiB 溢出（**新**） |
| `tests/unit-ng/test_pkg.c` | 清单解析矩阵 + 列表边界 + 篡改/哈希/字段校验（**新**） |
| `tests/unit-ng/test_syscall.c` | 31..35 分发/EFAULT/EINVAL 用例（**改**） |
| `tests/unit-ng/unit_main.c`、`tools/unit-ng.ps1` | 注册 3 组测试 + 源码/include（**改**） |
| `tools/build-ng.ps1` | 加 auth/log/pkg/svc_syscall/wp25 源与 include，49 步（**改**） |
| `tools/run-ng.ps1` | +12 固定行、+2 正则、WP2-5 失败模式（**改**） |
| `docs/design/auth-log-pkg-ng.md` | 设计/ABI/限制/D1 差异（**新**） |
| `docs/design/evolution.md`、`docs/design/README.md` | E2 进度/索引（**改**） |

## 环境

- 分支 `ng`，基线 HEAD `546b019`（WP2-4），仓库 `D:\OS_Project`。
- clang 23.1.4 / ld.lld 23.1.4（`D:\msys64\mingw64\bin`）；内核
  `clang --target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib
  -mno-red-zone -fno-stack-protector -mgeneral-regs-only`。
- QEMU 11.0.0：`-kernel dist-ng/aster-ng.elf -m 256 -cpu max -vga std
  -drive file=dist-ng/disk.img,format=raw,if=ide -display none`。

## 关键参数

### auth 哈希

| 项 | 值 |
|---|---|
| 算法 | SHA-256（FIPS 180-4，自实现） |
| salt | 16 字节 = `SHA-256("aster-ng-auth-salt:"+name)[0..15]`（D0 可重现；非随机源） |
| 迭代 | **4096 轮**：`h0=SHA256(salt||pass)`，`h(i)=SHA256(h(i-1)||salt)` |
| 记录 | `name:uid:salt_hex(32):hash_hex(64):iterations` |
| 锁定 | 连续失败 5 次，重新 `auth_init()` 清零；无时钟超时解锁 |
| 用户 | root uid 0（口令 `root`）、alice uid 1000（口令 `alice`） |

### log

| 项 | 值 |
|---|---|
| 容量 | **65536 字节**（按整行） |
| 行 | `tag: msg\n`，最长 145 字节 |
| tag/msg | 1..15 / 1..127，0x20..0x7E，tag 不含 `:` |
| 读 | 最近整行（不足则最新行末尾 max 字节）；syscall 32 上限 4096 |
| 内核写入 | `boot`（hello/paging）、`selftest`（WP PASS/FAIL）、`panic` |

### pkg manifest

```
name=hello
version=1.0.0
type=app
entry=bin/hello
sha256=<64 hex = SHA-256(entry 文件内容)>
```

- 允许注释/空行/CRLF/`key = value` 空白；未知/重复/缺字段/非法值 -> `EINVAL`；
- `name` `[a-z0-9._-]{1,31}`，`version` `[A-Za-z0-9._-]{1,15}`，
  `type ∈ {app,service,lib}`，`entry` 安全相对路径（无 `.`/`..`/空组件）；
- `pkg_verify` 哈希不符 -> `-EIO(-5)`，包/版本/清单缺失 -> `-ENOENT`。

## 证据（2026-10-31）

### 构建

```text
powershell -File tools/build-ng.ps1
=== build-ng (WP2-5) ===
...
user-elf: PASS (15872 bytes, 3 PT_LOAD, entry 0x40000000, end 0x400023b0)
...
  [38/49] src/services/auth/sha256.c
  [39/49] src/services/auth/auth.c
  [40/49] src/services/log/log.c
  [41/49] src/services/pkg/pkg.c
  ...
  [49/49] link -> dist-ng/aster-ng.elf
ELF64: PASS (1681992 bytes)
  class    2 (ELF64)
  machine  62 (EM_X86_64)
  type     2 (ET_EXEC)
  entry    0x0000000000101000
  phnum    7
segments: PASS
  5 PT_LOAD, 4 KiB aligned (per-section pages)
multiboot2: PASS
pvh-note: PASS
build-ng: PASS -> D:\OS_Project\dist-ng\aster-ng.elf
```

### host 单测

```text
powershell -File tools/unit-ng.ps1
=== unit-ng (WP1-8 host tests) ===
clang: D:\msys64\mingw64\bin\clang.exe
2791 checks, 0 failures
unit-ng: PASS (2791 checks, 0 failures)
```

WP2-4 为 2522，本 WP **+269**：

- `test_auth.c`：SHA-256 空串/`abc`/56/112 字节/1,000,000 `a` 标准向量、
  分块 update 一致性、跨块边界、hex 大小写/长度；auth_init 用户数/uid/
  iterations、`/etc/passwd` 行数/5 字段/长度、salt/hash 与重算一致、
  iterations/口令/salt 改变结果、rogue 参数 EINVAL；登录成功/错口令/
  未知用户/空参数、成功清零失败计数、5 次锁定/正确口令拒绝/重 init 解锁；
- `test_log.c`：行格式/顺序、max=10/5/4/1 的最近行语义、tag/msg 长度与
  控制字符拒绝、15+127 字节正好 145、1024×64 字节精确填满 64 KiB、
  第 1025 行丢最旧行（`dropped=1`、首行 `0001`、末行 `1024`）、重置；
- `test_pkg.c`：种子 manifest/文件/哈希一致、`pkg_list` 13/12 字节边界、
  `pkg_verify` 成功/ENOENT/非法名、清单合法（含 CRLF/注释/空格）与 20+
  非法路径（缺字段/重复/未知/非法 name/version/type/entry/sha）、
  篡改文件 -> EIO、manifest sha 不符 -> EIO、name 不符 -> EINVAL、
  第二个包 beta 列表/精确 23/24 字节边界/verify；
- `test_syscall.c`：31 root/alice/错口令/EFAULT、33 -> 0、32 读回内容与
  0 长度 EINVAL、34 -> 1、35 hello/nope。

### QEMU（主路径）

```text
powershell -File tools/run-ng.ps1
=== run-ng (WP2-5) ===
...
ng: WP2-4 selftest PASS
ng: auth users ok (2)
ng: auth login root ok (uid=0)
ng: auth login alice ok (uid=1000)
ng: auth bad password rejected
ng: log ring ok (bytes=235)
ng: dmesg syscall ok
ng: pkg list ok (n=1)
ng: pkg verify ok
ng: WP2-5 selftest PASS
...
ng: ring3 auth ok
ng: ring3 pkg ok
ng: ring3 svc ok
ng: ring3 input ok (key=a)
ng: ring3 exit ok
ng: WP1-7 selftest PASS
run-ng: matched 92/92 expected lines + 36 patterns
run-ng: PASS (all expected lines in 1.2s, cpu max)
```

`run-ng.ps1` 新增期望：

```text
12 条固定行：
  ng: auth users ok (2)
  ng: auth login root ok (uid=0)
  ng: auth login alice ok (uid=1000)
  ng: auth bad password rejected
  ng: log ring ok
  ng: dmesg syscall ok
  ng: pkg list ok
  ng: pkg verify ok
  ng: WP2-5 selftest PASS
  ng: ring3 auth ok / ng: ring3 pkg ok / ng: ring3 svc ok
2 条正则：
  ng: log ring ok \(bytes=[0-9]+\)
  ng: pkg list ok \(n=1\)
失败早停模式新增：
  ng: auth .* FAIL / ng: log .* FAIL / ng: pkg .* FAIL /
  ng: WP2-5 selftest FAIL
```

### 负向 / 降级 / 其他 CPU

```text
run-ng -Fallback       -> 91/91 lines + 30 patterns PASS（-vga none，VGA 文本）
run-ng -NoQmp          -> 92/92 lines + 36 patterns PASS（readkey timeout）
run-ng -Cpu qemu64     -> 92/92 lines + 36 patterns PASS（SMEP/SMAP off）
build-ng -PanicTest + run-ng -Panic
  -> PANIC: vec=0x6 err=0x0 rip=0x112bd1 cr2=0x123000；1/1 line + 1 pattern PASS
```

### 门禁

```text
tools/check-domains.ps1
checked: 234 files, 763 quoted includes
domain include violations: 0

tools/check-deps.ps1
interface layer violations: 0
module include violations: 0
OK: no cross-module include violations.

tools/ci-ng.ps1
  PASS check-domains    0 violations
  PASS check-deps       0 violations
  PASS unit-ng          2791 checks
  PASS build-ng         1681992 bytes, user-elf 15872 bytes
  PASS run-ng           92/92 lines + 36 patterns
  PASS legacy-32bit     305/305 parity checks
  === NG CI PASS ===
```

旧 32 位独立跑：第一次出现 3 项 `pkg CLI install/越权拒绝/完成` 失败
（302/305），紧接着重跑 **305/305**，ci-ng 内再跑也是 **305/305**；
与 `docs/parallel-sessions.md` 记录的旧树用户态偶发同族，本 WP 未改
`src/kernel/**`。

## syscall ABI（摘要）

```c
31 login(name, pass)        -> uid / -ENOENT / -EACCES / -EINVAL / -EFAULT
32 dmesg(buf, max)          -> bytes（max <= 4096）/ -EINVAL / -EFAULT
33 log_write(tag, msg)      -> 0 / -EINVAL / -EFAULT
34 pkg_list(buf, max)       -> count（max <= 512）/ -ENOSPC / -EINVAL / -EFAULT
35 pkg_verify(name)         -> 0 / -ENOENT / -EINVAL / -EIO / -EFAULT
```

用户指针全部经 `copyin_str`/`copyin`/`copyout`；内核 CR3 下坏指针返回
EFAULT，不触发 #PF。内核自检把一页 PMM 以 `U=1` 映射到 `0x60000000`，
用真实用户 VA 验证 31..35 成功路径；测试后 unmap + 归还。

## 自检项清单

1. auth：2 用户、uid 0/1000、salt 16B、SHA-256 4096 轮、`/etc/passwd`
   5 字段/2 行、登录成功/错口令/未知用户/空参数、5 次锁定、重 init 解锁。
2. log：64 KiB 精确填满、第 1025 行丢最旧、最近行读取（含 max 小于行）、
   长度/控制字符拒绝、内核 boot/selftest/panic 写入。
3. pkg：hello 种子、manifest 5 字段严格解析、list 计数/缓冲边界、
   verify 成功/ENOENT/字段不符/entry 越界/哈希不符、第二包列表。
4. syscall：31..35 用户指针成功路径、EFAULT、EINVAL、ENOSPC/EIO；
   ring3 同样调用并打印证据行。
5. host 2791/0；run 92/36（Fallback 91/30、qemu64 92/36、NoQmp 92/36）；
   旧 32 位 305/305；check-domains/check-deps 0；ci-ng 6/6；panic 负向 PASS。

## 未做 / 遗留（按 WP 边界）

- **签名/RSA**：pkg_verify 只校验清单字段 + 文件哈希，不验证发布者；D1 在
  manifest 加签名/公钥链并走签名更新通道。
- **随机 salt**：D0 用 SHA-256 派生可重现 salt；D1 接熵源（getrandom/TPM）
  并支持参数迁移。
- **口令常量时间**：当前逐字节 OR 累积，不是严格常量时间；D1 用专门的
  constant-time compare，并拆 `/etc/shadow`、加审计。
- **auth 文件权威性**：内存表是登录判定源，`/etc/passwd` 是种子/导出；
  文件加载/原子更新/锁定持久化留给后续 WP。
- **log**：无行索引、无优先级/限速/持久化；D1 由 logd 进程接管。
- **pkg**：无依赖解析、安装事务、卸载、网络仓库；`pkg_list` 静态上限
  16 包/8 版本。
- **服务进程化**：auth/log/pkg 仍以 L1 库链接进内核（D0 允许）；port+cap
  的进程化是后续 WP。
- **单核协作式**：服务无锁；多核/抢占时加 spinlock，API 不变。

## 出口

```text
powershell -File tools/build-ng.ps1    # PASS，1681992 bytes，49 步
powershell -File tools/run-ng.ps1      # PASS，92 行 + 36 正则
powershell -File tools/unit-ng.ps1     # PASS，2791 checks, 0 failures
powershell -File build.ps1 -Test       # PASS，305/305（旧 32 位）
powershell -File tools/check-domains.ps1   # 0 违规（234 files）
powershell -File tools/check-deps.ps1      # 0 违规
powershell -File tools/ci-ng.ps1           # 6/6 PASS
```
