# check-deps.ps1 - module boundary checker (see docs/modules.md)
#
# Rules:
#   - cross-module includes must go through src/kernel/include/<mod>.h
#   - same-module includes, lib/ and M0 primitives are fine
#   - migration forwarder headers are ignored
#
# usage: powershell -File tools/check-deps.ps1 [-Repo D:\OS_Project] [-Top 20]
param(
    [string]$Repo = 'D:\OS_Project',
    [int]$Top = 20
)

Set-Location $Repo
$root = Join-Path $Repo 'src\kernel'
$modules = @('core','lib','mm','proc','dev','fs','net','svc','security','user','apps','include')
$primitives = @('error.h','atomic.h','spinlock.h')

function Get-ModuleOf([string]$relPath) {
    $parts = $relPath -split '[\\/]'
    if ($parts.Length -ge 2) { return $parts[0] }
    return '(root)'
}

$violations = @()
$skippedFwd = 0
$files = Get-ChildItem $root -Recurse -File -Include *.c,*.h,*.asm
foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length).TrimStart('\','/')
    $from = Get-ModuleOf $rel
    $content = Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue

    # migration forwarder headers are transitional; do not count them
    if ($f.Extension -eq '.h' -and $content -match 'migration forwarder') {
        $skippedFwd++
        continue
    }

    $lines = Select-String -Path $f.FullName -Pattern '#include\s+"([^"]+)"'
    foreach ($m in $lines) {
        $inc = $m.Matches[0].Groups[1].Value
        if ($inc.StartsWith('include/')) { continue }

        $target = $null
        if ($inc -match '^\.\./([a-z]+)/') { $target = $matches[1] }
        elseif ($inc -match '^([a-z]+)/')  { $target = $matches[1] }
        else { continue }

        if ($modules -notcontains $target) { continue }
        if ($target -eq $from) { continue }
        if ($target -eq 'lib' -or $from -eq 'lib') { continue }

        $base = Split-Path $inc -Leaf
        if ($target -eq 'core' -and ($primitives -contains $base)) { continue }

        $violations += [pscustomobject]@{
            From = $from; To = $target; Header = $inc
            File = "src/kernel/$rel"; Line = $m.LineNumber
        }
    }
}

"forwarder headers skipped: $skippedFwd"
"module include violations: $($violations.Count)"
""
"--- by edge (from -> to) ---"
$violations | Group-Object From,To | Sort-Object Count -Descending |
    Select-Object -First $Top Count,Name | Format-Table -AutoSize

"--- by target module ---"
$violations | Group-Object To | Sort-Object Count -Descending |
    Select-Object -First 10 Count,Name | Format-Table -AutoSize

"--- first 20 violations ---"
$violations | Select-Object -First 20 | Format-Table -AutoSize

if (Test-Path (Join-Path $root 'core')) {
    $coreFiles = (Get-ChildItem (Join-Path $root 'core') -File | Measure-Object).Count
    ""
    "legacy core/ files: $coreFiles (target: kernel.c panic ksym version splash only)"
}
