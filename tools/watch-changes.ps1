# watch-changes.ps1 - repo change watcher (event-driven progress reporting)
#
# v4: reports only milestones, with debounce:
#   - HEAD changed (new commit)          <- main signal
#   - the set of UNTRACKED files changed <- new module/file appeared
# Version bumps while the tree is still dirty do NOT trigger (mid-work noise);
# the current version is still shown in every report.
#   When triggered, wait until the snapshot is stable for SettleSec, then report
#   once and exit 0.
#
# usage: pwsh -File tools/watch-changes.ps1 -Repo D:\OS_Project -IntervalSec 90 -SettleSec 120
param(
    [string]$Repo = 'D:\OS_Project',
    [int]$IntervalSec = 90,
    [int]$SettleSec = 120,
    [int]$MaxHours = 12
)

$ErrorActionPreference = 'Stop'
Set-Location $Repo

function Get-Snapshot {
    $head = (cmd /c "git rev-parse HEAD 2>nul")
    $ver = (Select-String -Path src\kernel\version.h -Pattern 'OS_VERSION' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Line.Trim() })
    $statusLines = (cmd /c "git status --porcelain 2>nul")
    $untracked = @($statusLines | Where-Object { $_.ToString().StartsWith('??') } |
                   ForEach-Object { $_.ToString().Substring(3).Trim() } | Sort-Object)
    $diffstat = ((cmd /c "git diff --stat 2>nul") | Select-Object -Last 1)

    # trigger key: HEAD + sorted set of untracked paths (version shown but not triggering)
    $key = "HEAD=$head`nUNTRACKED=" + ($untracked -join '|')
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($key))).Replace('-', '')

    return [pscustomobject]@{
        Hash = $hash
        Head = $head
        Ver  = $ver
        Untracked = $untracked
        Status = ($statusLines -join "`n")
        DiffStat = $diffstat
    }
}

$base = Get-Snapshot
$deadline = (Get-Date).AddHours($MaxHours)

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $IntervalSec
    $cur = Get-Snapshot
    if ($cur.Hash -eq $base.Hash) { continue }

    # debounce: wait until stable for SettleSec (bursts of edits/commits coalesce)
    $stableSince = Get-Date
    $last = $cur
    while (((Get-Date) - $stableSince).TotalSeconds -lt $SettleSec -and (Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 10
        $s = Get-Snapshot
        if ($s.Hash -ne $last.Hash) { $last = $s; $stableSince = Get-Date }
    }

    Write-Output '=== PROJECT CHANGED ==='
    Write-Output ("OLD HEAD: {0}" -f $base.Head)
    Write-Output ("NEW HEAD: {0}" -f $last.Head)
    Write-Output ("OLD VER : {0}" -f $base.Ver)
    Write-Output ("NEW VER : {0}" -f $last.Ver)
    Write-Output ("DIFFSTAT: {0}" -f $last.DiffStat)
    Write-Output '--- untracked files ---'
    if ($last.Untracked.Count -eq 0) { Write-Output '(none)' } else { $last.Untracked | ForEach-Object { Write-Output $_ } }
    exit 0
}

Write-Output '=== NO CHANGE (timeout) ==='
Write-Output ("HEAD: {0}" -f $base.Head)
exit 2
