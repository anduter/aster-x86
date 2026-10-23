; =============================================================================
; sysinfo.asm - 系统信息采集
;
; 采集内容：内存（E820）、CPU（CPUID）、主板（SMBIOS）、显卡（VBE）、磁盘
; 调用时机：加载器运行期间（实模式，进入保护模式之前）
; 结果位置：SYSINFO_BASE(0x5000)，供 32 位 C 内核读取
; 依赖：BOOT_DRIVE、386+ CPU
; =============================================================================

SYSINFO_BASE equ 0x5000

; ---- 内存信息区偏移 ----
MEM_COUNT    equ SYSINFO_BASE+0x000 ; dw E820 条目数
MEM_TABLE    equ SYSINFO_BASE+0x002 ; E820 条目表（每条 24 字节）
MEM_TOTAL    equ SYSINFO_BASE+0x200 ; dq 内存总字节数
MEM_USED     equ SYSINFO_BASE+0x208 ; dq 已用/保留字节数
MEM_FREE     equ SYSINFO_BASE+0x210 ; dq 可用字节数

; ---- 显示信息区偏移 ----
VBE_MODE_NUM equ SYSINFO_BASE+0x300 ; dw 目标图形模式号
FB_ADDR      equ SYSINFO_BASE+0x302 ; dd 显存物理地址
FB_SIZE      equ SYSINFO_BASE+0x306 ; dd 显存大小（字节）
VBE_MODE     equ SYSINFO_BASE+0x400 ; 256 字节模式信息块
VBE_CTRL     equ SYSINFO_BASE+0x500 ; 256 字节控制信息块

; ---- CPU 信息区偏移 ----
CPU_VENDOR   equ SYSINFO_BASE+0x600 ; 12 字节厂商字符串，如 'GenuineIntel'
CPU_VERSION  equ SYSINFO_BASE+0x60C ; dd CPUID.1.EAX
CPU_FAMILY   equ SYSINFO_BASE+0x610 ; db 家族
CPU_MODEL    equ SYSINFO_BASE+0x611 ; db 型号
CPU_STEPPING equ SYSINFO_BASE+0x612 ; db 步进
CPU_FEATURES equ SYSINFO_BASE+0x614 ; dd CPUID.1.EDX

; ---- SMBIOS 信息区偏移 ----
SMBIOS_PTR   equ SYSINFO_BASE+0x700 ; dd 入口点物理地址
SMBIOS_TABLE equ SYSINFO_BASE+0x704 ; dd 结构表地址
SMBIOS_LEN   equ SYSINFO_BASE+0x708 ; dd 结构表长度
SMBIOS_VER   equ SYSINFO_BASE+0x70C ; dw 版本号（主版本<<8 | 次版本）

; ---- 磁盘信息区偏移 ----
DISK_TYPE    equ SYSINFO_BASE+0x800 ; db 0=无 1/2=软盘 3=硬盘
DISK_CYL     equ SYSINFO_BASE+0x802 ; dw 柱面数
DISK_HEADS   equ SYSINFO_BASE+0x804 ; dw 磁头数
DISK_SPT     equ SYSINFO_BASE+0x806 ; dw 每磁道扇区数
DISK_SECTORS equ SYSINFO_BASE+0x808 ; dq 总扇区数
DISK_TOTAL   equ SYSINFO_BASE+0x810 ; dq 总容量（字节）
DISK_USED    equ SYSINFO_BASE+0x818 ; dq 已用字节数
DISK_FREE    equ SYSINFO_BASE+0x820 ; dq 剩余字节数

; VBE 目标模式：0x118 = 1024x768 24bpp（QEMU std VGA 支持）
VBE_TARGET_MODE equ 0x118

; =============================================================================
; collect_sysinfo - 入口，依次采集各项信息
; 说明：各项采集内部自行保存/恢复寄存器，这里不再输出阶段字符
; =============================================================================
collect_sysinfo:
    pushad
    call collect_memory
    call collect_cpu
    call collect_smbios
    call collect_vbe
    call collect_disk
    popad
    ret

; =============================================================================
; collect_memory - 用 int 0x15 E820 取内存布局，并统计总量/已用/可用
; =============================================================================
collect_memory:
    pushad
    mov word [MEM_COUNT], 0
    xor ebx, ebx            ; 续读标志（首次调用为 0）
    mov edi, MEM_TABLE
    xor ax, ax
    mov es, ax
.next:
    ; MEM_TABLE 从 0x5002 开始，而 MEM_TOTAL 在 0x5200，只放得下 21 条。
    ; 在写满之前停下，避免 E820 表把 MEM_TOTAL/MEM_USED/MEM_FREE 覆盖掉。
    cmp word [MEM_COUNT], 21
    jae .done
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150     ; 'SMAP'
    int 0x15
    jc  .done
    cmp eax, 0x534D4150
    jne .done
    inc word [MEM_COUNT]
    add edi, 24
    test ebx, ebx
    jnz .next
.done:
    ; 清零三个统计量
    mov dword [MEM_TOTAL], 0
    mov dword [MEM_TOTAL+4], 0
    mov dword [MEM_USED], 0
    mov dword [MEM_USED+4], 0
    mov dword [MEM_FREE], 0
    mov dword [MEM_FREE+4], 0
    mov cx, [MEM_COUNT]
    mov si, MEM_TABLE
.stat:
    ; 条目布局：基址(8) + 长度(8) + 类型(4) + 属性(4)
    mov eax, [si+8]         ; 长度低 32 位
    mov edx, [si+12]        ; 长度高 32 位
    add [MEM_TOTAL], eax
    adc [MEM_TOTAL+4], edx
    cmp dword [si+16], 1    ; 类型 1 = 可用内存
    jne .used
    add [MEM_FREE], eax
    adc [MEM_FREE+4], edx
    jmp .stat_next
.used:
    add [MEM_USED], eax
    adc [MEM_USED+4], edx
.stat_next:
    add si, 24
    loop .stat
    popad
    ret

; =============================================================================
; collect_cpu - 检测 CPUID 支持，读取厂商字符串/版本/特性
; =============================================================================
collect_cpu:
    pushfd
    pushad
    ; 检测 CPUID：尝试翻转 EFLAGS 的 ID 位（bit21）
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x200000
    push eax
    popfd
    pushfd
    pop eax
    xor eax, ecx
    and eax, 0x200000
    jz  .no_cpuid
    ; CPUID 功能 0：厂商字符串
    xor eax, eax
    cpuid
    mov [CPU_VENDOR], ebx
    mov [CPU_VENDOR+4], edx
    mov [CPU_VENDOR+8], ecx
    ; CPUID 功能 1：版本号与特性位
    mov eax, 1
    cpuid
    mov [CPU_VERSION], eax
    mov [CPU_FEATURES], edx
    ; 解析基础家族/型号/步进（暂不处理扩展家族/型号）
    mov [CPU_STEPPING], al
    mov ecx, eax
    shr eax, 4
    and eax, 0xF
    mov [CPU_MODEL], al
    mov eax, ecx
    shr eax, 8
    and eax, 0xF
    mov [CPU_FAMILY], al
.no_cpuid:
    popad
    popfd
    ret

; =============================================================================
; collect_smbios - 在 0xF0000 段扫描 "_SM_" 入口点
; =============================================================================
collect_smbios:
    pushad
    mov dword [SMBIOS_PTR], 0
    mov dword [SMBIOS_TABLE], 0
    mov dword [SMBIOS_LEN], 0
    mov word [SMBIOS_VER], 0
    mov ax, 0xF000
    mov es, ax              ; ES:SI -> 0xF0000+SI
    xor si, si
.scan:
    cmp si, 0xFFE0          ; 提前停下，避免 16 位 SI 回绕到 0 后死循环
    jae .exit               ; （只判断 JB 的话会漏掉回绕这一圈）
    mov eax, [es:si]        ; 一次比较 4 字节
    cmp eax, 0x5F4D535F     ; '_SM_'
    je  .found
    add si, 16
    jmp .scan
.exit:
    popad
    ret                     ; 没找到，直接返回
.found:
    ; 入口点物理地址 = 0xF0000 + SI
    ; （必须只用 SI：ESI 的高 16 位是调用者留下的垃圾）
    movzx eax, si
    add eax, 0xF0000
    mov [SMBIOS_PTR], eax
    ; 结构表地址（入口点偏移 0x18）
    mov eax, [es:si+0x18]
    mov [SMBIOS_TABLE], eax
    ; 结构表长度（入口点偏移 0x1C，2 字节）
    movzx eax, word [es:si+0x1C]
    mov [SMBIOS_LEN], eax
    ; 版本号（入口点偏移 0x06/0x07）
    mov ax, [es:si+0x06]
    mov [SMBIOS_VER], ax
    popad
    ret

; =============================================================================
; collect_vbe - 探测 VBE，记录目标模式号与显存信息
; =============================================================================
collect_vbe:
    pushad
    ; 取控制信息块
    mov di, VBE_CTRL
    xor ax, ax
    mov es, ax
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .no_vbe
    ; 取目标模式的模式信息块（重新置 ES=0，SMBIOS 扫描可能改过 ES）
    xor ax, ax
    mov es, ax
    mov ax, 0x4F01
    mov cx, VBE_TARGET_MODE
    mov di, VBE_MODE
    int 0x10
    cmp ax, 0x004F
    jne .no_vbe
    ; 记录模式号
    mov word [VBE_MODE_NUM], VBE_TARGET_MODE
    ; 显存地址 = PhysBasePtr（模式信息块偏移 0x28）
    mov eax, [VBE_MODE+0x28]
    mov [FB_ADDR], eax
    ; 显存大小 = 横向分辨率 * 纵向分辨率 * (色深/8)
    movzx eax, word [VBE_MODE+0x12] ; XResolution
    movzx ebx, word [VBE_MODE+0x14] ; YResolution
    mul ebx
    movzx ecx, byte [VBE_MODE+0x19] ; BitsPerPixel
    shr ecx, 3                      ; /8 -> 每像素字节数
    mul ecx
    mov [FB_SIZE], eax
.no_vbe:
    popad
    ret

; =============================================================================
; collect_disk - 磁盘类型 / 几何参数 / 容量 / 剩余空间
; 类型  ：int 0x13 AH=0x15；几何：int 0x13 AH=0x08
; 容量  = 柱面数*磁头数*每道扇区数*512
; 已用  = boot(1) + loader(32) = 33 个扇区（内核镜像启动时尚未计入）
; 剩余  = 总容量 - 已用
; 说明  ：启动阶段还没有文件系统，真实剩余空间要等以后的文件系统驱动
; =============================================================================
collect_disk:
    pushad
    mov byte [DISK_TYPE], 0
    mov dword [DISK_TOTAL], 0
    mov dword [DISK_TOTAL+4], 0

    ; ---- AH=0x08 读取磁盘几何参数 ----
    mov ah, 0x08
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc  .no_geom
    ; 每磁道扇区数 = CL & 0x3F
    movzx ax, cl
    and ax, 0x3F
    mov [DISK_SPT], ax
    ; 磁头数 = DH + 1
    movzx ax, dh
    inc ax
    mov [DISK_HEADS], ax
    ; 柱面数 = CH | ((CL>>6)<<8)
    movzx ax, ch
    movzx bx, cl
    shr bx, 6
    shl bx, 8
    or  ax, bx
    mov [DISK_CYL], ax
    ; 总扇区数 = 柱面数 * 磁头数 * 每道扇区数
    movzx eax, ax
    movzx ecx, word [DISK_HEADS]
    mul ecx
    movzx ecx, word [DISK_SPT]
    mul ecx                 ; edx:eax = 总扇区数
    mov [DISK_SECTORS], eax
    mov [DISK_SECTORS+4], edx
    ; 总容量 = 总扇区数 * 512（只保留低 32 位）
    shl eax, 9
    mov [DISK_TOTAL], eax
    mov dword [DISK_TOTAL+4], 0
.no_geom:
    ; ---- AH=0x15 读取磁盘类型 ----
    mov ah, 0x15
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc  .no_type
    mov [DISK_TYPE], ah     ; 0=无 1/2=软盘 3=硬盘
.no_type:
    ; 已用 = boot(1) + loader(32) = 33 个扇区（内核镜像启动时尚未计入）
    mov eax, 33
    shl eax, 9
    mov [DISK_USED], eax
    mov dword [DISK_USED+4], 0
    ; 剩余 = 总容量 - 已用
    mov eax, [DISK_TOTAL]
    mov edx, [DISK_TOTAL+4]
    sub eax, [DISK_USED]
    sbb edx, [DISK_USED+4]
    mov [DISK_FREE], eax
    mov [DISK_FREE+4], edx
    popad
    ret
