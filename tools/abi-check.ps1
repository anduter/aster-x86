# =============================================================================
# abi-check.ps1 - ABI 门禁（WP4-1: ABI 1.0 冻结；WP5-0a: + ABI 1.1 只追加）
#
# 比对调用号与关键常量：
#   docs/abi/abi-1.0.h          （1.0 冻结契约，机器可读）
#   docs/abi/abi-1.1.h          （1.1 增量：版本 1.1.0 + 新号 48–95；1.0 部分
#                                由 #include "abi-1.0.h" 继承）
#   sdk/include/aster/syscall.h （SDK 消费端，仍是 1.0）
#   src/ng/syscall.h            （内核实现）
#
# 另外核对：
#   - 错误码数值：ABI 头 vs SDK 头 vs 内核各模块的 E* 别名；
#   - IPC/VFS/进程/共享内存/认证/日志/包/输入上限：ABI 头 vs 内核宏；
#   - 结构体布局：vfs_stat / vfs_dirent / ipc_msg_hdr / ds_fb_info；
#   - docs/abi/abi-1.0.md 存在且标记为冻结，并链接后继版本 abi-1.1.md；
#   - ABI 1.1：版本 1.1.0、48–95 号段完整且不撞 1.0 号、ENOSYS(-38) 语义、
#     内核 default 分支返回 NG_SYS_ENOSYS；
#   - docs/abi/abi-1.1.md 存在且标记为草案（DRAFT）。
#
# 任何一项不一致 -> 打印差异并 exit 1。供 tools/release-ng.ps1 与 CI 调用。
#
# usage: powershell -File tools/abi-check.ps1 [-Repo <root>]
# 退出码：0 = PASS；1 = FAIL。
# =============================================================================
param(
    [string]$Repo = ''
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

function Fail([string]$msg) {
    Write-Host ("abi-check: FAIL ({0})" -f $msg) -ForegroundColor Red
    exit 1
}

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $Repo)) { Fail ("repo not found: {0}" -f $Repo) }
$root = (Resolve-Path -LiteralPath $Repo).Path

$files = @{
    Abi = Join-Path $root 'docs/abi/abi-1.0.h'
    AbiDoc = Join-Path $root 'docs/abi/abi-1.0.md'
    Abi11 = Join-Path $root 'docs/abi/abi-1.1.h'
    Abi11Doc = Join-Path $root 'docs/abi/abi-1.1.md'
    Sdk = Join-Path $root 'sdk/include/aster/syscall.h'
    Src = Join-Path $root 'src/ng/syscall.h'
}
foreach ($k in @('Abi', 'AbiDoc', 'Abi11', 'Abi11Doc', 'Sdk', 'Src')) {
    if (-not (Test-Path -LiteralPath $files[$k])) {
        Fail ("missing {0}: {1}" -f $k, $files[$k])
    }
}

Write-Host '=== abi-check (WP4-1: ABI 1.0 freeze + WP5-0a: ABI 1.1 additive) ==='
Write-Host ("repo: {0}" -f $root)
Write-Host ("abi:  {0}" -f $files.Abi)
Write-Host ("abi11:{0}" -f $files.Abi11)
Write-Host ("sdk:  {0}" -f $files.Sdk)
Write-Host ("src:  {0}" -f $files.Src)

# -----------------------------------------------------------------------------
# 宏读取 / 求值（够用的 C 常量表达式解析器：数字、标识符、+ - * << >> & | ^ ~）
# -----------------------------------------------------------------------------

function Read-Macros([string]$path) {
    $map = @{}
    $lines = [IO.File]::ReadAllLines($path)
    $i = 0
    while ($i -lt $lines.Count) {
        $line = $lines[$i]
        $i++
        while ($line -match '\\\s*$' -and $i -lt $lines.Count) {
            $line = $line -replace '\\\s*$', ''
            $line = $line + ' ' + $lines[$i]
            $i++
        }
        $m = [regex]::Match($line, '^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(\s+)(.*)$')
        if (-not $m.Success) { continue }
        $name = $m.Groups[1].Value
        $val = $m.Groups[3].Value
        $val = [regex]::Replace($val, '/\*.*?\*/', '')
        $cut = $val.IndexOf('//')
        if ($cut -ge 0) { $val = $val.Substring(0, $cut) }
        $val = $val.Trim()
        if ($val.Length -eq 0) { continue }
        $map[$name] = $val
    }
    return $map
}

function Merge-Macros([hashtable]$dst, [hashtable]$src, [string]$path) {
    foreach ($k in $src.Keys) {
        if ($dst.ContainsKey($k) -and $dst[$k] -ne $src[$k]) {
            Fail ("macro {0} defined twice with different values (at {1})" -f $k, $path)
        }
        $dst[$k] = $src[$k]
    }
}

function New-TokenList {
    $l = New-Object System.Collections.Generic.List[object]
    return ,$l
}

function ConvertTo-Tokens([string]$expr, [hashtable]$macros, [hashtable]$cache,
                          $stack) {
    $tokens = New-TokenList
    $pos = 0
    while ($pos -lt $expr.Length) {
        $c = $expr[$pos]
        if ([char]::IsWhiteSpace($c)) { $pos++; continue }

        if ($c -match '[A-Za-z_]') {
            $start = $pos
            while ($pos -lt $expr.Length -and $expr[$pos] -match '[A-Za-z0-9_]') {
                $pos++
            }
            $name = $expr.Substring($start, $pos - $start)
            $v = Resolve-Macro $name $macros $cache $stack
            $tokens.Add([pscustomobject]@{ K = 'num'; V = $v })
            continue
        }

        if ($c -match '[0-9]') {
            $start = $pos
            if ($c -eq '0' -and ($pos + 1) -lt $expr.Length -and
                ($expr[$pos + 1] -eq 'x' -or $expr[$pos + 1] -eq 'X')) {
                $pos += 2
                while ($pos -lt $expr.Length -and $expr[$pos] -match '[0-9A-Fa-f]') {
                    $pos++
                }
            } else {
                while ($pos -lt $expr.Length -and $expr[$pos] -match '[0-9]') {
                    $pos++
                }
            }
            $raw = $expr.Substring($start, $pos - $start)
            while ($pos -lt $expr.Length -and $expr[$pos] -match '[uUlL]') { $pos++ }
            if ($raw.Length -le 2 -and ($raw -eq '0x' -or $raw -eq '0X')) {
                Fail ("bad hex literal in '{0}'" -f $expr)
            }
            if ($raw.StartsWith('0x') -or $raw.StartsWith('0X')) {
                $u = [Convert]::ToUInt64($raw.Substring(2), 16)
                $tokens.Add([pscustomobject]@{ K = 'num'; V = [int64]$u })
            } else {
                $tokens.Add([pscustomobject]@{ K = 'num'; V = [int64]$raw })
            }
            continue
        }

        if ($pos + 1 -lt $expr.Length -and
            ($expr.Substring($pos, 2) -eq '<<' -or $expr.Substring($pos, 2) -eq '>>')) {
            $tokens.Add([pscustomobject]@{ K = 'op'; V = $expr.Substring($pos, 2) })
            $pos += 2
            continue
        }

        if ('()+-*&|^~'.IndexOf([string]$c) -ge 0) {
            $tokens.Add([pscustomobject]@{ K = 'op'; V = [string]$c })
            $pos++
            continue
        }

        Fail ("unsupported character '{0}' in expression: {1}" -f $c, $expr)
    }
    return ,$tokens
}

function Parse-Primary($st, [hashtable]$macros, [hashtable]$cache, $stack) {
    if ($st.Pos -ge $st.Tokens.Count) { Fail 'expression ended unexpectedly' }
    $t = $st.Tokens[$st.Pos]
    if ($t.K -eq 'num') {
        $st.Pos++
        return $t.V
    }
    if ($t.V -eq '(') {
        $st.Pos++
        $v = Parse-Or $st $macros $cache $stack
        if ($st.Pos -ge $st.Tokens.Count -or $st.Tokens[$st.Pos].V -ne ')') {
            Fail 'missing ) in constant expression'
        }
        $st.Pos++
        return $v
    }
    if ($t.V -eq '~') {
        $st.Pos++
        return (-bnot (Parse-Primary $st $macros $cache $stack))
    }
    if ($t.V -eq '-') {
        $st.Pos++
        return (-(Parse-Primary $st $macros $cache $stack))
    }
    if ($t.V -eq '+') {
        $st.Pos++
        return (+(Parse-Primary $st $macros $cache $stack))
    }
    Fail ("unexpected token '{0}' in constant expression" -f $t.V)
}

function Parse-Mul($st, [hashtable]$macros, [hashtable]$cache, $stack) {
    $v = Parse-Primary $st $macros $cache $stack
    while ($st.Pos -lt $st.Tokens.Count -and $st.Tokens[$st.Pos].K -eq 'op' -and
           $st.Tokens[$st.Pos].V -eq '*') {
        $st.Pos++
        $v = $v * (Parse-Primary $st $macros $cache $stack)
    }
    return $v
}

function Parse-Add($st, [hashtable]$macros, [hashtable]$cache, $stack) {
    $v = Parse-Mul $st $macros $cache $stack
    while ($st.Pos -lt $st.Tokens.Count -and $st.Tokens[$st.Pos].K -eq 'op' -and
           ($st.Tokens[$st.Pos].V -eq '+' -or $st.Tokens[$st.Pos].V -eq '-')) {
        $op = $st.Tokens[$st.Pos].V
        $st.Pos++
        $rhs = Parse-Mul $st $macros $cache $stack
        if ($op -eq '+') { $v = $v + $rhs } else { $v = $v - $rhs }
    }
    return $v
}

function Parse-Shift($st, [hashtable]$macros, [hashtable]$cache, $stack) {
    $v = Parse-Add $st $macros $cache $stack
    while ($st.Pos -lt $st.Tokens.Count -and $st.Tokens[$st.Pos].K -eq 'op' -and
           ($st.Tokens[$st.Pos].V -eq '<<' -or $st.Tokens[$st.Pos].V -eq '>>')) {
        $op = $st.Tokens[$st.Pos].V
        $st.Pos++
        $rhs = Parse-Add $st $macros $cache $stack
        if ($op -eq '<<') { $v = $v -shl [int]$rhs } else { $v = $v -shr [int]$rhs }
    }
    return $v
}

function Parse-And($st, [hashtable]$macros, [hashtable]$cache, $stack) {
    $v = Parse-Shift $st $macros $cache $stack
    while ($st.Pos -lt $st.Tokens.Count -and $st.Tokens[$st.Pos].K -eq 'op' -and
           $st.Tokens[$st.Pos].V -eq '&') {
        $st.Pos++
        $v = $v -band (Parse-Shift $st $macros $cache $stack)
    }
    return $v
}

function Parse-Xor($st, [hashtable]$macros, [hashtable]$cache, $stack) {
    $v = Parse-And $st $macros $cache $stack
    while ($st.Pos -lt $st.Tokens.Count -and $st.Tokens[$st.Pos].K -eq 'op' -and
           $st.Tokens[$st.Pos].V -eq '^') {
        $st.Pos++
        $v = $v -bxor (Parse-And $st $macros $cache $stack)
    }
    return $v
}

function Parse-Or($st, [hashtable]$macros, [hashtable]$cache, $stack) {
    $v = Parse-Xor $st $macros $cache $stack
    while ($st.Pos -lt $st.Tokens.Count -and $st.Tokens[$st.Pos].K -eq 'op' -and
           $st.Tokens[$st.Pos].V -eq '|') {
        $st.Pos++
        $v = $v -bor (Parse-Xor $st $macros $cache $stack)
    }
    return $v
}

function Resolve-Macro([string]$name, [hashtable]$macros, [hashtable]$cache,
                       $stack) {
    if ($cache.ContainsKey($name)) { return $cache[$name] }
    if (-not $macros.ContainsKey($name)) {
        Fail ("macro '{0}' not found" -f $name)
    }
    if ($stack.Contains($name)) {
        Fail ("macro cycle at '{0}'" -f $name)
    }
    $stack.Add($name)
    try {
        $tokens = ConvertTo-Tokens $macros[$name] $macros $cache $stack
        if ($tokens.Count -eq 0) { Fail ("empty macro '{0}'" -f $name) }
        $st = [pscustomobject]@{ Tokens = $tokens; Pos = 0 }
        $v = Parse-Or $st $macros $cache $stack
        if ($st.Pos -ne $tokens.Count) {
            Fail ("trailing tokens in macro '{0}': {1}" -f $name, $macros[$name])
        }
        $cache[$name] = $v
        return $v
    } finally {
        $stack.RemoveAt($stack.Count - 1)
    }
}

# -----------------------------------------------------------------------------
# 读取三处头文件 + 内核常量来源
# -----------------------------------------------------------------------------

$abi = Read-Macros $files.Abi
$abi11 = Read-Macros $files.Abi11
$sdk = Read-Macros $files.Sdk
$src = Read-Macros $files.Src

$kernel = @{}
$kernelFiles = @(
    'src/ng/syscall.c',                    # USER_WRITE_MAX
    'src/ng/uaccess.c',                    # UACCESS_USER_VA_MAX
    'src/ng/uaccess.h',
    'src/ng/handle.h',
    'src/ng/ipc.h',
    'src/ng/proc.h',
    'src/ng/shm.h',
    'src/ng/fbmap.h',
    'src/ng/fs_syscall.h',
    'src/ng/svc_syscall.h',
    'src/services/fs/include/vfs.h',
    'src/services/auth/include/auth.h',
    'src/services/log/include/log.h',
    'src/services/pkg/include/pkg.h',
    'src/drivers/include/input.h',
    'src/desktop/include/ds_protocol.h'
)
foreach ($rel in $kernelFiles) {
    $p = Join-Path $root $rel
    if (-not (Test-Path -LiteralPath $p)) { Fail ("missing kernel header: {0}" -f $rel) }
    Merge-Macros $kernel (Read-Macros $p) $rel
}

$abiCache = @{}
$sdkCache = @{}
$srcCache = @{}
$kernelCache = @{}
$abi11Cache = @{}

# ABI 1.1 头 #include "abi-1.0.h"：机器上等价于“1.0 宏 + 1.1 增量”。
# 这里用同样的模型求值：$abi11All = $abi 叠加 $abi11；1.0 的其它宏不允许被
# 1.1 重定义（只有 5 个版本宏在 1.1 里覆盖为 1.1.0）。
$versionMacros = @(
    'ASTER_ABI_VERSION_MAJOR', 'ASTER_ABI_VERSION_MINOR',
    'ASTER_ABI_VERSION_PATCH', 'ASTER_ABI_VERSION', 'ASTER_ABI_VERSION_STRING'
)
$abi11All = @{}
foreach ($k in $abi.Keys) { $abi11All[$k] = $abi[$k] }
foreach ($k in $abi11.Keys) {
    if ($abi.ContainsKey($k) -and ($versionMacros -notcontains $k)) {
        if ($abi[$k] -ne $abi11[$k]) {
            Fail ("ABI 1.1 redefines 1.0 macro {0}: {1} -> {2}" -f $k, $abi[$k], $abi11[$k])
        }
    }
    $abi11All[$k] = $abi11[$k]
}

function Val([string]$name, [hashtable]$macros, [hashtable]$cache) {
    $stack = New-Object System.Collections.Generic.List[string]
    return (Resolve-Macro $name $macros $cache $stack)
}

# -----------------------------------------------------------------------------
# 1. 调用号：三处逐号一致，且等于冻结表
# -----------------------------------------------------------------------------

$syscalls = @(
    @{ Name = 'WRITE'; Nr = 0 },
    @{ Name = 'EXIT'; Nr = 1 },
    @{ Name = 'PORT_CREATE'; Nr = 10 },
    @{ Name = 'PORT_SEND'; Nr = 11 },
    @{ Name = 'PORT_RECV'; Nr = 12 },
    @{ Name = 'PORT_REPLY'; Nr = 13 },
    @{ Name = 'PORT_CLOSE'; Nr = 14 },
    @{ Name = 'OPEN'; Nr = 20 },
    @{ Name = 'READ'; Nr = 21 },
    @{ Name = 'WRITE_FD'; Nr = 22 },
    @{ Name = 'CLOSE'; Nr = 23 },
    @{ Name = 'STAT'; Nr = 24 },
    @{ Name = 'LISTDIR'; Nr = 25 },
    @{ Name = 'MKDIR'; Nr = 26 },
    @{ Name = 'UNLINK'; Nr = 27 },
    @{ Name = 'READKEY'; Nr = 30 },
    @{ Name = 'LOGIN'; Nr = 31 },
    @{ Name = 'DMESG'; Nr = 32 },
    @{ Name = 'LOG_WRITE'; Nr = 33 },
    @{ Name = 'PKG_LIST'; Nr = 34 },
    @{ Name = 'PKG_VERIFY'; Nr = 35 },
    @{ Name = 'SPAWN'; Nr = 40 },
    @{ Name = 'WAIT'; Nr = 41 },
    @{ Name = 'GETPID'; Nr = 42 },
    @{ Name = 'KILL'; Nr = 43 },
    @{ Name = 'GETPPID'; Nr = 44 },
    @{ Name = 'SHM_ALLOC'; Nr = 45 },
    @{ Name = 'SHM_MAP'; Nr = 46 },
    @{ Name = 'FB_MAP'; Nr = 47 }
)

function Keys-With-Prefix([hashtable]$map, [string]$prefix) {
    return @($map.Keys | Where-Object { $_.StartsWith($prefix) } | Sort-Object)
}

function Compare-KeySets([string[]]$actual, [string[]]$expected, [string]$what) {
    $missing = @($expected | Where-Object { $actual -notcontains $_ })
    $extra = @($actual | Where-Object { $expected -notcontains $_ })
    if ($missing.Count -gt 0) {
        Fail ("{0}: missing {1}" -f $what, ($missing -join ', '))
    }
    if ($extra.Count -gt 0) {
        Fail ("{0}: unexpected {1}" -f $what, ($extra -join ', '))
    }
}

$expectedAbi = @($syscalls | ForEach-Object { 'ASTER_ABI_SYS_' + $_.Name })
$expectedSdk = @($syscalls | ForEach-Object { 'ASTER_SYS_' + $_.Name })
# 内核头额外有 NG_SYS_ENOSYS(-38)（ABI 1.1 的 default 返回值，不是调用号）。
$expectedSrc = @($syscalls | ForEach-Object { 'NG_SYS_' + $_.Name }) + @('NG_SYS_ENOSYS')
Compare-KeySets (Keys-With-Prefix $abi 'ASTER_ABI_SYS_') $expectedAbi 'ABI syscall set'
Compare-KeySets (Keys-With-Prefix $sdk 'ASTER_SYS_') $expectedSdk 'SDK syscall set'
Compare-KeySets (Keys-With-Prefix $src 'NG_SYS_') $expectedSrc 'kernel syscall set'

$seen = @{}
foreach ($s in $syscalls) {
    if ($seen.ContainsKey($s.Nr)) { Fail ("duplicate frozen syscall number {0}" -f $s.Nr) }
    $seen[$s.Nr] = $s.Name
    $a = Val ('ASTER_ABI_SYS_' + $s.Name) $abi $abiCache
    $k = Val ('NG_SYS_' + $s.Name) $src $srcCache
    $d = Val ('ASTER_SYS_' + $s.Name) $sdk $sdkCache
    if ($a -ne $s.Nr) {
        Fail ("ABI syscall {0}={1}, frozen table says {2}" -f $s.Name, $a, $s.Nr)
    }
    if ($k -ne $s.Nr) {
        Fail ("kernel syscall {0}={1}, frozen table says {2}" -f $s.Name, $k, $s.Nr)
    }
    if ($d -ne $s.Nr) {
        Fail ("SDK syscall {0}={1}, frozen table says {2}" -f $s.Name, $d, $s.Nr)
    }
}
Write-Host ("[ok] {0} syscalls agree (ABI/SDK/kernel): 0/1, 10-14, 20-27, 30-35, 40-47" -f $syscalls.Count) -ForegroundColor DarkGreen

# ABI 1.1 的 default 返回值（内核头常量；不在 $syscalls 里，因为它不是调用号）。
if ((Val 'NG_SYS_ENOSYS' $src $srcCache) -ne -38) {
    Fail ("kernel NG_SYS_ENOSYS={0}, expected -38" -f (Val 'NG_SYS_ENOSYS' $src $srcCache))
}
Write-Host '[ok] kernel NG_SYS_ENOSYS == -38 (ABI 1.1 unallocated/unimplemented)' -ForegroundColor DarkGreen

# -----------------------------------------------------------------------------
# 2. 错误码：ABI 全表 = 冻结值；SDK 镜像（缺 ESRCH/EINTR）；内核别名一致
# -----------------------------------------------------------------------------

$errors = @(
    @{ Name = 'EPERM'; Val = -1 },
    @{ Name = 'ENOENT'; Val = -2 },
    @{ Name = 'ESRCH'; Val = -3 },
    @{ Name = 'EINTR'; Val = -4 },
    @{ Name = 'EIO'; Val = -5 },
    @{ Name = 'EBADF'; Val = -9 },
    @{ Name = 'ECHILD'; Val = -10 },
    @{ Name = 'EAGAIN'; Val = -11 },
    @{ Name = 'ENOMEM'; Val = -12 },
    @{ Name = 'EACCES'; Val = -13 },
    @{ Name = 'EFAULT'; Val = -14 },
    @{ Name = 'EBUSY'; Val = -16 },
    @{ Name = 'EEXIST'; Val = -17 },
    @{ Name = 'ENODEV'; Val = -19 },
    @{ Name = 'ENOTDIR'; Val = -20 },
    @{ Name = 'EISDIR'; Val = -21 },
    @{ Name = 'EINVAL'; Val = -22 },
    @{ Name = 'ENFILE'; Val = -23 },
    @{ Name = 'EMFILE'; Val = -24 },
    @{ Name = 'ENOSPC'; Val = -28 },
    @{ Name = 'EROFS'; Val = -30 },
    @{ Name = 'ENAMETOOLONG'; Val = -36 },
    @{ Name = 'ENOSYS'; Val = -38 },
    @{ Name = 'ENOTEMPTY'; Val = -39 },
    @{ Name = 'ELOOP'; Val = -40 },
    @{ Name = 'EMSGSIZE'; Val = -90 },
    @{ Name = 'ETIMEDOUT'; Val = -110 },
    @{ Name = 'ECANCELED'; Val = -125 }
)

$expectedAbiErr = @($errors | ForEach-Object { 'ASTER_ABI_' + $_.Name })
Compare-KeySets (Keys-With-Prefix $abi 'ASTER_ABI_E') $expectedAbiErr 'ABI error set'

$sdkErrNames = @($errors | Where-Object { $_.Name -ne 'ESRCH' -and $_.Name -ne 'EINTR' -and $_.Name -ne 'ECHILD' } |
    ForEach-Object { 'ASTER_' + $_.Name })
Compare-KeySets (Keys-With-Prefix $sdk 'ASTER_E') $sdkErrNames 'SDK error set'

foreach ($e in $errors) {
    $a = Val ('ASTER_ABI_' + $e.Name) $abi $abiCache
    if ($a -ne $e.Val) {
        Fail ("ABI error {0}={1}, frozen table says {2}" -f $e.Name, $a, $e.Val)
    }
    if ($e.Name -ne 'ESRCH' -and $e.Name -ne 'EINTR' -and $e.Name -ne 'ECHILD') {
        $d = Val ('ASTER_' + $e.Name) $sdk $sdkCache
        if ($d -ne $e.Val) {
            Fail ("SDK error {0}={1}, ABI says {2}" -f $e.Name, $d, $e.Val)
        }
    }
}

$aliases = @{
    EPERM = @('HANDLE_EPERM', 'IPC_EPERM', 'PROC_EPERM', 'SHM_EPERM', 'VFS_EPERM', 'FBMAP_EPERM')
    ENOENT = @('HANDLE_ENOENT', 'IPC_ENOENT', 'PROC_ENOENT', 'SHM_ENOENT', 'VFS_ENOENT', 'AUTH_ENOENT', 'PKG_ENOENT')
    ESRCH = @('PROC_ESRCH')
    EINTR = @('PROC_EINTR')
    EIO = @('VFS_EIO', 'PKG_EIO')
    EBADF = @('HANDLE_EBADF', 'IPC_EBADF', 'SHM_EBADF', 'VFS_EBADF')
    ECHILD = @('PROC_ECHILD')
    EAGAIN = @('IPC_EAGAIN')
    ENOMEM = @('HANDLE_ENOMEM', 'IPC_ENOMEM', 'PROC_ENOMEM', 'SHM_ENOMEM', 'VFS_ENOMEM', 'FBMAP_ENOMEM')
    EACCES = @('AUTH_EACCES')
    EFAULT = @('UACCESS_EFAULT', 'IPC_EFAULT', 'PROC_EFAULT', 'SHM_EFAULT', 'VFS_EFAULT', 'FBMAP_EFAULT')
    EBUSY = @('PROC_EBUSY', 'SHM_EBUSY', 'VFS_EBUSY', 'FBMAP_EBUSY')
    EEXIST = @('VFS_EEXIST')
    ENODEV = @('FBMAP_ENODEV')
    ENOTDIR = @('VFS_ENOTDIR')
    EISDIR = @('VFS_EISDIR')
    EINVAL = @('HANDLE_EINVAL', 'IPC_EINVAL', 'PROC_EINVAL', 'SHM_EINVAL', 'VFS_EINVAL', 'AUTH_EINVAL', 'PKG_EINVAL', 'LOG_EINVAL', 'FBMAP_EINVAL')
    ENFILE = @('VFS_ENFILE')
    EMFILE = @('VFS_EMFILE')
    ENOSPC = @('HANDLE_ENOSPC', 'IPC_ENOSPC', 'PROC_ENOSPC', 'SHM_ENOSPC', 'VFS_ENOSPC', 'AUTH_ENOSPC', 'PKG_ENOSPC', 'LOG_ENOSPC')
    EROFS = @('VFS_EROFS')
    ENAMETOOLONG = @('PROC_ENAMETOOLONG', 'VFS_ENAMETOOLONG', 'LOG_ENAMETOOLONG', 'PKG_ENAMETOOLONG')
    ENOSYS = @()
    ENOTEMPTY = @('VFS_ENOTEMPTY')
    ELOOP = @('VFS_ELOOP')
    EMSGSIZE = @('IPC_EMSGSIZE')
    ETIMEDOUT = @('IPC_ETIMEDOUT')
    ECANCELED = @('IPC_ECANCELED')
}

$aliasChecks = 0
foreach ($e in $errors) {
    $list = $aliases[$e.Name]
    foreach ($m in $list) {
        $v = Val $m $kernel $kernelCache
        if ($v -ne $e.Val) {
            Fail ("kernel alias {0}={1}, ABI {2}={3}" -f $m, $v, $e.Name, $e.Val)
        }
        $aliasChecks++
    }
}
Write-Host ("[ok] {0} error codes frozen; SDK mirrors {1}; {2} kernel aliases agree" -f `
    $errors.Count, $sdkErrNames.Count, $aliasChecks) -ForegroundColor DarkGreen

# -----------------------------------------------------------------------------
# 3. 关键常量（ABI 头 vs 内核宏）
# -----------------------------------------------------------------------------

$constants = @(
    @{ Abi = 'ASTER_ABI_HANDLE_SLOT_MASK'; Src = @('HANDLE_SLOT_MASK') },
    @{ Abi = 'ASTER_ABI_HANDLE_GEN_SHIFT'; Src = @('HANDLE_GEN_SHIFT') },
    @{ Abi = 'ASTER_ABI_HANDLE_MAX'; Src = @('HANDLE_MAX') },
    @{ Abi = 'ASTER_ABI_FD_MAX'; Src = @('VFS_FD_MAX') },
    @{ Abi = 'ASTER_ABI_PORT_MAX'; Src = @('IPC_PORT_MAX') },
    @{ Abi = 'ASTER_ABI_SHM_MAX'; Src = @('SHM_MAX') },

    @{ Abi = 'ASTER_ABI_IPC_PAYLOAD_MAX'; Src = @('IPC_PAYLOAD_MAX') },
    @{ Abi = 'ASTER_ABI_IPC_MSG_MAX'; Src = @('IPC_MSG_MAX') },
    @{ Abi = 'ASTER_ABI_IPC_DEPTH_DEFAULT'; Src = @('IPC_DEPTH_DEFAULT') },
    @{ Abi = 'ASTER_ABI_IPC_DEPTH_MAX'; Src = @('IPC_DEPTH_MAX') },
    @{ Abi = 'ASTER_ABI_TIMEOUT_NOW'; Src = @('IPC_TIMEOUT_NOW') },
    @{ Abi = 'ASTER_ABI_TIMEOUT_FOREVER'; Src = @('IPC_TIMEOUT_FOREVER') },

    @{ Abi = 'ASTER_ABI_CONSOLE_MAX'; Src = @('USER_WRITE_MAX') },
    @{ Abi = 'ASTER_ABI_FD_IO_MAX'; Src = @('FS_SYS_IO_MAX') },
    @{ Abi = 'ASTER_ABI_LISTDIR_MAX'; Src = @('FS_SYS_LISTDIR_MAX') },
    @{ Abi = 'ASTER_ABI_VFS_PATH_MAX'; Src = @('VFS_PATH_MAX') },
    @{ Abi = 'ASTER_ABI_VFS_NAME_MAX'; Src = @('VFS_NAME_MAX') },
    @{ Abi = 'ASTER_ABI_VFS_MOUNT_MAX'; Src = @('VFS_MOUNT_MAX') },
    @{ Abi = 'ASTER_ABI_VFS_SYMLINK_MAX'; Src = @('VFS_SYMLINK_MAX') },

    @{ Abi = 'ASTER_ABI_O_RDONLY'; Src = @('VFS_O_RDONLY') },
    @{ Abi = 'ASTER_ABI_O_WRONLY'; Src = @('VFS_O_WRONLY') },
    @{ Abi = 'ASTER_ABI_O_RDWR'; Src = @('VFS_O_RDWR') },
    @{ Abi = 'ASTER_ABI_O_ACCMODE'; Src = @('VFS_O_ACCMODE') },
    @{ Abi = 'ASTER_ABI_O_CREAT'; Src = @('VFS_O_CREAT') },
    @{ Abi = 'ASTER_ABI_O_EXCL'; Src = @('VFS_O_EXCL') },
    @{ Abi = 'ASTER_ABI_O_TRUNC'; Src = @('VFS_O_TRUNC') },
    @{ Abi = 'ASTER_ABI_O_APPEND'; Src = @('VFS_O_APPEND') },
    @{ Abi = 'ASTER_ABI_O_DIRECTORY'; Src = @('VFS_O_DIRECTORY') },
    @{ Abi = 'ASTER_ABI_O_FLAGS_MASK'; Src = @('VFS_O_FLAGS_MASK') },

    @{ Abi = 'ASTER_ABI_S_IFREG'; Src = @('VFS_S_IFREG') },
    @{ Abi = 'ASTER_ABI_S_IFDIR'; Src = @('VFS_S_IFDIR') },
    @{ Abi = 'ASTER_ABI_S_IFLNK'; Src = @('VFS_S_IFLNK') },
    @{ Abi = 'ASTER_ABI_S_IFMT'; Src = @('VFS_S_IFMT') },

    @{ Abi = 'ASTER_ABI_KEY_F_SHIFT'; Src = @('INPUT_F_SHIFT') },
    @{ Abi = 'ASTER_ABI_KEY_F_CAPS'; Src = @('INPUT_F_CAPS') },
    @{ Abi = 'ASTER_ABI_KEY_F_CTRL'; Src = @('INPUT_F_CTRL') },
    @{ Abi = 'ASTER_ABI_KEY_F_EXT'; Src = @('INPUT_F_EXT') },

    @{ Abi = 'ASTER_ABI_LOGIN_NAME_MAX'; Src = @('AUTH_NAME_MAX') },
    @{ Abi = 'ASTER_ABI_LOGIN_PASS_MAX'; Src = @('AUTH_PASS_MAX') },
    @{ Abi = 'ASTER_ABI_AUTH_MAX_USERS'; Src = @('AUTH_MAX_USERS') },
    @{ Abi = 'ASTER_ABI_AUTH_ITERATIONS'; Src = @('AUTH_ITERATIONS') },
    @{ Abi = 'ASTER_ABI_AUTH_MAX_FAILS'; Src = @('AUTH_MAX_FAILS') },
    @{ Abi = 'ASTER_ABI_LOG_TAG_MAX'; Src = @('LOG_TAG_MAX') },
    @{ Abi = 'ASTER_ABI_LOG_MSG_MAX'; Src = @('LOG_MSG_MAX') },
    @{ Abi = 'ASTER_ABI_LOG_CAPACITY'; Src = @('LOG_CAPACITY') },
    @{ Abi = 'ASTER_ABI_DMESG_MAX'; Src = @('LOG_SYS_READ_MAX') },
    @{ Abi = 'ASTER_ABI_PKG_NAME_MAX'; Src = @('PKG_NAME_MAX') },
    @{ Abi = 'ASTER_ABI_PKG_LIST_MAX'; Src = @('SVC_PKG_LIST_MAX') },
    @{ Abi = 'ASTER_ABI_PKG_MAX_PACKAGES'; Src = @('PKG_MAX_PACKAGES') },

    @{ Abi = 'ASTER_ABI_PROC_MAX'; Src = @('PROC_MAX') },
    @{ Abi = 'ASTER_ABI_SPAWN_PATH_MAX'; Src = @('PROC_PATH_MAX') },
    @{ Abi = 'ASTER_ABI_SPAWN_ARGS_MAX'; Src = @('PROC_MAX_ARGS') },
    @{ Abi = 'ASTER_ABI_SPAWN_ARG_MAX'; Src = @('PROC_ARG_MAX') },
    @{ Abi = 'ASTER_ABI_SPAWN_ELF_MAX'; Src = @('PROC_ELF_MAX') },
    @{ Abi = 'ASTER_ABI_PROC_STACK_PAGES'; Src = @('PROC_STACK_PAGES') },
    @{ Abi = 'ASTER_ABI_PROC_MARKER_VA'; Src = @('PROC_MARKER_VA') },
    @{ Abi = 'ASTER_ABI_PROC_STATUS_KILL_FLAG'; Src = @('PROC_STATUS_KILL_FLAG') },
    @{ Abi = 'ASTER_ABI_USER_VA_MAX'; Src = @('UACCESS_USER_VA_MAX') },

    @{ Abi = 'ASTER_ABI_SHM_PAGES_MAX'; Src = @('SHM_PAGES_MAX') },
    @{ Abi = 'ASTER_ABI_SHM_MAPS_MAX'; Src = @('SHM_MAPS_MAX') },
    @{ Abi = 'ASTER_ABI_SHM_PAGE_SIZE'; Src = @('SHM_PAGE_SIZE') },
    @{ Abi = 'ASTER_ABI_SHM_VA_BASE'; Src = @('SHM_VA_BASE') },
    @{ Abi = 'ASTER_ABI_SHM_VA_LIMIT'; Src = @('SHM_VA_LIMIT') },
    @{ Abi = 'ASTER_ABI_FB_VA_BASE'; Src = @('FBMAP_VA_BASE') }
)

$constChecks = 0
foreach ($c in $constants) {
    $a = Val $c.Abi $abi $abiCache
    foreach ($m in $c.Src) {
        $v = Val $m $kernel $kernelCache
        if ($v -ne $a) {
            Fail ("constant {0}={1}, ABI {2}={3}" -f $m, $v, $c.Abi, $a)
        }
        $constChecks++
    }
}
Write-Host ("[ok] {0} ABI constants match {1} kernel-source checks" -f `
    $constants.Count, $constChecks) -ForegroundColor DarkGreen

# -----------------------------------------------------------------------------
# 4. 结构体布局（按字段自然对齐计算，不依赖编译器）
# -----------------------------------------------------------------------------

function Get-StructSize([string]$relPath, [string]$structName) {
    $p = Join-Path $root $relPath
    if (-not (Test-Path -LiteralPath $p)) { Fail ("missing struct source: {0}" -f $relPath) }
    $text = [IO.File]::ReadAllText($p)
    $pattern = 'struct\s+' + [regex]::Escape($structName) + '\s*\{([^}]*)\}'
    $m = [regex]::Match($text, $pattern, 'Singleline')
    if (-not $m.Success) { Fail ("struct {0} not found in {1}" -f $structName, $relPath) }
    $body = $m.Groups[1].Value
    $body = [regex]::Replace($body, '/\*.*?\*/', '', 'Singleline')
    $body = [regex]::Replace($body, '//[^\r\n]*', '')

    $sizes = @{ u8 = 1; u16 = 2; u32 = 4; u64 = 8; char = 1 }
    $offset = 0
    $maxAlign = 1
    foreach ($decl in ($body -split ';')) {
        $d = $decl.Trim()
        if ($d.Length -eq 0) { continue }
        $fm = [regex]::Match($d, '^(u8|u16|u32|u64|char)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[\s*([A-Za-z_0-9]+)\s*\])?$')
        if (-not $fm.Success) {
            Fail ("unsupported field '{0}' in struct {1}" -f $d, $structName)
        }
        $type = $fm.Groups[1].Value
        $count = 1
        if ($fm.Groups[3].Value -ne '') {
            $tok = $fm.Groups[3].Value
            if ($tok -match '^\d+$') {
                $count = [int]$tok
            } else {
                $count = [int](Val $tok $kernel $kernelCache)
            }
        }
        $sz = $sizes[$type] * $count
        $align = $sizes[$type]
        if ($align -gt $maxAlign) { $maxAlign = $align }
        $offset = [int]([math]::Ceiling($offset / [double]$align) * $align)
        $offset += $sz
    }
    $size = [int]([math]::Ceiling($offset / [double]$maxAlign) * $maxAlign)
    return $size
}

$structs = @(
    @{ Abi = 'ASTER_ABI_STRUCT_VFS_STAT_SIZE'; File = 'src/services/fs/include/vfs.h'; Struct = 'vfs_stat' },
    @{ Abi = 'ASTER_ABI_STRUCT_VFS_DIRENT_SIZE'; File = 'src/services/fs/include/vfs.h'; Struct = 'vfs_dirent' },
    @{ Abi = 'ASTER_ABI_STRUCT_IPC_MSG_HDR_SIZE'; File = 'src/ng/ipc.h'; Struct = 'ipc_msg_hdr' },
    @{ Abi = 'ASTER_ABI_STRUCT_DS_FB_INFO_SIZE'; File = 'src/desktop/include/ds_protocol.h'; Struct = 'ds_fb_info' }
)
$structParts = @()
foreach ($s in $structs) {
    $actual = Get-StructSize $s.File $s.Struct
    $a = Val $s.Abi $abi $abiCache
    if ($actual -ne $a) {
        Fail ("struct {0} size {1}, ABI {2}={3}" -f $s.Struct, $actual, $s.Abi, $a)
    }
    $structParts += ("{0}={1}" -f $s.Struct, $actual)
}
Write-Host ("[ok] struct layouts: {0}" -f ($structParts -join ', ')) -ForegroundColor DarkGreen

# -----------------------------------------------------------------------------
# 5. 文档标记（冻结）
# -----------------------------------------------------------------------------

$doc = [IO.File]::ReadAllText($files.AbiDoc)
if ($doc -notmatch '状态：\*\*冻结' -or $doc -notmatch 'ABI 1\.0') {
    Fail 'docs/abi/abi-1.0.md is not marked as frozen ABI 1.0'
}
if ($doc -notmatch 'abi-1\.1\.md') {
    Fail 'docs/abi/abi-1.0.md must link successor version abi-1.1.md'
}
Write-Host '[ok] docs/abi/abi-1.0.md is marked frozen and links abi-1.1.md' -ForegroundColor DarkGreen

# -----------------------------------------------------------------------------
# 6. 版本
# -----------------------------------------------------------------------------

$major = Val 'ASTER_ABI_VERSION_MAJOR' $abi $abiCache
$minor = Val 'ASTER_ABI_VERSION_MINOR' $abi $abiCache
$patch = Val 'ASTER_ABI_VERSION_PATCH' $abi $abiCache
$version = Val 'ASTER_ABI_VERSION' $abi $abiCache
if ($major -ne 1 -or $minor -ne 0 -or $patch -ne 0) {
    Fail ("ABI version {0}.{1}.{2}, expected 1.0.0" -f $major, $minor, $patch)
}
if ($version -ne 0x00010000) {
    Fail ("ASTER_ABI_VERSION=0x{0:x8}, expected 0x00010000" -f $version)
}
Write-Host ("[ok] ABI version {0}.{1}.{2} (0x{3:x8})" -f $major, $minor, $patch, $version) -ForegroundColor DarkGreen

# -----------------------------------------------------------------------------
# 7. ABI 1.1（只追加）：版本 1.1.0、号段 48–95、ENOSYS(-38)
# -----------------------------------------------------------------------------

$abi11Text = [IO.File]::ReadAllText($files.Abi11)
if ($abi11Text -notmatch '#include\s+"abi-1\.0\.h"') {
    Fail 'docs/abi/abi-1.1.h must #include "abi-1.0.h" (1.1 = 1.0 + delta)'
}

$syscalls11 = @(
    @{ Name = 'FSTAT'; Nr = 48 },
    @{ Name = 'LSEEK'; Nr = 49 },
    @{ Name = 'DUP'; Nr = 50 },
    @{ Name = 'DUP2'; Nr = 51 },
    @{ Name = 'PIPE'; Nr = 52 },
    @{ Name = 'CHDIR'; Nr = 53 },
    @{ Name = 'GETCWD'; Nr = 54 },
    @{ Name = 'GETUID'; Nr = 55 },
    @{ Name = 'WAITPID'; Nr = 56 },
    @{ Name = 'EXECVE'; Nr = 57 },
    @{ Name = 'READLINK'; Nr = 58 },
    @{ Name = 'RENAME'; Nr = 59 },
    @{ Name = 'RMDIR'; Nr = 60 },
    @{ Name = 'FCNTL'; Nr = 61 },
    @{ Name = 'GETDENTS'; Nr = 62 },
    @{ Name = 'BRK'; Nr = 63 },
    @{ Name = 'MMAP'; Nr = 64 },
    @{ Name = 'MUNMAP'; Nr = 65 },
    @{ Name = 'MPROTECT'; Nr = 66 },
    @{ Name = 'CLOCK_GETTIME'; Nr = 67 },
    @{ Name = 'NANOSLEEP'; Nr = 68 },
    @{ Name = 'GETRANDOM'; Nr = 69 },
    @{ Name = 'SCHED_YIELD'; Nr = 70 },
    @{ Name = 'RT_SIGACTION'; Nr = 71 },
    @{ Name = 'RT_SIGPROCMASK'; Nr = 72 },
    @{ Name = 'RT_SIGRETURN'; Nr = 73 },
    @{ Name = 'SIGNAL_SEND'; Nr = 74 },
    @{ Name = 'POLL'; Nr = 75 },
    @{ Name = 'SOCKET'; Nr = 76 },
    @{ Name = 'BIND'; Nr = 77 },
    @{ Name = 'CONNECT'; Nr = 78 },
    @{ Name = 'LISTEN'; Nr = 79 },
    @{ Name = 'ACCEPT'; Nr = 80 },
    @{ Name = 'SENDTO'; Nr = 81 },
    @{ Name = 'RECVFROM'; Nr = 82 },
    @{ Name = 'SETSOCKOPT'; Nr = 83 },
    @{ Name = 'GETSOCKOPT'; Nr = 84 },
    @{ Name = 'SHUTDOWN'; Nr = 85 },
    @{ Name = 'GETPEERNAME'; Nr = 86 },
    @{ Name = 'GETSOCKNAME'; Nr = 87 },
    @{ Name = 'CLONE'; Nr = 88 },
    @{ Name = 'FUTEX'; Nr = 89 },
    @{ Name = 'SET_TID_ADDRESS'; Nr = 90 },
    @{ Name = 'ARCH_PRCTL'; Nr = 91 },
    @{ Name = 'EXIT_THREAD'; Nr = 92 },
    @{ Name = 'GETTID'; Nr = 93 },
    @{ Name = 'SHM_FREE'; Nr = 94 },
    @{ Name = 'SPAWN_EX'; Nr = 95 }
)

$expectedAbi11 = @($syscalls11 | ForEach-Object { 'ASTER_ABI_SYS_' + $_.Name })
Compare-KeySets (Keys-With-Prefix $abi11 'ASTER_ABI_SYS_') $expectedAbi11 'ABI 1.1 syscall set'

# 1.1 号段完整：48..95 每个号都有且只有一个名字，且不与 1.0 的 29 个号冲突。
$seen11 = @{}
foreach ($s in $syscalls) { $seen11[$s.Nr] = $s.Name }
foreach ($s in $syscalls11) {
    if ($seen11.ContainsKey($s.Nr)) {
        Fail ("1.1 syscall {0}={1} collides with frozen 1.0 name {2}" -f `
            $s.Name, $s.Nr, $seen11[$s.Nr])
    }
    $seen11[$s.Nr] = $s.Name
    $a = Val ('ASTER_ABI_SYS_' + $s.Name) $abi11All $abi11Cache
    if ($a -ne $s.Nr) {
        Fail ("ABI 1.1 syscall {0}={1}, frozen table says {2}" -f $s.Name, $a, $s.Nr)
    }
}
if ($syscalls11.Count -ne 48) {
    Fail ("ABI 1.1 syscall count {0}, expected 48 (48..95)" -f $syscalls11.Count)
}
for ($n = 48; $n -le 95; $n++) {
    if (-not $seen11.ContainsKey($n)) {
        Fail ("ABI 1.1 number {0} is neither assigned nor declared reserved" -f $n)
    }
}
$lastNr = Val 'ASTER_ABI_LAST_ASSIGNED_NR' $abi11All $abi11Cache
$nextNr = Val 'ASTER_ABI_NEXT_FREE_NR' $abi11All $abi11Cache
if ($lastNr -ne 95 -or $nextNr -ne 96) {
    Fail ("ABI 1.1 number bounds last={0} next={1}, expected 95/96" -f $lastNr, $nextNr)
}

# 1.0 的 29 个号经 1.1 头（#include abi-1.0.h）必须解析成同样的值。
foreach ($s in $syscalls) {
    $a = Val ('ASTER_ABI_SYS_' + $s.Name) $abi11All $abi11Cache
    if ($a -ne $s.Nr) {
        Fail ("ABI 1.1 inherits syscall {0}={1}, frozen 1.0 says {2}" -f $s.Name, $a, $s.Nr)
    }
}
Write-Host ("[ok] 1.1: {0} syscalls ({1} legacy + {2} new 48-95), number segment frozen" -f `
    ($syscalls.Count + $syscalls11.Count), $syscalls.Count, $syscalls11.Count) -ForegroundColor DarkGreen

# 版本 1.1.0
$major11 = Val 'ASTER_ABI_VERSION_MAJOR' $abi11All $abi11Cache
$minor11 = Val 'ASTER_ABI_VERSION_MINOR' $abi11All $abi11Cache
$patch11 = Val 'ASTER_ABI_VERSION_PATCH' $abi11All $abi11Cache
$version11 = Val 'ASTER_ABI_VERSION' $abi11All $abi11Cache
if ($major11 -ne 1 -or $minor11 -ne 1 -or $patch11 -ne 0) {
    Fail ("ABI 1.1 version {0}.{1}.{2}, expected 1.1.0" -f $major11, $minor11, $patch11)
}
if ($version11 -ne 0x00010100) {
    Fail ("ASTER_ABI_VERSION(1.1)=0x{0:x8}, expected 0x00010100" -f $version11)
}
Write-Host ("[ok] 1.1: version {0}.{1}.{2} (0x{3:x8})" -f `
    $major11, $minor11, $patch11, $version11) -ForegroundColor DarkGreen

# ENOSYS：1.0 的 -38 保留值不变；1.1 用它表示未分配/未实现号。
$enosys10 = Val 'ASTER_ABI_ENOSYS' $abi $abiCache
$enosys11 = Val 'ASTER_ABI_UNALLOCATED_ERR' $abi11All $abi11Cache
if ($enosys10 -ne -38) {
    Fail ("ABI 1.0 ENOSYS={0}, expected -38" -f $enosys10)
}
if ($enosys11 -ne -38) {
    Fail ("ABI 1.1 unallocated errno={0}, expected -38 (ENOSYS)" -f $enosys11)
}

# 内核 default 必须返回 NG_SYS_ENOSYS（不是 -1）。
$srcSyscallC = [IO.File]::ReadAllText((Join-Path $root 'src/ng/syscall.c'))
if ($srcSyscallC -notmatch '(?s)default:.{0,800}?NG_SYS_ENOSYS') {
    Fail 'src/ng/syscall.c default branch does not return NG_SYS_ENOSYS (-38)'
}
if ($srcSyscallC -match 'f->rax\s*=\s*\(u64\)-1;') {
    Fail 'src/ng/syscall.c still returns (u64)-1 (must be NG_SYS_ENOSYS)'
}
Write-Host '[ok] 1.1: unallocated/unimplemented -> ENOSYS(-38); kernel default returns NG_SYS_ENOSYS' -ForegroundColor DarkGreen

# 1.1 文本：草案 + 只追加 + ENOSYS/号段说明。
$doc11 = [IO.File]::ReadAllText($files.Abi11Doc)
if ($doc11 -notmatch '状态：\*\*草案' -or $doc11 -notmatch 'ABI 1\.1') {
    Fail 'docs/abi/abi-1.1.md is not marked as DRAFT ABI 1.1'
}
if ($doc11 -notmatch 'ENOSYS' -or $doc11 -notmatch '48–95') {
    Fail 'docs/abi/abi-1.1.md must document 48-95 and ENOSYS'
}
Write-Host '[ok] docs/abi/abi-1.1.md is marked DRAFT (48-95 + ENOSYS)' -ForegroundColor DarkGreen

Write-Host ("abi-check: PASS ({0}+{1} syscalls, {2} errors, {3} constants, {4} struct sizes)" -f `
    $syscalls.Count, $syscalls11.Count, $errors.Count, $constants.Count, $structs.Count) -ForegroundColor Green
exit 0
