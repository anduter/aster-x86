# =============================================================================
# bench.ps1 - 构建/启动基准：内核体积、全量/增量构建耗时、-Test 耗时、启动到 shell ready
#
# 每次运行追加一行到 logs/bench.csv（logs/ 已 gitignore，不污染仓库）：
#   time,git,version,kernel_bytes,image_bytes,full_build_s,incremental_build_s,
#   test_s,checks,boot_ready_s,note
#
# 说明：
#   - 全量构建：先删掉隔离产物目录（-ObjDir/-DistDir，默认 .obj-bench/.dist-bench）
#   - 增量构建：紧接着再构建一次（无源码改动）
#   - 启动耗时：从 build.ps1 -Test 输出的 [t Xs] before qemu 到 [t Ys] boot ready 的差值，
#     即 QEMU 启动 -> "ushell: ready"；不是整个 -Test 的耗时
#   - -SkipTest：不跑 QEMU（也用于别的会话正在浸泡/回归时）
#   - 有 qemu-system-i386 在跑时自动跳过 -Test（不杀别人的 QEMU）
#
# 用法：
#   powershell -NoProfile -File tools/bench.ps1
#   powershell -NoProfile -File tools/bench.ps1 -Runs 3
#   powershell -NoProfile -File tools/bench.ps1 -SkipTest      # 只测构建（无 QEMU）
#   powershell -NoProfile -File tools/bench.ps1 -Csv logs/bench-local.csv
#
# 退出码：0 = 全部完成，1 = 构建/回归失败
# =============================================================================
param(
    [string]$Repo = '',
    [int]$Runs = 1,
    [switch]$SkipTest,
    [string]$ObjDir = '.obj-bench',
    [string]$DistDir = '.dist-bench',
    [int]$QmpPort = 4694,
    [string]$Csv = 'logs/bench.csv'
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path (Join-Path $Repo 'build.ps1'))) { throw "bench: $Repo 不像仓库根（找不到 build.ps1）" }
$root = (Resolve-Path $Repo).Path
$startDir = (Get-Location).Path
Set-Location $root

if ($Runs -lt 1) { throw 'bench: -Runs 必须 >= 1' }
$csvPath = if ([IO.Path]::IsPathRooted($Csv)) { $Csv } else { Join-Path $root $Csv }
$csvDir = Split-Path -Parent $csvPath
if ($csvDir) { New-Item -ItemType Directory -Force -Path $csvDir | Out-Null }

function Invariant([double]$v) {
    return [string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:F2}', $v)
}

function Get-Version {
    $vf = Join-Path $root 'src/kernel/core/version.h'
    if (-not (Test-Path $vf)) { return '?' }
    $m = [regex]::Match([IO.File]::ReadAllText($vf), '#define\s+OS_VERSION\s+"([^"]+)"')
    return $(if ($m.Success) { $m.Groups[1].Value } else { '?' })
}

function Invoke-Build([string[]]$extraArgs, [string]$tag) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') `
            -ObjDir $ObjDir -DistDir $DistDir -QmpPort "$QmpPort" -SerialLog ("bench-{0}-serial.log" -f $tag) `
            @extraArgs 2>&1 | Out-String)
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $savedEap
        $sw.Stop()
    }
    return @{ Code = $code; Out = $out; Seconds = $sw.Elapsed.TotalSeconds }
}

function Remove-BenchArtifacts {
    foreach ($rel in @($ObjDir, $DistDir)) {
        $p = Join-Path $root $rel
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $pAbs = [IO.Path]::GetFullPath($p)
        if (-not $pAbs.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw ("bench: 拒绝删除仓库外路径 {0}" -f $pAbs)
        }
        Remove-Item -LiteralPath $pAbs -Recurse -Force
    }
}

$gccQemu = @(Get-Process qemu-system-i386 -ErrorAction SilentlyContinue)
if ($gccQemu.Count -gt 0 -and -not $SkipTest) {
    Write-Host ("bench: 检测到 {0} 个 qemu-system-i386 正在运行；本次跳过 -Test（不干扰别人的浸泡/回归）" -f $gccQemu.Count) -ForegroundColor Yellow
    $SkipTest = $true
}

if (-not (Test-Path (Join-Path $root 'keys/shell_private.pem'))) {
    Write-Host 'bench: [warn] 缺少 keys/shell_private.pem，构建会失败；先跑 tools/gen-key.ps1' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '=== bench ===' -ForegroundColor Cyan
Write-Host ("  repo  : {0}" -f $root) -ForegroundColor DarkGray
Write-Host ("  obj   : {0}   dist: {1}" -f $ObjDir, $DistDir) -ForegroundColor DarkGray
Write-Host ("  csv   : {0}" -f $csvPath) -ForegroundColor DarkGray
Write-Host ("  qemu  : {0}" -f $(if ($SkipTest) { 'skip (-SkipTest / 已有 QEMU)' } else { 'run once' })) -ForegroundColor DarkGray

if (-not (Test-Path $csvPath)) {
    [IO.File]::WriteAllText($csvPath,
        "time,git,version,kernel_bytes,image_bytes,full_build_s,incremental_build_s,test_s,checks,boot_ready_s,note`n",
        (New-Object Text.UTF8Encoding($false)))
}

$git = (& git rev-parse --short HEAD) -join ''
$version = Get-Version
# 工作区是否干净：发版/对比性能时必须知道测的是 HEAD 还是别人未提交的改动
$dirtyFlag = if (@(& git -c core.quotepath=false status --porcelain).Count -gt 0) { 'dirty' } else { 'clean' }
$exit = 0

for ($run = 1; $run -le $Runs; $run++) {
    Write-Host ''
    Write-Host ("--- run {0}/{1} ---" -f $run, $Runs) -ForegroundColor Cyan
    Remove-BenchArtifacts

    Write-Host '  [1/3] full build ...' -ForegroundColor DarkGray
    $full = Invoke-Build @() 'full'
    if ($full.Code -ne 0) {
        ($full.Out -split "`r?`n" | Select-Object -Last 20) | ForEach-Object { Write-Host ("         {0}" -f $_) -ForegroundColor DarkGray }
        throw ("bench: 全量构建失败（exit={0}）" -f $full.Code)
    }
    $km = [regex]::Match($full.Out, '=== ok: .*\(kernel (\d+) bytes\) ===')
    $kernelBytes = if ($km.Success) { [int64]$km.Groups[1].Value } else { 0 }
    $kernelBin = Join-Path $root ($ObjDir + '/kernel.bin')
    if ($kernelBytes -eq 0 -and (Test-Path $kernelBin)) { $kernelBytes = (Get-Item $kernelBin).Length }
    $imgPath = Join-Path $root ($DistDir + '/floppy.img')
    $imageBytes = if (Test-Path $imgPath) { (Get-Item $imgPath).Length } else { 0 }
    Write-Host ("        ok  kernel={0} bytes  image={1} bytes  {2:N1}s" -f $kernelBytes, $imageBytes, $full.Seconds) -ForegroundColor Green

    Write-Host '  [2/3] incremental build ...' -ForegroundColor DarkGray
    $inc = Invoke-Build @() 'inc'
    if ($inc.Code -ne 0) { throw ("bench: 增量构建失败（exit={0}）" -f $inc.Code) }
    Write-Host ("        ok  {0:N1}s" -f $inc.Seconds) -ForegroundColor Green

    $testSeconds = ''
    $checks = ''
    $bootReady = ''
    $note = $dirtyFlag
    if ($SkipTest) {
        Write-Host '  [3/3] -Test skipped' -ForegroundColor DarkGray
        $note = "no-test,$dirtyFlag"
    } else {
        Write-Host '  [3/3] build.ps1 -Test ...' -ForegroundColor DarkGray
        $test = Invoke-Build @('-Test') 'test'
        if ($test.Code -ne 0) {
            ($test.Out -split "`r?`n" | Select-Object -Last 30) | ForEach-Object { Write-Host ("         {0}" -f $_) -ForegroundColor DarkGray }
            throw ("bench: -Test 失败（exit={0}）" -f $test.Code)
        }
        $tm = [regex]::Match($test.Out, '=== TEST PASS \((\d+)/\1\) ===')
        $checks = if ($tm.Success) { [int]$tm.Groups[1].Value } else { '' }
        $testSeconds = Invariant $test.Seconds
        $beforeQemu = [regex]::Match($test.Out, '\[t\s+([\d.]+)s\]\s+before qemu')
        $bootLine = [regex]::Match($test.Out, '\[t\s+([\d.]+)s\]\s+boot ready')
        if ($beforeQemu.Success -and $bootLine.Success) {
            $bootReady = Invariant ([double]$bootLine.Groups[1].Value - [double]$beforeQemu.Groups[1].Value)
            if ([double]$bootReady -lt 0) { $bootReady = '' }
        }
        Write-Host ("        ok  checks={0}  test={1:N1}s  boot->ready={2}s" -f $checks, $test.Seconds, $bootReady) -ForegroundColor Green
    }

    $fullS = Invariant $full.Seconds
    $incS = Invariant $inc.Seconds
    $row = @((Get-Date -Format 'yyyy-MM-ddTHH:mm:ss'), $git, $version, $kernelBytes, $imageBytes,
             $fullS, $incS, $testSeconds, $checks, $bootReady, $note) -join ','
    [IO.File]::AppendAllText($csvPath, $row + "`n", (New-Object Text.UTF8Encoding($false)))
    Write-Host ("  csv: {0}" -f $row) -ForegroundColor DarkGray
}

Set-Location $startDir
Write-Host ''
Write-Host ("=== BENCH OK ({0} run(s)) ===" -f $Runs) -ForegroundColor Green
exit $exit
