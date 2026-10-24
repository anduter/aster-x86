# check-deps.ps1 - module boundary checker (see docs/modules.md)
#
# Contract:
#   1. A cross-module include MUST use the interface header include/<mod>.h.
#   2. Allowed without an interface header:
#        - a module including its own headers (any relative path);
#        - lib/ (M0 libc) from anywhere;
#        - the M0 primitives core/error.h, core/atomic.h, core/spinlock.h;
#        - include/*.h (the interface layer itself, checked separately below).
#   3. Two explicit syscall-subsystem exceptions. These are internal syscall
#      contracts, NOT module public APIs, so they stay out of include/:
#        a) proc/trap/syscall_internal.h may be included by syscall group files:
#           any *_syscall.c (core/misc, fs, mm, net, proc, security, svc).
#        b) the seven group dispatch headers below may be included by the
#           syscall dispatcher proc/trap/syscall.c.
#      Every exception is listed verbatim here and in docs/modules.md sec. 8.
#   4. include/*.h must never include a *_internal.h.
#
# Includes are resolved against the including file's directory first, then
# against src/kernel (-I). This catches ../mod/x.h, ../../mod/x.h and
# plain include/<mod>.h alike.
#
# usage: powershell -File tools/check-deps.ps1 [-Repo D:\OS_Project] [-Top 20]
param(
    [string]$Repo = 'D:\OS_Project',
    [int]$Top = 20
)

$ErrorActionPreference = 'Stop'
Set-Location $Repo
$root = (Resolve-Path (Join-Path $Repo 'src\kernel')).Path
$rootPrefix = $root + '\'

$modules    = @('core','lib','mm','proc','dev','fs','net','svc','security','user','apps','include','ipc')
$primitives = @('core/error.h','core/atomic.h','core/spinlock.h')

# --- explicit syscall-subsystem exceptions (keep in sync with docs/modules.md) ---
$syscallSharedHeader = 'proc/trap/syscall_internal.h'
$syscallGroupHeaders = @(
    'core/misc_syscall.h',
    'fs/fs_syscall.h',
    'mm/mm_syscall.h',
    'net/net_syscall.h',
    'proc/proc_syscall.h',
    'security/security_syscall.h',
    'svc/svc_syscall.h'
)
$syscallDispatcher = 'proc/trap/syscall.c'

function Resolve-IncludePath([string]$dir, [string]$inc) {
    $p = $dir
    foreach ($seg in ($inc -split '[\\/]')) {
        if ($seg -eq '' -or $seg -eq '.') { continue }
        if ($seg -eq '..') { $p = Split-Path -Parent $p }
        else { $p = Join-Path $p $seg }
    }
    return $p
}

function Get-RelPath([string]$full) {
    return ($full.Substring($rootPrefix.Length) -replace '\\', '/')
}

$violations   = @()
$whitelisted  = @()
$external     = 0
$unresolved   = 0
$includeCount = 0
$files = Get-ChildItem $root -Recurse -File -Include *.c,*.h,*.asm

foreach ($f in $files) {
    $relFrom = Get-RelPath $f.FullName
    $from = ($relFrom -split '/')[0]
    if ($from -eq 'include') { continue }   # interface layer: checked separately

    $lines = Select-String -Path $f.FullName -Pattern '#include\s+"([^"]+)"'
    foreach ($m in $lines) {
        $inc = $m.Matches[0].Groups[1].Value
        $includeCount++

        $abs = Resolve-IncludePath (Split-Path -Parent $f.FullName) $inc
        if (-not (Test-Path -LiteralPath $abs)) {
            # quoted include may also resolve through -Isrc/kernel
            $alt = Resolve-IncludePath $root $inc
            if (Test-Path -LiteralPath $alt) { $abs = $alt }
            else { $unresolved++; continue }
        }
        $full = [System.IO.Path]::GetFullPath($abs)
        if (-not $full.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $external++   # e.g. src/user/... or generated headers: outside this check
            continue
        }

        $relTo = Get-RelPath $full
        $to = ($relTo -split '/')[0]
        if ($modules -notcontains $to) { continue }        # root files: types.h, io.h, linker.ld
        if ($to -eq 'include') { continue }                # interface header: allowed
        if ($to -eq $from) { continue }                    # same module
        if ($to -eq 'lib' -or $from -eq 'lib') { continue } # M0 libc is shared
        if ($primitives -contains $relTo) { continue }      # M0 primitives

        # exception (a): syscall subsystem shared helper header
        $leaf = Split-Path $relFrom -Leaf
        if ($relTo -eq $syscallSharedHeader -and
            ($leaf -match '_syscall\.c$' -or $leaf -eq 'syscall.c' -or $leaf -eq 'syscall_util.c')) {
            $whitelisted += [pscustomobject]@{
                Reason = 'syscall internal helper'; From = $from; To = $to
                Header = $inc; File = "src/kernel/$relFrom"; Line = $m.LineNumber
            }
            continue
        }

        # exception (b): syscall dispatcher includes the seven group headers
        if ($relFrom -eq $syscallDispatcher -and ($syscallGroupHeaders -contains $relTo)) {
            $whitelisted += [pscustomobject]@{
                Reason = 'syscall group dispatch'; From = $from; To = $to
                Header = $inc; File = "src/kernel/$relFrom"; Line = $m.LineNumber
            }
            continue
        }

        $violations += [pscustomobject]@{
            From = $from; To = $to; Header = $inc
            File = "src/kernel/$relFrom"; Line = $m.LineNumber
        }
    }
}

# interface headers must not leak internal headers
$ifViolations = @()
Get-ChildItem (Join-Path $root 'include') -File -Filter *.h | ForEach-Object {
    Select-String -Path $_.FullName -Pattern '#include\s+"([^"]*_internal\.h)"' | ForEach-Object {
        $ifViolations += [pscustomobject]@{
            File = "src/kernel/include/$($_.Filename)"
            Line = $_.LineNumber
            Header = $_.Matches[0].Groups[1].Value
        }
    }
}

"checked: $($files.Count) files, $includeCount quoted includes"
"interface layer violations (include/*.h -> *_internal.h): $($ifViolations.Count)"
"whitelisted syscall-subsystem includes: $($whitelisted.Count)"
"module include violations: $($violations.Count)"
if ($external -gt 0) { "  (includes outside src/kernel, ignored: $external)" }
if ($unresolved -gt 0) { "  (unresolved includes, ignored: $unresolved)" }
""

if ($whitelisted.Count -gt 0) {
    "--- whitelisted (explicit, see script header + docs/modules.md sec. 8) ---"
    $whitelisted | Sort-Object File,Line | Format-Table -AutoSize
    ""
}

if ($ifViolations.Count -gt 0) {
    "--- interface header violations ---"
    $ifViolations | Format-Table -AutoSize
    ""
}

if ($violations.Count -gt 0) {
    "--- by edge (from -> to) ---"
    $violations | Group-Object From,To | Sort-Object Count -Descending |
        Select-Object -First $Top Count,Name | Format-Table -AutoSize

    "--- violations (first $Top) ---"
    $violations | Sort-Object File,Line | Select-Object -First $Top |
        Format-Table -AutoSize
} else {
    "OK: no cross-module include violations."
}

if ($ifViolations.Count -gt 0 -or $violations.Count -gt 0) { exit 1 }
exit 0
