# 组件模型 MVP（签名 .pkg + `_pkgd`）

状态：v0.73 开发线（阶段 2 完成）。实现：`src/kernel/pkg/`、
`src/kernel/include/pkg.h`、`tools/mkpkg.ps1`、`tools/pkg-verify.ps1`。

## 1. 目标

把"服务/应用"从内核里硬编码的 `prog_register()` 提升为**可签名安装的组件**：

- 单文件 `.pkg` 容器：清单 + 文件表 + 每文件 SHA-256 + RSA-2048 签名；
- `_pkgd` 负责 `install / list / remove / verify`；
- 安装后由 supervisor 拉起服务；
- 合法/坏签名/缺依赖/回滚/卸载运行中服务/重复/越权/用户应用 8 类回归进
  `build.ps1 -Test` 检查项。

## 2. 容器格式（全部小端）

```
偏移            内容
0               header (64B, pkg_header_t)
manifest_off    manifest 文本（key=value，每行一条）
filetab_off     file_count × pkg_file_ent_t (76B)
data_off        文件数据区
sig_off         RSA-2048 签名 (256B)
```

header 字段：`magic="APKG"`、`version=1`、`type`(1=service,2=app)、
`manifest_off/len`、`file_count`、`filetab_off`、`data_off/len`、`sig_off`、
`total_len`、`flags`(必须 0)、`signer[16]`。

文件表项：`name[32]`、`offset`（相对 data_off）、`size`、`mode`、
`sha256[32]`。数据区必须**连续、无空洞、无重叠**，且总和等于 `data_len`。

签名覆盖 `[0, sig_off)` 的全部字节（header + manifest + 文件表 + 数据），
用 `keys/shell_private.pem` 签、内核内嵌的 `pkg_modulus[256]` 验
（PKCS#1 v1.5 + SHA-256，复用 `security/trust.c`）。

### manifest 字段

| 字段 | 说明 |
| --- | --- |
| `name` | 组件名，`[a-z0-9][a-z0-9._-]*`，< 32 |
| `version` | 版本，`[0-9][0-9A-Za-z._-]*`，< 16 |
| `type` | `service` / `app` |
| `entry` | service：程序名（内核程序表）或将来的可执行路径；app：`bin/<name>` |
| `requires` | 逗号分隔：`service:<name>`（服务注册表）、`abi:<major>.<minor>`（≤ 内核 ABI） |
| `capabilities` | 逗号分隔的 `CAP_*`；未知名字、app 申请任何 cap 都拒绝 |
| `account` | service 必须 `root`；app 必须是**安装者自己**的用户名 |
| `files` | 与文件表逐项同名同序的逗号列表 |

`CAP_SYS_ADMIN` / `CAP_PKG_ADMIN` 是**签名更新器保留位**：普通组件包申请会被
`PKG_E_POLICY` 拒绝（否则装一个包就等于拿到改系统目录的权限），只有内核里的
`_pkgd` 自己持有。

## 3. 目录与组件 DB

```
system:/System/Packages/<name>/<version>/<file>     系统组件
<home>/Apps/<name>/<version>/<file>                 用户应用
system:/System/.staging/<name>/                     staging（失败回滚时删掉）
system:/System/packages.db                          系统组件 DB
<home>/Apps/apps.db                                 用户应用 DB
```

DB 是纯文本，一行一个组件：

```
name=echosvc version=1.0.0 type=service entry=echosvc euid=0 root=system:/System/Packages/echosvc/1.0.0 files=1
```

可读、可审计、可直接 diff；`_pkgd list` 只是把它按 `name version type`
打印出来。

## 4. 安装流程

1. **结构校验**：magic/版本/各段边界/文件表连续/名字合法/不重名；
2. **manifest**：8 个字段齐全、type 与 header 一致、capabilities 合法；
3. **文件哈希**：数据区每个文件重新算 SHA-256，与文件表比对；
4. **签名**：`trust_verify_image(container[0,sig_off), sig, pkg_modulus)`；
5. **requires**：`service:` 查服务注册表，`abi:` 与 `OS_ABI_VERSION_MAJOR/MINOR`
   比较（只接受同 major、minor ≤ 当前）；
6. **策略**：service 需 `euid==0` + `account=root`；app 需 `account==` 调用者
   用户名（未知 uid 拒绝）；
7. **staging + 逐文件复验**：写入 `system:/System/.staging/<name>/`，写回后再算
   一次 SHA-256（防"签名对但落盘错"）；
8. **原子 rename** 到正式目录（目标是非空目录时失败 → 回滚只删 staging，
   **不碰**原本就在目标上的内容）；
9. **DB 追加**（失败 → 删掉刚 rename 的目录，保持无残留）；
10. **通知 supervisor**：发 `start:<entry>`，由 supervisor `proc_spawn` 拉起。

整个安装/卸载过程处于 **SIP-lite 的签名更新器作用域**（`sip_updater_enter/exit`，
实现在 `src/kernel/fs/sip.c`）：`system:/System`、`system:/bin`、`system:/drivers`
对所有人（含 root）只读，只有 `_pkgd` 进入作用域的那个线程可以写。作用域绑定
pid，不是全局开关，所以别的线程不会被顺带放行。详见 `docs/security-model.md` 4.13。

卸载：先发 `stop:<entry>`（supervisor 停服务并等注册表里消失），再递归删组件
目录与 DB 行。普通用户只能卸载自己 DB 里的 `type=app`。

## 5. `_pkgd` 服务协议

服务名 `pkgd`（服务注册表），请求/回复都是 ≤64 字节文本（channel 上限）：

| 请求 | 回复 |
| --- | --- |
| `install <file.pkg>` | `ok <detail>` / `err <code> <detail>` |
| `remove <name>` | 同上 |
| `verify <name>` | 同上（DB 行 + 目录 + 每个文件哈希 vs 内建包目录） |
| `list` | `ok <name version type>...` / `ok empty` |

`install` 的包来自**构建期内建包目录**（`obj/pkg_catalog.c`，由 build.ps1
生成），这样回归不依赖 FAT 镜像里的文件；客户端身份取 `proc_current()`
的 euid，不由请求文本自述（不能靠改命令行冒充 root）。

## 6. 构建期工具

```powershell
# 打包 + 签名 + 生成内核 C 数组
powershell -NoProfile -File tools/mkpkg.ps1 `
    -Name echosvc -Version 1.0.0 -Type service -Entry echosvc `
    -Requires "service:supervisor,abi:1.3" -Capabilities CAP_SVC_REGISTER `
    -Account root -RootDir packages/echosvc -Files echosvc.conf `
    -Out obj/pkg/echosvc-1.0.0.pkg -OutC obj/pkg_echosvc.c `
    -ArrayPrefix pkg_blob_echosvc -ModulusC obj/pkg_modulus.c

# 宿主侧校验（与内核同一套规则）
powershell -NoProfile -File tools/pkg-verify.ps1 -Package obj/pkg/echosvc-1.0.0.pkg
```

`build.ps1` 会打包 4 个测试包（`echosvc`、`needsdep`、`broken`、`helloapp`），
每个都先过 `pkg-verify.ps1` 再编译进内核；成品 `dist/echosvc-1.0.0.pkg`
可直接拿走检查。

## 7. 回归（`build.ps1 -Test`，15 项）

| 断言行 | 覆盖 |
| --- | --- |
| `pkgd: service online port=N` | 服务注册 |
| `pkg: install echosvc-1.0.0 ok` | 合法安装 |
| `pkg: installed ... -> system:/System/Packages/echosvc/1.0.0 (1 file(s))` | 落盘位置 |
| `pkg: supervisor start echosvc ok` | 安装后通知 supervisor |
| `pkg: duplicate install rejected (exist)` | 重复安装 |
| `pkg: bad signature rejected (signature)` | 坏签名（只改签名区） |
| `pkg: missing requires rejected (requires)` | 缺依赖 |
| `pkg: install failure rollback ok` | rename 失败回滚、DB/staging 干净 |
| `pkg: uninstall running service ok` | 卸载运行中服务（先停） |
| `pkg: non-root system install denied (policy)` | 普通用户越权 |
| `pkg: user app installed to /home/alice/Apps ok` | 用户应用 + 归属 |
| `pkg: verify installed echosvc ok` | 已安装完整性 |
| `pkg: list shows echosvc 1.0.0 service` | 组件 DB |
| `pkgd: service protocol ok` | 服务协议 |
| `pkg: reinstall service online ok` | 重装 + 服务重新上线 |

## 8. 已知限制（下一阶段）

1. `entry` 目前指向**内核内建程序**（`echosvc` 还是编译进内核的服务线程），
   不是从组件目录加载 ELF。拆 ring3 服务时把 `entry` 解释为
   `system:/System/Packages/.../<entry>` 即可，容器格式不用改。
2. 同名组件只能有一个版本，没有升级/回滚到旧版本（`install` 拒绝已存在）。
3. 没有用户态 `pkg` 命令：目前只有内核 API + `_pkgd` 服务协议 + 自检驱动。
4. 组件 DB 与组件目录的落盘依赖显式 `sync`（`disk_selftest` 在安装之前跑）。
5. 签名密钥是单一构建密钥（`keys/shell_private.pem`），没有多签名/吊销。
