# check-deps.ps1 - module boundary checker
#
# Rules (see docs/modules.md):
#   - cross-module includes must go through src/kernel/include/<mod>.h
#   - same-module includes and lib/ are fine
#   - legacy "core/" directory is reported (must be dismantled)
#
# usage: pwsh -File tools/check-deps.ps1 [-Repo D:\OS_Project] [-Top 20]
param(
    [string]$Repo = 'D:\OS_Project',
    [int]$Top = 20
)

Set-Location $Repo
$root = Join-Path $Repo 'src\kernel'
$modules = @('core','lib','mm','proc','dev','fs','net','svc','security','user','apps','include')

function Get-Module([string]$relPath) {
    $parts = $relPath -split '[\\/]'
    if ($parts.Length -ge 2) { return $parts[0] }
    return '(root)'
}

$violations = @()
$files = Get-ChildItem $root -Recurse -File -Include *.c,*.h,*.asm
foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length).TrimStart('\','/')
    $from = Get-Module $rel
    $lines = Select-String -Path $f.FullName -Pattern '#include\s+"([^"]+)"'
    foreach ($m in $lines) {
        $inc = $m.Matches[0].Groups[1].Value
        if ($inc.StartsWith('include/')) { continue }

        # resolve target module
        $target = $null
        if ($inc -match '^\.\./([a-z]+)/') { $target = $matches[1] }
        elseif ($inc -match '^([a-z]+)/')  { $target = $matches[1] }
        else { continue }   # same-directory include

        if ($modules -notcontains $target) { continue }
        if ($target -eq $from) { continue }
        if ($target -eq 'include') { continue }
        if ($from -eq 'lib' -or $target -eq 'lib') { continue }   # lib is shared

        $violations += [pscustomobject]@{
            From = $from; To = $target; Header = $inc; File = "src/kernel/$rel"; Line = $m.LineNumber
        }
    }
}

"module include violations: $($violations.Count)"
""
$violations | Group-Object From,To | Sort-Object Count -Descending |
    Select-Object -First $Top Count,Name | Format-Table -AutoSize

$violations | Group-Object To | Sort-Object Count -Descending |
    Select-Object -First 10 Count,Name | Format-Table -AutoSize

$violations | Select-Object -First 25 | Format-Table -AutoSize

if (Test-Path (Join-Path $root 'core')) {
    $coreFiles = (Get-ChildItem (Join-Path $root 'core') -File | Measure-Object).Count
    ""
    "legacy core/ files: $coreFiles (target: kernel.c panic ksym version splash only)"
}
