# =============================================================================
# d0-demo.ps1 - 外部开发者示例 sdk/examples/hello_d0 的构建 + QEMU 真跑（WP4-2）
#
# 与 tools/sdk-demo.ps1 的关系：
#   - 同样用 `build-ng.ps1 -SdkDemo` 把 SDK 示例放到 /bin/demo 的位置，走
#     init -> displayd -> demo spawn 的产品启动链；
#   - 但 **不修改 build-ng.ps1**（它只认 sdk/examples/hello_gui/main.c），也不
#     改默认构建：本脚本在系统临时目录里搭一个 overlay（src/tools/sdk/include
#     等目录是指回仓库的 junction，只有 sdk/examples/hello_gui/main.c 是一个
#     `#include "../hello_d0/main.c"` 的三行代理文件），然后用仓库里**原封不动**
#     的 build-ng.ps1 -SdkDemo 构建到 overlay 自己的 .dist-d0/.obj-d0。
#     仓库工作区不会产生任何构建产物，build-ng 默认行为也不变。
#
# 断言（缺任何一个 -> exit 1）：
#   ng: d0 boot ok (sdk=..)
#   ng: surface title ok (id=.. title=D0 Demo)
#   ng: d0 demo up (pid=.. id=.. x=.. y=.. w=480 h=300 token=0x.. session=0|1)
#   QMP: space 注入后 ng: d0 key ok (key=0x20 count=..)（按键 + 重绘）
#   QMP: stop -> screendump -> 窗口内 (x+30,y+60)=#c0504a（红卡片）、
#        (x+40,y+170)=#16202b（计数面板）、(x+460,y+30)=#18222e（标题栏）
#   QMP: q 注入后 ng: d0 demo bye (... reason=q/esc)（q 退出）
#   （8s 空闲自动退出的证据在串口日志里：ng: d0 quit ok (reason=idle ...)）
#
# 说明：hello_d0 是普通应用（没有 hello_gui 的 window/hold demo harness 角色），
# 所以它会被内核 WP3-1 自检当成 interactive demo 直接 spawn，而不是经
# sessiond/LAUNCH。脚本只断言本应用自己的行为与像素，不要求 WP3-1/3-2/3-3
# selftest PASS（它们会因为 /bin/demo 不再是带 harness 角色的示例而按设计失败）。
#
# 用法（仓库根目录）：
#   powershell -ExecutionPolicy Bypass -File tools/d0-demo.ps1
#   powershell ... -NoQemu      # 只做语法 + freestanding 交叉编译/链接校验
#   powershell ... -KeepTemp    # 保留临时 overlay/串口日志/截图
#   powershell ... -Cpu qemu64  # 其它 CPU 模型（默认 max）
#
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = '',
    [string]$Cpu = 'max',
    [int]$TimeoutSec = 90,
    [switch]$NoQemu,
    [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $Repo)) {
    Write-Host ("d0-demo: FAIL (repo not found: {0})" -f $Repo) -ForegroundColor Red
    exit 1
}
$Repo = (Resolve-Path -LiteralPath $Repo).Path
$OnWindows = ($env:OS -eq 'Windows_NT')

$script:qemuProc = $null
$script:tmpDir = $null
$script:ovDir = $null
$script:serialLog = $null
$script:keep = [bool]$KeepTemp

function Write-Step([string]$s) {
    Write-Host ("d0-demo: {0}" -f $s)
}

# 只删除 junction/符号链接本身（rmdir 不跟随目标），再删普通临时目录。
function Remove-OverlayLinks {
    if (-not $script:ovDir -or -not (Test-Path -LiteralPath $script:ovDir)) { return }
    $links = @(
        (Join-Path $script:ovDir 'src'),
        (Join-Path $script:ovDir 'tests'),
        (Join-Path $script:ovDir 'tools'),
        (Join-Path $script:ovDir 'sdk/include'),
        (Join-Path $script:ovDir 'sdk/examples/hello_d0')
    )
    foreach ($l in $links) {
        if (Test-Path -LiteralPath $l) {
            cmd /c "rmdir `"$l`"" 2>$null | Out-Null
        }
    }
}

function Remove-Temp {
    if ($script:keep) {
        if ($script:tmpDir) { Write-Host ("d0-demo: temp kept: {0}" -f $script:tmpDir) }
        return
    }
    Remove-OverlayLinks
    if ($script:tmpDir -and (Test-Path -LiteralPath $script:tmpDir)) {
        Remove-Item -LiteralPath $script:tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Stop-Qemu {
    if ($script:qemuProc) {
        if (-not $script:qemuProc.HasExited) {
            try { $script:qemuProc.Kill() } catch { }
            $script:qemuProc.WaitForExit(3000) | Out-Null
        }
    }
}

function Fail([string]$msg) {
    Write-Host ("d0-demo: FAIL ({0})" -f $msg) -ForegroundColor Red
    if ($script:serialLog -and (Test-Path -LiteralPath $script:serialLog)) {
        Write-Host 'd0-demo: serial tail:' -ForegroundColor DarkGray
        Get-Content -LiteralPath $script:serialLog -Tail 30 -ErrorAction SilentlyContinue |
            ForEach-Object { Write-Host ("  {0}" -f $_) -ForegroundColor DarkGray }
    }
    Stop-Qemu
    Remove-Temp
    exit 1
}

# ---- QMP 小工具（与 sdk-demo.ps1 同一套路，独立实现） ----------------------
function Connect-Qmp([int]$Port) {
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $client = [Net.Sockets.TcpClient]::new()
            $client.Connect('127.0.0.1', $Port)
            $stream = $client.GetStream()
            $stream.ReadTimeout = 3000
            $reader = New-Object IO.StreamReader($stream)
            $writer = New-Object IO.StreamWriter($stream)
            $writer.AutoFlush = $true
            $null = $reader.ReadLine()   # greeting
            $writer.WriteLine('{"execute":"qmp_capabilities"}')
            $null = $reader.ReadLine()
            return @{ Client = $client; Reader = $reader; Writer = $writer }
        } catch {
            Start-Sleep -Milliseconds 50
        }
    }
    return $null
}

function Send-QmpJson($Qmp, [string]$Json) {
    if (-not $Qmp) { return $false }
    try {
        $Qmp.Writer.WriteLine($Json)
        for ($i = 0; $i -lt 8; $i++) {
            $resp = $Qmp.Reader.ReadLine()
            if (-not $resp) { return $false }
            if ($resp -match '"return"') { return $true }
            if ($resp -match '"error"') {
                Write-Host ("d0-demo: QMP error: {0}" -f $resp) -ForegroundColor DarkYellow
                return $false
            }
            Write-Host ("d0-demo: QMP event: {0}" -f $resp) -ForegroundColor DarkGray
        }
    } catch {
        return $false
    }
    return $false
}

function Send-QmpKey($Qmp, [string]$Key) {
    if (-not $Qmp) { return $false }
    $events = ('{{"execute":"input-send-event","arguments":{{"events":[' +
        '{{"type":"key","data":{{"down":true,"key":{{"type":"qcode","data":"{0}"}}}}}},' +
        '{{"type":"key","data":{{"down":false,"key":{{"type":"qcode","data":"{0}"}}}}}}' +
        ']}}}}') -f $Key
    if (Send-QmpJson $Qmp $events) { return $true }
    return (Send-QmpJson $Qmp ('{{"execute":"human-monitor-command","arguments":{{"command-line":"sendkey {0}"}}}}' -f $Key))
}

function Send-QmpStop($Qmp) {
    return (Send-QmpJson $Qmp '{"execute":"stop"}')
}

function Send-QmpCont($Qmp) {
    return (Send-QmpJson $Qmp '{"execute":"cont"}')
}

function Send-QmpScreendump($Qmp, [string]$Path) {
    $jsonPath = ($Path -replace '\\', '/')
    return (Send-QmpJson $Qmp ('{"execute":"screendump","arguments":{"filename":"' + $jsonPath + '"}}'))
}

function Read-Ppm([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $i = 0
    $tokens = @()
    while ($tokens.Count -lt 4) {
        while ($i -lt $bytes.Length -and ($bytes[$i] -eq 32 -or $bytes[$i] -eq 10 -or
                $bytes[$i] -eq 13 -or $bytes[$i] -eq 9)) { $i++ }
        if ($i -ge $bytes.Length) { return $null }
        if ($bytes[$i] -eq 35) {
            while ($i -lt $bytes.Length -and $bytes[$i] -ne 10) { $i++ }
            continue
        }
        $start = $i
        while ($i -lt $bytes.Length -and -not ($bytes[$i] -eq 32 -or $bytes[$i] -eq 10 -or
                $bytes[$i] -eq 13 -or $bytes[$i] -eq 9)) { $i++ }
        $tokens += [Text.Encoding]::ASCII.GetString($bytes, $start, $i - $start)
    }
    $i++
    if ($tokens[0] -ne 'P6') { return $null }
    return @{ W = [int]$tokens[1]; H = [int]$tokens[2]; Offset = $i; Bytes = $bytes }
}

function Get-PpmPixel($Ppm, [int]$X, [int]$Y) {
    if ($null -eq $Ppm -or $X -lt 0 -or $Y -lt 0 -or $X -ge $Ppm.W -or $Y -ge $Ppm.H) {
        return $null
    }
    $o = $Ppm.Offset + ($Y * $Ppm.W + $X) * 3
    if ($o + 2 -ge $Ppm.Bytes.Length) { return $null }
    return @($Ppm.Bytes[$o], $Ppm.Bytes[$o + 1], $Ppm.Bytes[$o + 2])
}

function Format-Pixel($Px) {
    if ($null -eq $Px) { return '(none)' }
    return ('#{0:x2}{1:x2}{2:x2}' -f $Px[0], $Px[1], $Px[2])
}

function Read-Serial {
    if (-not $script:serialLog -or -not (Test-Path -LiteralPath $script:serialLog)) { return '' }
    try {
        $t = Get-Content -LiteralPath $script:serialLog -Raw -ErrorAction Stop
        if ($null -eq $t) { return '' }
        return [string]$t
    } catch {
        return ''
    }
}

function Wait-Serial([string]$Pattern, [int]$Seconds) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        $t = Read-Serial
        if ($t -and [regex]::IsMatch($t, $Pattern)) { return $t }
        if ($script:qemuProc -and $script:qemuProc.HasExited) { return $t }
        Start-Sleep -Milliseconds 100
    }
    return (Read-Serial)
}

# 最小 user-ELF 校验：ELF64/ET_EXEC/EM_X86_64、入口 0x40000000、PT_LOAD>=2、
# 4 KiB 对齐、W^X、镜像结束 <= 0x40010000。
function Test-UserElf([string]$Path) {
    $problems = New-Object System.Collections.Generic.List[string]
    $b = [IO.File]::ReadAllBytes($Path)
    $info = @{ Ok = $false; Problems = $problems; Loads = 0; End = 0; Entry = 0; Size = $b.Length }
    if ($b.Length -lt 64) {
        $problems.Add('ELF smaller than ELF64 header')
        return $info
    }
    if (-not ($b[0] -eq 0x7F -and $b[1] -eq 0x45 -and $b[2] -eq 0x4C -and $b[3] -eq 0x46)) {
        $problems.Add('bad ELF magic')
    }
    if ($b[4] -ne 2) { $problems.Add(("e_class={0}, want 2 (ELF64)" -f $b[4])) }
    if ($b[5] -ne 1) { $problems.Add(("e_data={0}, want 1 (LSB)" -f $b[5])) }
    $type = [BitConverter]::ToUInt16($b, 16)
    $machine = [BitConverter]::ToUInt16($b, 18)
    $entry = [BitConverter]::ToUInt64($b, 24)
    $phOff = [BitConverter]::ToUInt64($b, 32)
    $phEnt = [BitConverter]::ToUInt16($b, 54)
    $phNum = [BitConverter]::ToUInt16($b, 56)
    $info.Entry = $entry
    if ($type -ne 2) { $problems.Add(("e_type={0}, want 2 (ET_EXEC)" -f $type)) }
    if ($machine -ne 62) { $problems.Add(("e_machine={0}, want 62" -f $machine)) }
    if ($phEnt -ne 56) { $problems.Add(("e_phentsize={0}, want 56" -f $phEnt)) }
    if ($phNum -lt 1) { $problems.Add('no program headers') }
    if ($entry -lt 0x40000000 -or $entry -ge 0x40010000) {
        $problems.Add(("e_entry=0x{0:x} outside [0x40000000,0x40010000)" -f $entry))
    }
    for ($i = 0; $i -lt $phNum; $i++) {
        $ph = [int]$phOff + ($i * $phEnt)
        if ($ph + 56 -gt $b.Length) {
            $problems.Add('phdr outside file')
            break
        }
        if ([BitConverter]::ToUInt32($b, $ph) -ne 1) { continue } # PT_LOAD
        $info.Loads++
        $flags = [BitConverter]::ToUInt32($b, $ph + 4)
        $vaddr = [BitConverter]::ToUInt64($b, $ph + 16)
        $filesz = [BitConverter]::ToUInt64($b, $ph + 32)
        $memsz = [BitConverter]::ToUInt64($b, $ph + 40)
        $align = [BitConverter]::ToUInt64($b, $ph + 48)
        if (($vaddr % 4096) -ne 0) { $problems.Add(("PT_LOAD 0x{0:x} not 4 KiB aligned" -f $vaddr)) }
        if ($align -lt 4096) { $problems.Add(("PT_LOAD 0x{0:x} align < 4096" -f $vaddr)) }
        if ($filesz -gt $memsz) { $problems.Add('PT_LOAD filesz > memsz') }
        if (($flags -band 3) -eq 3) { $problems.Add(("PT_LOAD 0x{0:x} is W+X" -f $vaddr)) }
        if ($vaddr -lt 0x40000000) { $problems.Add(("PT_LOAD 0x{0:x} below base" -f $vaddr)) }
        if (($vaddr + $memsz) -gt 0x40010000) { $problems.Add('PT_LOAD ends above 0x40010000') }
        if (($vaddr + $memsz) -gt $info.End) { $info.End = $vaddr + $memsz }
    }
    if ($info.Loads -lt 2) { $problems.Add(("PT_LOAD count {0} < 2" -f $info.Loads)) }
    $info.Ok = ($problems.Count -eq 0)
    return $info
}

# ============================================================================
# 主流程
# ============================================================================

# 工具链（与 build-ng/sdk-demo 相同的 MSYS2 clang/lld）。
$mingw = 'D:\msys64\mingw64\bin'
if ((Test-Path (Join-Path $mingw 'clang.exe')) -and
    ($env:PATH -notlike ('*' + $mingw + '*'))) {
    $env:PATH = $mingw + ';' + $env:PATH
}
$clangCmd = Get-Command clang -ErrorAction SilentlyContinue
if (-not $clangCmd) {
    Write-Host 'd0-demo: FAIL (clang not found; add D:\msys64\mingw64\bin to PATH)' -ForegroundColor Red
    exit 1
}
$clang = $clangCmd.Source
$lldCmd = Get-Command ld.lld -ErrorAction SilentlyContinue
if (-not $lldCmd) {
    Write-Host 'd0-demo: FAIL (ld.lld not found; install lld)' -ForegroundColor Red
    exit 1
}

$totalSw = [Diagnostics.Stopwatch]::StartNew()
$tag = [guid]::NewGuid().ToString('N').Substring(0, 8)
$script:tmpDir = Join-Path ([IO.Path]::GetTempPath()) ("aster-d0-{0}-{1}" -f $PID, $tag)
New-Item -ItemType Directory -Force -Path $script:tmpDir | Out-Null

$srcMain = Join-Path $Repo 'sdk/examples/hello_d0/main.c'
$userLd = Join-Path $Repo 'src/ng/user/user.ld'
if (-not (Test-Path -LiteralPath $srcMain)) { Fail ("missing {0}" -f $srcMain) }
if (-not (Test-Path -LiteralPath $userLd)) { Fail ("missing {0}" -f $userLd) }

Write-Host ("d0-demo: clang = {0}" -f $clang)
Write-Host ("d0-demo: repo  = {0}" -f $Repo)

# ---- 1. freestanding 交叉编译 + 链接到 0x40000000 -------------------------
$obj = Join-Path $script:tmpDir 'hello_d0.o'
$elf = Join-Path $script:tmpDir 'hello_d0.elf'
$cargs = @(
    '--target=x86_64-elf', '-ffreestanding', '-nostdlib', '-mno-red-zone',
    '-fno-stack-protector', '-fcf-protection=none', '-mno-sse',
    '-mgeneral-regs-only', '-fno-builtin', '-fno-pic', '-fno-pie', '-std=c11',
    '-Os', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
    '-fdata-sections', '-I', (Join-Path $Repo 'sdk/include'), '-c', $srcMain,
    '-o', $obj
)
Write-Step 'compile (x86_64-elf, -nostdlib, -Wall -Wextra -Werror)'
& $clang @cargs
if ($LASTEXITCODE -ne 0) { Fail 'clang compile failed' }
$largs = @(
    '--target=x86_64-elf', '-fuse-ld=lld', '-nostdlib',
    ('-Wl,-T,' + $userLd), '-Wl,--build-id=none', '-Wl,--fatal-warnings',
    '-Wl,--gc-sections', '-o', $elf, $obj
)
Write-Step 'link (src/ng/user/user.ld, entry 0x40000000)'
& $clang @largs
if ($LASTEXITCODE -ne 0) { Fail 'link failed' }

$elfInfo = Test-UserElf $elf
if (-not $elfInfo.Ok) {
    foreach ($p in $elfInfo.Problems) { Write-Host ("  ! {0}" -f $p) -ForegroundColor DarkRed }
    Fail 'user ELF verification'
}
Write-Step ("link ok ({0} bytes, {1} PT_LOAD, entry 0x{2:x}, end 0x{3:x})" -f
    $elfInfo.Size, $elfInfo.Loads, $elfInfo.Entry, $elfInfo.End)

if ($NoQemu) {
    Write-Step ("PASS (link-only, {0:N1}s; QEMU skipped by -NoQemu)" -f $totalSw.Elapsed.TotalSeconds)
    Remove-Temp
    exit 0
}

# ---- 2. QEMU 可用性 + overlay 构建（build-ng.ps1 原样使用） ----------------
$qemu = $null
$qcmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
if ($qcmd) {
    $qemu = $qcmd.Source
} elseif ($OnWindows -and (Test-Path 'D:\Program Files\qemu\qemu-system-x86_64.exe')) {
    $qemu = 'D:\Program Files\qemu\qemu-system-x86_64.exe'
}
if (-not $qemu) { Fail 'qemu-system-x86_64 not found (PATH or D:\Program Files\qemu)' }

$script:ovDir = Join-Path $script:tmpDir 'overlay'
$ov = $script:ovDir
New-Item -ItemType Directory -Force -Path (Join-Path $ov 'sdk/examples/hello_gui') | Out-Null
$junctions = @(
    @{ Path = 'src'; Target = (Join-Path $Repo 'src') },
    @{ Path = 'tests'; Target = (Join-Path $Repo 'tests') },
    @{ Path = 'tools'; Target = (Join-Path $Repo 'tools') },
    @{ Path = 'sdk/include'; Target = (Join-Path $Repo 'sdk/include') },
    @{ Path = 'sdk/examples/hello_d0'; Target = (Join-Path $Repo 'sdk/examples/hello_d0') }
)
foreach ($j in $junctions) {
    New-Item -ItemType Junction -Path (Join-Path $ov $j.Path) -Target $j.Target | Out-Null
}
# build-ng -SdkDemo 只认 sdk/examples/hello_gui/main.c：三行代理转到 hello_d0。
$proxy = @'
/* generated by tools/d0-demo.ps1: build-ng -SdkDemo 的 demo 源指向 hello_d0。 */
#include "../hello_d0/main.c"
'@
[IO.File]::WriteAllText((Join-Path $ov 'sdk/examples/hello_gui/main.c'),
    $proxy, (New-Object Text.UTF8Encoding $false))

Write-Host 'd0-demo: === overlay build (build-ng -SdkDemo, repo 未修改) ==='
$psHost = $null
try { $psHost = (Get-Process -Id $PID).Path } catch { }
if (-not $psHost) { $psHost = 'powershell' }
$buildArgs = @('-NoProfile')
if ($OnWindows) { $buildArgs += @('-ExecutionPolicy', 'Bypass') }
$buildArgs += @('-File', (Join-Path $ov 'tools/build-ng.ps1'), '-Repo', $ov,
    '-DistDir', '.dist-d0', '-ObjDir', '.obj-d0', '-SdkDemo')
& $psHost @buildArgs
if ($LASTEXITCODE -ne 0) { Fail 'build-ng.ps1 -SdkDemo failed' }

$dist = Join-Path $ov '.dist-d0'
$kernel = Join-Path $dist 'aster-ng.elf'
$disk = Join-Path $dist 'disk.img'
if (-not (Test-Path -LiteralPath $kernel)) { Fail 'kernel ELF not produced' }
if (-not (Test-Path -LiteralPath $disk)) { Fail 'disk image not produced' }

# ---- 3. QEMU + QMP：启动、注入按键、截图像素、q 退出 ----------------------
$script:serialLog = Join-Path $script:tmpDir 'serial.log'
$errLog = Join-Path $script:tmpDir 'qemu-err.log'
$outLog = Join-Path $script:tmpDir 'qemu-out.log'
$shot = Join-Path $script:tmpDir 'd0-screen.ppm'

$qmpPort = 0
try {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $qmpPort = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
} catch {
    Fail 'cannot reserve a QMP port'
}

Write-Host ("d0-demo: kernel = {0} ({1} bytes)" -f $kernel, (Get-Item -LiteralPath $kernel).Length)
$qemuArgs = @(
    '-kernel', ('"' + $kernel + '"'),
    '-serial', ('"file:' + $script:serialLog + '"'),
    '-drive', ('"file=' + $disk + ',format=raw,if=ide"'),
    '-display', 'none',
    '-no-reboot',
    '-m', '256',
    '-cpu', $Cpu,
    '-vga', 'std',
    '-qmp', ("tcp:127.0.0.1:{0},server=on,wait=off" -f $qmpPort)
)
$spArgs = @{
    FilePath = $qemu
    ArgumentList = $qemuArgs
    PassThru = $true
    RedirectStandardError = $errLog
    RedirectStandardOutput = $outLog
}
if ($OnWindows) { $spArgs['WindowStyle'] = 'Hidden' }
$script:qemuProc = Start-Process @spArgs
$qmp = Connect-Qmp -Port $qmpPort
if (-not $qmp) { Fail 'QMP connect failed' }
Write-Step ("qemu up (cpu={0}, qmp={1})" -f $Cpu, $qmpPort)

# 启动到 /bin/demo（hello_d0）自报窗口位置；8s 空闲窗口内完成按键/截图。
$text = Wait-Serial 'ng: d0 demo up \(' $TimeoutSec
$up = [regex]::Match($text,
    'ng: d0 demo up \(pid=(\d+) id=(\d+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) token=(0x[0-9a-f]+) session=([01])\)')
if (-not $up.Success) { Fail 'app did not report "ng: d0 demo up"' }
$winX = [int]$up.Groups[3].Value
$winY = [int]$up.Groups[4].Value
$winW = [int]$up.Groups[5].Value
$winH = [int]$up.Groups[6].Value
Write-Step ("app up (pid={0} id={1} x={2} y={3} w={4} h={5} token={6} session={7})" -f
    $up.Groups[1].Value, $up.Groups[2].Value, $winX, $winY, $winW, $winH,
    $up.Groups[7].Value, $up.Groups[8].Value)
if ($winW -ne 480 -or $winH -ne 300) {
    Fail ("window is {0}x{1}, want 480x300" -f $winW, $winH)
}
if (-not [regex]::IsMatch($text, 'ng: surface title ok \(id=\d+ title=D0 Demo\)')) {
    Fail 'window title "D0 Demo" not seen'
}

# 先注入 space（重置 8s 空闲计时），确认按键 -> count+1 并重绘。
if (-not (Send-QmpKey $qmp 'spc')) { Fail 'QMP space injection failed' }
$text = Wait-Serial 'ng: d0 key ok \(key=0x20 count=\d+\)' 5
$key = [regex]::Match($text, 'ng: d0 key ok \(key=0x20 count=(\d+)\)')
if (-not $key.Success) { Fail 'injected space was not delivered to the app' }
Write-Step ("key ok (space -> count={0})" -f $key.Groups[1].Value)

# 冻结画面截图，断言窗口内三个采样点（红卡片 / 计数面板 / 标题栏）。
if (-not (Send-QmpStop $qmp)) { Fail 'QMP stop failed' }
if (-not (Send-QmpScreendump $qmp $shot)) { Fail 'QMP screendump failed' }
for ($i = 0; $i -lt 30 -and -not (Test-Path -LiteralPath $shot); $i++) {
    Start-Sleep -Milliseconds 100
}
$ppm = Read-Ppm $shot
if ($null -eq $ppm) { Fail 'screendump PPM not readable' }
$pxCard = Get-PpmPixel $ppm ($winX + 30) ($winY + 60)
$pxPanel = Get-PpmPixel $ppm ($winX + 40) ($winY + 170)
$pxHead = Get-PpmPixel $ppm ($winX + 460) ($winY + 30)
Write-Step ("screendump card=({0},{1}){2} panel=({3},{4}){5} header=({6},{7}){8}" -f
    ($winX + 30), ($winY + 60), (Format-Pixel $pxCard),
    ($winX + 40), ($winY + 170), (Format-Pixel $pxPanel),
    ($winX + 460), ($winY + 30), (Format-Pixel $pxHead))
if ((Format-Pixel $pxCard) -ne '#c0504a') {
    Fail ("card pixel {0}, want #c0504a" -f (Format-Pixel $pxCard))
}
if ((Format-Pixel $pxPanel) -ne '#16202b') {
    Fail ("panel pixel {0}, want #16202b" -f (Format-Pixel $pxPanel))
}
if ((Format-Pixel $pxHead) -ne '#18222e') {
    Fail ("header pixel {0}, want #18222e" -f (Format-Pixel $pxHead))
}
$null = Send-QmpCont $qmp

# 注入 q：应用应打印 reason=q/esc 并以 0 退出。
if (-not (Send-QmpKey $qmp 'q')) { Fail 'QMP q injection failed' }
$text = Wait-Serial 'ng: d0 demo bye \(' 5
$bye = [regex]::Match($text, 'ng: d0 demo bye \(id=\d+ count=\d+ rounds=\d+ reason=q/esc\)')
if (-not $bye.Success) { Fail 'q did not quit the app cleanly' }
Write-Step 'q quit ok (reason=q/esc)'

# 8s 空闲自动退出是 hello_d0 的第三类退出路径；这里给出日志证据（不作为
# PASS 前提，因为本脚本已经在 8s 内用 q 主动退出）。
$idleProof = [regex]::IsMatch($text, 'ng: d0 quit ok \(reason=idle')
if ($idleProof) { Write-Step 'idle-exit evidence also present' }

$null = Send-QmpJson $qmp '{"execute":"quit"}'
Start-Sleep -Milliseconds 200
Stop-Qemu

Write-Step ("PASS (480x300 D0 Demo, 3 cards, key+redraw, q quit; {0:N1}s, cpu {1})" -f
    $totalSw.Elapsed.TotalSeconds, $Cpu)
Remove-Temp
exit 0
