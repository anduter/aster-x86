# =============================================================================
# tools/audit-fs.ps1 - fs 审计复现驱动（独立构建目录 / QMP 4542 / 串口日志）
#
# 用法：
#   powershell -NoProfile -File tools/audit-fs.ps1 -Build -Unit
#   powershell -NoProfile -File tools/audit-fs.ps1 -Scenarios
#   powershell -NoProfile -File tools/audit-fs.ps1 -Scenario type_none
#   powershell -NoProfile -File tools/audit-fs.ps1 -Repo D:\OS_Project\.wt-audit-fs -Scenarios
#
# 约定：
#   - 构建产物 .obj-audit-fs / .dist-audit-fs（相对 -Repo）；
#   - QEMU QMP 只用 4542，串口写 obj/audit/<scenario>.log；
#   - 不监听 12345，不运行 build.ps1 -Test（避免和并行回归会话抢回显端口）；
#   - 启动成功的判据：串口出现 "login:" 且没有 KERNEL PANIC、没有
#     "fs: cannot create system:/bin/shell"（后者表示 mount 失败后丢了默认卷）。
# =============================================================================
param(
    [string]$Repo = '',
    [switch]$Build,
    [switch]$Unit,
    [switch]$Scenarios,
    [string]$Scenario = '',
    [string]$ObjDir = '.obj-audit-fs',
    [string]$DistDir = '.dist-audit-fs',
    [int]$QmpPort = 4542,
    [int]$TimeoutSec = 90
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }

if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$root = (Resolve-Path $Repo).Path
Set-Location $root

$allScenarios = @(
    'good', 'nlink_mismatch', 'magic_bad', 'version_bad', 'inode_size_bad',
    'empty', 'orphan', 'bad_size', 'bad_data_lba', 'bad_parent_type',
    'type_none', 'dup_root', 'volname_empty'
)

function Find-Qemu {
    $cmd = Get-Command qemu-system-i386 -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return 'qemu-system-i386'
}

function Build-Images {
    $floppy = Join-Path $root "$DistDir/floppy.img"
    $fat = Join-Path $root "$DistDir/fat.img"
    if ((Test-Path $floppy) -and (Test-Path $fat) -and -not $Build) { return }
    Write-Host ("=== build ({0}) ===" -f $root) -ForegroundColor Cyan
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') `
        -ObjDir $ObjDir -DistDir $DistDir -QmpPort $QmpPort -SerialLog 'obj/audit/fs.log'
    if ($LASTEXITCODE -ne 0) { throw 'build.ps1 failed' }
}

function Craft-Image([string]$name) {
    $dir = Join-Path $root 'obj/audit/img'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $img = Join-Path $dir ($name + '.img')
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { throw 'audit-fs: python not found (needed to craft images)' }
    & $py.Source (Join-Path $root 'tests/audit/fs/craft_xfs.py') $name $img | Out-Null
    if ($LASTEXITCODE -ne 0) { throw ("craft_xfs failed: {0}" -f $name) }
    return $img
}

function Test-PortFree([int]$port) {
    try {
        $c = New-Object Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', $port)
        $c.Close()
        return $false
    } catch { return $true }
}

function Stop-Qemu($proc, [int]$port) {
    try {
        $tcp = New-Object Net.Sockets.TcpClient('127.0.0.1', $port)
        $st = $tcp.GetStream()
        $rd = New-Object IO.StreamReader($st)
        $wr = New-Object IO.StreamWriter($st)
        $null = $rd.ReadLine()
        $wr.WriteLine('{"execute":"qmp_capabilities"}'); $wr.Flush()
        $null = $rd.ReadLine()
        $wr.WriteLine('{"execute":"quit"}'); $wr.Flush()
        Start-Sleep -Milliseconds 300
        $tcp.Close()
    } catch { try { $proc.Kill() } catch { } }
    try { Wait-Process -Id $proc.Id -Timeout 5 -ErrorAction SilentlyContinue } catch { }
}

function Run-Scenario([string]$name) {
    $img = Craft-Image $name
    $log = Join-Path $root ('obj/audit/' + $name + '.log')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $log) | Out-Null
    Remove-Item $log -ErrorAction SilentlyContinue
    if (-not (Test-PortFree $QmpPort)) { throw ("QMP {0} 已被占用" -f $QmpPort) }

    $floppy = Join-Path $root "$DistDir/floppy.img"
    $fat = Join-Path $root "$DistDir/fat.img"
    $qargs = @(
        '-drive', ("file={0},format=raw,if=floppy" -f ($floppy -replace '\\', '/')), '-boot', 'a',
        '-drive', ("file={0},format=raw,if=ide,index=0" -f ($img -replace '\\', '/')),
        '-drive', ("file={0},format=raw,if=ide,index=1" -f ($fat -replace '\\', '/')),
        '-m', '256', '-vga', 'std', '-cpu', 'qemu32,+nx,+smep,+smap', '-smp', '2',
        '-nic', 'user,model=e1000', '-display', 'none', '-no-reboot',
        '-serial', ("file:{0}" -f ($log -replace '\\', '/')),
        '-qmp', ("tcp:127.0.0.1:{0},server,nowait" -f $QmpPort)
    )
    $q = Start-Process -FilePath (Find-Qemu) -ArgumentList $qargs -PassThru -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $state = 'timeout'
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $log) {
            $s = Get-Content $log -Raw -ErrorAction SilentlyContinue
            if ($s -and $s.Contains('KERNEL PANIC')) { $state = 'panic'; break }
            if ($s -and $s.Contains('login:')) { $state = 'login'; break }
        }
        Start-Sleep -Milliseconds 250
    }
    Stop-Qemu $q $QmpPort

    $text = if (Test-Path $log) { Get-Content $log -Raw } else { '' }
    $lost = $text.Contains('fs: cannot create system:/bin/shell')
    $ok = ($state -eq 'login') -and (-not $text.Contains('KERNEL PANIC')) -and (-not $lost)
    $why = @()
    if ($state -ne 'login') { $why += $state }
    if ($lost) { $why += 'default-volume-lost' }
    $result = if ($ok) { 'PASS' } else { 'FAIL' }
    $color = if ($ok) { 'Green' } else { 'Red' }
    Write-Host ("  [{0}] {1,-16} {2} {3}" -f $result, $name, $state, ($why -join ',')) -ForegroundColor $color
    return [pscustomobject]@{ Name = $name; Ok = $ok; State = $state; Lost = $lost; Log = $log }
}

if ($Build -or $Unit -or $Scenarios -or $Scenario) {
    Build-Images
}

if ($Unit) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools/audit-fs-unit.ps1') -Repo $root
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

$results = @()
if ($Scenario) { $results += Run-Scenario $Scenario }
if ($Scenarios) {
    foreach ($s in $allScenarios) { $results += Run-Scenario $s }
}

if ($results.Count -gt 0) {
    $bad = @($results | Where-Object { -not $_.Ok })
    Write-Host ''
    if ($bad.Count -eq 0) {
        Write-Host ("=== AUDIT-FS PASS ({0}/{0}) ===" -f $results.Count) -ForegroundColor Green
        exit 0
    }
    Write-Host ("=== AUDIT-FS FAIL ({0}/{1}) ===" -f $bad.Count, $results.Count) -ForegroundColor Red
    foreach ($b in $bad) { Write-Host ("  * {0}: {1} (log {2})" -f $b.Name, $b.State, $b.Log) -ForegroundColor Red }
    exit 1
}

if (-not ($Build -or $Unit)) {
    Write-Host 'audit-fs: nothing to do (use -Build / -Unit / -Scenarios / -Scenario)' -ForegroundColor DarkGray
}
exit 0
