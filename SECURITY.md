# 安全策略（Security Policy）

Aster / x86 OS 是一个**教学与实验性质**的 32 位操作系统。它可以启动、跑用户态 shell、
做进程/文件系统/网络/账户隔离的最小演示，但**不要把它当作生产系统**。

## 支持范围

| 版本 | 状态 |
|---|---|
| `0.71.x`（当前主线，ABI v1.3） | 接受安全报告，尽力修复 |
| 更早的 tag / fork | 不保证 |

安全边界以 `docs/security-model.md`、`docs/api.md` 与 `docs/native-abi.md` 描述为准；
文档里明确写为"教学简化"的部分（例如下面"已知限制"）不算未知漏洞，但欢迎讨论改进。

## 报告漏洞

**不要用公开 issue 报安全问题。**

- 仓库托管到 GitHub 之后：优先用 *Security → Report a vulnerability*（私有 advisory）。
- 本地/离线阶段：直接联系当前维护者，或在私有渠道提交，附上：
  - 受影响版本（`git log -1 --oneline`）与构建参数；
  - 复现步骤/最小输入（QEMU 命令、串口日志、`-Test` 输出）；
  - 影响评估（提权？越界？拒绝服务？签名绕过？）。

我们会在确认后：

1. 复现并给出严重性判断；
2. 在私有分支修复 + 加回归检查；
3. 发布补丁与说明（必要时轮换签名密钥）。

## 已知限制（设计取舍，不是"待修 bug"）

- **构建期自签名**：内核信任的是本次构建用私钥推导出的模数（`obj/shell_signature.c`）。
  这能防"镜像被替换后仍能启动"，但**不能**证明发布者身份；长期信任锚需要离线签名/HSM。
- **RSA 验签不是常数时间**：`security/trust.c` 的大数模幂是教学实现，不抗侧信道。
- **口令摘要为 SHA-256（无盐）**：`/etc/shadow` 的格式是教学简化，抗离线爆破能力有限。
- **单机 QEMU 模型**：SMP 支持有限、无 ASLR、无 IOMMU、无 SMEP/SMAP，依赖 ring 0/3 与页表隔离。
- 私钥只应存在于本地或 CI secret；**仓库里永远不会有私钥**（见 `keys/README.md`）。

## 密钥管理

- 私钥文件：`keys/shell_private.pem`（`.gitignore: /keys/*.pem`，不提交、不上传、不进日志）。
- 公钥：`keys/shell_public.pem`（可提交；公钥不是秘密）。
- 生成/轮换：`powershell -NoProfile -File tools/gen-key.ps1 [-Force]`，
  随后必须 `build.ps1 -Test` 全 PASS（旧签名作废）。
- CI：用 secret 注入私钥，或在 job 内生成一次性密钥；fork PR 不暴露 secret。
- **私钥泄露处置**：立即 `tools/gen-key.ps1 -Force` 轮换、更新 secret、重建并重新发布；
  把泄露的密钥视为永久失效，不要试图"从历史里删掉就没事"。
- **历史中的旧测试私钥**：初始提交 `8cb1413` 曾包含 `keys/shell_private.pem`，`44cb980`
  删除并轮换了密钥（旧签名已作废）。开源前建议用 `git filter-repo` 重写历史；
  不重写也不影响新构建的信任链，但应视为"已泄露的旧测试密钥"。详见 `keys/README.md`。

## 供应链 / 发布完整性

- `tools/release.ps1` 生成的发布说明包含 `sha256` 校验和与模数指纹（`sha256:<hex>`）。
- 发布产物请与 `keys/shell_public.pem` 的模数指纹核对；不一致即拒绝。
- 依赖：交叉工具链 `tools/i686-elf`（约 3.5GB，gitignored）需自行获取；
  完整清单与校验方式见 README 的"自带交叉工具链"一节。

## 本仓库的自动检查

- `tools/unit.ps1`：host 侧单元测试（不启动 QEMU）。
- `tools/ci.ps1 -Quick -StrictBom`：提交卫生（BOM、禁用产物、模块边界）。
- `tools/ci.ps1 -Full`：构建 + 连续 2 次 `build.ps1 -Test` 全 PASS。
- `tools/pre-commit.ps1` + `tools/githooks/pre-commit`：提交时拦截无 BOM 脚本/构建产物。

## 披露致谢

默认在修复发布后致谢报告者；如希望匿名请提前说明。
