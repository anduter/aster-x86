# =============================================================================
# audit-syscall.ps1 - user->kernel 边界审计的回归驱动（audit-syscall 会话）
#
# 通过 build.ps1 -Test 构建并在 QEMU 里启动 ring3 shell，再用 QMP sendkey 注入
# 一串 `sys <nr> <a> <b> <c>` 命令（src/user/user_shell.c 的审计调试命令），
# 最后解析串口日志断言返回值。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit-syscall.ps1
#   ... -Suite guard    # 修复后的断言（默认）：内核地址缓冲必须 -1
#   ... -Suite probe    # 修复前的复现（信息性：记录漏洞行为）
#   ... -Suite priv     # 修复后：非 root 的 panic/reboot/poweroff/hang 必须 -1
#   ... -Suite full     # 标准 225 项回归（默认按键），只看 build.ps1 的结论
#   ... -BuildRoot D:\some\snapshot   # 在另一份源码快照里构建（并行会话隔离）
#
# 端口：QMP 4541；-Test 需要宿主 12345 空闲。运行前先探测，避免假失败。
# =============================================================================
param(
    [ValidateSet('guard', 'probe', 'priv', 'full')]
    [string]$Suite = 'guard',
    [string]$BuildRoot = '',
    [string]$ObjDir = '.obj-audit-sys',
    [string]$DistDir = '.dist-audit-sys',
    [int]$QmpPort = 4541,
    [int]$TimeoutSec = 20
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($BuildRoot -eq '') { $BuildRoot = $root }
$BuildRoot = (Resolve-Path $BuildRoot).Path

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

if (-not (Test-PortFree -Port $QmpPort)) {
    throw ("QMP 端口 {0} 已被占用（另一个会话的 QEMU 还在跑？）" -f $QmpPort)
}
if (-not (Test-PortFree -Port 12345 -Proto udp) -or
    -not (Test-PortFree -Port 12345 -Addr '0.0.0.0')) {
    throw '宿主 12345 已被占用：-Test 的回显端口冲突（同一时刻只能跑一个 -Test 会话）'
}

# ---- 用例表：key = 注入的命令行；check = 期望的 sys 返回值 ----
# legacy 编号：1 WRITE / 22 BRK / 27 REBOOT / 28 PANIC / 35 PCI /
#              36 POWEROFF / 37 MOUSE / 38 HANG / 39 ETH / 40 RESINFO /
#              48 MMAP / 49 MUNMAP / 57 BLK_READ
$probeLines = @(
    'sys 48 4096 3 0',            # 0: 正常 mmap，应成功
    'sys 49 1077936128 4096 0',   # 1: munmap(0x40400000,4096)
    'sys 22 0 0 0',               # 2: brk(0) 查询
    'sys 22 3221225472 0 0',      # 3: brk(0xC0000000) 回绕（漏洞）
    'sys 22 0 0 0',               # 4: 确认 brk 已被带到内核地址
    'sys 40 2097152 0 0',         # 5: RESINFO 写内核 0x200000（PMM 位图）
    'sys 37 8388608 0 0',         # 6: MOUSE 写内核 0x800000
    'res',                        # 7: 合法用户缓冲仍然可用
    'mouse'                       # 8: 合法用户缓冲仍然可用
)
$guardLines = @(
    'sys 48 4096 3 0',                  # 0: 正常 mmap
    'sys 49 1077936128 4096 0',         # 1: munmap
    'sys 49 1077936128 4096 0',         # 2: 重复 munmap -> -1（无 double free）
    'sys 22 0 0 0',                     # 3: brk(0)
    'sys 22 3221225472 0 0',            # 4: brk 回绕 -> -1
    'sys 22 2147483648 0 0',            # 5: 负向增量越过堆底 -> -1
    'sys 1 1048576 8 0',                # 6: SYS_WRITE 内核源 -> -1
    'sys 1 1073741824 4294967295 0',    # 7: SYS_WRITE 超大 len -> -1
    'sys 48 4294967295 3 0',            # 8: mmap len 回绕成 0 -> -1
    'sys 48 4294963200 3 0',            # 9: mmap len 0xFFFFF000 -> -1
    'sys 62 1077936128 4096 2',         # 10: mprotect 未映射页 -> -1
    'sys 62 1077936129 4096 2',         # 11: mprotect 未对齐 -> -1
    'sys 62 1077936128 0 2',            # 12: mprotect len=0 -> -1
    'sys 40 2097152 0 0',               # 13: RESINFO 内核目标 -> -1
    'sys 37 8388608 0 0',               # 14: MOUSE 内核目标 -> -1
    'sys 35 0 2097152 0',               # 15: PCI 内核目标 -> -1
    'sys 39 0 2097152 0',               # 16: ETH 内核目标 -> -1
    'sys 57 0 2097152 0',               # 17: BLK_READ 内核目标 -> -1
    'res',                              # 18: 合法用户缓冲
    'mouse'                             # 19: 合法用户缓冲
)
$privLines = @(
    'drop 1000',
    'sys 28 0 0 0',
    'sys 27 0 0 0',
    'sys 36 0 0 0',
    'sys 38 0 0 0'
)
$fullKeys = 'help`nfork`nsectest`nlinktest`necho hi`nreexec'

switch ($Suite) {
    'probe' { $lines = $probeLines; $keys = ($probeLines -join "`n"); $tag = 'probe' }
    'guard' { $lines = $guardLines; $keys = ($guardLines -join "`n"); $tag = 'guard' }
    'priv'  { $lines = $privLines;  $keys = ($privLines  -join "`n"); $tag = 'priv' }
    'full'  { $lines = @();         $keys = $fullKeys;                 $tag = 'full' }
}

$logDir = Join-Path $root 'obj\audit'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$serial = Join-Path $logDir ("syscall-{0}.log" -f $tag)
if (Test-Path $serial) { Remove-Item $serial -Force }

$buildPs1 = Join-Path $BuildRoot 'build.ps1'
if (-not (Test-Path $buildPs1)) { throw "找不到 build.ps1：$buildPs1" }

$buildArgs = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildPs1,
    '-Test', '-ObjDir', $ObjDir, '-DistDir', $DistDir,
    '-QmpPort', "$QmpPort", '-SerialLog', $serial,
    '-TimeoutSec', "$TimeoutSec", '-Keys', $keys
)
Write-Host ("[audit-syscall] suite={0} build-root={1}" -f $tag, $BuildRoot) -ForegroundColor Cyan
& powershell @buildArgs
$buildExit = $LASTEXITCODE

if (-not (Test-Path $serial)) { throw "串口日志不存在：$serial（构建/启动失败？）" }
$out = Get-Content $serial -Raw

# ---- 断言 ----
$fail = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) {
        Write-Host ("  [PASS] {0} {1}" -f $name, $detail) -ForegroundColor Green
    } else {
        Write-Host ("  [FAIL] {0} {1}" -f $name, $detail) -ForegroundColor Red
        $script:fail++
    }
}

if ($Suite -eq 'full') {
    $pass = ($out -match '=== TEST PASS \(\d+/\d+\)') -and ($buildExit -eq 0)
    Check '标准 225 项回归' $pass ("(build exit=$buildExit)")
    $dump = @($out -split "`r?`n" | Where-Object { $_ -match '\[FAIL\]' -or $_ -match 'TEST (PASS|FAIL)' })
    foreach ($d in $dump) { Write-Host ("    " + $d) }
} else {
    $sysLines = @([regex]::Matches($out, 'sys: nr=(\d+) a=(\d+) b=(\d+) c=(\d+) ret=(-?\d+)') |
                  ForEach-Object { $_.Value })
    $rets = @([regex]::Matches($out, 'sys: nr=(\d+) a=(\d+) b=(\d+) c=(\d+) ret=(-?\d+)') |
              ForEach-Object { [int]$_.Groups[5].Value })

    if ($Suite -eq 'probe') {
        # 信息性：记录修复前的脆弱行为（成功把内核地址当用户缓冲）。
        Check 'probe: 注入了 7 条 sys 命令' ($rets.Count -ge 7) ("(got {0})" -f $rets.Count)
        if ($rets.Count -ge 7) {
            Check 'probe: mmap 成功' ($rets[0] -gt 0) ("ret={0}" -f $rets[0])
            Check 'probe: munmap 成功' ($rets[1] -eq 0) ("ret={0}" -f $rets[1])
            Check 'probe: brk(0xC0000000) 回绕成功（漏洞）' `
                  ($rets[3] -gt 0 -and $rets[3] -lt 0x40000000) ("ret={0} (0x{0:x})" -f $rets[3])
            Check 'probe: brk 被带到 <0x40000000（漏洞）' `
                  ($rets[4] -eq $rets[3] -and $rets[4] -lt 0x40000000) ("ret={0}" -f $rets[4])
            Check 'probe: RESINFO 写内核 0x200000（漏洞）' ($rets[5] -eq 0) ("ret={0}" -f $rets[5])
            Check 'probe: MOUSE 写内核 0x800000（漏洞）' ($rets[6] -eq 0) ("ret={0}" -f $rets[6])
        }
        Check 'probe: 内核未 panic（信息性）' (-not $out.Contains('KERNEL PANIC'))
    } elseif ($Suite -eq 'guard') {
        Check 'guard: 注入了 18 条 sys 命令' ($rets.Count -ge 18) ("(got {0})" -f $rets.Count)
        Check 'guard: 启动自检 brk range guard' ($out.Contains('mm: brk range guard ok'))
        if ($rets.Count -ge 18) {
            Check 'guard: mmap 正常' ($rets[0] -eq 1077936128) ("ret={0}" -f $rets[0])
            Check 'guard: munmap 正常' ($rets[1] -eq 0) ("ret={0}" -f $rets[1])
            Check 'guard: 重复 munmap 被拒' ($rets[2] -eq -1) ("ret={0}" -f $rets[2])
            Check 'guard: brk(0) 返回堆内地址' `
                  ($rets[3] -ge 0x40200000 -and $rets[3] -le 0x40300000) ("ret={0}" -f $rets[3])
            Check 'guard: brk 回绕被拒' ($rets[4] -eq -1) ("ret={0}" -f $rets[4])
            Check 'guard: brk 负向越过堆底被拒' ($rets[5] -eq -1) ("ret={0}" -f $rets[5])
            Check 'guard: SYS_WRITE 内核源被拒' ($rets[6] -eq -1) ("ret={0}" -f $rets[6])
            Check 'guard: SYS_WRITE 超大 len 被拒' ($rets[7] -eq -1) ("ret={0}" -f $rets[7])
            Check 'guard: mmap len 回绕被拒' ($rets[8] -eq -1) ("ret={0}" -f $rets[8])
            Check 'guard: mmap 超大 len 被拒' ($rets[9] -eq -1) ("ret={0}" -f $rets[9])
            Check 'guard: mprotect 未映射页被拒' ($rets[10] -eq -1) ("ret={0}" -f $rets[10])
            Check 'guard: mprotect 未对齐被拒' ($rets[11] -eq -1) ("ret={0}" -f $rets[11])
            Check 'guard: mprotect len=0 被拒' ($rets[12] -eq -1) ("ret={0}" -f $rets[12])
            Check 'guard: RESINFO 内核目标被拒' ($rets[13] -eq -1) ("ret={0}" -f $rets[13])
            Check 'guard: MOUSE 内核目标被拒' ($rets[14] -eq -1) ("ret={0}" -f $rets[14])
            Check 'guard: PCI 内核目标被拒' ($rets[15] -eq -1) ("ret={0}" -f $rets[15])
            Check 'guard: ETH 内核目标被拒' ($rets[16] -eq -1) ("ret={0}" -f $rets[16])
            Check 'guard: BLK_READ 内核目标被拒' ($rets[17] -eq -1) ("ret={0}" -f $rets[17])
            Check 'guard: 内核内存未被打印（无 KRNL 泄漏）' (
                -not ($out -match 'KRNLsys: nr=1'))
        }
        Check 'guard: res 合法缓冲仍可用' ($out -match 'res: mem\s+free=\d+/\d+ pages')
        Check 'guard: mouse 合法缓冲仍可用' ($out -match 'mouse: x=-?\d+ y=-?\d+ buttons=\d+')
        Check 'guard: 无 KERNEL PANIC' (-not $out.Contains('KERNEL PANIC'))
    } elseif ($Suite -eq 'priv') {
        Check 'priv: 已 drop 到 uid 1000' ($out.Contains('drop: now uid=1000'))
        Check 'priv: 注入了 4 条特权调用' ($rets.Count -ge 4) ("(got {0})" -f $rets.Count)
        if ($rets.Count -ge 4) {
            Check 'priv: 非 root SYS_PANIC 被拒' ($rets[0] -eq -1) ("ret={0}" -f $rets[0])
            Check 'priv: 非 root SYS_REBOOT 被拒' ($rets[1] -eq -1) ("ret={0}" -f $rets[1])
            Check 'priv: 非 root SYS_POWEROFF 被拒' ($rets[2] -eq -1) ("ret={0}" -f $rets[2])
            Check 'priv: 非 root SYS_HANG 被拒' ($rets[3] -eq -1) ("ret={0}" -f $rets[3])
        }
        Check 'priv: 无 KERNEL PANIC' (-not $out.Contains('KERNEL PANIC'))
    }
}

Write-Host ("[audit-syscall] suite={0} => {1}" -f $tag, $(if ($fail -eq 0) { 'PASS' } else { "FAIL ($fail)" })) `
    -ForegroundColor $(if ($fail -eq 0) { 'Green' } else { 'Red' })
Write-Host ("[audit-syscall] serial: {0}" -f $serial)
if ($fail -ne 0) { exit 1 }
exit 0
