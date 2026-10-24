# fix-includes.ps1 - repair relative #include paths after moving files
#
# For every .c/.h under src/kernel, each #include at the start of a line that
# does not resolve relative to the including file is matched by basename
# against the tree. If exactly one candidate exists, the include is rewritten
# to a relative path. Ambiguous or missing includes are reported.
#
# usage: powershell -File tools/fix-includes.ps1 [-Repo D:\OS_Project] [-Apply]
param(
    [string]$Repo = 'D:\OS_Project',
    [switch]$Apply
)

Set-Location $Repo
$root = (Resolve-Path 'src\kernel').Path
$files = Get-ChildItem $root -Recurse -File -Include *.c,*.h
$byName = @{}
foreach ($f in $files) {
    if (-not $byName.ContainsKey($f.Name)) { $byName[$f.Name] = @() }
    $byName[$f.Name] += $f.FullName
}

function Get-RelPath([string]$fromDir, [string]$toFile) {
    $from = (Resolve-Path $fromDir).Path.TrimEnd('\') -split '\\'
    $to = (Resolve-Path $toFile).Path -split '\\'
    $i = 0
    while ($i -lt $from.Count -and $i -lt ($to.Count - 1) -and $from[$i] -eq $to[$i]) { $i++ }
    $upCount = $from.Count - $i
    $down = ($to[$i..($to.Count - 1)] -join '/')
    if ($upCount -gt 0) { return (('../' * $upCount) + $down) }
    return $down
}

$fixed = 0; $ambiguous = @(); $missing = @()
$enc = New-Object Text.UTF8Encoding $false
foreach ($f in $files) {
    $text = [IO.File]::ReadAllText($f.FullName)
    $orig = $text
    $dir = $f.DirectoryName
    $matches = [regex]::Matches($text, '(?m)^\s*#include\s+"([^"]+)"')
    foreach ($m in $matches) {
        $inc = $m.Groups[1].Value
        $candidate = Join-Path $dir $inc
        if (Test-Path $candidate) { continue }

        $leaf = Split-Path $inc -Leaf
        if (-not $byName.ContainsKey($leaf)) { $missing += "$($f.Name): $inc"; continue }
        $hits = $byName[$leaf]
        if ($hits.Count -gt 1) {
            $real = @($hits | Where-Object { -not ((Get-Content $_ -Raw -ErrorAction SilentlyContinue) -match 'FORWARD_H') })
            if ($real.Count -eq 1) { $hits = $real }
            else { $ambiguous += "$($f.Name): $inc -> $($hits.Count) candidates"; continue }
        }

        $rel = Get-RelPath $dir $hits[0]
        if (-not $rel) { continue }
        $text = $text.Replace("#include `"$inc`"", "#include `"$rel`"")
        $fixed++
    }
    if ($text -ne $orig -and $Apply) {
        [IO.File]::WriteAllText($f.FullName, $text, $enc)
    }
}

"rewritten includes: $fixed (apply=$($Apply.IsPresent))"
if ($ambiguous.Count) { "ambiguous:"; $ambiguous | Select-Object -First 20 }
if ($missing.Count)   { "missing:";   $missing   | Select-Object -First 20 }
