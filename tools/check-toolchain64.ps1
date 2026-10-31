# check-toolchain64.ps1 - WP0-4: 64-bit ELF toolchain probe
#
# 目标：找到一个能产出 **x86_64 ELF** 的 freestanding 编译器，并实际链接一个 hello。
# 候选（按优先级）：
#   1. $env:X86_64_ELF_GCC
#   2. x86_64-elf-gcc / x86_64-unknown-elf-gcc
#   3. clang --target=x86_64-elf -fuse-ld=lld
#   4. 其它 -m64 编译器（只接受 ELF64 输出；PE/COFF 明确拒绝）
#
# 输出：PASS（找到并链接成功）/ BLOCKED（无可用 ELF64 工具链）。
# 退出码：0 = PASS；2 = BLOCKED；1 = 脚本/IO 错误。
#
# usage: powershell -File tools/check-toolchain64.ps1 [-Repo <root>] [-Keep]
param(
    [string]$Repo = '',
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('aster-tc64-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$src = Join-Path $tmp 'hello64.c'
$out = Join-Path $tmp 'hello64.elf'
@'
void _start(void) { for (;;) { __asm__ volatile ("hlt"); } }
'@ | Set-Content -Path $src -Encoding ASCII

function Get-Exe([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

function Test-Elf64([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 20) { return $false }
    if ($b[0] -ne 0x7F -or $b[1] -ne 0x45 -or $b[2] -ne 0x4C -or $b[3] -ne 0x46) { return $false }
    if ($b[4] -ne 2) { return $false }                    # ELFCLASS64
    $machine = [BitConverter]::ToUInt16($b, 18)           # e_machine (little endian)
    return ($machine -eq 62)                              # EM_X86_64
}

$candidates = New-Object System.Collections.Generic.List[object]
if ($env:X86_64_ELF_GCC) { $candidates.Add(@{ Name = 'env:X86_64_ELF_GCC'; Exe = $env:X86_64_ELF_GCC; Args = @() }) }
foreach ($n in 'x86_64-elf-gcc', 'x86_64-unknown-elf-gcc') {
    $e = Get-Exe $n
    if ($e) { $candidates.Add(@{ Name = $n; Exe = $e; Args = @() }) }
}
$clang = Get-Exe 'clang'
if ($clang) { $candidates.Add(@{ Name = 'clang --target=x86_64-elf'; Exe = $clang; Args = @('--target=x86_64-elf', '-fuse-ld=lld') }) }
foreach ($n in 'x86_64-w64-mingw32-gcc', 'gcc') {
    $e = Get-Exe $n
    if ($e) { $candidates.Add(@{ Name = "$n -m64"; Exe = $e; Args = @('-m64') }) }
}

$found = $null
$tried = New-Object System.Collections.Generic.List[string]
foreach ($c in $candidates) {
    if (-not (Test-Path -LiteralPath $c.Exe)) { continue }
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    $argv = @($c.Args) + @('-ffreestanding', '-nostdlib', '-Wl,-e,_start', '-Wl,--build-id=none', '-o', $out, $src)
    $log = (& $c.Exe @argv 2>&1 | Out-String)
    $code = $LASTEXITCODE
    if ($code -eq 0 -and (Test-Elf64 $out)) {
        $found = $c
        break
    }
    $kind = if (Test-Path -LiteralPath $out) {
        $b = [IO.File]::ReadAllBytes($out)
        if ($b.Length -ge 2 -and $b[0] -eq 0x4D -and $b[1] -eq 0x5A) { 'PE/COFF' }
        elseif ($b.Length -ge 4 -and $b[0] -eq 0x7F -and $b[1] -eq 0x45) { 'ELF(非 64)' }
        else { 'unknown' }
    } else { 'no output' }
    $tried.Add(("{0}: exit={1} output={2}" -f $c.Name, $code, $kind))
    if ($log.Trim()) { $tried.Add(("  {0}" -f (($log -split "`r?`n")[0]))) }
}

if ($found) {
    Write-Host ("toolchain64: PASS ({0})" -f $found.Name)
    Write-Host ("  exe: {0}" -f $found.Exe)
    Write-Host ("  elf: {0} bytes, ELF64 x86_64" -f (Get-Item $out).Length)
    if (-not $Keep) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
    exit 0
}

Write-Host 'toolchain64: BLOCKED (no x86_64 ELF compiler)'
foreach ($t in $tried) { Write-Host ("  {0}" -f $t) }
Write-Host ''
Write-Host 'install one of:'
Write-Host '  - x86_64-elf-gcc (binutils+gcc cross toolchain), or'
Write-Host '  - clang + lld with --target=x86_64-elf, or'
Write-Host '  - set X86_64_ELF_GCC=<path to x86_64-elf-gcc>'
if ($Keep) { Write-Host ("  kept: {0}" -f $tmp) } else { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
exit 2
