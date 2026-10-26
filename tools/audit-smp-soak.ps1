# =============================================================================
# audit-smp-soak.ps1 - 4-core fork/exit/mmap/signal soak with QMP key injection
#
#   powershell -NoProfile -File tools\audit-smp-soak.ps1 -Minutes 12
#
# Uses the autologin (-Test) images in .dist-audit-smp, copies them to
# .dist-audit-smp4 (separate -smp 4 image), boots QEMU on QMP 4544 and keeps
# injecting `sectest` / `fork` / `linktest` for N minutes. At the end it checks
# the serial log for panics/oops and for a living heartbeat + repeated
# fork/exit workloads. Never touches port 12345.
# =============================================================================
param(
    [int]$Minutes = 12,
    [string]$DistDir = '.dist-audit-smp4',
    [string]$SerialLog = 'obj/audit/smp4-soak.log',
    [int]$QmpPort = 4544
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$dist = if ([IO.Path]::IsPathRooted($DistDir)) { $DistDir } else { Join-Path $root $DistDir }
$serial = if ([IO.Path]::IsPathRooted($SerialLog)) { $SerialLog } else { Join-Path $root $SerialLog }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $serial) | Out-Null
New-Item -ItemType Directory -Force -Path $dist | Out-Null
foreach ($f in @('floppy.img', 'disk.img', 'fat.img')) {
    Copy-Item (Join-Path (Join-Path $root '.dist-audit-smp') $f) (Join-Path $dist $f) -Force
}

Get-CimInstance Win32_Process -Filter "Name='qemu-system-i386.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match [regex]::Escape($serial) } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Remove-Item -LiteralPath $serial -ErrorAction SilentlyContinue
$qargs = @(
    '-drive', ("file={0},format=raw,if=floppy" -f ((Join-Path $dist 'floppy.img') -replace '\\','/')),
    '-boot', 'a',
    '-drive', ("file={0},format=raw,if=ide,index=0" -f ((Join-Path $dist 'disk.img') -replace '\\','/')),
    '-drive', ("file={0},format=raw,if=ide,index=1" -f ((Join-Path $dist 'fat.img') -replace '\\','/')),
    '-m', '256', '-vga', 'std', '-cpu', 'qemu32,+nx,+smep,+smap', '-smp', '4',
    '-nic', 'user,model=e1000', '-display', 'none', '-no-reboot',
    '-serial', ("file:{0}" -f ($serial -replace '\\','/')),
    '-qmp', ("tcp:127.0.0.1:{0},server,nowait" -f $QmpPort)
)
Write-Host ("[soak] boot smp=4 minutes={0} serial={1}" -f $Minutes, $SerialLog) -ForegroundColor Cyan
$q = Start-Process -FilePath 'qemu-system-i386' -ArgumentList $qargs -PassThru -WindowStyle Hidden

function Get-Snap { if (Test-Path $serial) { return [string](Get-Content $serial -Raw -ErrorAction SilentlyContinue) } return '' }
function Wait-For([string]$needle, [int]$sec) {
    $deadline = (Get-Date).AddSeconds($sec)
    while ((Get-Date) -lt $deadline) {
        if (([string](Get-Snap)).Contains($needle)) { return $true }
        if ($q.HasExited) { return $false }
        Start-Sleep -Milliseconds 250
    }
    return ([string](Get-Snap)).Contains($needle)
}

$client = $null; $reader = $null; $writer = $null
function Qmp-Connect {
    $script:client = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $QmpPort)
    $script:reader = New-Object System.IO.StreamReader($script:client.GetStream())
    $script:writer = New-Object System.IO.StreamWriter($script:client.GetStream())
    $script:writer.AutoFlush = $true
    $null = $script:reader.ReadLine()
    $script:writer.WriteLine('{"execute":"qmp_capabilities"}')
    $null = $script:reader.ReadLine()
}
function Qmp-Cmd([string]$line) {
    $script:writer.WriteLine($line)
    try { return $script:reader.ReadLine() } catch { return '' }
}
function Send-Line([string]$text) {
    $map = @{ ' '='spc'; '-'='minus'; '.'='dot'; '/'='slash'; ':'='shift-semicolon' }
    foreach ($ch in $text.ToCharArray()) {
        $name = [string]$ch
        if ($ch -eq "`n") { $name = 'ret' }
        elseif ($map.ContainsKey($name)) { $name = $map[$name] }
        elseif ([char]::IsUpper($ch)) { $name = 'shift-' + $ch.ToString().ToLower() }
        $null = Qmp-Cmd ('{"execute":"human-monitor-command","arguments":{"command-line":"sendkey ' + $name + '"}}')
        Start-Sleep -Milliseconds 20
    }
}

if (-not (Wait-For 'ushell: ready' 75)) { Write-Host '[soak] shell did not come up' -ForegroundColor Red; $q.Kill(); exit 1 }
Qmp-Connect
Write-Host '[soak] shell ready; starting workload' -ForegroundColor Green

$cmds = @('sectest', 'fork', 'linktest', 'fork', 'sectest')
$deadline = (Get-Date).AddMinutes($Minutes)
$n = 0
$lastLog = Get-Date
while ((Get-Date) -lt $deadline) {
    $cmd = $cmds[$n % $cmds.Count]
    Send-Line ("{0}`n" -f $cmd)
    $n++
    if (((Get-Date) - $lastLog).TotalSeconds -ge 60) {
        $snap = [string](Get-Snap)
        $hb = ([regex]::Matches($snap, 'thread: heartbeat')).Count
        $sec = ([regex]::Matches($snap, 'sectest: ')).Count
        Write-Host ("[soak] {0:N0}s cmds={1} heartbeat_lines={2} sectest_lines={3}" -f
            ((Get-Date) - ($deadline.AddMinutes(-$Minutes))).TotalSeconds, $n, $hb, $sec)
        $lastLog = Get-Date
    }
    Start-Sleep -Seconds 8
    if ($q.HasExited) { break }
}

try { $null = Qmp-Cmd '{"execute":"quit"}' } catch { }
try { $q.WaitForExit(5000) | Out-Null } catch { }
if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue }
try { $client.Close() } catch { }

$snap = [string](Get-Snap)
$hb = ([regex]::Matches($snap, 'thread: heartbeat')).Count
$sec = ([regex]::Matches($snap, 'sectest: ')).Count
$bad = ($snap -match 'panic:|oops:|KERNEL PANIC') -or ($snap -match 'audit: smp selftest FAIL')
Write-Host ("[soak] done: cmd_sent={0} heartbeat_lines={1} sectest_lines={2} panic/oops={3}" -f $n, $hb, $sec, $bad)
if ($bad) { Write-Host '[soak] FAIL (panic/oops detected)' -ForegroundColor Red; exit 2 }
if ($hb -lt 60) { Write-Host '[soak] FAIL (heartbeat did not advance)' -ForegroundColor Red; exit 1 }
if ($sec -lt 40) { Write-Host '[soak] FAIL (sectest did not run repeatedly)' -ForegroundColor Red; exit 1 }
Write-Host '[soak] PASS (4-core fork/exit/mmap/signal workload alive)' -ForegroundColor Green
exit 0