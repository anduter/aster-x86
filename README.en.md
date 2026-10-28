# x86 OS

English | [简体中文](README.md)

A 32-bit x86 teaching operating system built from scratch: a hand-written boot sector, a two-stage loader, and a 32-bit C kernel. It boots on QEMU (`-vga std`, VBE 0x118 1024x768x24) and brings up a framebuffer text console, physical and virtual memory management, a heap, memory self-tests, an IDT, and timer/keyboard interrupts.

Current version **v0.74.2** (development line): **254** end-to-end regression checks, and **10 host unit suites / 520 checks** (`tools/unit.ps1`, no QEMU; includes path normalization, the PMM bitmap, and FAT16 directory entries).

## System model (target architecture)

[`docs/system-model.md`](docs/system-model.md) defines Aster's target structure: capability-driven strongly typed objects, per-process private namespaces, ring3 service-based filesystem/network/drivers, and a point-by-point comparison with Unix/Linux, Windows NT, seL4, Mach, Plan 9, Fuchsia, and Minix 3/QNX.

The native program interface is specified in [`docs/native-abi.md`](docs/native-abi.md): `int 0x80`, typed handles, Port/Channel IPC, `process_create`, and the ELF32 startup contract. A POSIX/Linux compatibility layer comes later.

P3 security boundaries and engineering rules live in [`docs/security-model.md`](docs/security-model.md): separated ELF segment permissions, read-only code pages, COW restricted to writable pages, explicit limits, and a security test matrix.

> Note: those documents describe the migration target; they do not mean the current implementation already has all of it. The code is still a prototype.

## Features

| Module | Status |
|---|---|
| Boot sector (MBR) | Reads the loader, verifies the `LOADER` signature, readable error codes |
| Two-stage loader | 386+ check, E820/CPUID/SMBIOS/VBE/disk-info collection, chunked kernel load in Unreal Mode, VBE mode switch, entry into protected mode |
| Kernel | Flat segments, framebuffer, console, kprintf, panic, PMM, VMM (PAE: 2 MB identity map + 4 KB NX user pages), heap, memtest, IDT/PIC/PIT/keyboard |
| Object model | Five peer object classes — program / file / device / channel — with type-tagged handles (not "everything is a file") |
| Boot self-test | `demo_run()` proves the object classes are independent and handle types cannot be mixed |
| shell | **A ring3 program**: the kernel loads it from `/system/bin/shell`; during the migration it reads the keyboard and touches files through `int 0x81` |
| Processes / scheduling | Process table, kernel threads, context switches, round-robin time slices; **blocking waits, sleep timeouts, COW fork, signals (user handlers can be registered), pipes/dup2** |
| Native ABI | **`int 0x80` (DPL3)**: ABI version, exit/yield, typed handle close/dup/info, debug output; verified from a ring3 demo |
| Legacy syscalls | `int 0x81` transition gate: the original table (write / getpid / uptime / yield / exit, …) |
| P3 security boundary | ELF code pages read-only, data pages writable, RWX segments rejected, COW only on writable pages; compiler `-Werror`; full limits in the security model |
| CPU security features | SMEP/SMAP enabled per CPUID; under SMAP, user copies and signal frames go through STAC/CLAC; verified with QEMU `-cpu max` |
| Stack/heap/trampoline ASLR | Stack top within the top 512 KB; heap base and trampoline page randomized per process within their own 256 KB ranges |
| Sandbox | legacy + native syscall allowlists; both `SYS_OPEN` and `HANDLE_DUP` denied, verified with `userdemo` |
| Image signing | RSA-2048/SHA-256 PKCS#1 v1.5; verified at boot and in `user_exec`; tampered images are rejected |
| Component model | **Signed `.pkg`** (manifest + file table + per-file SHA-256 + RSA-2048 signature), `_pkgd` install/remove/verify, the `system:/System/packages.db` component DB, and user apps installed under `~/Apps`; see [`docs/pkg.md`](docs/pkg.md) |
| Address-space teardown | Process exit/exec frees user physical pages and PT/PD/PDPT; fork COW shared pages are reclaimed by refcount |
| User mode | Per-process page directory + ELF32 loader + argc/argv startup stack + mini-libc; the user stack grows on demand; a signal trampoline page supports `sigreturn` |
| Screen widgets | Heartbeat `hb N *` bottom-right, `uptime N s` on the left; periodic output only refreshes the status bar, the content area stays still |
| Filesystem | **Drives are roots** (Windows-style `system:/bin`, `data:/a.txt`): the top level *is* the volume table; there is no virtual `/`; **64 KB** per file; ramfs **hard links / symlinks** (`ln [-s]`, `readlink`; hard-link relationships are not persisted); **SIP-lite** makes `system:/System|bin|drivers` read-only at runtime (root included; the signed-updater scope is the exception) |
| Block devices / persistence | `DEV_BLOCK` + **ATA PIO (`ata0` XFS1 system disk + `ata1` FAT16 exchange disk) + `ram0` RAM disk**; `XFS1` **v5**; **files survive a reboot**; `fat:/` exchanges files directly with the host |
| Network | **ARP/ICMP/UDP**; **TCP client robustness**: sequence/ACK, MSS/window, checksum, RFC6298 RTO with exponential backoff, fast retransmit, out-of-order reassembly, FIN/RST/TIME_WAIT, slow start, 16 KB streaming echo; `listen/accept` and the user-space socket API are in a parallel batch |
| Platform / peripherals | **Serial input (IRQ4), PCI enumeration, ACPI S5 poweroff, LAPIC probe + NMI watchdog, PS/2 mouse (IRQ12, `mouse`), e1000 NIC (MAC/link, ARP, ICMP echo rx/tx)** |

## Quick start

Requirements: `nasm`, `i686-elf-gcc/ld/objcopy`, `qemu-system-i386`, `openssl`.
**The cross toolchain is not shipped in the repo** (about 2.7 GB; `.gitignore` excludes `tools/`, so a fresh `git clone` does not contain it): unpack it into `tools/i686-elf/bin` (`build.ps1` prepends that directory to `PATH`), or make `i686-elf-gcc/ld/objcopy` available on `PATH`.

**The signing private key is not in the repo** (an open-source repo must not carry private keys). Generate a key pair locally before the first build:

```powershell
.\tools\gen-key.ps1    # writes keys/shell_private.pem (gitignored) + keys/shell_public.pem
```

The build signs the shell ELF and `kernel.bin` with `keys/shell_private.pem`; the script emits the public modulus into `obj/shell_signature.c`, so the kernel only embeds the public key. Key rotation and CI secrets: see `keys/README.md` and `SECURITY.md`.

**Windows (recommended, no POSIX shell needed)**

```powershell
.\build.ps1              # build dist\floppy.img
.\build.ps1 -Run         # build and launch with a window (serial mirrored to the console)
.\build.ps1 -Test        # build + headless boot + assertions (non-zero exit on failure)
.\build.ps1 -Shot a.ppm  # build and capture a screenshot
```

The default is `-Cpu max` and the tests hard-assert SMEP/SMAP; `-Cpu qemu32` is only for observing the unsupported path, where those security assertions do not apply.

**GNU Make (Git Bash / MSYS2 / WSL)**

```sh
make                 # build
make run             # launch with a window
make run-headless    # headless, serial only
make debug           # start paused and wait for GDB on port 1234
make test            # automated verification (calls build.ps1 -Test)
make clean
```

## Directory layout

```
docs/architecture.md  architecture: five object classes, namespace separation, syscall ABI, rollout order
src/user/      user programs: user_shell.c (ring3 shell) + user.ld, linked at 0x40000000
src/boot/      boot sector (512 bytes)
  boot.asm         reads the loader and verifies its signature
  print.asm        real-mode string/hex output
src/loader/    two-stage loader (32 sectors = 16 KB)
  loader.asm       main flow and stage banners
  kernel_loader.asm chunked kernel read + Unreal Mode copy to 1 MB
  sysinfo.asm      E820 / CPUID / SMBIOS / VBE / disk-info collection
  vesa_mode.asm    switches to VBE 0x118
  pm_entry.asm     GDT/IDT setup, entry into 32-bit protected mode
src/kernel/
  head.asm         kernel entry (0x100008)
  kernel.c         kernel main flow and idle loop
  console.c/h      text console (framebuffer + serial mirror)
  fb.c/h           framebuffer driver and font (BIOS VGA 8x8 glyphs + integer scaling)
  intr/            interrupt subsystem (IDT / stubs / IRQ)
    interrupt.h      public interface and interrupt_frame_t
    idt.c            IDT, exception dispatch (handed to panic)
    isr.asm          exception/IRQ stubs and the entry-address table
    irq.c            8259 remap, PIT tick, keyboard, IRQ registration, spurious handling
    syscall.c/h      syscall dispatch and sys_call() (int 0x80)
    uaccess.c/h      safe user-memory access (fault-recoverable)
  core/            object model (P0)
    object.c/h       object table and typed handles (low 24 bits slot, high 8 bits type)
    program.c/h      program registry: programs are registrations, not file paths
    device.c/h       device registry: strongly typed functions by class, no /dev
    demo.c/h         boot self-test (the object model proving itself)
  apps/            program layer (not kernel modules)
    shell.c/h        command interpreter: reads kbd0, runs programs via prog_run
    heartbeat.c/h    heartbeat thread: one line per second, proves scheduling
  proc/            processes and scheduling (P1)
    proc.h           process interface
    proc_internal.h  process table and switch helpers (shared by process.c / sched.c)
    process.c        process table, thread creation, exit, context-switch wrappers
    sched.c          ring round-robin, time slices, preemption
    switch.asm       context_switch and thread_trampoline
  user/            user mode (P2)
    gdt.c/h          GDT (kernel/user segments) + TSS
    user_entry.asm   gdt_flush / tss_flush / user_enter (iret into ring3)
    user_demo.asm    a tiny position-independent ring3 program (copied to a user page)
    user_mode.c/h    allocates user pages, adds U/S mappings, enters ring3
  fs/fs.h          public filesystem interface: paths, directories, file handles
  fs/ramfs.c       memory backend: node tree, path resolution, handles, directory walks
  panic.c/h        fatal-error handling
  lib/kprintf.c    printf/formatting
  lib/string.c     memmove/memset/strlen and friends
  mm/pmm.c         physical page bitmap (32-bit word scan + rover)
  mm/vmm.c         PAE page tables, 2 MB identity map, 4 KB NX user pages, COW, vmalloc
  mm/memtest.c     memory self-test (8 checks)
  heap.c           fixed-region heap allocator
  version.h        system name/version/build time
```

## Boot flow

```
BIOS -> LBA0 boot.bin (0x7C00)
       └─ reads LBA1..32 to 0x8000, checks the trailing "LOADER"
          └─ loader: CPU check -> collects system info into 0x5000
                     -> reads the kernel in 28-sector chunks to 0x1000, copies to 0x100000
                     -> switches to VBE 0x118
                     -> protected mode, jumps to 0x100008
                        └─ kernel_main()
```

## Physical memory layout

| Address | Use |
|---|---|
| `0x0000`–`0x04FF` | interrupt vector table / BIOS data area (do not touch) |
| `0x1000`–`0x47FF` | loader chunk-read buffer |
| `0x5000`–`0x58FF` | system-info table (E820 / VBE / CPU / disk) |
| `0x7C00` | boot sector load address / temporary stack |
| `0x8000`–`0xBFFF` | the loader itself |
| `0x100000` | kernel image (first 8 bytes are the `KRNL` header) |
| `0x200000` | PMM bitmap |
| `0x800000`–`0xC00000` | heap (16 MB, already reserved in the PMM) |
| `0xFD000000` | VBE linear framebuffer (mapped uncached) |

On the virtual side, the PAE identity map covers all physical memory with 2 MB pages,
and the `vmalloc` region sits at `0xC0000000` (3 GB) for 32 MB, clear of the identity-map
large pages. User pages use 4 KB PTEs: text R+X, rodata R+NX, data/stack/heap RW+NX.

## Crash handling

- **Double fault (vec 8) has a dedicated stack**: `isr8` first reads the old err/eip/cs/eflags from the faulting stack, switches to the 4 KB static `df_stack`, and only then enters the common entry, so a bad stack cannot triple-fault into a reset;
- **panic dump**: prints the reason plus `stack:` (16 32-bit words) to serial; the on-screen panel appends the first 4 words; the serial log is the persisted scene (`-serial file:`);
- **ring0 oops**: a kernel exception prints the exception name/eip/err/cr2 plus a **symbolized EBP backtrace** (`function+offset`) and kills the current thread instead of halting; the symbol table comes from a two-pass link at build time (`nm` → `obj/symbols.c`, 367 function symbols);
- **ring3 exceptions**: try growing the user stack first, kill the process only if that fails;
- **`SYS_REBOOT` (27)**: user mode can reset the machine (8042 `0xFE`); shell command `reboot`;
- **`SYS_PANIC` (28)**: debug entry; the shell command `crash` triggers a panic on purpose to check the dump;
- **`SYS_OOPS` (29)**: debug entry; the shell command `oops` triggers a kernel page fault to check the symbolized backtrace;
- **NMI watchdog**: `apic_watchdog_start()` maps the LAPIC page, enables the APIC, calibrates the LAPIC timer against the 100 Hz PIT tick (counting over 100 ms), then configures the LVT timer as "periodic + NMI delivery" (about 250 ms). The NMI handler compares `timer_ticks + syscall_count`: four consecutive non-advancing checks (about 1 s) → panic. **NMI ignores IF**, so even a `cli; hlt` hang is caught; the `hang` command verifies it: `NMI watchdog: no progress for ~1s (eip=...)`;
- **e1000 NIC**: find 8086:100e on PCI → **enable MEMORY+BUS MASTER in CMD** (without bus mastering, TX `TDH` still advances but frames never reach the wire — that was the root cause behind RX and TX both "looking successful") → map BAR0 → read MAC/link → initialize the TX/RX descriptor rings → **ARP-resolve the gateway MAC + IPv4/ICMP echo rx/tx** (boot log `net: ARP reply gw=…`, `net: ICMP echo reply seq=1`, both in the automated regression); UDP and a layered protocol stack are still missing (a soak run declaring a hang after 180 s of silent serial).

## Interrupts

- The 8259s are remapped to `0x20` / `0x28`; PIT channel 0 runs in mode 3 at 100 Hz;
- **IRQs are registrable**: `irq_register(irq, handler, arg, name)`; the timer and keyboard register themselves in `interrupt_init()`, and `irq.c` no longer hard-codes device logic;
- **PIC masking is managed per line**: `irq_mask/irq_unmask` read-modify-write the mask; unmasking a slave IRQ automatically unmasks master IRQ2;
- **Spurious IRQ handling**: IRQ7/IRQ15 read the ISR first to tell real from spurious, and a spurious IRQ gets no EOI (a spurious slave IRQ still EOIs the master); an IRQ with no handler is masked automatically so it cannot fire repeatedly;
- **Tiered exception handling**: a handler registered with `exception_register(vec, handler)` returns `EXC_HANDLED` (fixed, retry the instruction) / `EXC_KILL` (kill the current thread) / `EXC_PANIC` (dump and halt); unregistered vectors follow the default policy: **ring3 exceptions kill the thread, only ring0 exceptions panic**;
- **Recoverable page faults (generic exception table)**: `intr/fault.c` keeps a `(start, end, fixup)` table of up to 8 entries; `copy_from_user/copy_to_user` go through the assembly `uaccess_copy`, whose `[start,end)` is registered, so a #PF hook rewrites eip to the fixup and the kernel keeps running (the boot log line `sys: unmapped user pointer -> -1` is exactly this path);
- **Keyboard extras**: Shift (both), CapsLock, shifted symbol table; the `0xE0` extended-key prefix is swallowed correctly;
- Statistics via `irq_count/irq_name/irq_spurious_count`; the shell `irq` command shows them:

```
shell> irq
irq: ticks=301 spurious=0
  irq0 timer count=301
  irq1 keyboard count=7
```

## Boot self-test and shell

### Boot self-test (kernel behaviour)

The kernel runs `demo_run()` once at boot to prove the three object-model constraints:

```
demo: programs = 3
  prog shell@1
  prog hello@1
  prog memtest@1
demo: fs write/read ok (16 bytes)
demo: handle type check ok (prog-as-file=-4, file-as-device=-4)
demo: devices = 3
  dev char:kbd0
  dev video:fb0
  dev timer:pit0
```

The last line matters: using a program handle as a file, or a file handle as a device, is rejected with `E_TYPE(-4)`.

### shell (a ring3 program)

The shell is no longer a kernel module but a **user-mode program**:

- source in [src/user/user_shell.c](src/user/user_shell.c), linked at `0x40000000`;
- the build turns it into a flat binary and embeds it in the kernel; at boot the kernel writes it to `/system/bin/shell`;
- `shell@1`'s entry is just "exec `/system/bin/shell`": `user_exec()` reads the image from the filesystem, loads it into user pages, and `iret`s into ring3 (falling back to the built-in kernel shell only on failure);
- every capability of the ring3 shell comes from syscalls: `SYS_WRITE` to print, `SYS_GETCHAR` to read the keyboard, `SYS_OPEN/FREAD/FWRITE/CLOSE` for files, `SYS_LIST` for directories, `SYS_DRIVES` for drive letters, `SYS_EXIT` to quit.

```
ushell: ready (ring3, int 0x80)
ushell> help
ushell: commands
  help [cmd]             this help / help for one command
  drives                 list drives (system, data, fat, ...)
  ls [-l] [path]         no path = list drives; -l = long format
  cat [path]             show a file, e.g. system:/docs/a.txt
  write <path> <text>    create/overwrite a file (quotes allowed)
  mkdir <path>           create a directory
  rm <path>              remove a file/dir
  cd <path>              change directory; cd - returns
  pwd                    show current directory
  stat <path>            show type/size/mtime
  echo <args...>         print arguments
  sleep <ms>             sleep milliseconds
  fork                   create a child process, wait for it
  exec <path> [args]     replace this shell with a program
  <cmd> > <file>         redirect stdout
  <cmd> < <file>         redirect stdin
  <cmd1> | <cmd2>        pipe
  history                show recent commands
  !!  / !n               repeat last / n-th command
  reboot / crash / trap / kill / sync / fsck / date / exit
```

Interaction details:

- **Quotes and escapes**: `write system:/a.txt 'hello world'`, `"..."`, and `\ ` all work;
- **`ls -l`**: `-  16  2026-10-22 14:30:04  hello.txt` (type/size/mtime);
- **History**: `history` lists the last 8 commands, `!!` replays the previous one, `!3` replays the third;
- **`cd -`**: back to the previous directory;
- **`help <cmd>`**: usage for a single command;
- **Errors go to fd 2**: `cat nope > out.txt` does not write the error into out.txt;
- **Backspace**: edit in the input box directly; Enter moves the whole line into the output area.

`prog run memtest` really executes the registered `memtest` program and prints its result.

### UI: output box + input box

- The font is the **standard 8×8 glyph set from the BIOS VGA ROM (F000:FA6E)**, tidier than a hand-drawn set; if the ROM is unavailable it falls back to the built-in 8×8 font;
- `FONT_SCALE` controls integer scaling: **2×** by default (crisp and readable); `make FONT_SCALE=1` goes back to compact mode. Title bar, status bar, input box, and output box all scale together;
- every pixel of every glyph is drawn as a `scale × scale` block with no interpolation, so edges stay sharp.

The content area is split into two bordered boxes, stacked:

```
┌──────────── output box (scrollable) ────────────┐
│ logs, command history, command results           │
└──────────────────────────────────────────────────┘
┌──────────── input box (single line) ────────────┐
│ shell> what you are typing                       │
└──────────────────────────────────────────────────┘
```

- typing is drawn only in the lower input box (no wrapping, no serial echo, no effect on the output area);
- only after Enter is the whole line written to the output box, forming the command history;
- Backspace redraws just that input line;
- the status bar keeps uptime on the left and the heartbeat widget on the right.

The shell itself is already a ring3 program (see "User mode"): it prints through `SYS_WRITE` and reads the keyboard through `SYS_GETCHAR` instead of calling `kprintf`. The console only paints the bytes that the kernel and user mode write into the boxes; it does not care who wrote them.

## Filesystem

`fs/` is a standalone module whose public interface is [fs.h](src/kernel/fs/fs.h); the backend is a memory volume:

```
fs_init()                      creates the default drive "system" (the top level is the volume table itself)
fs_mount("data")               mount another drive (name up to 31 bytes, at most 4)
fs_volume_count() / fs_volume_name(i, buf, max)
fs_open(path, flags)           open/create a file -> an OBJ_FILE handle
fs_read / fs_write / fs_close  handle-based I/O (handle type is checked)
fs_stat / fs_mkdir / fs_unlink metadata and directory operations
fs_listdir(path, index, ...)   directory walk (empty path lists the drives)
```

Path rules (**each drive is its own root**, Windows semantics):

| Form | Meaning |
|---|---|
| `system:/` | root of drive system |
| `system:/bin/shell` | bin/shell on drive system |
| `data:/a.txt` | a.txt on drive data |
| `/docs/a.txt` | drive omitted → default drive |
| `docs/a.txt` | relative path → joined to the current cwd (default `system:/`) |
| `..` / `.` | resolved inside each drive tree; `..` stops at the drive root |
| `ls` (no argument) | the top level is the volume table: lists all drives |

- there is no virtual `/` root node — the top level *is* the filesystem module's volume table;
- a drive's root node is named after the drive, and `..` at `system:/` stays at the root;
- a non-empty path with `:` naming a missing volume is an error; without `:` the default drive is used.

Properties:

- **Directory tree**: nodes linked by `parent / first_child / next_sibling`, with segment-by-segment descent;
- **Drives**: one independent tree per volume, root named after the drive, names up to 32 bytes, at most 4;
- **Handle type safety**: a file handle is `OBJ_FILE`; using it as a device is rejected with `E_TYPE`;
- **Capacity**: 64 nodes (shared by all volumes), file/directory names up to **64 characters**, **64 KB** per file, at most 32 files open at once;
- **Persistence**: `fs_sync` / `fs_mount_disk` serialize the whole tree to a block device (XFS1 **v5**); at boot `ata0` (the 8 MB disk image) is mounted first and `ram0` is the fallback when there is no disk; `write`/`mkdir`/`rm` in the shell flush automatically, and `sync` forces it;
- **Links**: ramfs supports hard links and symbolic links (`ln` / `ln -s` / `readlink`, max 8 hops); symlinks persist in XFS1, **hard-link relationships do not** (they degrade to independent copies after reboot);
- **SIP-lite**: `system:/System`, `system:/bin`, `system:/drivers` are read-only at runtime (root included; `.`/`..` normalized before the decision); only the signed updater `_pkgd`'s install/remove scope may write; `sip=0` disables it;
- **RTC timestamps**: files and directories record `mtime` (CMOS clock, interpreted as UTC); `stat` shows it and `date` prints the current time;
- **Sector cache**: an 8-entry write-through cache at the block layer (invalidated on write) to cut repeated disk reads;
- **fsck**: the `fsck` command checks types/sizes/child back-pointers/parent cycles and prints `nodes=… files=… dirs=… errors=…`.

### FAT16 exchange volume (`fat:/`)

- The second disk `ata1` (`dist/fat.img`, 8 MB) is mounted as FAT16 under the drive name `fat`;
- the host tool [tools/mkfat16.ps1](tools/mkfat16.ps1) formats, writes, lists, and extracts files (no dependency on mkfs.fat):

```
.\tools\mkfat16.ps1 -Path dist\fat.img -File HOST.TXT -Content "hello from host"
.\tools\mkfat16.ps1 -Path dist\fat.img -List
.\tools\mkfat16.ps1 -Path dist\fat.img -Extract K.TXT
```

- inside the kernel it is just a normal path: `ls fat:/`, `cat fat:/host.txt`, `write fat:/k.txt hi`;
- the boot self-test reads `host.txt` and prints `fat16: host.txt = ...`;
- limits: root directory only, 8.3 short names (auto-uppercased), no long names/subdirectories/timestamps.

Usage in the shell:

```
ushell> mkdir docs
ok
ushell> write docs/a.txt hello
ok
ushell> cat docs/a.txt
hello
```

## Block devices and persistence

`block/` is the block layer; `ram0` is a 256 KB RAM disk (512 sectors of 512 B):

```c
typedef struct block_device {
    char name[16];
    uint32_t sector_size, sector_count;
    int (*read)(struct block_device *, uint32_t lba, void *buf);
    int (*write)(struct block_device *, uint32_t lba, const void *buf);
} block_device_t;
```

On top of it the filesystem defines a real on-disk format, **XFS1**:

| Sector | Contents |
|---|---|
| 0 | superblock: magic `XFS1`, version, sector count, inode count/size, inode-table location, data-area location, drive-name list |
| 1..7 | inode table: 64 × 52 bytes (used/type/parent/size/data_lba/volume/name) |
| 8..511 | data area (file contents stored in consecutive sectors) |

- `fs_sync("ram0")`: depth-first walk of the whole tree → writes the inode table + file data + superblock;
- `fs_mount_disk("ram0")`: verify the superblock → clear the in-memory tree → rebuild in inode order (a parent's index always precedes its children) → read the file data back;
- the boot self-test does `sync` → `fs_init()` clears → `mount_disk` restores → checks the size of `/system/bin/shell`: `disk: remount ok (sectors=512, /system/bin/shell=13064 bytes)`.

## System calls

IDT vector `0x80` is a **DPL3 interrupt gate** (`0xEE`), so user mode can already issue `int 0x80` directly.

Convention (classic i386 ABI):

```
eax = call number    ebx/ecx/edx = arguments   return value in eax
```

| # | Name | Arguments | Returns |
|---|---|---|---|
| 1 | `SYS_WRITE` | ebx=buf, ecx=len | bytes written |
| 2 | `SYS_GETPID` | — | current pid |
| 3 | `SYS_UPTIME` | — | seconds since boot |
| 4 | `SYS_YIELD` | — | 0 (yield the CPU) |
| 5 | `SYS_EXIT` | ebx=exit code | does not return |
| 6 | `SYS_OPEN` | ebx=path, ecx=flags | **fd** (≥3) |
| 7 | `SYS_FREAD` | ebx=fd, ecx=buf, edx=len | bytes read |
| 8 | `SYS_FWRITE` | ebx=fd, ecx=buf, edx=len | bytes written |
| 9 | `SYS_CLOSE` | ebx=fd | 0 |
| 10 | `SYS_GETCHAR` | — | character (blocking) |
| 11 | `SYS_LIST` | ebx=path, ecx=index, edx=buf | 0 / -1 |
| 12 | `SYS_DRIVES` | ebx=index, ecx=buf, edx=max | 0 / -1 |
| 13 | `SYS_MKDIR` | ebx=path | 0 / -1 |
| 14 | `SYS_UNLINK` | ebx=path | 0 / -1 |
| 15 | `SYS_STAT` | ebx=path, ecx=buf(8B) | 0 / -1 |
| 16 | `SYS_CHDIR` | ebx=path | 0 / -1 |
| 17 | `SYS_GETCWD` | ebx=buf, ecx=max | length / -1 |
| 18 | `SYS_FORK` | — | child pid; 0 in the child |
| 19 | `SYS_EXEC` | ebx=path, ecx=arg string | does not return (on success) |
| 20 | `SYS_WAIT` | ebx=pid, ecx=&status | child pid / -1 |
| 21 | `SYS_INPUT_SET` | ebx=buf, ecx=len | 0 / -1 (draw into the input box) |
| 22 | `SYS_BRK` | ebx=increment | new heap top / -1 |
| 23 | `SYS_KILL` | ebx=pid, ecx=sig | 0 / -1 |
| 24 | `SYS_SLEEP` | ebx=milliseconds | 0 |
| 25 | `SYS_PIPE` | ebx=&fds[2] | 0 / -1 |
| 26 | `SYS_DUP2` | ebx=old fd, ecx=new fd | new fd / -1 |
| 27 | `SYS_REBOOT` | — | does not return (8042 reset) |
| 28 | `SYS_PANIC` | — | does not return (debug: force a panic) |
| 29 | `SYS_OOPS` | — | does not return (debug: force a kernel oops) |
| 30 | `SYS_SIGACTION` | ebx=sig, ecx=handler | previous handler / -1 |
| 31 | `SYS_SIGRETURN` | — | restores the pre-signal context |
| 32 | `SYS_SYNC` | — | 0 / -1 (flush to the persistent device) |
| 33 | `SYS_TIME` | ebx=buf, ecx=len | 0 (formatted RTC time) |
| 34 | `SYS_FSCK` | ebx=buf, ecx=len | error count (summary in buf) |
| 35 | `SYS_PCI` | ebx=index, ecx=buf | 0 (device info) / -1 |
| 36 | `SYS_POWEROFF` | — | does not return (ACPI S5) |
| 37 | `SYS_MOUSE` | ebx=int[3] | 0 ({x,y,buttons}) |
| 38 | `SYS_HANG` | — | does not return (debug: cli + infinite loop) |
| 39 | `SYS_ETH` | ebx=0 info / 1 send a test frame | 0 / -1 |

Each process has its own fd table (0=stdin, 1=stdout, 2=stderr, the rest are files), and
`SYS_OPEN` returns an fd rather than a global object handle.

The entry points `isr128`/`isr129` share the `isr_common` stack layout with exceptions: vector 128
goes to `native_syscall_dispatch()`, vector 129 to the legacy `syscall_dispatch()`, and the return
value is written back into the `eax` slot of the saved frame, so it is in `eax` after `iret`.

The self-test really walks both gates at boot:

```
sys: int 0x80 native + int 0x81 legacy gates installed (DPL3)
sys: int 0x81 works
sys: write=20 uptime=0 s
native: ABI 0.1 gate ok (features=0)
native: handle table ok
```

The ring3 demo also performs a real Native ABI call:

```
native: ring3 ABI 0.1 ok
```

The shell's `sys` command runs another round:

```
shell> sys
sys: hello via int 0x80
sys: write=24 pid=1 uptime=3 s (total calls=5)
```

**Note**: today the calls still come from ring0 kernel threads with kernel pointers, so they can be
dereferenced directly. Once P2 lands user mode, `syscall_dispatch` must first check that a pointer
falls inside the user address range — otherwise user mode can read kernel memory through
`SYS_WRITE`. That is the first thing to fix next.

## User mode

The kernel really switches from ring0 to ring3 and back:

1. the GDT gains DPL3 user code (`0x18`) / data (`0x20`) segments and loads a TSS (`0x28`);
2. **one page directory per process**: `proc_spawn` copies the kernel mappings and clears the user-area PDEs, and a thread switch loads the matching CR3 — user pages belong only to that process;
3. code and user-stack pages come from the PMM and are mapped with 4 KB pages at `0x40000000` / `0x401FF000`, with **the U/S bit set in both PDEs and PTEs**;
4. **ELF32 loader**: parse the program headers, allocate a physical page per `PT_LOAD` segment as needed, copy the file contents, zero `.bss`, then map at the segment address; `e_entry` is the entry point;
5. **argc/argv**: the kernel builds a cdecl call frame on the user stack, `[ret][argc][argv pointer]`, with the argv array and strings on the stack page; `SYS_EXEC(path, args)` splits the argument string and pushes it;
6. `TSS.esp0` points at the current thread's kernel stack, and `iret` enters ring3;
7. **mini-libc**: [src/user/ulib.c](src/user/ulib.c) provides `printf/puts/malloc` and string functions, printing through `SYS_WRITE` and allocating through `SYS_BRK` (page-mapped from `0x40080000`).

Measured log:

```
disk: remount ok (sectors=512, /system/bin/shell=13064 bytes)
user: entering ring3 (/system/bin/shell, entry=0x40000000 pid=1)
ushell: ready (ring3, int 0x80)
ushell: argc=0
ushell: libc ok (argc=0, ptr=0x40080000, str=libc)
ushell> echo hi there
hi there
ushell> exec /system/bin/shell foo bar
user: exec /system/bin/shell (entry=0x40000000, args='foo bar')
ushell: argc=2 argv0=foo
```

`SYS_WRITE` validates the user buffer: the pointer must be below `0xC0000000`, the length at most
64 KB, and the copy goes through `copy_from_user` so it is recoverable — ring3 cannot hand the
kernel a kernel address or an unmapped address.

**Current limits**: there is only one user program image (`/system/bin/shell`); the user shell reads
the keyboard blockingly; there is no fork/exec semantics yet, no wait queues, and no general loader
that runs a program from an arbitrary filesystem path.

## Automated tests

`make test` / `.\build.ps1 -Test` build the image, boot it headless, inject commands (`help` + `fork` by default), and then assert:

1. the serial port produced output;
2. `selftest: PASS` (all 8 memory checks pass, including the PAE 2 MB identity map);
3. `obj: subsystems ready` plus `drive: system` / `drive: data` (multiple volumes);
4. `demo: fs write/read ok` and `demo: handle type check ok`;
5. `kernel: ready`;
6. `intr: handlers irq0=timer irq1=keyboard` (IRQ handlers are registered);
7. `went to sleep on` (the wait queue works; the thread really slept);
8. `ushell: cwd=system:` and `ushell: fs syscalls ok` (cwd + filesystem syscalls);
9. `sys: int 0x81 works` and `sys: write=N` (the legacy syscall gate works);
10. `native: ABI 0.1 gate ok`, `native: handle table ok`, `native: ring3 ABI 0.1 ok` (Native ABI entry, handle table, and ring3 calls all work);
11. `user: entering ring3` and `hello from ring3` (user mode really runs);
12. `sys: unmapped user pointer -> -1` (the fault was recovered, no panic);
13. `thread: heartbeat N pid=M` with an advancing count (the timer tick is moving);
14. `ushell: ready` (the ring3 shell is loaded from `/system/bin/shell` and runs);
15. the injected `help` is executed (`ushell: commands`);
16. the injected `fork` creates and reaps a child (`child: hello from fork child`, `parent: waited pid N, status 3`);
17. `disk: remount ok` (synced to the block device and remounted with files intact);
18. `ushell: argc=0` (the kernel builds argc/argv per the ELF startup contract);
19. `ushell> echo hi` prints `hi` (arguments really reach the user program);
20. `ushell: libc ok (ptr=0x402xxxxx)` (malloc goes through `SYS_BRK`, printf through `SYS_FWRITE`);
21. `ushell: pipe ok sleep ok` (pipe + sleep syscalls);
22. `user: ELF text RO / data RW (W^X loader)` (ELF code pages read-only, data pages writable, RWX segments rejected);
23. `user: layout code=0x40000000..0x40100000 stack=0x40100000..0x40200000 heap=0x40200000..0x40300000 tramp=0x40300000` (user address-space partitioning, guard page and trampoline page separated);
24. `sectest: code write pid=N status=142` (writing a code page raises #PF and the child is killed);
25. `sectest: guard write pid=N status=142` (writing the stack guard raises #PF and the child is killed);
26. `cpu: SMEP supported=1 enabled=1` (SMEP enabled);
27. `cpu: SMAP supported=1 enabled=1` (SMAP enabled; the full regression passes under SMAP);
28. `ASLR stack/heap/trampoline` (at least two processes differ in `stack_init`, `heap_base`, and `tramp`);
29. `vmm: PAE on, NX enabled` (PAE paging and NX enabled);
30. `sectest: stack exec pid=N status=142` (executing a stack address raises an NX #PF);
31. `sectest: rodata exec pid=N status=142` (executing a rodata address raises an NX #PF);
32. `sandbox: open denied` (the sandboxed process's `SYS_OPEN` is rejected by the allowlist);
33. `trust: shell signature OK` (the RSA-2048/SHA-256 signature verifies);
34. `trust: tampered image rejected` (an image with one flipped byte is rejected);
35. `vmm: address space teardown ok` (PMM accounting is restored after reclaiming user pages/PT/PD/PDPT);
36. `exec address-space switch` (the ring3 shell is entered again after `reexec`);
37. `sandbox: native denied` (native `HANDLE_DUP` is rejected by the allowlist);
38. **no kernel panic** (the whole regression run shows no kernel panic or canary corruption);
39. `pmm: linear first-fit selftest ok` (reserved regions are not allocatable, 256 pages are unique, accounting is restored after free);
40. `vmm: kernel W^X active` (kernel text RX, rodata R+NX, data RW+NX);
41. `boot: KERNEL SIGNATURE OK` (the loader verifies the RSA signature before jumping into the kernel);
42. `demo: fs refcount ok` (one close after an fs-handle ref still leaves the handle readable);
43. `sectest: fd write denied` (a write capability on a read-only fd is denied);
44. `madt: 2 cpu(s)` / `smp: BSP online ... AP startup pending` (two cores discovered);
45. `smp: 2/2 cpus online, 1 AP GDT/TSS/IDT ready, AP ticks=... ipi=...` (the AP's local timer/IPI works);
46. `thread: heartbeat ... cpu=1` (the heartbeat is scheduled on the AP);
47. `sectest: cap drop denied (-1)` (after explicitly revoking the fd WRITE capability, writes are denied immediately);
48. `sectest: cap recv hello from ramfs` / `cap xfer ... status=0` (cross-process fd capability transfer);
49. `cap: table selftest ok` (the generic per-process capability table install/transfer/close);
50. `native: process_create/wait ok` (create a child by registered program name and wait; the child may run on the AP);
51. `user: entering ring3 (builtin demo ... cpu=1)` (the ring3 userdemo runs on the AP);
52. `sched: pid N (spin) running on cpu 1` (load placement: the AP is chosen by runqueue load, not by program name);
53. `native: handle table ok` (native handle restrict/transfer/generation self-test);
54. `sectest: mmap ok` (anonymous mmap, write/read, munmap);
55. `native: channel ok` (send/recv over a paired channel endpoint);
56. `native: port ok` (Port create → connect → accept → channel data);
57. channel/port blocking recv/accept wired into `proc_block` plus send/connect wakeups;
58. `native: channel block ok` (the fork child sleeps and then sends; the parent's blocking recv wakes up);
59. `native: service ok` (service registry: register → connect by name → accept);
60. `native: supervisor ok` (the supervisor service queries the registry and replies);
61. `user: entering ring3 (ring3 AP probe ... cpu=1)` + `ring3 AP probe ok`;
62. `native: manifest ok` (the supervisor checks the supervisor + echo service manifest);
63. `native: handle revoke ok` (after revoke the handle fails but close still releases the object reference);
64. `net: UDP echo reply` (QEMU user networking + a host PowerShell UDP echo);
65. `sectest: udp ok` (ring3 completes a UDP echo via `SYS_UDP_SEND/RECV`);
66. `sectest: socket udp ok` (socket → sendto → recvfrom → close);
67. `svcwatch: restart echo -> pid N` (a periodic watchdog notices the missing echo service and starts echosvc);
68. `sectest: chroot ok` (per-process namespace; after rooting, `..` escapes are rejected);
69. `native: restart ok` (reconnect to echo after a restart and complete a data round-trip);
70. `sectest: fd limit ok` (per-process fd limit of 16; 10 files open at once);
71. `net: TCP echo reply` (TCP client: SYN/SYN-ACK/ACK/data, multi-connection retries, data retransmission);
72. `sectest: fs service ok` (the ring3 fsd memory file service, read through Port/Channel);
73. `sectest: fs write ok` (the fsd write path: 'w'+data, then the VFS svc reads the new contents back);
74. `sectest: svc vfs ok` (the VFS `svc:` backend routes open/read to the ring3 fsd);
75. `sectest: svc vfs write ok` (VFS write buffers, then sends to the ring3 fsd on close);
76. `sectest: blk service ok` (the ring3 blkd virtual block service, read through the VFS `blk:`);
77. `sectest: tcp ok` (user mode drives a TCP echo; boot no longer probes TCP);
78. `sectest: blk write ok` (VFS `blk:` write buffers, then sends to the ring3 blkd on close).

A failure returns non-zero, so it drops straight into CI.

**Network end-to-end self-test (bypassing slirp)**:

```powershell
.\tools\net_peer_test.ps1        # 20 seconds, expect NET PEER PASS
```

It uses QEMU's socket network backend and plays the peer itself: answering ARP requests with ARP replies, ICMP echoes with echo replies, and mirroring UDP back, then asserts that `net: ARP reply gw=` / `net: ICMP echo reply` / `net: UDP echo reply` appear on the serial port.

Why not `-nic user`: slirp answers ARP/ICMP on behalf of the gateway but does **not** hand guest UDP destined for `10.0.2.2` to a host listening port, so a UDP echo cannot be asserted reliably. The socket backend is fully deterministic and needs no internet.

## Stress / soak testing

```powershell
.\tools\stress.ps1 -Hours 8                    # 8-hour soak (smp=2 by default)
.\tools\stress.ps1 -Hours 0.05 -InjectEverySec 15   # 3-minute smoke run
New-Item stress-run\STOP                        # stop early
```

What the script does: write the serial log to a file and **tail it by byte offset** (no dependence on a TCP serial port), inject one command every 60 seconds through HMP `sendkey` (`ls` / `echo` / `pwd` / `fork` / `cat`), and continuously scan for `KERNEL PANIC`, `CPU exception`, `user: exception`, `Triple fault`; 300 seconds without serial growth counts as a hang. A deliberate crash or hang writes `CRASH.txt` and exits with status 2.

> Do not use `tools/soak.ps1` for multi-hour runs: its serial link goes over TCP, which goes silent mid-run on this QEMU build (the guest keeps running), so it looks like a hang.

**One completed 8-hour run (2026-10-24 01:45–09:45, smp=2):**

| Metric | Value |
|---|---|
| Duration | 480 minutes (finished on time) |
| Injected commands | 478, all 478 echoed back with `ushell>` |
| Heartbeats | 28,559 (≈1/s, no gap) |
| `KERNEL PANIC` / exceptions / oops / stack canary | 0 / 0 / 0 / 0 |
| `vmm: WARN` / allocation failures | 0 / 0 |
| Crash files | none |

Covered: dual-core scheduling and migration, APIC/PIT, the shell command loop, `fork`/`exec`/COW, filesystem reads/writes and flush, capability/pipe, console lock contention, boot-time ARP/ICMP/UDP.
Not covered: sustained network traffic, precise memory-leak quantification (which needs a before/after `pmm` free-page comparison).

## Known limitations

- **SMP is 2-core shared-memory multiprocessing (limited)**: BSP + 1 AP. Per-CPU GDT/TSS, a shared IDT, LAPIC timer, IPI/TLB shootdown, and **per-CPU runqueues with load-based placement/migration** are all in place; `heartbeat` is pinned to the AP as a probe via a program flag (`PROG_F_PIN_AP`), other processes are placed by `sched_place()` according to load, and `sched_balance()` migrates READY processes when the load difference is ≥2;
- **SMP shared state**: the object table, pipes, and the filesystem (one global ramfs lock, wrapped at the public entry points) are serialized; some process-table traversal (`proc_wakeup`/`proc_find`) and the kernel page-table modification paths are not locked entry by entry, so theoretical races remain under cross-core contention, and no stress validation has been done;
- the filesystem lives in memory plus an `ata0` disk image (`dist/disk.img`, survives a reboot) or the `ram0` RAM disk: at most 4 drives, 64 nodes, names ≤64 characters, 64 KB per file, paths ≤255 characters; drive names are reserved and cannot be used as top-level directory names;
- **FAT16 supports only the root directory with 8.3 short names** (no LFN, no subdirectories, mtime 0), and both reads and writes only target root-directory files;
- RTC time is interpreted as **UTC** (no time zones or DST);
- user mode gives each process its own page directory, but there is only one user program image, `/system/bin/shell`;
- the user shell uses blocking `SYS_GETCHAR` (the thread sleeps on the keyboard channel);
- the identity map uses 2 MB PAE large pages; the kernel image range is split into 4 KB pages with kernel W^X enabled: text RX, rodata R+NX, data/bss RW+NX; user pages and kernel high addresses (vmalloc, MMIO) use 4 KB PTEs;
- the heap is a fixed 16 MB region with 4-byte-aligned first-fit and no `realloc`;
- a kernel-mode exception prints an oops plus a backtrace and kills the current thread; it panics only when there is no current thread at all;
- signals support `SIG_DFL` (terminate) / `SIG_IGN` / user handlers; when a handler returns, the trampoline page (`0x40300000`, `mov eax,31; int 0x81`) calls `SYS_SIGRETURN` to restore the context; only one signal frame exists at a time and signals do not nest;
- user address layout: code/data `0x40000000..0x40100000`, stack `0x40100000..0x40200000` (grows down, 4 KB guard at the bottom), heap `0x40200000..0x40300000`, signal trampoline page `0x40300000`;
- fork uses **COW**: only writable pages get a COW mapping, code pages stay permanently read-only; physical pages are refcounted;
- the generic exception table has only 8 slots and currently registers a single uaccess region;
- the keyboard handles only printable keys in scan-code set 1 (including Shift/CapsLock/Ctrl+C); no arrow or function keys.

## License

Unless stated otherwise, this repository is released under the **Apache License 2.0** (see [`LICENSE`](LICENSE)).
Copyright 2026 Aster / x86 OS contributors.

Contributing means you agree to the [DCO 1.1](https://developercertificate.org/) and license your work under the same terms; see [`CONTRIBUTING.md`](CONTRIBUTING.md) for the workflow, commit conventions, and test requirements, [`SECURITY.md`](SECURITY.md) for reporting security issues, and [`keys/README.md`](keys/README.md) for signing-key management.

> The repository **does not contain the signing private key**; `keys/shell_private.pem` is gitignored and generated locally by `tools/gen-key.ps1` before the first build. The modulus fingerprint of a released image is published in the release notes and can be checked against `keys/shell_public.pem`.
