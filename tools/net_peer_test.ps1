# =============================================================================
# tools/net_peer_test.ps1 - act as the network peer (QEMU socket backend)
#
# Why not -nic user: slirp answers ARP/ICMP for the gateway but does NOT deliver
# guest->10.0.2.2 UDP to a host listening socket, so a UDP echo cannot be
# asserted reliably. Here we play the peer ourselves: whatever arrives, we
# answer per protocol. Deterministic, no external network needed.
#
# Usage: .\tools\net_peer_test.ps1 [-Port 12353] [-Seconds 20]
# =============================================================================
param(
    [int]$Port = 12353,
    [int]$Seconds = 20
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$Image = Join-Path $root 'dist\floppy.img'
$Log   = Join-Path $root 'serial.log'

$ourMac = [byte[]]@(0x52,0x54,0x00,0x12,0x34,0x57)
$gwIp   = [byte[]]@(10,0,2,2)
$localIp= [byte[]]@(10,0,2,15)

function Get-BE16([byte[]]$f, [int]$o) { return ([int]$f[$o] -shl 8) + [int]$f[$o+1] }

function Send-Frame($stream, [byte[]]$f) {
    $hdr = [byte[]]@(0,0,([byte](($f.Length -shr 8) -band 0xFF)),([byte]($f.Length -band 0xFF)))
    $stream.Write($hdr, 0, 4)
    $stream.Write($f, 0, $f.Length)
    $stream.Flush()
}

# Build a reply: swap MACs, swap IPs, fix up protocol fields.
function New-Reply([byte[]]$f, [int]$len) {
    $r = New-Object byte[] $len
    [Array]::Copy($f, 0, $r, 0, $len)
    for ($i = 0; $i -lt 6; $i++) { $r[$i] = $f[6+$i]; $r[6+$i] = $ourMac[$i] }
    $et = Get-BE16 $f 12
    if ($et -eq 0x0806 -and (Get-BE16 $f 20) -eq 1) {
        $r[20] = 0; $r[21] = 2
        for ($i = 0; $i -lt 6; $i++) { $r[22+$i] = $ourMac[$i]; $r[32+$i] = $f[22+$i] }
        for ($i = 0; $i -lt 4; $i++) { $r[28+$i] = $f[38+$i]; $r[38+$i] = $f[28+$i] }
        return ,$r
    }
    if ($et -ne 0x0800) { return $null }
    $ihl = ($f[14] -band 0x0F) * 4
    for ($i = 0; $i -lt 4; $i++) { $r[26+$i] = $f[30+$i]; $r[30+$i] = $f[26+$i] }
    $proto = $f[23]
    if ($proto -eq 1) {
        $icmp = 14 + $ihl
        if ($f[$icmp] -eq 8) { $r[$icmp] = 0; return ,$r }
        return $null
    }
    if ($proto -eq 17) {
        $udp = 14 + $ihl
        $r[$udp]   = $f[$udp+2]; $r[$udp+1] = $f[$udp+3]
        $r[$udp+2] = $f[$udp];   $r[$udp+3] = $f[$udp+1]
        return ,$r
    }
    return $null
}

if (-not (Test-Path $Image)) { throw "missing $Image (run build.ps1 first)" }
Remove-Item $Log -ErrorAction SilentlyContinue

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
$listener.Start()
$qemu = Start-Process -FilePath 'qemu-system-i386' -ArgumentList @(
    '-drive', ("file={0},format=raw,if=floppy" -f ($Image -replace '\\','/')),
    '-boot','a','-m','256','-vga','std','-cpu','max','-smp','2',
    '-netdev', ("socket,id=n0,connect=127.0.0.1:{0}" -f $Port),
    '-device','e1000,netdev=n0',
    '-display','none','-serial',("file:{0}" -f $Log), '-no-reboot'
) -PassThru -WindowStyle Hidden

$connected = $false
for ($i = 0; $i -lt 100; $i++) { if ($listener.Pending()) { $connected = $true; break }; Start-Sleep -Milliseconds 100 }
if (-not $connected) { $listener.Stop(); if (-not $qemu.HasExited) { $qemu.Kill() }; throw "qemu did not connect" }

$client = $listener.AcceptTcpClient()
$stream = $client.GetStream()
$stream.ReadTimeout = 300

$stats = @{ arp = 0; icmp = 0; udp = 0 }
$buf = New-Object byte[] 4096
$acc = New-Object System.Collections.Generic.List[byte]
$deadline = (Get-Date).AddSeconds($Seconds)

while ((Get-Date) -lt $deadline) {
    try { $n = $stream.Read($buf, 0, $buf.Length) } catch { $n = 0 }
    if ($n -gt 0) { for ($j = 0; $j -lt $n; $j++) { $acc.Add($buf[$j]) } }
    while ($acc.Count -ge 4) {
        $len = ([int]$acc[0] -shl 24) -bor ([int]$acc[1] -shl 16) -bor ([int]$acc[2] -shl 8) -bor [int]$acc[3]
        if ($len -le 0 -or $acc.Count -lt 4 + $len) { break }
        $frame = $acc.GetRange(4, $len).ToArray()
        $acc.RemoveRange(0, 4 + $len)
        if ($len -lt 42) { continue }
        $et = Get-BE16 $frame 12
        $reply = New-Reply $frame $len
        if ($et -eq 0x0806) { $stats.arp++ }
        if ($et -ne 0x0806 -and $frame[23] -eq 1) { $stats.icmp++ }
        if ($et -ne 0x0806 -and $frame[23] -eq 17) { $stats.udp++ }
        if ($reply -ne $null) { Send-Frame $stream $reply }
    }
}

$client.Close(); $listener.Stop()
if (-not $qemu.HasExited) { $qemu.Kill() }
Start-Sleep -Milliseconds 300

Write-Host ("peer: answered arp={0} icmp={1} udp={2}" -f $stats.arp, $stats.icmp, $stats.udp)
if (-not (Test-Path $Log)) { throw "no serial log" }
$out = Get-Content $Log -Raw
$checks = @(
    @{ N = 'ARP reply';  Ok = ($out -match 'net: ARP reply gw=') },
    @{ N = 'ICMP echo';  Ok = ($out -match 'net: ICMP echo reply seq=\d+') },
    @{ N = 'UDP echo';   Ok = ($out -match 'net: UDP echo reply') }
)
$fail = 0
foreach ($c in $checks) {
    if ($c.Ok) { Write-Host ("  [PASS] {0}" -f $c.N) -ForegroundColor Green }
    else { Write-Host ("  [FAIL] {0}" -f $c.N) -ForegroundColor Red; $fail++ }
}
if ($fail -ne 0) { Write-Host ("=== NET PEER FAIL ({0}) ===" -f $fail) -ForegroundColor Red; exit 1 }
Write-Host "=== NET PEER PASS ===" -ForegroundColor Green
