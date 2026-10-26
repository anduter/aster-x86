# =============================================================================
# build.ps1 - Windows 一键构建 / 测试脚本（不依赖 POSIX shell）
#
#   .\build.ps1                  # 只构建 dist\floppy.img
#   .\build.ps1 -Test            # 构建 + 无界面启动 + 自动断言（失败返回非 0）
#   .\build.ps1 -Run             # 构建 + 带窗口启动（串口打到控制台）
#   .\build.ps1 -Shot x.ppm      # 构建 + 无界面启动 + 截屏到 x.ppm
#   -Mem 512 -Vga cirrus         # 透传 QEMU 参数
#   -Keys "help"                 # -Test 时注入的命令（自动补回车）
#
# 多会话并行（各会话一套产物，互不覆盖）：
#   .\build.ps1 -ObjDir .obj-a -DistDir .dist-a -QmpPort 4481 -SerialLog serial-a.log
#   .\build.ps1 -Test -ObjDir .obj-b -DistDir .dist-b -QmpPort 4491 -SerialLog serial-b.log
# 注意：-Test 会在宿主监听 UDP/TCP 12345（**客户机里编译期固定的回显端口**；
# 可用 -EchoPort 覆盖内核侧端口，但用户态 user_shell.c 仍固定 12345），
# 所以同一时刻只能有一个会话跑 -Test；脚本会在启动 QEMU 前探测该端口并明确报错。
# =============================================================================
param(
    [switch]$Test,
    [switch]$Run,
    [string]$Shot = '',
    [int]$Mem = 256,
    [string]$Vga = 'std',
    [string]$Cpu = 'qemu32,+nx,+smep,+smap',
    [int]$FontScale = 1,
    [int]$TimeoutSec = 12,
    [string]$Keys = "help`nfork`nsectest`nlinktest`necho hi`nreexec",
    # 并行会话隔离：产物目录 / 镜像目录 / QMP 端口 / 串口日志都可就近覆盖
    [string]$ObjDir = 'obj',
    [string]$DistDir = 'dist',
    [int]$QmpPort = 4444,
    [int]$EchoPort = 12345,
    [string]$SerialLog = ''
)

try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

# ---- 并行会话：把共享路径收敛成变量，默认值与历史行为一致 ----
function Resolve-UnderRoot([string]$p) {
    if ([IO.Path]::IsPathRooted($p)) { return $p }
    return (Join-Path $root $p)
}
$objRoot  = Resolve-UnderRoot $ObjDir
$distRoot = Resolve-UnderRoot $DistDir
$serial   = if ($SerialLog) { Resolve-UnderRoot $SerialLog } else { Join-Path $root 'serial.log' }
$tcpLog   = Join-Path $distRoot 'tcp_host.log'

# nasm 的 incbin 写死了 obj/ 前缀。用 -p 预包含一个生成文件把真实路径注入进去，
# 避免在命令行上传带引号的 -D（PowerShell 5.1 的原生参数转发会把引号吃掉）。
# 见 loader.asm / ap_trampoline_blob.asm 里的 %ifndef 默认值。
$nasmInc = Join-Path $objRoot 'nasm_paths.inc'

# 自带交叉工具链优先
$tc = Join-Path $root 'tools\i686-elf\bin'
if (Test-Path $tc) { $env:Path = "$tc;$env:Path" }

$CFLAGS = @(
    '-ffreestanding', '-fno-pic', '-fno-pie', '-m32', '-nostdlib',
    '-fno-asynchronous-unwind-tables', '-fno-stack-protector', '-fno-builtin',
    "-DFONT_SCALE=$FontScale",
    "-DNET_ECHO_PORT=$EchoPort",
    '-Wall', '-Wextra', '-Werror', '-g', '-O0', '-Isrc/kernel/include', '-Isrc/kernel'
)

# ---- 测试模式开关（账号系统 P1） ----
# -Test / -Shot 是开发/回归用的构建：内核参数默认带 autologin=root，bootseq 直接
# spawn shell 而不经过 getty -> login，保证既有回归不被登录流程打断。
# 生产构建（不带这两个开关）不带这个 define，cmdline 为空 -> 必须登录。
if ($Test -or $Shot) { $CFLAGS += '-DOS_TEST_BUILD=1' }

# ---- 增量构建：参数未变且对象比源文件新时跳过编译（头文件变化则全量重编） ----
$swTotal = [Diagnostics.Stopwatch]::StartNew()
New-Item -ItemType Directory -Force -Path $objRoot | Out-Null
$flagsFile = Join-Path $objRoot '.flags'
$flagsText = ($CFLAGS -join ' ') + '|' + $nasmInc + '|' + $FontScale
$shaFlags = [Security.Cryptography.SHA256]::Create()
$flagsHash = [BitConverter]::ToString($shaFlags.ComputeHash([Text.Encoding]::UTF8.GetBytes($flagsText)))
$forceAll = $true
if (Test-Path $flagsFile) {
    $old = (Get-Content $flagsFile -Raw) -replace '\s', ''
    if (($old -replace '-', '') -eq ($flagsHash -replace '-', '')) { $forceAll = $false }
}
Set-Content -Path $flagsFile -Value $flagsHash -Encoding ASCII
$newestHeader = Get-ChildItem (Join-Path $root 'src\kernel') -Recurse -Filter *.h -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$compiled = 0
$skipped  = 0

function Test-ObjectFresh([string]$obj, [string]$src) {
    if ($forceAll) { return $false }
    if (-not (Test-Path $obj)) { return $false }
    $o = (Get-Item $obj).LastWriteTimeUtc
    if ($o -lt (Get-Item $src).LastWriteTimeUtc) { return $false }
    if ($newestHeader -and $o -lt $newestHeader.LastWriteTimeUtc) { return $false }
    return $true
}

function T([string]$m) { Write-Host ("[t {0,6:N1}s] {1}" -f $swTotal.Elapsed.TotalSeconds, $m) -ForegroundColor DarkGray }

function Invoke-Step {
    param([string]$Name, [string]$Exe, [string[]]$ArgList)
    Write-Host ("  [{0}]" -f $Name) -ForegroundColor DarkGray
    & $Exe @ArgList
    if ($LASTEXITCODE -ne 0) {
        throw ("{0} failed with exit code {1}" -f $Name, $LASTEXITCODE)
    }
}

# 通过 QMP 往客户机注入按键（等价于 monitor 的 sendkey）
function Send-Keys {
    param([string]$Text, [int]$Port = 4444)
    $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
    $stream = $client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true

    $null = $reader.ReadLine()                                   # 问候 JSON
    $writer.WriteLine('{"execute":"qmp_capabilities"}')
    $null = $reader.ReadLine()                                   # 能力协商应答

    foreach ($ch in $Text.ToCharArray()) {
        $key = if ($ch -eq "`n" -or $ch -eq "`r") { 'ret' }
               elseif ($ch -eq ' ') { 'spc' }
               else { [string]$ch }
        $cmd = '{"execute":"human-monitor-command","arguments":{"command-line":"sendkey ' + $key + '"}}'
        $writer.WriteLine($cmd)
        $null = $reader.ReadLine()
        Start-Sleep -Milliseconds 120
    }
    $client.Close()
}

# 通过 QMP 截屏
function Save-Screenshot {
    param([string]$Path, [int]$Port = 4444)
    $client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
    $stream = $client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true

    $null = $reader.ReadLine()
    $writer.WriteLine('{"execute":"qmp_capabilities"}')
    $null = $reader.ReadLine()
    $writer.WriteLine(('{{"execute":"screendump","arguments":{{"filename":"{0}"}}}}' -f ($Path -replace '\\', '/')))
    $null = $reader.ReadLine()
    Start-Sleep -Seconds 1
    $client.Close()
}

# 端口占用探测：并行会话要快速失败，而不是等 60 秒超时后报一堆假失败
function Test-PortFree {
    param([int]$Port, [string]$Proto = 'tcp', [string]$Addr = '127.0.0.1')
    try {
        if ($Proto -eq 'udp') {
            $u = New-Object System.Net.Sockets.UdpClient($Port)
            $u.Close()
        } else {
            $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse($Addr), $Port)
            $l.Start(); $l.Stop()
        }
        return $true
    } catch {
        return $false
    }
}

New-Item -ItemType Directory -Force -Path $objRoot, $distRoot | Out-Null

# 生成 nasm 预包含文件：把本次的 obj 目录写成 incbin 能用的绝对路径
$incText = (@(
    ('%define AP_TRAMPOLINE_BIN "{0}"' -f ((Join-Path $objRoot 'ap_trampoline.bin') -replace '\\', '/')),
    ('%define BOOT_VERIFY_BIN  "{0}"' -f ((Join-Path $objRoot 'boot_verify.bin')  -replace '\\', '/')),
    ''
) -join "`r`n")
[IO.File]::WriteAllText($nasmInc, $incText, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("=== session: obj={0} dist={1} qmp={2} serial={3} ===" -f $objRoot, $distRoot, $QmpPort, $serial) -ForegroundColor DarkCyan

Write-Host "=== build ===" -ForegroundColor Cyan

Invoke-Step 'boot sector' 'nasm' @('-f', 'bin', '-i', 'src/boot/', 'src/boot/boot.asm', '-o', (Join-Path $objRoot 'boot.bin'))
Invoke-Step 'ap trampoline' 'nasm' @('-f', 'bin', 'src/boot/ap_trampoline.asm', '-o', (Join-Path $objRoot 'ap_trampoline.bin'))

# ---- 用户态程序：独立编译，链接到 0x40000000，产出 ELF ----
Write-Host "=== user program ===" -ForegroundColor Cyan
Invoke-Step 'user_shell.c' 'i686-elf-gcc' @(
    '-ffreestanding', '-fno-pic', '-fno-pie', '-m32', '-nostdlib', '-fno-builtin',
    '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-Wall', '-Wextra', '-Werror', '-O0',
    '-c', 'src/user/user_shell.c', '-o', (Join-Path $objRoot 'user_shell.o')
)
Invoke-Step 'ulib.c' 'i686-elf-gcc' @(
    '-ffreestanding', '-fno-pic', '-fno-pie', '-m32', '-nostdlib', '-fno-builtin',
    '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-Wall', '-Wextra', '-Werror', '-O0',
    '-c', 'src/user/ulib.c', '-o', (Join-Path $objRoot 'ulib.o')
)
Invoke-Step 'user_shell.elf' 'i686-elf-ld' @('-m', 'elf_i386', '-T', 'src/user/user.ld', '-o', (Join-Path $objRoot 'user_shell.elf'), (Join-Path $objRoot 'user_shell.o'), (Join-Path $objRoot 'ulib.o'))
Invoke-Step 'user_shell.bin' 'i686-elf-objcopy' @('-O', 'binary', (Join-Path $objRoot 'user_shell.elf'), (Join-Path $objRoot 'user_shell.bin'))
Invoke-Step 'sign shell' 'powershell' @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'sign_image.ps1',
    '-Image', (Join-Path $objRoot 'user_shell.elf'), '-Key', 'keys/shell_private.pem',
    '-Signature', (Join-Path $objRoot 'shell.sig'), '-OutC', (Join-Path $objRoot 'shell_signature.c')
)

# src/kernel 下所有 .asm（含 intr/isr.asm 中断桩）都要汇编
$objs = @()
Write-Host '=== signature object ===' -ForegroundColor Cyan
& i686-elf-gcc @CFLAGS -c (Join-Path $objRoot 'shell_signature.c') -o (Join-Path $objRoot 'shell_signature.o')
if ($LASTEXITCODE -ne 0) { throw 'shell signature compile failed' }
$objs += (Join-Path $objRoot 'shell_signature.o')
$asmFiles = Get-ChildItem -Path 'src/kernel' -Recurse -Filter *.asm
foreach ($f in $asmFiles) {
    $rel = $f.FullName.Substring((Resolve-Path 'src/kernel').Path.Length + 1)
    $obj = Join-Path $objRoot ($rel -replace '\.asm$', '.o')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $obj) | Out-Null
    # user_blob.asm incbin 了 user_shell.elf，但 elf 不在增量判据里：
    # shell 变化后必须强制重汇编，否则嵌入旧 blob（trust 验签失败 / 假失败）。
    $forceBlob = $false
    if ($rel -eq 'user\user_blob.asm' -or $rel -eq 'user/user_blob.asm') {
        $shellElf = Join-Path $objRoot 'user_shell.elf'
        if ((Test-Path $shellElf) -and ((-not (Test-Path $obj)) -or ((Get-Item $shellElf).LastWriteTimeUtc -gt (Get-Item $obj).LastWriteTimeUtc))) {
            $forceBlob = $true
        }
    }
    if ((-not $forceBlob) -and (Test-ObjectFresh $obj $f.FullName)) { $skipped++; $objs += $obj; continue }
    Write-Host ("  [{0}]" -f $rel) -ForegroundColor DarkGray
    & nasm -f elf32 -i $objRoot -p $nasmInc $f.FullName -o $obj

    $compiled++
    if ($LASTEXITCODE -ne 0) { throw ("assemble failed: {0}" -f $rel) }
    $objs += $obj
}

# src/kernel 下所有 .c，保留相对路径放到 obj 下
$cFiles = Get-ChildItem -Path 'src/kernel' -Recurse -Filter *.c
foreach ($f in $cFiles) {
    $rel = $f.FullName.Substring((Resolve-Path 'src/kernel').Path.Length + 1)
    $obj = Join-Path $objRoot ($rel -replace '\.c$', '.o')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $obj) | Out-Null
    if (Test-ObjectFresh $obj $f.FullName) { $skipped++; $objs += $obj; continue }
    Write-Host ("  [{0}]" -f $rel) -ForegroundColor DarkGray
    & i686-elf-gcc @CFLAGS -c $f.FullName -o $obj

    $compiled++
    if ($LASTEXITCODE -ne 0) { throw ("compile failed: {0}" -f $rel) }
    $objs += $obj
}

# ---- 组件包（.pkg）：用构建签名私钥打包，并生成内核内建包目录 ----
# 包格式/验签见 src/kernel/include/pkg.h；宿主侧先自检一遍（tools/pkg-verify.ps1），
# 免得坏包要等 QEMU 里安装失败才暴露。
Write-Host '=== packages ===' -ForegroundColor Cyan
$pkgOutDir = Join-Path $objRoot 'pkg'
New-Item -ItemType Directory -Force -Path $pkgOutDir | Out-Null
$mkpkg = Join-Path $root 'tools\mkpkg.ps1'
$pkgVerify = Join-Path $root 'tools\pkg-verify.ps1'
$pkgSpecs = @(
    @{ Name='echosvc';  Version='1.0.0'; Type='service'; Entry='echosvc';     Requires='service:supervisor,abi:1.3'; Caps='CAP_SVC_REGISTER'; Account='root';  Files='echosvc.conf';  Dir='packages\echosvc' },
    @{ Name='needsdep'; Version='1.0.0'; Type='service'; Entry='needsdep';    Requires='service:nosuchsvc,abi:1.3';  Caps='';                  Account='root';  Files='needsdep.conf'; Dir='packages\needsdep' },
    @{ Name='broken';   Version='1.0.0'; Type='service'; Entry='broken';      Requires='abi:1.3';                    Caps='';                  Account='root';  Files='broken.conf';   Dir='packages\broken' },
    @{ Name='helloapp'; Version='1.0.0'; Type='app';     Entry='bin/helloapp'; Requires='abi:1.3';                   Caps='';                  Account='alice'; Files='helloapp.conf'; Dir='packages\helloapp' }
)
$pkgGenC = @()
$catalogRows = @()
$modulusC = Join-Path $objRoot 'pkg_modulus.c'
$firstPkg = $true
foreach ($s in $pkgSpecs) {
    $pkgFile = Join-Path $pkgOutDir ("{0}-{1}.pkg" -f $s.Name, $s.Version)
    $blobC   = Join-Path $objRoot ("pkg_{0}.c" -f $s.Name)
    $mkArgs = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $mkpkg,
        '-Name', $s.Name, '-Version', $s.Version, '-Type', $s.Type, '-Entry', $s.Entry,
        '-Account', $s.Account,
        '-RootDir', $s.Dir, '-Files', $s.Files, '-Key', 'keys/shell_private.pem',
        '-ArrayPrefix', ('pkg_blob_' + $s.Name),
        '-Out', $pkgFile, '-OutC', $blobC
    )
    # 空的 -Requires/-Capabilities 经 powershell -File 会被当成"缺参数"，只在非空时传
    if ($s.Requires) { $mkArgs += @('-Requires', $s.Requires) }
    if ($s.Caps) { $mkArgs += @('-Capabilities', $s.Caps) }
    if ($firstPkg) { $mkArgs += @('-ModulusC', $modulusC); $firstPkg = $false }
    & powershell @mkArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw ("mkpkg failed: {0}" -f $s.Name) }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $pkgVerify -Package $pkgFile -Quiet | Out-Null
    if ($LASTEXITCODE -ne 0) { throw ("pkg-verify failed: {0}" -f (Split-Path -Leaf $pkgFile)) }
    $pkgGenC += $blobC
    $catalogRows += ('    {{ "{0}-{1}.pkg", pkg_blob_{0}, {2} }},' -f $s.Name, $s.Version, (Get-Item $pkgFile).Length)
}
# echosvc 的成品包也放一份到 dist/，方便手工检查/分发（dist 不进 git）
Copy-Item -LiteralPath (Join-Path $pkgOutDir 'echosvc-1.0.0.pkg') -Destination (Join-Path $distRoot 'echosvc-1.0.0.pkg') -Force -ErrorAction SilentlyContinue

$catalogExterns = (($pkgSpecs | ForEach-Object { 'extern const unsigned char pkg_blob_{0}[];' -f $_.Name }) -join "`r`n")
$catalogText = @"
/* Auto-generated by build.ps1 - 内核内建包目录（不要手改）。 */
#include "include/pkg.h"

$catalogExterns

const pkg_blob_t pkg_catalog[] = {
$($catalogRows -join "`r`n")
};
const unsigned int pkg_catalog_count = $($pkgSpecs.Count);
"@
$catalogC = Join-Path $objRoot 'pkg_catalog.c'
[IO.File]::WriteAllText($catalogC, $catalogText, (New-Object System.Text.UTF8Encoding($false)))
$pkgGenC += $catalogC
$pkgGenC += $modulusC
foreach ($g in $pkgGenC) {
    $go = ($g -replace '\.c$', '.o')
    & i686-elf-gcc @CFLAGS -c $g -o $go
    if ($LASTEXITCODE -ne 0) { throw ("pkg generated compile failed: {0}" -f $g) }
    $objs += $go
}

Invoke-Step 'link'    'i686-elf-ld' (@('-m', 'elf_i386', '--no-warn-rwx-segments', '-T', 'src/kernel/arch/x86/linker.ld', '-o', (Join-Path $objRoot 'kernel.elf')) + $objs)
Invoke-Step 'objcopy' 'i686-elf-objcopy' @('-O', 'binary', (Join-Path $objRoot 'kernel.elf'), (Join-Path $objRoot 'kernel.bin'))

# ---- loader 内核签名验证器 ----
Invoke-Step 'sign kernel' 'powershell' @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'sign_image.ps1',
    '-Image', (Join-Path $objRoot 'kernel.bin'), '-Key', 'keys/shell_private.pem',
    '-Signature', (Join-Path $objRoot 'kernel.sig'), '-OutC', (Join-Path $objRoot 'kernel_signature.c'),
    '-Prefix', 'kernel'
)
Write-Host '=== boot verifier ===' -ForegroundColor Cyan
& i686-elf-gcc @CFLAGS '-DBOOT_VERIFY' -c src/kernel/security/trust.c -o (Join-Path $objRoot 'boot_trust.o')
if ($LASTEXITCODE -ne 0) { throw 'boot trust compile failed' }
& i686-elf-gcc @CFLAGS -c src/boot/verify_support.c -o (Join-Path $objRoot 'boot_verify_support.o')
if ($LASTEXITCODE -ne 0) { throw 'boot verify support compile failed' }
& i686-elf-gcc @CFLAGS -c src/boot/verify_entry.c -o (Join-Path $objRoot 'boot_verify_entry.o')
if ($LASTEXITCODE -ne 0) { throw 'boot verify entry compile failed' }
& i686-elf-gcc @CFLAGS -c (Join-Path $objRoot 'kernel_signature.c') -o (Join-Path $objRoot 'kernel_signature.o')
if ($LASTEXITCODE -ne 0) { throw 'kernel signature compile failed' }
& i686-elf-ld -m elf_i386 -T src/boot/verify.ld -o (Join-Path $objRoot 'boot_verify.elf') `
    (Join-Path $objRoot 'boot_verify_entry.o') (Join-Path $objRoot 'boot_trust.o') (Join-Path $objRoot 'boot_verify_support.o') (Join-Path $objRoot 'kernel_signature.o')
if ($LASTEXITCODE -ne 0) { throw 'boot verifier link failed' }
& i686-elf-objcopy -O binary (Join-Path $objRoot 'boot_verify.elf') (Join-Path $objRoot 'boot_verify.bin')
if ($LASTEXITCODE -ne 0) { throw 'boot verifier objcopy failed' }

Invoke-Step 'loader' 'nasm' @('-f', 'bin', '-i', 'src/loader/', '-p', $nasmInc, 'src/loader/loader.asm', '-o', (Join-Path $objRoot 'loader.bin'))

# ---- 软盘镜像：LBA0 引导 | LBA1..32 加载器 | LBA33.. 内核 ----
$b = [IO.File]::ReadAllBytes((Join-Path $objRoot 'boot.bin'))
$l = [IO.File]::ReadAllBytes((Join-Path $objRoot 'loader.bin'))
$k = [IO.File]::ReadAllBytes((Join-Path $objRoot 'kernel.bin'))
if ($b.Length -ne 512)   { throw "boot.bin is not 512 bytes" }
if ($l.Length -ne 16384) { throw "loader.bin is not 16384 bytes" }
if ($k.Length -gt 1474560 - 33 * 512) { throw "kernel.bin exceeds 1.44MB floppy payload capacity" }

$img = New-Object byte[] 1474560
[Array]::Copy($b, 0, $img, 0, $b.Length)
[Array]::Copy($l, 0, $img, 512, $l.Length)
[Array]::Copy($k, 0, $img, 33 * 512, $k.Length)
$imgPath = Join-Path $distRoot 'floppy.img'
try {
    [IO.File]::WriteAllBytes($imgPath, $img)
} catch [System.IO.IOException] {
    # 镜像可能正被 QEMU 打开：本次改用备用文件名，避免整个测试流程失败
    $imgPath = Join-Path $distRoot 'floppy-run.img'
    Write-Warning ("floppy.img 被占用，本次改用 {0}" -f $imgPath)
    [IO.File]::WriteAllBytes($imgPath, $img)
}

Write-Host ("=== ok: {0}  (kernel {1} bytes) ===" -f $imgPath, $k.Length) -ForegroundColor Green

# 8MB 硬盘镜像（XFS1 持久化）。测试模式每次重建，-Run 模式保留数据。
$diskPath = Join-Path $distRoot 'disk.img'
if ($Test -or $Shot) {
    Remove-Item $diskPath -ErrorAction SilentlyContinue
}
if (-not (Test-Path $diskPath)) {
    $diskBytes = New-Object byte[] (8 * 1024 * 1024)
    [IO.File]::WriteAllBytes($diskPath, $diskBytes)
}
$diskArg = ("file={0},format=raw,if=ide,index=0" -f ($diskPath -replace '\\', '/'))

# 8MB FAT16 交换盘（ata1）：宿主工具生成，内核以 fat:/ 挂载
$fatPath = Join-Path $distRoot 'fat.img'
if ($Test -or $Shot) {
    Remove-Item $fatPath -ErrorAction SilentlyContinue
}
if (-not (Test-Path $fatPath)) {
    & (Join-Path $root 'tools\mkfat16.ps1') -Path $fatPath -File 'HOST.TXT' -Content 'hello from host'
}
$fatArg = ("file={0},format=raw,if=ide,index=1" -f ($fatPath -replace '\\', '/'))

if ($Run) {
    & qemu-system-i386 -fda $imgPath -drive $diskArg -drive $fatArg -boot a -m $Mem -vga $Vga -cpu $Cpu -smp 2 -nic user,model=e1000 -serial stdio -no-reboot
    return
}

if ($Test -or $Shot) {
    Remove-Item $serial -ErrorAction SilentlyContinue

    # 并行会话：端口被占要立刻说清楚，而不是等到回归报一堆"假失败"
    if (-not (Test-PortFree -Port $QmpPort)) {
        throw ("QMP 端口 {0} 已被占用（可能另一个会话的 QEMU 还在跑）。换一个 -QmpPort，例如 4481。" -f $QmpPort)
    }
    if ($Test) {
        if (-not (Test-PortFree -Port $EchoPort -Proto udp) -or
            -not (Test-PortFree -Port $EchoPort -Addr '0.0.0.0')) {
            throw "宿主 $EchoPort 端口已被占用：同一时刻只能跑一个 -Test（回显端口在客户机里是编译期常量 $EchoPort）。"
        }
    }
    $udpEcho = $null
    if ($Test) {
        $udpEcho = Start-Job -ArgumentList $EchoPort -ScriptBlock {
            param($EchoPort)
            $u = New-Object System.Net.Sockets.UdpClient($EchoPort)
            $u.Client.ReceiveTimeout = 250   # interruptible: Stop-Job must not wait on a blocked Receive
            try {
                $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
                $deadline = (Get-Date).AddSeconds(120)
                $served = 0
                while ((Get-Date) -lt $deadline -and $served -lt 16) {
                    try {
                        $data = $u.Receive([ref]$ep)
                        [void]$u.Send($data, $data.Length, $ep)
                        $served++
                    } catch { }
                }
            } finally { $u.Close() }
        }
    }

    $tcpEcho = $null
    if ($Test) {
        # 流式回显：阶段 4c 的大传输（16KB）需要"收多少回多少"，不能只读一次 64B。
        # 每个连接循环 read -> write 直到对端 FIN（Read 返回 0）或总字节上限。
        $tcpEcho = Start-Job -ArgumentList $tcpLog, $EchoPort -ScriptBlock {
            param($log, $EchoPort)
            "listen" | Set-Content -Path $log -Encoding ASCII
            $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, $EchoPort)
            try {
                $l.Start()
                $deadline = (Get-Date).AddSeconds(180)
                $i = 0
                while ((Get-Date) -lt $deadline -and $i -lt 8) {
                    if (-not $l.Pending()) { Start-Sleep -Milliseconds 50; continue }
                    $c = $l.AcceptTcpClient()
                    $c.ReceiveTimeout = 1000   # 短连接（3B 回显）靠空闲超时收尾
                    "accept $i" | Add-Content -Path $log -Encoding ASCII
                    $s = $c.GetStream()
                    $buf = New-Object byte[] 1500
                    $total = 0
                    try {
                        while ($total -lt 262144) {
                            $n = $s.Read($buf, 0, $buf.Length)
                            if ($n -le 0) { break }
                            $s.Write($buf, 0, $n)
                            $s.Flush()
                            $total += $n
                        }
                    } catch { }
                    "conn $i echoed $total" | Add-Content -Path $log -Encoding ASCII
                    try { $c.Close() } catch { }
                    $i++
                }
            } catch { "error $_" | Add-Content -Path $log -Encoding ASCII } finally { $l.Stop() }
        }
    }

    # 等上一个 QEMU 退出（最多 5s），避免镜像/端口冲突；不再固定 sleep 2 秒
    for ($i = 0; $i -lt 50; $i++) {
        if (-not (Get-Process qemu-system-i386 -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 100
    }

    $qargs = @(
        '-drive', ("file={0},format=raw,if=floppy" -f ($imgPath -replace '\\', '/')), '-boot', 'a',
        '-drive', $diskArg,
        '-drive', $fatArg,
        '-m', "$Mem", '-vga', $Vga, '-cpu', $Cpu, '-smp', '2',
        '-nic', 'user,model=e1000',
        '-display', 'none', '-no-reboot',
        '-serial', "file:$serial",
        '-qmp', ("tcp:127.0.0.1:{0},server,nowait" -f $QmpPort)
    )

    T 'before qemu'
    Write-Host "=== qemu (headless) ===" -ForegroundColor Cyan
    $q = Start-Process -FilePath 'qemu-system-i386' -ArgumentList $qargs -PassThru -WindowStyle Hidden

    # 等"shell 真的起来了"再注入，而不是固定 sleep：
    # 启动路径越加越长（服务注册、echosvc、ring3probe、net 的 TCP 超时探测等），
    # 固定 10 秒会在慢一点的机器上把按键注入到 shell 还没开始读键盘的时候。
    $bootDeadline = (Get-Date).AddSeconds([Math]::Max(90, $TimeoutSec))
    $bootTicks = 0
    while ((Get-Date) -lt $bootDeadline) {
        if (Test-Path $serial) {
            $snap = Get-Content $serial -Raw -ErrorAction SilentlyContinue
            if ($snap -and $snap.Contains('ushell: ready')) { break }
        }
        Start-Sleep -Milliseconds 250
        $bootTicks++
        if (($bootTicks % 20) -eq 0) {
            Write-Host ("  [wait boot: {0}s]" -f [int]($bootTicks / 4)) -ForegroundColor DarkGray
        }
    }

    T 'boot ready'
    # 组件模型回归线程（pkgtest）会并发安装/卸载服务。它跑完之前注入按键会被抢：
    # shell 阻塞在 fork/wait 时键盘缓冲会丢字符（sectest 曾被拼成 "sectesecho hi"）。
    # 先等它收尾，再注入命令——这是"等被测系统就绪"，不是放宽断言。
    if ($Test) {
        $pkgDeadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $pkgDeadline) {
            if (Test-Path $serial) {
                $snap = Get-Content $serial -Raw -ErrorAction SilentlyContinue
                if ($snap -and $snap.Contains('pkg: selftest done')) { break }
            }
            Start-Sleep -Milliseconds 100
        }
    }
    if ($Test) {
        try {
            Write-Host ("  [inject command: {0}]" -f $Keys) -ForegroundColor DarkGray
            Send-Keys -Text ($Keys + "`n") -Port $QmpPort
        } catch {
            Write-Warning "key injection failed: $_"
        }
    }

    # 等注入的命令跑完。不能用"串口静默"判断：内核每秒都打心跳，永远不会静默，
    # 那样每次都会白等到超时。用末条命令的产物做哨兵：
    # reexec 成功后 shell 会重新打印第二次 "ushell: ready"；没有 reexec 就固定等 5 秒。
    T 'keys injected'
    if ($Keys -match 'reexec') {
        $doneDeadline = (Get-Date).AddSeconds(90)
        $doneTicks = 0
        while ((Get-Date) -lt $doneDeadline) {
            if (Test-Path $serial) {
                $snap = Get-Content $serial -Raw -ErrorAction SilentlyContinue
                if ($snap -and ([regex]::Matches($snap, 'ushell: ready')).Count -ge 2) { break }
            }
            Start-Sleep -Milliseconds 300
            $doneTicks++
            if (($doneTicks % 17) -eq 0) {
                Write-Host ("  [wait commands: {0}s]" -f [int]($doneTicks * 0.3)) -ForegroundColor DarkGray
            }
        }
        # 第二次 ready 只代表 shell 起来了；内置 demo 的 native 自检链
        # (manifest→restart→process_create，restart 自身最多轮询 2s) 与注入
        # 命令并发，必须等它打出最终行再采样，否则 svcwatch/restart 会假失败。
        $nativeDeadline = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $nativeDeadline) {
            if (Test-Path $serial) {
                $snap = Get-Content $serial -Raw -ErrorAction SilentlyContinue
                if ($snap -and ($snap -match 'native: process_create/wait (ok|FAILED)')) { break }
            }
            Start-Sleep -Milliseconds 100
        }
        Start-Sleep -Milliseconds 800
    } else {
        Start-Sleep -Seconds 5
    }

    if ($Shot) {
        try {
            Save-Screenshot -Path $Shot -Port $QmpPort
            Write-Host ("screendump -> {0}" -f $Shot) -ForegroundColor Green
        } catch {
            Write-Warning "screendump failed: $_"
        }
    }

    # 关键：-serial file: 是带缓冲的，Stop-Process 强杀会丢掉未刷盘的日志，
    # harness 只读到前 1~2KB，导致大量“假失败”。先走 QMP quit 让 QEMU 正常退出，
    # 并且要等到进程真的退出（最多 5 秒）再读日志。
    if (-not $q.HasExited) {
        try {
            $qc = New-Object Net.Sockets.TcpClient('127.0.0.1', $QmpPort)
            $qs = $qc.GetStream()
            $qr = New-Object IO.StreamReader($qs)
            $qw = New-Object IO.StreamWriter($qs)
            $null = $qr.ReadLine()                      # QMP greeting
            $qw.WriteLine('{"execute":"qmp_capabilities"}'); $qw.Flush()
            $null = $qr.ReadLine()
            $qw.WriteLine('{"execute":"quit"}'); $qw.Flush()
            $qc.Close()
        } catch {
            # QMP 不可用就退回强杀
        }
        for ($i = 0; $i -lt 50; $i++) {
            if ($q.HasExited) { break }
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force }
    Start-Sleep -Milliseconds 300
    if ($udpEcho -ne $null) {
        Stop-Job -Job $udpEcho -ErrorAction SilentlyContinue
        Remove-Job -Job $udpEcho -Force -ErrorAction SilentlyContinue
    }
    if ($tcpEcho -ne $null) {
        Stop-Job -Job $tcpEcho -ErrorAction SilentlyContinue
        Remove-Job -Job $tcpEcho -Force -ErrorAction SilentlyContinue
    }

    # TCP listen/accept 自检在负载下可能晚于固定等待收尾；等它打出最终行再做断言。
    if ($Test) {
        $tcpDeadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $tcpDeadline) {
            if (Test-Path $serial) {
                $snap = Get-Content $serial -Raw -ErrorAction SilentlyContinue
                if ($snap -and $snap.Contains('net: tcp selftest PASS') -and $snap.Contains('net: tcp socket fd close releases port ok')) { break }
                if ($snap -and $snap.Contains('net: tcp selftest FAIL')) { break }
            }
            Start-Sleep -Milliseconds 100
        }
    }

    $out = if (Test-Path $serial) { Get-Content $serial -Raw } else { '' }
    T 'teardown done'
    Write-Host "=== serial ===" -ForegroundColor Cyan
    Write-Host $out

    if ($Test) {
        $aslrVals = @([regex]::Matches($out, 'user: ASLR stack_top=0x[0-9a-f]+ stack_init=0x([0-9a-f]+)') |
                      ForEach-Object { $_.Groups[1].Value } |
                      Select-Object -Unique)
        $aslrHeap = @([regex]::Matches($out, 'heap_base=0x([0-9a-f]+)') |
                      ForEach-Object { $_.Groups[1].Value } |
                      Select-Object -Unique)
        $aslrTramp = @([regex]::Matches($out, 'tramp=0x([0-9a-f]+)') |
                       ForEach-Object { $_.Groups[1].Value } |
                       Select-Object -Unique)
        # 心跳计数必须严格递增（取相邻两次 N 比较 N2>N1）。CPU 不限：
        # 线程被负载均衡搬到哪颗核与"时钟在推进"无关；AP 调度由
        # "AP 进入 idle"（AP ticks/ipi/tlb）和"负载放置把线程送上 AP"覆盖。
        $hbCounts = @([regex]::Matches($out, 'thread: heartbeat (\d+) pid=\d+ cpu=\d+') |
                      ForEach-Object { [int]$_.Groups[1].Value })
        $hbStrict = $false
        for ($hi = 1; $hi -lt $hbCounts.Count; $hi++) {
            if ($hbCounts[$hi] -gt $hbCounts[$hi - 1]) { $hbStrict = $true; break }
        }
        $checks = @(
            @{ Name = '串口有输出';              Ok = ($out.Length -gt 0) },
            @{ Name = '无内核 panic';             Ok = (($out -cnotmatch 'KERNEL PANIC') -and ($out -cnotmatch 'kernel stack canary corrupted')) },
            @{ Name = '开机画面已绘制';          Ok = ($out -match 'boot: graphical splash shown') },
            @{ Name = '镜像签名验证';            Ok = ($out -match 'trust: shell signature OK') },
            @{ Name = '篡改镜像被拒绝';          Ok = ($out -match 'trust: tampered image rejected') },
            @{ Name = '内存自检 PASS';            Ok = ($out -match 'selftest: PASS') },
            @{ Name = 'PMM 线性分配自检';        Ok = ($out -match 'pmm: linear first-fit selftest ok') },
            @{ Name = '地址空间回收';            Ok = ($out -match 'vmm: address space teardown ok') },
            @{ Name = 'PAE 2MB 恒等映射';         Ok = ($out -match '7/8 PAE 2MB identity \.\.\. PASS') },
            @{ Name = '对象子系统就绪';           Ok = ($out -match 'obj: subsystems ready') },
            @{ Name = '多卷/驱动号可用';          Ok = (($out -match 'drive: system') -and ($out -match 'drive: data')) },
            @{ Name = '块设备持久化可用';         Ok = ($out -match 'disk: remount ok') },
            @{ Name = '块设备已注册到设备表';     Ok = ($out -match 'dev block:ram0') },
            @{ Name = '管道/睡眠可用';            Ok = ($out -match 'ushell: pipe ok sleep ok') },
            @{ Name = '文件读写与类型校验通过';   Ok = (($out -match 'demo: fs write/read ok') -and ($out -match 'demo: handle type check ok') -and ($out -match 'demo: fs refcount ok')) },
            @{ Name = '内核进入就绪状态';         Ok = ($out -match 'kernel: ready') },
            @{ Name = '中断处理已注册';           Ok = ($out -match 'intr: handlers irq0=timer irq1=keyboard') },
            @{ Name = '阻塞式等待可用';           Ok = ($out -match 'went to sleep on') },
            @{ Name = 'CWD 系统调用可用';         Ok = ($out -match 'ushell: cwd=system:/') },
            @{ Name = '文件系统调用可用';         Ok = ($out -match 'ushell: fs syscalls ok') },
            @{ Name = 'legacy int 0x81 系统调用可用'; Ok = (($out -match 'sys: int 0x81 works') -and ($out -match 'sys: write=\d+')) },
            @{ Name = 'native int 0x80 ABI 可用'; Ok = (($out -match 'native: ABI 0\.1 gate ok') -and ($out -match 'native: handle table ok') -and ($out -match 'native: ring3 ABI 0\.1 ok')) },
            @{ Name = 'native handle revoke';      Ok = ($out -match 'native: handle revoke ok') },
            @{ Name = 'native process_create/wait'; Ok = ($out -match 'native: process_create/wait ok') },
            @{ Name = 'native channel IPC';       Ok = ($out -match 'native: channel ok') },
            @{ Name = 'channel 跨进程阻塞唤醒';   Ok = ($out -match 'native: channel block ok') },
            @{ Name = 'native port connect';      Ok = ($out -match 'native: port ok') },
            @{ Name = 'service registry';          Ok = ($out -match 'native: service ok') },
            @{ Name = 'supervisor service';        Ok = ($out -match 'native: supervisor ok') },
            @{ Name = 'service manifest';          Ok = ($out -match 'native: manifest ok') },
            @{ Name = 'svcwatch 周期重启 echo';    Ok = ($out -match 'svcwatch: restart echo -> pid \d+') },
            @{ Name = 'restart 后服务可用';        Ok = ($out -match 'native: restart ok') },
            @{ Name = 'service 生命周期无泄漏';    Ok = ($out -match 'service: lifecycle ok \(200 cycles\)') },
            @{ Name = '沙箱拒绝 SYS_OPEN';        Ok = ($out -match 'sandbox: open denied') },
            @{ Name = '沙箱拒绝 native HANDLE_DUP'; Ok = ($out -match 'sandbox: native denied') },
            @{ Name = 'ELF W^X 加载'; Ok = ($out -match 'user: ELF text RO / data RW \(W\^X loader\)') },
            @{ Name = '用户地址布局'; Ok = ($out -match 'user: layout code=0x40000000\.\.0x40100000 stack=0x40100000\.\.0x40200000 heap=0x40200000\.\.0x40300000 tramp=0x403[0-3][0-9a-f]{4}') },
            @{ Name = 'ASLR 栈/堆/蹦床'; Ok = (($aslrVals.Count -ge 2) -and ($aslrHeap.Count -ge 2) -and ($aslrTramp.Count -ge 2)) },
            @{ Name = 'SMEP 已启用'; Ok = ($out -match 'SMEP supported=1 enabled=1') },
            @{ Name = 'SMAP 已启用'; Ok = ($out -match 'SMAP supported=1 enabled=1') },
            @{ Name = 'PAE/NX 已启用'; Ok = ($out -match 'vmm: PAE on, NX enabled') },
            @{ Name = '内核高半区 W^X'; Ok = ($out -match 'vmm: kernel W\^X active') },
            @{ Name = '启动链内核签名'; Ok = ($out -match 'boot: KERNEL SIGNATURE OK') },
            @{ Name = '用户态程序运行';           Ok = (($out -match 'user: entering ring3') -and ($out -match 'hello from ring3')) },
            @{ Name = 'ring3 userdemo 运行';     Ok = ($out -match 'user: entering ring3 \(builtin demo') },
            @{ Name = 'ring3 AP 探针';           Ok = ($out -match 'user: entering ring3 \(ring3 AP probe.*cpu=1') },
            @{ Name = '异常可恢复';               Ok = ($out -match 'sys: unmapped user pointer -> -1') },
            @{ Name = '时钟中断在推进';           Ok = $hbStrict },
            @{ Name = 'ring3 shell 已启动';       Ok = ($out -match 'ushell: ready') },
            @{ Name = '内核线程被调度';           Ok = ($out -match 'thread: heartbeat \d+ pid=\d+ cpu=1') },
            @{ Name = '注入的命令被执行';         Ok = ($out -match 'ushell: commands') },
            @{ Name = 'exec 地址空间切换';        Ok = (($out -match 'user: exec system:/bin/shell') -and ([regex]::Matches($out, 'ushell: ready').Count -ge 2)) },
            @{ Name = 'fork/wait 可用';           Ok = (($out -match 'child: hello from fork child') -and ($out -match 'parent: waited pid \d+, status 3')) },
            @{ Name = '代码页写保护';             Ok = ($out -match 'sectest: code write pid=\d+ status=142') },
            @{ Name = '栈 guard 写保护';          Ok = ($out -match 'sectest: guard write pid=\d+ status=142') },
            @{ Name = '栈不可执行（NX）';         Ok = ($out -match 'sectest: stack exec pid=\d+ status=142') },
            @{ Name = 'rodata 不可执行（NX）';    Ok = ($out -match 'sectest: rodata exec pid=\d+ status=142') },
            @{ Name = '只读 fd 写被拒绝';         Ok = ($out -match 'sectest: fd write denied') },
            @{ Name = 'capability 降权生效';      Ok = ($out -match 'sectest: cap drop denied') },
            @{ Name = 'capability 跨进程转移';    Ok = (($out -match 'sectest: cap recv hello') -and ($out -match 'sectest: cap xfer pid=\d+ fd=5 status=0')) },
            @{ Name = '匿名 mmap/munmap';         Ok = ($out -match 'sectest: mmap ok') },
            @{ Name = '通用 capability 表';       Ok = ($out -match 'cap: table selftest ok') },
            @{ Name = 'ELF/argv 启动栈可用';      Ok = ($out -match 'ushell: argc=0') },
            @{ Name = '命令行参数传递可用';       Ok = (($out -match 'ushell> echo hi') -and ($out -match '(?s)ushell> echo hi.*?\r?\nhi\r?\n')) },
            @{ Name = '用户态 libc 可用';         Ok = (($out -match 'ushell: libc ok') -and ($out -match 'ptr=0x402[0-9a-f]{5}')) },
            @{ Name = 'COW fork 可用';            Ok = (($out -match 'vmm: first COW copy at 0x401[0-9a-f]{4}') -and ($out -match 'child: hello from fork child')) },
            @{ Name = '信号处理函数可用';         Ok = (($out -match 'ushell: caught signal 15') -and ($out -match 'ushell: signal selftest done')) },
            @{ Name = 'FAT16 交换盘已挂载';       Ok = ($out -match 'fat16: ata1 mounted as fat:/') },
            @{ Name = 'FAT16 能读宿主文件';       Ok = ($out -match 'fat16: host\.txt = hello from host') },
            @{ Name = 'VFS 后端一致性';           Ok = ($out -match 'fs: backend selftest ok') },
            @{ Name = 'FS rename 可用';           Ok = ($out -match 'selftest: rename ok') },
            @{ Name = 'PCI 枚举可用';             Ok = ($out -match 'pci: [1-9]\d* device') },
            @{ Name = 'ACPI 表可用';              Ok = ($out -match 'acpi: PM1a_CNT=0x') },
            @{ Name = 'APIC 已探测';              Ok = ($out -match 'apic: LAPIC present') },
            @{ Name = 'PS/2 鼠标已启用';          Ok = ($out -match 'mouse: PS/2 mouse enabled') },
            @{ Name = 'NMI 看门狗已武装';         Ok = ($out -match 'apic: NMI watchdog armed') },
            @{ Name = 'e1000 网卡可用';           Ok = (($out -match 'e1000: MAC=') -and ($out -match 'e1000: TX test frame sent')) },
            @{ Name = 'PCI bus master 已启用';    Ok = ($out -match 'e1000: PCI CMD=\w+ \(memory=1 busmaster=1\)') },
            @{ Name = 'ARP 应答可用';             Ok = ($out -match 'net: ARP reply gw=') },
            @{ Name = 'ICMP echo 可用';           Ok = ($out -match 'net: ICMP echo reply seq=\d+') },
            @{ Name = 'UDP echo 可用';            Ok = ($out -match 'net: UDP echo reply') },
            @{ Name = 'TCP echo 可用';            Ok = ($out -match 'net: TCP echo reply') },
            @{ Name = '用户态 UDP';               Ok = ($out -match 'sectest: udp ok') },
            @{ Name = 'socket fd UDP';            Ok = ($out -match 'sectest: socket udp ok') },
            @{ Name = 'VFS chroot 越界拒绝';      Ok = ($out -match 'sectest: chroot ok') },
            @{ Name = 'VFS 打开 10 文件';         Ok = ($out -match 'sectest: fd limit ok') },
            @{ Name = 'ring3 FS 服务';            Ok = ($out -match 'sectest: fs service ok') },
            @{ Name = 'ring3 FS 写路径';          Ok = ($out -match 'sectest: fs write ok') },
            @{ Name = 'VFS svc 后端';             Ok = ($out -match 'sectest: svc vfs ok') },
            @{ Name = 'VFS svc 写路径';           Ok = ($out -match 'sectest: svc vfs write ok') },
            @{ Name = 'VFS blk 服务';             Ok = ($out -match 'sectest: blk service ok') },
            @{ Name = 'VFS blk 写路径';           Ok = ($out -match 'sectest: blk write ok') },
            @{ Name = '用户触发 TCP';             Ok = ($out -match 'sectest: tcp ok') },
            @{ Name = '权限模型可用';             Ok = ($out -match 'auth: permission model ok') },
            @{ Name = 'MADT 枚举多核';            Ok = ($out -match 'madt: \d+ cpu') },
            @{ Name = 'SMP 地基就绪';             Ok = ($out -match 'smp: BSP online.*\d+ cpu\(s\) discovered.*AP startup pending') },
            @{ Name = 'AP 进入 idle';             Ok = ($out -match 'smp: \d+/\d+ cpus online, \d+ AP GDT/TSS/IDT ready, AP ticks=[1-9]\d* ipi=[1-9]\d* tlb=[1-9]\d*') },
            @{ Name = '负载放置把线程送上 AP';    Ok = ($out -match 'sched: pid \d+ \(spin\) running on cpu [1-9]\d*') },

            # ---- 账号系统 P1（身份）：Linux 式 uid/gid、/etc/{passwd,shadow,group}、euid 判权 ----
            @{ Name = '账号文件 /etc/passwd 0644'; Ok = ($out -match 'acct: /etc/passwd 0644 root ok') },
            @{ Name = '账号文件 /etc/shadow 0600'; Ok = ($out -match 'acct: /etc/shadow 0600 root ok') },
            @{ Name = '账号文件 /etc/group 0644';  Ok = ($out -match 'acct: /etc/group 0644 root ok') },
            @{ Name = '账户表 root/系统/普通用户'; Ok = ($out -match 'acct: accounts root=0 alice=1000 bob=1001 ok') },
            @{ Name = 'wheel 组用于 sudo';         Ok = ($out -match 'acct: group wheel gid=10 root-member=1 alice-member=0 ok') },
            @{ Name = 'shadow 对 uid 1000 拒绝';   Ok = (($out -match 'acct: shadow denied for uid 1000 ok') -and ($out -match 'acct: shadow readable by root ok')) },
            @{ Name = '家目录 0700 边界';          Ok = ($out -match 'acct: home 0700 boundary ok') },
            @{ Name = '附加组参与判权';            Ok = ($out -match 'acct: supplementary group grants group bits ok') },
            @{ Name = '口令 sha256 + 恒定时间';    Ok = (($out -match 'acct: password verify ok ct-steps=64') -and ($out -match 'acct: wrong password rejected ct-steps=64') -and ($out -match 'acct: constant-time compare equal iterations ok')) },
            @{ Name = 'shadow 摘要格式稳定';       Ok = ($out -match 'acct: sha256 hex length=64 stable=1 ok') },
            @{ Name = '账号自检通过';              Ok = ($out -match 'acct: selftest PASS') },
            @{ Name = '测试模式自动登录 root';     Ok = ($out -match 'acct: autologin=root \(test build\) -> shell without login') },
            @{ Name = 'shell 报告 euid/gid';       Ok = ([regex]::Matches($out, 'ushell: euid=0 uid=0 gid=0 egid=0 groups=0').Count -ge 2) },
            @{ Name = 'fork 继承凭据';             Ok = (($out -match 'acct: fork child euid=0 uid=0') -and ($out -match 'acct: fork creds inherited')) },

            # ---- 账号系统 P2（文件边界） ----
            @{ Name = '0600 文件对其他用户拒绝';   Ok = ($out -match 'acct: file 0600 denies other uid ok') },
            @{ Name = '/tmp sticky 阻止他人删除';  Ok = ($out -match 'acct: /tmp sticky blocks other unlink ok') },
            @{ Name = '0700 目录阻断路径穿越';     Ok = ($out -match 'acct: path traversal through 0700 denied ok') },
            @{ Name = '无 x 目录阻断下钻';         Ok = ($out -match 'acct: dir without x blocks descent ok') },
            @{ Name = 'setuid exec 提权';          Ok = (($out -match 'acct: setuid exec elevates euid ok') -and ($out -match 'acct: setuid exec system:/tmp/su-test -> euid=0')) },
            @{ Name = 'setgid exec 提权';          Ok = (($out -match 'acct: setgid exec elevates egid ok') -and ($out -match 'acct: setgid exec system:/tmp/sg-test -> egid=10')) },
            @{ Name = 'FAT16 挂载 nosuid';         Ok = ($out -match 'acct: nosuid mount, setuid bits ignored for fat:/host\.txt') },
            @{ Name = 'chown 清 setuid 位';        Ok = ($out -match 'acct: chown clears setuid bits ok') },
            @{ Name = 'umask 生效';                Ok = ($out -match 'acct: umask 077 creates 0600 ok') },
            @{ Name = '多用户文件矩阵通过';        Ok = ($out -match 'acct: file matrix PASS') }
            # ---- 账号系统 P3（提权与审计） ----
            @{ Name = '失败 5 次锁定';            Ok = ($out -match 'acct: failed logins=5 threshold=5 ok') },
            @{ Name = '锁定后正确口令仍被拒';      Ok = ($out -match 'acct: locked account rejects correct password ok') },
            @{ Name = '锁定可清零';                Ok = ($out -match 'acct: failure lockout cleared ok') },
            @{ Name = 'sudo 只看 wheel 组';        Ok = ($out -match 'acct: sudo policy root=1 wheel-member=1 other=0 ok') },
            @{ Name = '普通用户旧口令校验';        Ok = ($out -match 'acct: passwd rejects wrong old password ok') },
            @{ Name = 'useradd/passwd/userdel';    Ok = (($out -match 'acct: useradd carol uid=1002 ok') -and ($out -match 'acct: passwd \(root\) set carol ok') -and ($out -match 'acct: userdel carol ok')) },
            @{ Name = 'usermod 加组';              Ok = (($out -match 'acct: usermod carol -> wheel ok') -and ($out -match 'acct: carol in wheel=1')) },
            @{ Name = '审计含登录/管理/拒绝';      Ok = ($out -match 'acct: audit has LOGIN_FAIL=1 USERADD=1 PASSWD_DENY=1 ok') },
            @{ Name = 'getty 生产入口已注册';      Ok = ($out -match 'acct: getty program registered=1') }

            # ---- 组件模型 MVP（阶段 2）：签名 .pkg + _pkgd + echosvc 组件化 ----
            @{ Name = 'pkg 服务已注册';            Ok = ($out -match 'pkgd: service online port=\d+') },
            @{ Name = 'pkg 合法安装';              Ok = ($out -match 'pkg: install echosvc-1\.0\.0 ok') },
            @{ Name = 'pkg 组件文件已落盘';        Ok = ($out -match 'pkg: installed echosvc-1\.0\.0 -> system:/System/Packages/echosvc/1\.0\.0 \(1 file\(s\)\)') },
            @{ Name = 'pkg 安装后通知 supervisor'; Ok = ($out -match 'pkg: supervisor start echosvc') },
            @{ Name = 'pkg 重复安装被拒';          Ok = ($out -match 'pkg: duplicate install rejected \(exist\)') },
            @{ Name = 'pkg 坏签名被拒';            Ok = ($out -match 'pkg: bad signature rejected \(signature\)') },
            @{ Name = 'pkg 缺依赖被拒';            Ok = ($out -match 'pkg: missing requires rejected \(requires\)') },
            @{ Name = 'pkg 安装失败回滚';          Ok = ($out -match 'pkg: install failure rollback ok') },
            @{ Name = 'pkg 卸载运行中服务';        Ok = ($out -match 'pkg: uninstall running service ok') },
            @{ Name = 'pkg 普通用户越权被拒';      Ok = ($out -match 'pkg: non-root system install denied \(policy\)') },
            @{ Name = 'pkg 用户应用装 ~/Apps';     Ok = ($out -match 'pkg: user app installed to /home/alice/Apps ok') },
            @{ Name = 'pkg 已安装完整性校验';      Ok = ($out -match 'pkg: verify installed echosvc ok') },
            @{ Name = 'pkg 组件 DB 列表';          Ok = ($out -match 'pkg: list shows echosvc 1\.0\.0 service') },
            @{ Name = 'pkgd 服务协议可用';         Ok = ($out -match 'pkgd: service protocol ok') },
            @{ Name = 'pkg 重装后服务重新上线';    Ok = ($out -match 'pkg: reinstall service online ok') }

            # ---- SIP-lite（阶段 3）：系统路径运行时只读 + 更新器例外 ----
            @{ Name = 'SIP 生产默认开启';          Ok = ($out -match 'sip: enabled \(system:/System, system:/bin, system:/drivers are read-only\)') },
            @{ Name = 'SIP 拒写 system:/System';   Ok = ($out -match 'sip: root write system:/System denied ok') },
            @{ Name = 'SIP 拒写 system:/bin';      Ok = ($out -match 'sip: root write system:/bin denied ok') },
            @{ Name = 'SIP 拒写 system:/drivers';  Ok = ($out -match 'sip: root write system:/drivers denied ok') },
            @{ Name = 'SIP 拒写/删/改名/改权限';   Ok = ($out -match 'sip: root write/unlink/rename/chmod/chown denied ok') },
            @{ Name = 'SIP 路径穿越规范化';        Ok = ($out -match 'sip: path traversal normalized ok') },
            @{ Name = 'SIP 包仓库对 root 只读';    Ok = ($out -match 'sip: package store write denied for root ok') },
            @{ Name = 'SIP 更新器作用域放行';      Ok = ($out -match 'sip: updater scope bypass ok') },
            @{ Name = 'SIP sip=0 可关闭';          Ok = ($out -match 'sip: sip=0 disables protection ok') },
            @{ Name = 'SIP 自检通过';              Ok = ($out -match 'sip: selftest PASS') }

            # ---- FS 64KB / XFS1 v5（阶段 4a） ----
            @{ Name = 'XFS1 v5 挂载 64KB 限额';    Ok = ($out -match 'fs: XFS1 v5 mounted, file limit 65536 bytes') },
            @{ Name = '单文件 64KB 上限生效';      Ok = ($out -match 'fs: 64KB file limit ok \(wrote 65536 bytes, one-past rejected\)') }

            # ---- 硬链接 / 符号链接（阶段 4b） ----
            @{ Name = '硬链接 nlink 与共享数据';   Ok = ($out -match 'fs: hard link ok \(nlink=2, write via link visible\)') },
            @{ Name = '硬链接删一个名字不丢数据';  Ok = ($out -match 'fs: hard link unlink keeps data ok') },
            @{ Name = '目录禁止硬链接';            Ok = ($out -match 'fs: hard link rejects directory ok') },
            @{ Name = '跨卷硬链接被拒';            Ok = ($out -match 'fs: hard link rejects cross-volume ok') },
            @{ Name = '符号链接可跟随';            Ok = ($out -match 'fs: symlink follow ok') },
            @{ Name = 'readlink/lstat 可用';       Ok = ($out -match 'fs: readlink/lstat ok') },
            @{ Name = 'O_NOFOLLOW 拒绝跟随';       Ok = ($out -match 'fs: O_NOFOLLOW rejects symlink ok') },
            @{ Name = '软链接环有跳数上限';        Ok = ($out -match 'fs: symlink loop limited ok') },
            @{ Name = 'FAT16 link 操作 E_INVAL';   Ok = ($out -match 'fs: FAT16 link ops E_INVAL ok') },
            @{ Name = '软链接不能绕过 SIP';        Ok = ($out -match 'fs: symlink into system path denied ok') },
            @{ Name = 'link 自检通过';             Ok = ($out -match 'fs: link selftest PASS') }

            # ---- 用户态 ln / ln -s / readlink / lstat（阶段 4b） ----
            @{ Name = 'shell 硬链接 ln';           Ok = ($out -match 'ushell: ln ok') },
            @{ Name = 'shell 符号链接 ln -s';      Ok = ($out -match 'ushell: ln -s ok') },
            @{ Name = 'shell readlink';            Ok = ($out -match 'ushell: readlink ok') },
            @{ Name = 'shell lstat 报链接类型';    Ok = ($out -match 'ushell: lstat link type ok') },
            @{ Name = '用户态 link 回归通过';      Ok = ($out -match 'ushell: linktest PASS') }

            # ---- TCP 健壮性（阶段 4c） ----
            @{ Name = 'TCP 握手 SYN/SYN-ACK/ACK';  Ok = ($out -match 'net: tcp handshake ok \(SYN/SYN-ACK/ACK\)') },
            @{ Name = 'TCP 乱序重组';              Ok = ($out -match 'net: tcp out-of-order reassembly ok') },
            @{ Name = 'TCP 坏校验和丢弃';          Ok = ($out -match 'net: tcp checksum rejects corrupt ok') },
            @{ Name = 'TCP RST 关闭连接';          Ok = ($out -match 'net: tcp RST closes connection ok') },
            @{ Name = 'TCP 大传输 + 丢包重传';     Ok = ($out -match 'net: tcp large transfer ok \(16384 bytes, retrans=[1-9]\d*') },
            @{ Name = 'TCP RTT/RTO 生效';          Ok = ($out -match 'net: tcp rto ok \(srtt=\d+ticks rto=\d+ticks\)') },
            @{ Name = 'TCP FIN/TIME_WAIT';         Ok = ($out -match 'net: tcp FIN/TIME_WAIT ok') },
            @{ Name = 'TCP 健壮性自检通过';        Ok = ($out -match 'net: tcp selftest PASS') },
            # ---- 并行批次：安全/文件/内存（2026-10-27）----
            @{ Name = 'shadow PBKDF2';             Ok = ($out.Contains('acct: pbkdf2 ok')) },
            @{ Name = 'shadow 旧格式迁移';         Ok = ($out.Contains('acct: shadow migrate ok')) },
            @{ Name = '审计环 128 行';             Ok = ($out.Contains('audit: ring 128 ok')) },
            @{ Name = '审计追加刷盘';              Ok = ($out.Contains('audit: append flush ok')) },
            @{ Name = '登录会话槽复用';            Ok = ($out.Contains('acct: session slot reuse ok')) },
            @{ Name = 'XFS v5 镜像兼容挂载';       Ok = ($out.Contains('fs: xfs v5 image mount compat ok')) },
            @{ Name = 'XFS v6 硬链接持久化';       Ok = ($out.Contains('fs: xfs v6 hardlink persisted ok')) },
            @{ Name = 'XFS v6 挂载';               Ok = ($out.Contains('fs: XFS1 v6 mounted')) },
            @{ Name = 'RSS 记账';                  Ok = ($out.Contains('mm: rss accounting ok')) },
            @{ Name = 'ps RSS 输出';               Ok = ($out.Contains('mm: ps pid=0 name=rss-selftest')) },
            @{ Name = '文件映射 mmap';             Ok = ($out.Contains('mm: file mmap ok')) },

            # ---- 并行批次：TCP listen/accept（2026-10-27）----
            @{ Name = 'TCP listen + 重复 bind';    Ok = ($out.Contains('net: tcp listen ok')) },
            @{ Name = 'TCP 回环 connect';          Ok = ($out.Contains('net: tcp loopback connect ok')) },
            @{ Name = 'TCP accept 收数据';         Ok = ($out.Contains('net: tcp accept data ok')) },
            @{ Name = 'TCP accept 回数据';         Ok = ($out.Contains('net: tcp accept reply ok')) },
            @{ Name = 'TCP TIME_WAIT 占端口';      Ok = ($out.Contains('net: tcp TIME_WAIT holds port ok')) },
            @{ Name = 'TCP TIME_WAIT 后复用';      Ok = ($out.Contains('net: tcp port reuse after close ok')) },
            @{ Name = 'TCP backlog 溢出';          Ok = ($out.Contains('net: tcp backlog overflow ok')) },
            @{ Name = 'TCP accept 超时';           Ok = ($out.Contains('net: tcp accept timeout ok')) },
            @{ Name = 'TCP backlog 握手';          Ok = ($out.Contains('net: tcp backlog handshake ok')) },
            @{ Name = 'TCP listen/accept 自检';    Ok = ($out.Contains('net: tcp listen/accept selftest PASS')) },
            @{ Name = 'TCP listen/accept syscall'; Ok = ($out.Contains('net: tcp listen/accept syscall ok')) },
            @{ Name = 'TCP close 释放端口';        Ok = ($out.Contains('net: tcp socket fd close releases port ok')) },

            # ---- 并行批次：组件 ELF / pkg CLI（2026-10-27）----
            @{ Name = '组件 ELF entry';            Ok = ($out.Contains('pkg: elf entry ok')) },
            @{ Name = '组件 ELF 验签';             Ok = ($out.Contains('pkg: elf entry verify ok')) },
            @{ Name = '组件 ELF 服务在线';         Ok = ($out.Contains('pkg: elf entry service online ok')) },
            @{ Name = '组件 ELF 篡改拒绝';         Ok = ($out.Contains('pkg: elf entry tamper rejected')) },
            @{ Name = '组件 ELF 恢复';             Ok = ($out.Contains('pkg: elf entry restore ok')) },
            @{ Name = '组件进程运行';              Ok = ($out -match 'pkg: component echosvc running pid=\d+') },
            @{ Name = '组件进程停止';              Ok = ($out -match 'pkg: component echosvc stopped \(pid=\d+') },
            @{ Name = 'pkg CLI list';              Ok = ($out.Contains('pkg: user cli list ok')) },
            @{ Name = 'pkg CLI verify';            Ok = ($out.Contains('pkg: user cli verify ok')) },
            @{ Name = 'pkg CLI install';           Ok = ($out.Contains('pkg: user cli install ok')) },
            @{ Name = 'pkg CLI 越权拒绝';          Ok = ($out.Contains('pkg: user cli policy deny ok')) },
            @{ Name = 'pkg CLI 完成';              Ok = ($out.Contains('pkg: user cli done')) },

            # ---- 并行批次：per-CPU 调度 / IOAPIC（2026-10-27）----
            @{ Name = 'SMP 双核 online';           Ok = ($out -match 'smp: \d+ cpus online') },
            @{ Name = 'SMP TLB shootdown ack';     Ok = ($out -match 'smp: tlb shootdown ack=\d+/\d+ ok') },
            @{ Name = 'per-CPU 调度锁';            Ok = ($out.Contains('sched: per-cpu locks ok')) },
            @{ Name = 'runqueue cpu0';             Ok = ($out -match 'sched: runqueue cpu0 mask=0x[0-9a-f]+') },
            @{ Name = 'runqueue cpu1';             Ok = ($out -match 'sched: runqueue cpu1 mask=0x[0-9a-f]+') },
            @{ Name = 'MADT IRQ0 override';        Ok = ($out.Contains('madt: irq0 -> gsi2')) },
            @{ Name = 'IOAPIC irq0 路由';          Ok = ($out.Contains('ioapic: irq0 -> gsi2 apic 0 vector=0x20')) },
            @{ Name = 'IOAPIC 全量路由';           Ok = ($out.Contains('ioapic: legacy IRQ routing done')) },
            # ---- 审计批次（2026-10-27）：fs/net/smp ----
            @{ Name = 'fs 审计：坏镜像拒绝';       Ok = ($out.Contains('fs: audit corrupt image mounts rejected')) },
            @{ Name = 'fs 审计：syscall 缓冲守卫'; Ok = ($out.Contains('fs: audit syscall chroot/time/fsck buffer guards ok')) },
            @{ Name = 'fs 审计：硬链接/nlink';     Ok = ($out.Contains('fs: audit hardlink nlink / ino_promote ok')) },
            @{ Name = 'fs 审计：路径边界';         Ok = ($out.Contains('fs: audit deep/long/.. path edges ok')) },
            @{ Name = 'fs 审计自检通过';           Ok = ($out.Contains('fs: audit selftest PASS')) },
            @{ Name = 'net 审计：IPv4 入口';       Ok = ($out.Contains('net: audit tcp ip ingress rejects malformed ok')) },
            @{ Name = 'net 审计：UDP 入口';        Ok = ($out.Contains('net: audit udp ingress rejects malformed ok')) },
            @{ Name = 'net 审计：伪造 SYN';        Ok = ($out.Contains('net: audit tcp forged SYN ignored ok')) },
            @{ Name = 'net 审计：握手 seq';        Ok = ($out.Contains('net: audit tcp handshake seq validated ok')) },
            @{ Name = 'net 审计：接收记账';        Ok = ($out.Contains('net: audit tcp receive accounting ok')) },
            @{ Name = 'net 审计：listener 归属';   Ok = ($out.Contains('net: audit listen socket owned by pid ok')) },
            @{ Name = 'net 审计：ARP 防欺骗';      Ok = ($out.Contains('net: audit arp spoof rejected ok')) },
            @{ Name = 'net 审计：DNS txid 熵';     Ok = ($out.Contains('net: audit dns txid entropy ok')) },
            @{ Name = 'net 审计：伪造 ACK';        Ok = ($out.Contains('net: audit tcp forged ACK ignored ok')) },
            @{ Name = 'smp 审计：sigreturn 帧';    Ok = ($out.Contains('audit: sigreturn frame validation ok')) },
            @{ Name = 'smp 审计：kill uid';        Ok = ($out.Contains('audit: kill uid policy ok')) },
            @{ Name = 'smp 审计：uaccess 窗口';    Ok = ($out.Contains('audit: uaccess user window ok')) },
            @{ Name = 'smp 审计：spawn 竞态';      Ok = ($out -match 'audit: smp spawn race PASS \(rounds=\d+\)') },
            @{ Name = 'smp 审计：wake-vs-block';   Ok = ($out -match 'audit: smp wake-vs-block PASS') },
            @{ Name = 'smp 审计：vmalloc 并发';    Ok = ($out -match 'audit: mm vmalloc concurrent PASS') },
            @{ Name = 'smp 审计自检通过';          Ok = ($out.Contains('audit: smp selftest PASS')) },
            @{ Name = 'smp 审计自检结束';          Ok = ($out.Contains('audit: smp selftest done')) },
            @{ Name = 'PIT 频率 1:1（非 2x）';    Ok = ($out -match 'irq: pit rate ok \(1200 ticks / \d+ s\)') }
        )

        T 'checks begin'
        Write-Host "=== checks ===" -ForegroundColor Cyan
        $failed = 0
        foreach ($c in $checks) {
            if ($c.Ok) {
                Write-Host ("  [PASS] {0}" -f $c.Name) -ForegroundColor Green
            } else {
                Write-Host ("  [FAIL] {0}" -f $c.Name) -ForegroundColor Red
                $failed++
            }
        }

        Write-Host ("=== compile: {0} compiled, {1} skipped ===" -f $compiled, $skipped) -ForegroundColor DarkCyan
        Write-Host ("=== elapsed: {0:N1}s ===" -f $swTotal.Elapsed.TotalSeconds) -ForegroundColor DarkCyan

        if ($failed -eq 0) {
            Write-Host ("=== TEST PASS ({0}/{0}) ===" -f $checks.Count) -ForegroundColor Green
            exit 0
        }
        Write-Host ("=== TEST FAIL ({0} of {1} checks failed) ===" -f $failed, $checks.Count) -ForegroundColor Red
        exit 1
    }
}
