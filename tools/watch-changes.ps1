# watch-changes.ps1 - repo change watcher (event-driven progress reporting)
#
# usage: pwsh -File tools/watch-changes.ps1 -Repo D:\OS_Project -IntervalSec 30
#
# behavior:
#   1. take a snapshot at start (HEAD + version + git status + src file mtimes);
#   2. re-snapshot every IntervalSec seconds;
#   3. print the change details and exit 0 as soon as the snapshot differs;
#   4. exit 2 after MaxHours without any change (avoid hanging forever).
param(
    [string]$Repo = 'D:\OS_Project',
    [int]$IntervalSec = 30,
    [int]$MaxHours = 12
)

$ErrorActionPreference = 'Stop'
Set-Location $Repo

function Get-Snapshot {
    $head = (cmd /c "git rev-parse HEAD 2>nul")
    $status = (cmd /c "git status --porcelain 2>nul") -join "`n"
    $ver = (Select-String -Path src\kernel\version.h -Pattern 'OS_VERSION' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Line.Trim() })
    $srcHash = (Get-ChildItem src -Recurse -File -Include *.c,*.h,*.asm,*.ld |
                Sort-Object FullName |
                ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join "`n"

    $raw = "HEAD=$head`nVER=$ver`nSTATUS=$status`nSRC=$srcHash"
    $sha = [Security.Cryptography.SHA256]::Create()
    $bytes = [Text.Encoding]::UTF8.GetBytes($raw)
    $hash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '')
    return [pscustomobject]@{ Hash = $hash; Head = $head; Ver = $ver; Status = $status }
}

$base = Get-Snapshot
$deadline = (Get-Date).AddHours($MaxHours)

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $IntervalSec
    $cur = Get-Snapshot
    if ($cur.Hash -ne $base.Hash) {
        Write-Output '=== PROJECT CHANGED ==='
        Write-Output ("OLD HEAD: {0}" -f $base.Head)
        Write-Output ("NEW HEAD: {0}" -f $cur.Head)
        Write-Output ("OLD VER : {0}" -f $base.Ver)
        Write-Output ("NEW VER : {0}" -f $cur.Ver)
        Write-Output '--- git status (porcelain) ---'
        if ([string]::IsNullOrWhiteSpace($cur.Status)) { Write-Output '(clean)' } else { Write-Output $cur.Status }
        exit 0
    }
}

Write-Output '=== NO CHANGE (timeout) ==='
Write-Output ("HEAD: {0}" -f $base.Head)
exit 2
