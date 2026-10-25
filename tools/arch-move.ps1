# arch-move.ps1 - move platform primitives into arch/x86 (option B, no types.h move)
#
#   src/kernel/head.asm   -> src/kernel/arch/x86/head.asm
#   src/kernel/io.c|h     -> src/kernel/arch/x86/io.c|h
#   src/kernel/linker.ld  -> src/kernel/arch/x86/linker.ld
#
# types.h stays at src/kernel/types.h (option C is intentionally NOT done).
# Updates build.ps1 / Makefile paths, runs two full tests, then commits.
#
# NOTE: this script is ASCII-only on purpose (PS 5.1 reads non-BOM scripts as
# ANSI; non-ASCII text here would corrupt the parse).
#
# usage: powershell -File tools\arch-move.ps1 [-NoCommit]
param(
    [string]$Repo = 'D:\OS_Project',
    [switch]$NoCommit
)
$ErrorActionPreference = 'Stop'
Set-Location $Repo
$encBom = New-Object Text.UTF8Encoding $true
$encPlain = New-Object Text.UTF8Encoding $false

$dirty = (cmd /c "git status --porcelain -- src 2>nul" | Measure-Object -Line).Lines
if ($dirty -ne 0) { throw 'source tree is not clean; commit/stash changes before running arch-move' }

New-Item -ItemType Directory -Force -Path 'src\kernel\arch\x86' | Out-Null
git mv src/kernel/head.asm     src/kernel/arch/x86/head.asm
git mv src/kernel/io.c         src/kernel/arch/x86/io.c
git mv src/kernel/io.h         src/kernel/arch/x86/io.h
git mv src/kernel/linker.ld    src/kernel/arch/x86/linker.ld
Write-Host '[arch-move] files moved'

# build.ps1: linker path (preserve UTF-8 BOM)
$bp = 'build.ps1'
$t = [IO.File]::ReadAllText($bp)
$t = $t.Replace("'src/kernel/linker.ld'", "'src/kernel/arch/x86/linker.ld'")
[IO.File]::WriteAllText($bp, $t, $encBom)
$b = [IO.File]::ReadAllBytes($bp)
if (-not ($b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)) { throw 'build.ps1 lost its BOM' }

# Makefile: linker path + recursive asm wildcard (arch/x86 is one level deeper)
$mp = 'Makefile'
$m = [IO.File]::ReadAllText($mp)
$m = $m.Replace('-T $(KERNEL_DIR)/linker.ld', '-T $(KERNEL_DIR)/arch/x86/linker.ld')
$asmOld = '                $(wildcard $(KERNEL_DIR)/*/*.asm)'
$asmNew = $asmOld + ' \' + "`r`n" + '                $(wildcard $(KERNEL_DIR)/*/*/*.asm)'
$m = $m.Replace($asmOld, $asmNew)
[IO.File]::WriteAllText($mp, $m, $encPlain)
Write-Host '[arch-move] build scripts updated'

# sanity build
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'build.ps1')
if ($LASTEXITCODE -ne 0) { throw 'build failed after arch move' }
Write-Host '[arch-move] build ok'

# two isolated tests
for ($i = 1; $i -le 2; $i++) {
    $serial = "serial-arch-$i.log"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'build.ps1') -Test `
        -ObjDir '.obj-arch' -DistDir '.dist-arch' -QmpPort 4631 -SerialLog $serial
    if ($LASTEXITCODE -ne 0) { throw "test run $i failed" }
    Write-Host "[arch-move] test run $i PASS"
}

# document the layout change
$dp = 'docs\modules.md'
$d = [IO.File]::ReadAllText($dp)
$noteLines = @(
    '',
    '### arch/x86 platform layer (option B, 2026-10-25)',
    '',
    '- head.asm / io.c / io.h / linker.ld moved to src/kernel/arch/x86/',
    '- types.h stays at src/kernel/types.h (option C not done: wide churn, low value)',
    '- build.ps1 linker path and Makefile asm wildcard updated'
)
$note = $noteLines -join "`n"
if ($d -notmatch 'arch/x86 platform layer') {
    $d = $d.TrimEnd() + "`n" + $note
    [IO.File]::WriteAllText($dp, $d, $encPlain)
}
Write-Host '[arch-move] docs updated'

if ($NoCommit) { Write-Host '[arch-move] -NoCommit: skipping commit'; exit 0 }

$paths = @(
    'src/kernel/arch', 'src/kernel/head.asm', 'src/kernel/io.c', 'src/kernel/io.h',
    'src/kernel/linker.ld', 'build.ps1', 'Makefile', 'docs/modules.md'
)
git add @paths
git commit -m "[refactor] move platform primitives to arch/x86 (head/io/linker.ld); keep types.h at root" -- @paths
Write-Host '[arch-move] committed'
exit 0
