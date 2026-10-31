# ng-WP0-4：64 位工具链探针

## 结论

**BLOCKED** —— 本机没有可产出 **x86_64 ELF** 的 freestanding 工具链。

## 证据（2026-10-31）

`
toolchain64: BLOCKED (no x86_64 ELF compiler)
  x86_64-w64-mingw32-gcc -m64: exit=0 output=PE/COFF
  gcc -m64: exit=0 output=PE/COFF
`

- 链接本身成功，但产物是 **PE/COFF**（Windows），不是内核需要的 **ELF64 x86_64**；
- 未发现 x86_64-elf-gcc / x86_64-unknown-elf-gcc / clang；
- 探针脚本：`tools/check-toolchain64.ps1`（`-Keep` 保留临时产物）。

## 探针行为

- 候选顺序：`X86_64_ELF_GCC` → `x86_64-elf-gcc` → `x86_64-unknown-elf-gcc` → `clang --target=x86_64-elf` → `*-m64`；
- 实际链接 `void _start(void){ for(;;) hlt; }`（`-ffreestanding -nostdlib -Wl,-e,_start`）；
- 校验 ELF magic/class/machine（`7F 45 4C 46`、class=2、machine=62）；
- 退出码：`0`=PASS、`2`=BLOCKED、`1`=脚本错误。

## 解除阻塞（任选其一）

1. 安装 **x86_64-elf-gcc**（binutils + gcc 交叉工具链）；
2. 安装 **clang + lld**，用 `--target=x86_64-elf`；
3. 设置 `X86_64_ELF_GCC=<path>` 指向现成交叉编译器；
4. CI 镜像里预装上述之一。

## 影响

- E1（64 位核心）在工具链就绪前**不能开始**；
- 32 位 main 已冻结在 `v0.75-32bit-final`（`69ab006`），不受影响；
- WP0-3（check-domains）已通过并入库。

## 出口命令

`
powershell -File tools/check-toolchain64.ps1      # 期望：PASS
`

当前结果：`exit=2 / BLOCKED`（工具链缺失）。