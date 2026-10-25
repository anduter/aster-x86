# =============================================================================
# x86 OS 构建系统
#
# 依赖：nasm、i686-elf-gcc 工具链、GNU make。Windows 上没有 POSIX 环境时，
# 用 build.ps1（纯 PowerShell）走同样的流程。
#
#   make                 构建 dist/floppy.img
#   make run             构建并在 QEMU 里跑（串口打到控制台）
#   make run-headless    无窗口运行
#   make debug           QEMU 带 GDB 桩，端口 1234
#   make test            自动测试（调用 build.ps1 -Test）
#   make clean           清理构建产物
#
# 镜像布局（软盘 1.44MB）：
#   LBA 0      boot sector  (obj/boot.bin, 1 扇区)
#   LBA 1..32  loader       (obj/loader.bin, 32 扇区 = 16KB)
#   LBA 33..   kernel       (obj/kernel.bin)
# =============================================================================
SRC_DIR     := src
BOOT_DIR    := $(SRC_DIR)/boot
LOADER_DIR  := $(SRC_DIR)/loader
KERNEL_DIR  := $(SRC_DIR)/kernel
OBJ_DIR     := obj
DIST_DIR    := dist

# 工具链用 "=" 而不是 "?="：避免环境里已有的 CC=cc / LD=ld 把交叉编译器顶掉
NASM    = nasm
CC      = i686-elf-gcc
LD      = i686-elf-ld
OBJCOPY = i686-elf-objcopy
QEMU    = qemu-system-i386
PWSH    = powershell -NoProfile -ExecutionPolicy Bypass -File

# 控制台字体缩放：默认 1（紧凑）；make FONT_SCALE=2 可切换到大字体
FONT_SCALE ?= 1

CFLAGS  := -ffreestanding -fno-pic -fno-pie -m32 -nostdlib \
           -fno-asynchronous-unwind-tables -fno-stack-protector -fno-builtin \
           -MMD -MP \
           -DFONT_SCALE=$(FONT_SCALE) \
           -Wall -Wextra -Werror -g -O0 -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)
LDFLAGS := -m elf_i386 --no-warn-rwx-segments -T $(KERNEL_DIR)/arch/x86/linker.ld

FLOPPY_IMG := $(DIST_DIR)/floppy.img
BOOT_BIN   := $(OBJ_DIR)/boot.bin
LOADER_BIN := $(OBJ_DIR)/loader.bin
KERNEL_ELF := $(OBJ_DIR)/kernel.elf
KERNEL_BIN := $(OBJ_DIR)/kernel.bin

# src/kernel 下任意层级的 .c / .asm 自动参与编译：新增模块不需要改 Makefile
KERNEL_CSRCS := $(wildcard $(KERNEL_DIR)/*.c) \
                $(wildcard $(KERNEL_DIR)/*/*.c) \
                $(wildcard $(KERNEL_DIR)/*/*/*.c)
KERNEL_ASMS  := $(wildcard $(KERNEL_DIR)/*.asm) \
                $(wildcard $(KERNEL_DIR)/*/*.asm) \
                $(wildcard $(KERNEL_DIR)/*/*/*.asm)
KERNEL_OBJS  := $(patsubst $(KERNEL_DIR)/%.c,$(OBJ_DIR)/%.o,$(KERNEL_CSRCS)) \
                $(patsubst $(KERNEL_DIR)/%.asm,$(OBJ_DIR)/%.o,$(KERNEL_ASMS))

# ---- 用户态程序：独立编译，链接到 0x40000000 ----
USER_DIR    := $(SRC_DIR)/user
USER_CFLAGS := -ffreestanding -fno-pic -fno-pie -m32 -nostdlib -fno-builtin \
               -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -O0
USER_OBJ    := $(OBJ_DIR)/user_shell.o
USER_OBJS   := $(USER_OBJ) $(OBJ_DIR)/ulib.o
USER_ELF    := $(OBJ_DIR)/user_shell.elf
USER_BIN    := $(OBJ_DIR)/user_shell.bin

$(USER_OBJ): $(USER_DIR)/user_shell.c | $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/ulib.o: $(USER_DIR)/ulib.c | $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_ELF): $(USER_OBJS) $(USER_DIR)/user.ld
	$(LD) -m elf_i386 -T $(USER_DIR)/user.ld -o $@ $(USER_OBJS)

$(USER_BIN): $(USER_ELF)
	$(OBJCOPY) -O binary $< $@

# ---- shell 镜像签名：构建时 OpenSSL RSA-2048/SHA-256 ----
SIGN_KEY := keys/shell_private.pem
SIG_C    := $(OBJ_DIR)/shell_signature.c
SIG_O    := $(OBJ_DIR)/shell_signature.o

KERNEL_OBJS += $(SIG_O)

$(SIG_C): $(USER_ELF) $(SIGN_KEY) sign_image.ps1 | $(OBJ_DIR)
	$(PWSH) sign_image.ps1 -Image $(USER_ELF) -Key $(SIGN_KEY) \
		-Signature $(OBJ_DIR)/shell.sig -OutC $@

$(SIG_O): $(SIG_C) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- 启动链：kernel 签名 + loader 内嵌的 32 位 RSA 验证器 ----
KERNEL_SIG_C  := $(OBJ_DIR)/kernel_signature.c
KERNEL_SIG_O  := $(OBJ_DIR)/kernel_signature.o
BOOT_TRUST_O  := $(OBJ_DIR)/boot_trust.o
BOOT_SUP_O    := $(OBJ_DIR)/boot_verify_support.o
BOOT_ENTRY_O  := $(OBJ_DIR)/boot_verify_entry.o
BOOT_VERIFY_ELF := $(OBJ_DIR)/boot_verify.elf
BOOT_VERIFY_BIN := $(OBJ_DIR)/boot_verify.bin

$(KERNEL_SIG_C): $(KERNEL_BIN) $(SIGN_KEY) sign_image.ps1 | $(OBJ_DIR)
	$(PWSH) sign_image.ps1 -Image $(KERNEL_BIN) -Key $(SIGN_KEY) \
		-Signature $(OBJ_DIR)/kernel.sig -OutC $@ -Prefix kernel

$(KERNEL_SIG_O): $(KERNEL_SIG_C)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_TRUST_O): $(KERNEL_DIR)/security/trust.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -DBOOT_VERIFY -c $< -o $@

$(BOOT_SUP_O): $(BOOT_DIR)/verify_support.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_ENTRY_O): $(BOOT_DIR)/verify_entry.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BOOT_VERIFY_ELF): $(BOOT_ENTRY_O) $(BOOT_TRUST_O) $(BOOT_SUP_O) $(KERNEL_SIG_O) src/boot/verify.ld
	$(LD) -m elf_i386 -T src/boot/verify.ld -o $@ \
		$(BOOT_ENTRY_O) $(BOOT_TRUST_O) $(BOOT_SUP_O) $(KERNEL_SIG_O)

$(BOOT_VERIFY_BIN): $(BOOT_VERIFY_ELF)
	$(OBJCOPY) -O binary $< $@

# 内建程序映像：nasm 用 incbin 把 ELF 嵌进内核（内核侧解析程序头加载）
$(OBJ_DIR)/user/user_blob.o: $(KERNEL_DIR)/user/user_blob.asm $(USER_ELF) | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -i $(OBJ_DIR) $< -o $@

.PHONY: all check-tools clean run run-headless debug test

# 显式指定默认目标：文件末尾 -include 进来的 .d 文件里也有目标，
# 不指定的话 make 会把 .d 里的第一个目标当成默认目标，导致只编一个文件。
.DEFAULT_GOAL := all

all: check-tools $(FLOPPY_IMG)
	@echo "[OK] build finished: $(FLOPPY_IMG)"

# ---- 工具检查：缺工具时给出明确错误，而不是一堆编译错误 ----
check-tools:
	@$(NASM) -v > /dev/null 2>&1 || { echo "[ERR] 找不到 nasm"; exit 1; }
	@$(CC) --version > /dev/null 2>&1 || { echo "[ERR] 找不到 $(CC)"; exit 1; }
	@$(LD) --version > /dev/null 2>&1 || { echo "[ERR] 找不到 $(LD)"; exit 1; }
	@$(OBJCOPY) --version > /dev/null 2>&1 || { echo "[ERR] 找不到 $(OBJCOPY)"; exit 1; }

$(OBJ_DIR) $(DIST_DIR):
	mkdir -p $@

$(BOOT_BIN): $(BOOT_DIR)/boot.asm | $(OBJ_DIR)
	$(NASM) -f bin -i $(BOOT_DIR)/ $< -o $@

AP_TRAMP_BIN := $(OBJ_DIR)/ap_trampoline.bin
$(AP_TRAMP_BIN): $(BOOT_DIR)/ap_trampoline.asm | $(OBJ_DIR)
	$(NASM) -f bin $< -o $@

$(OBJ_DIR)/core/ap_trampoline_blob.o: $(AP_TRAMP_BIN)

$(LOADER_BIN): $(LOADER_DIR)/loader.asm $(BOOT_VERIFY_BIN) | $(OBJ_DIR)
	$(NASM) -f bin -i $(LOADER_DIR)/ $< -o $@

# 汇编（head.asm、isr.asm 等）
$(OBJ_DIR)/%.o: $(KERNEL_DIR)/%.asm | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

# C 源码
$(OBJ_DIR)/%.o: $(KERNEL_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 两遍链接：先链接出 pass1，用它生成符号表，再带符号表重新链接
KERNEL_PASS1 := $(OBJ_DIR)/kernel-pass1.elf
SYMS_C       := $(OBJ_DIR)/symbols.c
SYMS_O       := $(OBJ_DIR)/symbols.o

$(KERNEL_PASS1): $(KERNEL_OBJS) | $(OBJ_DIR)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

$(SYMS_C): $(KERNEL_PASS1) tools/gensyms.ps1
	$(PWSH) tools/gensyms.ps1 -Elf $(KERNEL_PASS1) -Out $(SYMS_C)

$(SYMS_O): $(SYMS_C)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS) $(SYMS_O) | $(OBJ_DIR)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS) $(SYMS_O)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(FLOPPY_IMG): $(BOOT_BIN) $(LOADER_BIN) $(KERNEL_BIN) | $(DIST_DIR)
	@echo "[..] building $(FLOPPY_IMG)"
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BOOT_BIN)   of=$@ bs=512 seek=0  conv=notrunc status=none
	dd if=$(LOADER_BIN) of=$@ bs=512 seek=1  conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=33 conv=notrunc status=none
	@echo "[OK] image: $@"

# ---- 运行 ----
# 8MB 硬盘镜像：存放 XFS1 文件系统，重启后文件还在；不存在就创建
DISK_IMG := $(DIST_DIR)/disk.img
$(DISK_IMG): | $(DIST_DIR)
	@echo "[..] creating $(DISK_IMG) (8MB)"
	dd if=/dev/zero of=$@ bs=1M count=8 status=none

QEMU_DISK := -drive file=$(DISK_IMG),format=raw,if=ide,index=0

# 8MB FAT16 交换盘（ata1）：宿主工具生成，内核挂成 fat:/
FAT_IMG := $(DIST_DIR)/fat.img
$(FAT_IMG): tools/mkfat16.ps1 | $(DIST_DIR)
	@echo "[..] creating $(FAT_IMG) (FAT16)"
	$(PWSH) tools/mkfat16.ps1 -Path $(FAT_IMG) -File HOST.TXT -Content "hello from host"

QEMU_FAT := -drive file=$(FAT_IMG),format=raw,if=ide,index=1

run: $(FLOPPY_IMG) $(DISK_IMG) $(FAT_IMG)
	@echo "[..] starting QEMU, serial console on stdio, press Ctrl+C to quit"
	$(QEMU) -fda $(FLOPPY_IMG) $(QEMU_DISK) $(QEMU_FAT) -boot a -m 256M -vga std -serial stdio -no-reboot

# ---- 无窗口运行 ----
run-headless: $(FLOPPY_IMG) $(DISK_IMG) $(FAT_IMG)
	@echo "[..] starting QEMU headless, serial console on stdio, press Ctrl+C to quit"
	$(QEMU) -fda $(FLOPPY_IMG) $(QEMU_DISK) $(QEMU_FAT) -boot a -m 256M -vga std -display none -serial stdio -no-reboot

# ---- 调试：QEMU 带 GDB 桩，端口 1234 ----
debug: $(FLOPPY_IMG) $(DISK_IMG) $(FAT_IMG)
	@echo "[..] starting QEMU with GDB stub on port 1234, press Ctrl+C to quit"
	$(QEMU) -fda $(FLOPPY_IMG) $(QEMU_DISK) $(QEMU_FAT) -boot a -m 256M -vga std -s -S -no-reboot

# ---- 自动测试：无界面启动 + 注入命令 + 断言，失败返回非 0 ----
test: $(FLOPPY_IMG)
	@echo "[..] running automated boot test"
	$(PWSH) build.ps1 -Test

clean:
	@echo "[..] cleaning $(OBJ_DIR) and $(DIST_DIR)"
	rm -rf $(OBJ_DIR) $(DIST_DIR)

# 头文件依赖（-MMD 生成）；放在文件末尾，避免影响默认目标
-include $(KERNEL_OBJS:.o=.d)
