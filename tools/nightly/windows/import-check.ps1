#!/usr/bin/env pwsh
# Import check for a staged Windows bin\ directory, the counterpart of the Linux bundle's
# `ldd | grep 'not found'`: walk the static imports of every .exe and .dll in the directory
# with dumpbin (from the MSVC developer environment) and fail if one resolves neither in the
# directory itself nor in System32.  LoadLibrary reports such a hole only as "error 126" with
# no module name, so find it at bundle time instead.
#
# Usage: import-check.ps1 -Bin <dir> [-Dumpbin <path>] [-System32 <dir>]
param(
    [Parameter(Mandatory = $true)][string]$Bin,
    [string]$Dumpbin = "",
    [string]$System32 = ""
)
$ErrorActionPreference = "Stop"

if (-not $Dumpbin) {
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $cmd) { Write-Warning "dumpbin.exe not on PATH (no MSVC developer environment); skipping the import check"; exit 0 }
    $Dumpbin = $cmd.Source
}
if (-not $System32) { $System32 = Join-Path $env:SystemRoot "System32" }

# Names dumpbin prints for one file: the block after "has the following dependencies" up to
# "Summary", one DLL per line.
function Get-Imports([string]$file) {
    $inList = $false
    foreach ($line in (& $Dumpbin /nologo /dependents $file 2>$null)) {
        if ($line -match 'has the following dependencies') { $inList = $true; continue }
        if ($line -match '^\s*Summary') { break }
        if (-not $inList) { continue }
        $dep = $line.Trim()
        if ($dep -match '\.dll$') { $dep }
    }
}

$have = @{}
foreach ($f in Get-ChildItem -Path $Bin -File -Filter *.dll) { $have[$f.Name.ToLower()] = $true }
# Import names are compared case-insensitively, as the Windows loader does (dumpbin prints
# them as the importing binary spelled them, e.g. KERNEL32.dll).
$system = @{}
foreach ($f in Get-ChildItem -Path $System32 -File -Filter *.dll -ErrorAction SilentlyContinue) { $system[$f.Name.ToLower()] = $true }

$missing = [ordered]@{}
$checked = 0
foreach ($f in Get-ChildItem -Path $Bin -File | Where-Object { $_.Extension -in ".exe", ".dll" }) {
    $checked++
    foreach ($dep in Get-Imports $f.FullName) {
        $key = $dep.ToLower()
        if ($have[$key]) { continue }
        if ($key -like 'api-ms-win-*' -or $key -like 'ext-ms-*') { continue }   # API sets, always present
        if ($system[$key]) { continue }
        if (-not $missing.Contains($dep)) { $missing[$dep] = $f.Name }
    }
}

if ($missing.Count) {
    foreach ($m in $missing.GetEnumerator()) { Write-Host ("missing import: {0} (first needed by {1})" -f $m.Key, $m.Value) }
    throw "$($missing.Count) import(s) resolve neither in $Bin nor in System32"
}
Write-Host ("import check: every static import of {0} files in {1} resolves" -f $checked, $Bin)
