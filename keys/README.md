# Test signing key

`shell_private.pem` 是 Aster P3 的**测试用** RSA-2048 私钥，仅用于本地构建与 QEMU 回归。

- 构建脚本用它给 `obj/user_shell.elf` 签名。
- 内核只内嵌公钥模数，不读取私钥。
- 生产环境不能把私钥放在仓库或构建机上；应使用离线签名机/HSM，并补撤销、公证与密钥轮换。
