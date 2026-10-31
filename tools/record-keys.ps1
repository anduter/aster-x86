# record-keys.ps1 - poll GetAsyncKeyState and log a key timeline (no admin/install)
param(
    [int]$Seconds = 60,
    [string]$Out = ""
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root 'obj-ng\feedback'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
if ([string]::IsNullOrWhiteSpace($Out)) {
    $Out = Join-Path $dir ("keys-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".csv")
}
$sig = '[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);'
Add-Type -Namespace KeyLog -Name Native -MemberDefinition $sig
$names = @{
    0x08='Backspace'; 0x09='Tab'; 0x0D='Enter'; 0x1B='Esc'; 0x20='Space';
    0x25='Left'; 0x26='Up'; 0x27='Right'; 0x28='Down';
    0x2E='Delete'; 0x24='Home'; 0x23='End'; 0x21='PgUp'; 0x22='PgDn'; 0x14='Caps';
    0x10='Shift'; 0x11='Ctrl'; 0x12='Alt';
    0xA0='LShift'; 0xA1='RShift'; 0xA2='LCtrl'; 0xA3='RCtrl'; 0xA4='LAlt'; 0xA5='RAlt';
    0x5B='LWin'; 0x5C='RWin'
}
for ($i = 0x30; $i -le 0x39; $i++) { $names[$i] = [string][char]$i }
for ($i = 0x41; $i -le 0x5A; $i++) { $names[$i] = [string][char]$i }
$vks = @(0x08,0x09,0x0D,0x1B,0x20,0x25,0x26,0x27,0x28,0x2E,0x24,0x23,0x21,0x22,0x14,0x10,0x11,0x12,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0x5B,0x5C) + (0x30..0x39) + (0x41..0x5A)
$state = @{}
$start = Get-Date
$deadline = $start.AddSeconds($Seconds)
[IO.File]::WriteAllText($Out, "time,vk,name,event" + [Environment]::NewLine, (New-Object Text.UTF8Encoding $false))
Write-Host ("keys logging " + $Seconds + "s -> " + $Out)
while ((Get-Date) -lt $deadline) {
    foreach ($vk in $vks) {
        $down = ([KeyLog.Native]::GetAsyncKeyState($vk) -band 0x8000) -ne 0
        $prev = $state[$vk]
        $name = if ($names.ContainsKey($vk)) { $names[$vk] } else { ('VK_' + $vk.ToString('X2')) }
        if ($down -and -not $prev) {
            Add-Content -Path $Out -Value ((Get-Date).ToString('HH:mm:ss.fff') + ',' + $vk + ',' + $name + ',down') -Encoding ASCII
            $state[$vk] = $true
        } elseif (-not $down -and $prev) {
            Add-Content -Path $Out -Value ((Get-Date).ToString('HH:mm:ss.fff') + ',' + $vk + ',' + $name + ',up') -Encoding ASCII
            $state[$vk] = $false
        }
    }
    Start-Sleep -Milliseconds 10
}
Write-Host ('saved: ' + $Out)
