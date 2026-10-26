# =============================================================================
# tests/audit/smp/tcp-repro.ps1 - tcptest startup-hang reproducer (audit-smp)
#
# Boots the -Test image N times with a host TCP/UDP echo server and checks that
# `net: tcp selftest PASS` + `proc: pid N (tcptest) exited` show up within
# WaitSec. On a miss it dumps `info registers` via QMP and keeps going.
#
#   powershell -NoProfile -File tests\audit\smp\tcp-repro.ps1 -Runs 10
#   powershell -NoProfile -File tests\audit\smp\tcp-repro.ps1 -Runs 10 `
#       -Root D:\OS_Project\.wt-audit-smp-base -DistDir .dist-base -QmpPort 4546 -EchoPort 45446
#
# Baseline 38161af hit ~10% (1/10); with the audit-smp scheduler fixes 0/20.
# Never uses 12345; QMP defaults to 4544.
# =============================================================================
param([int]$Runs = 10, [int]$WaitSec = 60, [int]$Smp = 2, [string]$DistDir = ".dist-audit-smp", [int]$QmpPort = 4544, [int]$EchoPort = 45445, [string]$Root = "")
$ErrorActionPreference = 'Continue'
if (-not $Root) {
    $Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
}
Set-Location $Root
$dist = Join-Path $Root $DistDir
$echoPort = $EchoPort
$misses = 0
for ($i = 1; $i -le $Runs; $i++) {
    $serial = Join-Path $Root ("obj\audit\tcp-repro-{0}.log" -f $i)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $serial) | Out-Null
    $serialFull = $serial.Replace('\','/')
    Remove-Item $serial -ErrorAction SilentlyContinue
    $udp = Start-Job -ArgumentList $echoPort -ScriptBlock {
        param($port)
        $u = New-Object System.Net.Sockets.UdpClient($port)
        $u.Client.ReceiveTimeout = 300
        try {
            $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $deadline = (Get-Date).AddSeconds(130)
            while ((Get-Date) -lt $deadline) {
                try { $d = $u.Receive([ref]$ep); [void]$u.Send($d, $d.Length, $ep) } catch { }
            }
        } finally { $u.Close() }
    }
    $tcp = Start-Job -ArgumentList $echoPort -ScriptBlock {
        param($port)
        try {
            $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, $port)
            $l.Start()
            $deadline = (Get-Date).AddSeconds(120)
            $n = 0
            while ((Get-Date) -lt $deadline -and $n -lt 8) {
                if (-not $l.Pending()) { Start-Sleep -Milliseconds 50; continue }
                $c = $l.AcceptTcpClient()
                $c.ReceiveTimeout = 1500
                $s = $c.GetStream()
                $buf = New-Object byte[] 2048
                $total = 0
                try {
                    while ($total -lt 262144) {
                        $r = $s.Read($buf, 0, $buf.Length)
                        if ($r -le 0) { break }
                        $s.Write($buf, 0, $r); $s.Flush(); $total += $r
                    }
                } catch { }
                try { $c.Close() } catch { }
                $n++
            }
            $l.Stop()
        } catch { }
    }
    Start-Sleep -Milliseconds 300
    $listenOk = $false
    for ($t = 0; $t -lt 20; $t++) {
        try { $probe = New-Object System.Net.Sockets.TcpClient; $probe.Connect("127.0.0.1", $echoPort); $probe.Close(); $listenOk = $true; break } catch { Start-Sleep -Milliseconds 150 }
    }
    if (-not $listenOk) { Write-Output ("run {0}: ECHO NOT LISTENING on {1}" -f $i, $echoPort); Stop-Job $udp,$tcp -ErrorAction SilentlyContinue | Out-Null; Remove-Job $udp,$tcp -Force -ErrorAction SilentlyContinue; continue }
    $qargs = @(
        '-drive', ("file={0},format=raw,if=floppy" -f ((Join-Path $dist 'floppy.img') -replace '\\','/')),
        '-boot', 'a',
        '-drive', ("file={0},format=raw,if=ide,index=0" -f ((Join-Path $dist 'disk.img') -replace '\\','/')),
        '-drive', ("file={0},format=raw,if=ide,index=1" -f ((Join-Path $dist 'fat.img') -replace '\\','/')),
        '-m', '256', '-vga', 'std', '-cpu', 'qemu32,+nx,+smep,+smap', '-smp', "$Smp",
        '-nic', 'user,model=e1000', '-display', 'none', '-no-reboot',
        '-serial', ("file:{0}" -f $serialFull),
        '-qmp', ('tcp:127.0.0.1:{0},server,nowait' -f $QmpPort)
    )
    $q = Start-Process -FilePath 'qemu-system-i386' -ArgumentList $qargs -PassThru -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds($WaitSec)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $serial) {
            $s = [string](Get-Content $serial -Raw -ErrorAction SilentlyContinue)
            if ($s -match 'proc: pid \d+ \(tcptest\) exited') { $ok = $true; break }
        }
        if ($q.HasExited) { break }
        Start-Sleep -Milliseconds 300
    }
    $regs = ''
    if (-not $ok -and -not $q.HasExited) {
        try {
            $cl = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $QmpPort)
            $st = $cl.GetStream(); $rd = New-Object System.IO.StreamReader($st); $wr = New-Object System.IO.StreamWriter($st)
            $wr.AutoFlush = $true
            $null = $rd.ReadLine()
            $wr.WriteLine('{"execute":"qmp_capabilities"}'); $null = $rd.ReadLine()
            foreach ($cpu in 0,1) {
                $wr.WriteLine('{"execute":"human-monitor-command","arguments":{"command-line":"cpu ' + $cpu + '"}}'); $null = $rd.ReadLine()
                $wr.WriteLine('{"execute":"human-monitor-command","arguments":{"command-line":"info registers"}}')
                $regs += $rd.ReadLine()
            }
            $wr.WriteLine('{"execute":"quit"}')
            try { $null = $rd.ReadLine() } catch { }
            $cl.Close()
        } catch { }
    }
    if (-not $q.HasExited) { try { $q.WaitForExit(5000) | Out-Null } catch { }; if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue } }
    Stop-Job $udp,$tcp -ErrorAction SilentlyContinue | Out-Null
    Remove-Job $udp,$tcp -Force -ErrorAction SilentlyContinue
    $hb = 0
    $tcpPass = $false
    if (Test-Path $serial) {
        $snap = [string](Get-Content $serial -Raw)
        $hb = ([regex]::Matches($snap, 'thread: heartbeat')).Count
        $tcpPass = $snap.Contains('net: tcp selftest PASS')
    }
    Write-Output ("run {0}: tcptest_exit={1} tcp_pass={2} heartbeats={3}" -f $i, $ok, $tcpPass, $hb)
    if (-not $ok) { Write-Output ("REGS run {0}: {1}" -f $i, $regs); $misses++ }
}
Write-Output ("SUMMARY: runs={0} misses={1} hit_rate={2:P0}" -f $Runs, $misses, ($misses / [double]$Runs))
