# =============================================================================
# ng WP1-1: 32 位启动入口 -> 长模式（long mode） -> 64 位 C 入口
#
# 镜像同时带两个“启动头”，都落在同一个 32 位入口 _start：
#   - multiboot2 header：给 GRUB 之类的 multiboot2 引导器；
#   - Xen PVH note：给 QEMU 的 -kernel。QEMU 11 的 multiboot 加载器只收
#     32 位 ELF（"Cannot load x86-64 image, give a 32bit one"），ELF64 只能
#     走 PVH 路径（"Error loading uncompressed kernel without PVH ELF Note"）。
#   两条路径的进入状态相同：32 位保护模式、分页关闭、平坦段。
#
# 本文件只做 WP1-1 的最小闭环：建 1 GiB 恒等页表 -> 开 PAE/LME/PG ->
# far jump 进 64 位 -> 调 kmain。不做 trap/syscall/PMM（后续 WP）。
# =============================================================================

# ---- multiboot2 header（8 字节对齐，必须在前 32 KiB 文件内）----------------
    .section .multiboot, "a"
    .align 8
mb2_header:
    .long 0xE85250D6                      # magic
    .long 0                               # architecture: i386 保护模式
    .long mb2_header_end - mb2_header     # header length
    .long -(0xE85250D6 + (mb2_header_end - mb2_header))  # checksum
    .short 0                              # end tag: type
    .short 0                              # end tag: flags
    .long 8                               # end tag: size
mb2_header_end:

# ---- Xen PVH note（QEMU -kernel 的入口；desc = 32 位入口地址）-------------
    .section .note.Xen, "a", @note
    .align 4
    .long 4                               # namesz: "Xen\0"
    .long 4                               # descsz: 4 字节入口
    .long 18                              # XEN_ELFNOTE_PHYS32_ENTRY
    .ascii "Xen\0"
    .align 4
    .long _start
    .align 4

# ---- GDT：null / 32 位代码 / 32 位数据 / 64 位代码 / 64 位数据 -------------
    .section .rodata
    .align 8
gdt64:
    .quad 0x0000000000000000              # 0x00 null
    .quad 0x00CF9A000000FFFF              # 0x08 32-bit code, base 0, limit 4 GiB
    .quad 0x00CF92000000FFFF              # 0x10 32-bit data
    .quad 0x00AF9A000000FFFF              # 0x18 64-bit code (L=1, D=0)
    .quad 0x00CF92000000FFFF              # 0x20 data（长模式下 base/limit 忽略）
gdt64_end:
gdt64_ptr:
    .short gdt64_end - gdt64 - 1
    .quad gdt64

# ---- 页表与启动栈（.bss；QEMU/GRUB 加载 ELF 时按 p_memsz 清零，入口再显式清零）--
    .section .bss
    .align 4096
pml4:
    .skip 4096
    .align 4096
pdpt:
    .skip 4096
    .align 4096
pd:
    .skip 4096

    .align 16
boot_stack_bottom:
    .skip 16384
boot_stack_top:

# ---- 32 位入口 -------------------------------------------------------------
    .section .text
    .code32
    .global _start
    .type _start, @function
_start:
    cli
    cld
    # EBX 由引导器给：multiboot2 = information pointer，PVH = hvm_start_info。
    # WP1-1 还不用，先存进 ESI 留给 kmain（64 位下作为第一个参数）。
    movl %ebx, %esi

    # 检查 CPUID 是否支持长模式：CPUID.80000001h:EDX[29] = LM
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb .Lno_long_mode
    movl $0x80000001, %eax
    cpuid
    btl $29, %edx
    jnc .Lno_long_mode

    # 32 位启动栈（64 位入口会重新设置一次；这里先保证能跑 C 之前的代码）
    movl $boot_stack_top, %esp
    xorl %ebp, %ebp

    # 载入带 64 位段的 GDT，并用 far jump 刷新 CS
    lgdt gdt64_ptr
    ljmp $0x08, $.Lpm32
.Lpm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    # 先把三张表清零：不依赖加载器对 .bss 的清零行为
    movl $pml4, %edi
    xorl %eax, %eax
    movl $(3 * 1024), %ecx                # 3 页 = 3 * 1024 个 dword
    rep stosl

    # PML4[0] -> PDPT，PDPT[0] -> PD（present + writable）
    movl $pdpt, %eax
    orl $0x3, %eax
    movl %eax, pml4
    movl $pd, %eax
    orl $0x3, %eax
    movl %eax, pdpt

    # PD[i] = i * 2 MiB | PS | RW | P，i = 0..511：恒等映射前 1 GiB
    xorl %ecx, %ecx
.Lfill_pd:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax                       # 0x80 = PS（2 MiB 页）
    movl %eax, pd(,%ecx,8)
    movl $0, pd+4(,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne .Lfill_pd

    # CR4.PAE = 1
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4
    # CR3 = PML4 物理地址（此时恒等映射，虚拟地址 == 物理地址）
    movl $pml4, %eax
    movl %eax, %cr3
    # EFER.LME = 1
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr
    # CR0.PG = 1：从这里开始进入长模式（compatibility 子模式，CS 仍是 32 位）
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    # far jump 到 64 位代码段 0x18，CPU 此时才真正在 64 位模式下取指
    ljmp $0x18, $.Llong_mode

.Lno_long_mode:
    cli
.Lhang32:
    hlt
    jmp .Lhang32

# ---- 64 位入口 -------------------------------------------------------------
    .code64
.Llong_mode:
    # 长模式下 DS/ES/SS 的 base 被忽略，但仍要有合法数据段
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $boot_stack_top, %esp            # 32 位写 ESP 会零扩展到 RSP
    xorl %ebp, %ebp

    # System V AMD64：第一个参数放 RDI；movl 会零扩展 ESI
    movl %esi, %edi
    call kmain

.Lhang64:
    hlt
    jmp .Lhang64

    .size _start, . - _start
