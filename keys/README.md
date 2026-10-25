# 镜像签名密钥

## 现状（开源仓库里有什么）

| 文件 | 是否提交 | 说明 |
|---|---|---|
| `keys/shell_private.pem` | **不提交**（`.gitignore: /keys/*.pem`） | RSA-2048 私钥；`build.ps1` 用它签名 |
| `keys/shell_public.pem` | 提交 | 对应公钥（SubjectPublicKeyInfo），供审计/发布校验；公钥不是秘密 |
| `keys/README.md` | 提交 | 本文件 |

签名格式是 OpenSSL `dgst -sha256 -sign`，即 **RSA-2048 + SHA-256 + PKCS#1 v1.5**。
`sign_image.ps1` 在每次构建时从私钥推导 256 字节模数，写进 `obj/shell_signature.c`；
内核 `security/trust.c` 只内嵌这个模数（`shell_modulus[256]`）并用它验签。
所以公钥不是手写进 `trust.c` 的，而是**构建时由脚本生成**——私钥换了，模数自然跟着换，
旧签名立即作废。

> 这是教学用的**构建期自签名**模型：信任锚 = 本次构建用的密钥，不是发布者的长期身份。
> 生产环境需要离线签名机/HSM、公钥固定进源码或固件、以及吊销/公证流程（见 `SECURITY.md`）。

## 生成 / 轮换

```powershell
# 首次生成（已有密钥时拒绝）
powershell -NoProfile -File tools/gen-key.ps1

# 轮换（覆盖旧密钥对；旧签名全部作废）
powershell -NoProfile -File tools/gen-key.ps1 -Force

# 重新构建并回归（必须！）
powershell -NoProfile -File build.ps1 -Test
```

`gen-key.ps1` 会：

1. `openssl genrsa -traditional` 生成 PKCS#1 私钥（与历史格式一致）；
2. `openssl rsa -check` 自检；
3. 导出公钥 `keys/shell_public.pem`；
4. 打印模数指纹 `sha256:<hex>`（发布说明里可以引用它）；
5. 在 Windows 上把私钥 ACL 收紧到仅当前用户。

**轮换检查清单**

- [ ] `keys/shell_private.pem` 仍然被 `.gitignore` 覆盖（`git check-ignore -v keys/shell_private.pem`）
- [ ] `keys/shell_public.pem` 已更新并提交
- [ ] `build.ps1` 通过（新模数写进 `obj/shell_signature.c`）
- [ ] `build.ps1 -Test` 连续 2 次全 PASS（`trust: shell signature OK` + `tampered image rejected`）
- [ ] 旧镜像/旧签名不再分发；发布说明写明"密钥轮换，旧签名作废"
- [ ] 若 CI 使用 secret：同时更新 secret，并重跑一次 `-Full`

## CI 如何用（私钥绝不进仓库）

两条路，按威胁模型选：

**A. 一次性密钥（推荐给托管/自托管 CI 的普通回归）**

因为内核信任的是本次构建生成的模数，CI 可以在 job 内现生成一对密钥：

```yaml
- name: Generate ephemeral signing key
  shell: pwsh
  run: powershell -NoProfile -File tools/gen-key.ps1
```

代价：CI 产物不能被其他构建/用户预先信任（本来也不该信任）。

**B. 固定 secret（发布构建 / 需要可复现公钥）**

把私钥放进仓库 secret（例如 `SIGNING_KEY_PEM`），构建前写入被忽略的路径：

```yaml
- name: Install signing key from secret
  shell: pwsh
  run: |
    New-Item -ItemType Directory -Force keys | Out-Null
    $pem = $env:SIGNING_KEY_PEM
    if (-not $pem) { throw 'SIGNING_KEY_PEM is not configured' }
    [IO.File]::WriteAllText('keys/shell_private.pem', $pem, (New-Object Text.UTF8Encoding($false)))
```

注意：fork PR 默认拿不到 secret；`pull_request_target` 不要把 secret 暴露给不可信代码。

## 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `sign_image: 签名私钥不存在` | 没生成密钥：`tools/gen-key.ps1`；或 CI 忘了注入 secret |
| `trust: shell signature INVALID` | 构建产物与密钥不匹配：删 `obj/` 重新 `build.ps1`（或密钥刚轮换，旧 `obj/*.o` 缓存了旧签名） |
| `unexpected RSA modulus size` | 不是 RSA-2048；用 `tools/gen-key.ps1` 重新生成 |
| 私钥误提交 | 立即轮换 + 从历史里清除（`git filter-repo`），并视为已泄露 |
