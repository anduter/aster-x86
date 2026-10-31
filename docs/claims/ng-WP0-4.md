# ng-WP0-4：64 位工具链探针（PASS）

## 结论

**PASS** —— 通过 MSYS2 安装 **clang 23.1.4 + lld**，clang --target=x86_64-elf -fuse-ld=lld 可产出 **ELF64 x86_64**。

## 证据（2026-10-31）

`
toolchain64: PASS (clang --target=x86_64-elf)
  exe: D:\msys64\mingw64\bin\clang.exe
  elf: 1024 bytes, ELF64 x86_64
`

安装命令（MSYS2 pacman，已验证）：

`
D:\msys64\usr\bin\pacman.exe -Sy --noconfirm mingw-w64-x86_64-clang mingw-w64-x86_64-lld
`

探针脚本 `tools/check-toolchain64.ps1`：候选顺序 env → x86_64-elf-gcc → x86_64-unknown-elf-gcc → clang --target=x86_64-elf → *-m64；实际链接 freestanding hello；校验 ELF magic/class/machine。退出码 0=PASS、2=BLOCKED。

## 注意

- 运行探针前需把 `D:\msys64\mingw64\bin` 加入 PATH（或设置 `X86_64_ELF_GCC` 指向 clang）；
- clang 默认 target 是 windows-gnu（PE），必须显式 `--target=x86_64-elf` 才是 ELF64；
- 构建脚本（`tools/build-ng.ps1`）应显式传 `--target=x86_64-elf -fuse-ld=lld -ffreestanding -nostdlib`。

## 出口

`
powershell -File tools/check-toolchain64.ps1   # PASS
`

## 下一步

WP1-1：multiboot2 + long mode + 串口 64 位 hello。