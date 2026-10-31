# check-domains.ps1 - 7-domain dependency checker (docs/design/architecture.md)
#
# 规则（E0 骨架）：
#   L0 内核、L1 系统服务、L1D 设备服务、L2 桌面、L3 应用；A/B 为横切域，允许被任何层依赖。
#   依赖只能向下：L3 -> L2 -> L1 -> L0；L1D 只允许依赖 L0/A/B。
#   同一层允许互相依赖。
#
# 现阶段的目录映射（E0，避免打断 64 位主线）：
#   src/kernel/**  -> L0（旧单体，迁到 src/services、src/drivers 后再收紧）
#   src/services/**-> L1
#   src/drivers/** -> L1D
#   src/desktop/** -> L2
#   src/apps/**、src/user/** -> L3
#   sdk/**         -> A
#   tools/**       -> B
#
# 解析：`#include "x.h"` 先相对文件目录、再相对 src/；解析不到的视为外部头，忽略。
#
# usage: powershell -File tools/check-domains.ps1 [-Repo <root>] [-SelfTest] [-Top 20]
# 退出码：0 = 通过；1 = 有违规/自检失败。
param(
    [string]$Repo = '',
    [switch]$SelfTest,
    [int]$Top = 20
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path
Set-Location $Repo

$rank = @{ 'L0' = 0; 'L1' = 1; 'L1D' = 1; 'L2' = 2; 'L3' = 3 }
$cross = @('A', 'B')

function Get-Domain([string]$rel) {
    if ($rel -match '^src/kernel/') { return 'L0' }      # E0 旧单体
    if ($rel -match '^src/services/') { return 'L1' }
    if ($rel -match '^src/drivers/') { return 'L1D' }
    if ($rel -match '^src/desktop/') { return 'L2' }
    if ($rel -match '^src/apps/') { return 'L3' }
    if ($rel -match '^src/user/') { return 'L3' }
    if ($rel -match '^sdk/') { return 'A' }
    if ($rel -match '^tools/') { return 'B' }
    return ''
}

function Test-Upward([string]$from, [string]$to) {
    if (-not $from -or -not $to) { return $false }
    if ($cross -contains $from -or $cross -contains $to) { return $false }
    if (-not $rank.ContainsKey($from) -or -not $rank.ContainsKey($to)) { return $false }
    if ($from -eq 'L1D' -and $to -ne 'L0') { return $true }
    return ($rank[$to] -gt $rank[$from])
}

if ($SelfTest) {
    $cases = @(
        @{ From = 'L0';  To = 'L1';  Expect = $true  },  # 内核依赖服务 -> 违规
        @{ From = 'L1';  To = 'L0';  Expect = $false },  # 服务依赖内核 -> 允许
        @{ From = 'L2';  To = 'L3';  Expect = $true  },  # 桌面依赖应用 -> 违规
        @{ From = 'L3';  To = 'L2';  Expect = $false },
        @{ From = 'L1D'; To = 'L2';  Expect = $true  },  # 设备服务依赖桌面 -> 违规
        @{ From = 'L1D'; To = 'L0';  Expect = $false },
        @{ From = 'A';   To = 'L3';  Expect = $false },  # 横切域允许
        @{ From = 'L1';  To = 'L1';  Expect = $false }   # 同层允许
    )
    $bad = 0
    foreach ($c in $cases) {
        $got = Test-Upward $c.From $c.To
        if ($got -ne $c.Expect) { $bad++; Write-Host ("SELFTEST FAIL: {0}->{1} expect={2} got={3}" -f $c.From, $c.To, $c.Expect, $got) }
    }
    if ($bad -eq 0) { Write-Host 'check-domains: selftest PASS (8 rule cases)'; exit 0 }
    exit 1
}

function Resolve-Inc([string]$dir, [string]$inc) {
    $p = $dir
    foreach ($seg in ($inc -split '[\\/]')) {
        if ($seg -eq '' -or $seg -eq '.') { continue }
        if ($seg -eq '..') { $p = Split-Path -Parent $p }
        else { $p = Join-Path $p $seg }
    }
    return $p
}

$srcRoot = Join-Path $Repo 'src'
$violations = New-Object System.Collections.Generic.List[string]
$files = Get-ChildItem $srcRoot -Recurse -File -Include *.c,*.h,*.asm -ErrorAction SilentlyContinue
$scanned = 0
$includes = 0

foreach ($f in $files) {
    $rel = ($f.FullName.Substring($Repo.Length + 1) -replace '\\', '/')
    $from = Get-Domain $rel
    if (-not $from -or $from -eq 'A' -or $from -eq 'B') { continue }
    $scanned++
    $lines = Select-String -Path $f.FullName -Pattern '#include\s+"([^"]+)"' -ErrorAction SilentlyContinue
    foreach ($m in $lines) {
        $inc = $m.Matches[0].Groups[1].Value
        $includes++
        $cand = Resolve-Inc $f.DirectoryName $inc
        if (-not (Test-Path -LiteralPath $cand)) {
            $alt = Resolve-Inc $srcRoot $inc
            if (Test-Path -LiteralPath $alt) { $cand = $alt } else { continue }
        }
        $full = (Resolve-Path -LiteralPath $cand).Path
        if (-not $full.StartsWith($Repo, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $trel = ($full.Substring($Repo.Length + 1) -replace '\\', '/')
        $to = Get-Domain $trel
        if (Test-Upward $from $to) {
            $violations.Add(("{0} ({1}) -> {2} ({3})" -f $rel, $from, $trel, $to))
        }
    }
}

Write-Host ("checked: {0} files, {1} quoted includes" -f $scanned, $includes)
if ($violations.Count -eq 0) {
    Write-Host 'domain include violations: 0'
    exit 0
}
Write-Host ("domain include violations: {0}" -f $violations.Count)
$violations | Select-Object -First $Top | ForEach-Object { Write-Host ("  - {0}" -f $_) }
exit 1
