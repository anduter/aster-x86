# =============================================================================
# unit.ps1 - host 侧单元测试（不启动 QEMU、不需要 i686-elf 工具链）
#
# 把"纯逻辑"从整机回归里拆出来：路径/ABI/字符串/环形缓冲/manifest 这类
# 不依赖硬件与调度的代码，用宿主编译器直接编译真实内核源文件并断言。
#
# 设计要点：
#   - 用 tools/w64devkit/bin/gcc.exe（或 PATH 上的 gcc）；不依赖 i686-elf。
#   - 内核符号在 host 上与 libc 冲突（memcpy/strcmp...），用 -D 改名，
#     测试 TU 提供最小实现；不修改内核源码。
#   - 依赖内核头的用例通过 tests/unit/shim 提供最小接口（自旋锁/阻塞原语退化为 no-op）。
#   - 产物放 obj/unit/（已 gitignore），失败退出码 1。
#
# 用法：
#   powershell -NoProfile -File tools/unit.ps1
#   powershell -NoProfile -File tools/unit.ps1 -Only trust,pipe
#   powershell -NoProfile -File tools/unit.ps1 -Gcc C:\msys64\mingw64\bin\gcc.exe
#
# 退出码：0 = 全部 PASS，1 = 编译失败或断言失败
# =============================================================================
param(
    [string]$Repo = '',
    [string]$Gcc = '',
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path (Join-Path $Repo 'build.ps1'))) { throw "unit: $Repo 不像仓库根（找不到 build.ps1）" }
$root = (Resolve-Path $Repo).Path
$startDir = (Get-Location).Path
Set-Location $root

# ---- 找宿主编译器 ----
if (-not $Gcc) {
    $vendored = Join-Path $root 'tools/w64devkit/bin/gcc.exe'
    if (Test-Path $vendored) {
        $Gcc = $vendored
    } else {
        $cmd = Get-Command gcc -ErrorAction SilentlyContinue
        if ($cmd) { $Gcc = $cmd.Source }
    }
}
if (-not $Gcc -or -not (Test-Path $Gcc)) {
    throw 'unit: 找不到 host gcc（期望 tools/w64devkit/bin/gcc.exe 或 PATH 上的 gcc）'
}

$outDir = Join-Path $root 'obj/unit'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# 每个用例：真实内核源文件 + 测试 TU；Defines 负责符号改名/BOOT_VERIFY
$tests = @(
    @{
        Name    = 'abi'
        Sources = @('tests/unit/test_abi.c')
        Defines = @()
        Inc     = @()
    },
    @{
        Name    = 'string'
        Sources = @('src/kernel/lib/string.c', 'tests/unit/test_string.c')
        Defines = @(
            'memcpy=k_memcpy', 'memset=k_memset', 'memmove=k_memmove',
            'memcmp=k_memcmp', 'strlen=k_strlen', 'strcmp=k_strcmp',
            'strncmp=k_strncmp', 'strcpy=k_strcpy', 'strncpy=k_strncpy',
            'strchr=k_strchr', 'utoa=k_utoa', 'itoa=k_itoa'
        )
        Inc     = @()
    },
    @{
        Name    = 'trust'
        Sources = @('src/kernel/security/trust.c', 'tests/unit/test_trust.c')
        Defines = @(
            'BOOT_VERIFY',
            'memcpy=unit_memcpy', 'memset=unit_memset', 'memcmp=unit_memcmp'
        )
        Inc     = @()
    },
    @{
        Name    = 'pipe'
        Sources = @('src/kernel/fs/pipe.c', 'tests/unit/test_pipe.c')
        Defines = @('memset=unit_memset')
        Inc     = @('tests/unit/shim')
    },
    @{
        Name    = 'service'
        Sources = @('src/kernel/svc/service.c', 'tests/unit/test_service.c')
        Defines = @(
            'strcmp=k_strcmp', 'strlen=k_strlen', 'strncpy=k_strncpy',
            'memset=k_memset', 'kprintf=unit_kprintf'
        )
        Inc     = @('tests/unit/shim')
    }
)

if ($Only.Count -gt 0) {
    $want = @($Only | ForEach-Object { $_ -split '[,;]' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $tests = @($tests | Where-Object { $want -contains $_.Name })
    if ($tests.Count -eq 0) { throw ("unit: -Only 没有匹配的用例（可选：{0}）" -f (($tests | ForEach-Object { $_.Name }) -join ', ')) }
}

$common = @('-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-builtin', '-Wno-unused-parameter')

Write-Host ''
Write-Host '=== unit (host) ===' -ForegroundColor Cyan
Write-Host ("  repo : {0}" -f $root) -ForegroundColor DarkGray
Write-Host ("  gcc  : {0}" -f $Gcc) -ForegroundColor DarkGray
$gccVer = (& $Gcc --version 2>&1 | Select-Object -First 1)
Write-Host ("  ver  : {0}" -f $gccVer) -ForegroundColor DarkGray
$swAll = [Diagnostics.Stopwatch]::StartNew()

$failed = New-Object System.Collections.Generic.List[string]
$passed = 0
foreach ($t in $tests) {
    $exe = Join-Path $outDir ('test-' + $t.Name + '.exe')
    $args = @($common)
    foreach ($d in $t.Defines) { $args += ('-D' + $d) }
    foreach ($i in $t.Inc) { $args += ('-I' + $i) }
    $args += '-o', $exe
    $args += $t.Sources

    Write-Host ''
    Write-Host ("--- {0} ---" -f $t.Name) -ForegroundColor Cyan
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $compile = (& $Gcc @args 2>&1 | Out-String)
        $compileCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
    }
    if ($compileCode -ne 0) {
        Write-Host ("  [FAIL] compile ({0})" -f $t.Name) -ForegroundColor Red
        foreach ($line in ($compile -split "`r?`n")) { if ($line.Trim()) { Write-Host ("         {0}" -f $line) -ForegroundColor DarkGray } }
        $failed.Add(('{0}: compile' -f $t.Name))
        continue
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $run = (& $exe 2>&1 | Out-String)
        $runCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
        $sw.Stop()
    }

    foreach ($line in ($run -split "`r?`n")) {
        if ($line.Trim()) { Write-Host ("         {0}" -f $line.TrimEnd()) -ForegroundColor DarkGray }
    }
    $m = [regex]::Match($run, '(\d+) checks, (\d+) failures')
    $ok = ($runCode -eq 0 -and $m.Success -and [int]$m.Groups[2].Value -eq 0 -and [int]$m.Groups[1].Value -gt 0)
    if ($ok) {
        Write-Host ("  [PASS] {0}  ({1} checks, {2:N0} ms)" -f $t.Name, $m.Groups[1].Value, $sw.Elapsed.TotalMilliseconds) -ForegroundColor Green
        $passed++
    } else {
        Write-Host ("  [FAIL] {0}  (exit={1})" -f $t.Name, $runCode) -ForegroundColor Red
        $failed.Add(('{0}: assertions' -f $t.Name))
    }
}

$swAll.Stop()
Write-Host ''
Write-Host ("elapsed: {0:N1}s" -f $swAll.Elapsed.TotalSeconds) -ForegroundColor DarkGray
if ($failed.Count -eq 0) {
    Write-Host ("=== UNIT PASS ({0}/{0}) ===" -f $passed) -ForegroundColor Green
    Set-Location $startDir
    exit 0
} else {
    Write-Host '--- failures ---' -ForegroundColor Red
    foreach ($f in $failed) { Write-Host ("  * {0}" -f $f) -ForegroundColor Red }
    Write-Host ("=== UNIT FAIL ({0}/{1}) ===" -f $failed.Count, $tests.Count) -ForegroundColor Red
    Set-Location $startDir
    exit 1
}
