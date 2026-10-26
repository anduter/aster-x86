# =============================================================================
# tests/audit/smp/check.ps1 - assert the audit-smp serial selftest lines
#
#   powershell -NoProfile -File tests\audit\smp\check.ps1 -Log obj\audit\smp.log
#   powershell -NoProfile -File tests\audit\smp\check.ps1 -Log obj\audit\smp4.log
#
# Exit 0 = all audit lines present and no panic/oops; 1 = missing line or panic.
# =============================================================================
param(
    [string]$Log = 'obj/audit/smp.log',
    [int]$Cpus = 0
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding $false } catch { }
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$path = if ([IO.Path]::IsPathRooted($Log)) { $Log } else { Join-Path $root $Log }
if (-not (Test-Path $path)) { Write-Host ("check: missing log {0}" -f $path) -ForegroundColor Red; exit 1 }
$s = [string](Get-Content $path -Raw)

$checks = @(
    @{ Name = 'sigreturn frame validation'; Ok = ($s -match 'audit: sigreturn frame validation ok') },
    @{ Name = 'kill uid policy';           Ok = ($s -match 'audit: kill uid policy ok') },
    @{ Name = 'uaccess user window';       Ok = ($s -match 'audit: uaccess user window ok') },
    @{ Name = 'smp selftest start';        Ok = ($s -match 'audit: smp selftest start \(cpus=[24]\)') },
    @{ Name = 'spawn race';                Ok = ($s -match 'audit: smp spawn race PASS') },
    @{ Name = 'wake-vs-block';             Ok = ($s -match 'audit: smp wake-vs-block PASS') },
    @{ Name = 'vmalloc concurrent';        Ok = ($s -match 'audit: mm vmalloc concurrent PASS') },
    @{ Name = 'smp selftest PASS';         Ok = ($s -match 'audit: smp selftest PASS') },
    @{ Name = 'no panic/oops';             Ok = (-not ($s -cmatch '(?m)^(\*\*\* KERNEL PANIC|oops:|panic:)')) }
)
if ($Cpus -gt 0) {
    $checks += @{ Name = ('start reports {0} cpus' -f $Cpus);
                  Ok = ($s -match ('audit: smp selftest start \(cpus={0}\)' -f $Cpus)) }
}

$failed = 0
foreach ($c in $checks) {
    if ($c.Ok) { Write-Host ("  [PASS] {0}" -f $c.Name) -ForegroundColor Green }
    else { Write-Host ("  [FAIL] {0}" -f $c.Name) -ForegroundColor Red; $failed++ }
}
if ($failed -gt 0) { Write-Host ("check: FAIL ({0} missing)" -f $failed) -ForegroundColor Red; exit 1 }
Write-Host ("check: PASS ({0}/{0})" -f $checks.Count) -ForegroundColor Green
exit 0