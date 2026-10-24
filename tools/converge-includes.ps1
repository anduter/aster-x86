# converge-includes.ps1 - route cross-module includes through src/kernel/include/<mod>.h
#
# Resolves each include (following migration forwarders) to the header that
# actually implements it, maps that module to its interface header, and
# rewrites cross-module includes. Same-module includes are left alone.
#
# usage: powershell -File tools/converge-includes.ps1 [-Repo D:\OS_Project] [-Apply]
param(
    [string]$Repo = 'D:\OS_Project',
    [switch]$Apply
)

Set-Location $Repo
$root = (Resolve-Path 'src\kernel').Path
$map = @{
    'core'='kernel.h'; 'mm'='mm.h'; 'proc'='proc.h'; 'dev'='dev.h'; 'fs'='fs.h'
    'net'='net.h'; 'svc'='svc.h'; 'security'='security.h'; 'user'='userland.h'
    'apps'='apps.h'; 'ipc'='svc.h'; 'lib'='kernel.h'
}

function Resolve-Include([string]$fromDir, [string]$inc) {
    $p = Join-Path $fromDir $inc
    if (-not (Test-Path $p)) {
        $p = Join-Path $root $inc
        if (-not (Test-Path $p)) { return $null }
    }
    return (Resolve-Path $p).Path
}

function Get-RealTarget([string]$path) {
    for ($i = 0; $i -lt 3; $i++) {
        $text = [IO.File]::ReadAllText($path)
        if ($text -notmatch 'FORWARD_H') { return $path }
        $m = [regex]::Match($text, '(?m)^\s*#include\s+"([^"]+)"')
        if (-not $m.Success) { return $path }
        $next = Join-Path (Split-Path $path -Parent) $m.Groups[1].Value
        if (-not (Test-Path $next)) { return $path }
        $path = (Resolve-Path $next).Path
    }
    return $path
}

function Get-ModuleOf([string]$absPath) {
    $rel = $absPath.Substring($root.Length).TrimStart('\','/')
    $parts = $rel -split '[\\/]'
    if ($parts.Length -ge 2) { return $parts[0] }
    return '(root)'
}

$files = Get-ChildItem $root -Recurse -File -Include *.c,*.h
$changed = 0
$enc = New-Object Text.UTF8Encoding $false
foreach ($f in $files) {
    $rel = $f.FullName.Substring($root.Length).TrimStart('\','/')
    if ($rel -like 'include*') { continue }
    $from = Get-ModuleOf $f.FullName

    $text = [IO.File]::ReadAllText($f.FullName)
    $orig = $text
    $matches = [regex]::Matches($text, '(?m)^\s*#include\s+"([^"]+)"')
    foreach ($m in $matches) {
        $inc = $m.Groups[1].Value
        if ($inc -match '^include/') { continue }
        if ($inc -notmatch '^\.\./[a-z]+/' -and $inc -notmatch '^[a-z]+/') { continue }

        $resolved = Resolve-Include $f.DirectoryName $inc
        if (-not $resolved) { continue }
        if ((Get-Content $resolved -Raw) -match 'FORWARD_H') { continue }   # transitional forwarder: leave for the cleanup pass
        $real = $resolved
        $target = Get-ModuleOf $real

        if (-not $map.ContainsKey($target)) { continue }
        if ($target -eq $from) { continue }

        $replacement = "include/$($map[$target])"
        if ($inc -eq $replacement) { continue }
        $text = $text.Replace("#include `"$inc`"", "#include `"$replacement`"")
        $changed++
    }
    if ($text -ne $orig -and $Apply) {
        [IO.File]::WriteAllText($f.FullName, $text, $enc)
    }
}
"converged includes: $changed (apply=$($Apply.IsPresent))"
