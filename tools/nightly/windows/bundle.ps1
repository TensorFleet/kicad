#!/usr/bin/env pwsh
# Stage a relocatable Windows tree of the headless kicad-cli from a build directory:
#
#   kicad-cli\
#     bin\kicad-cli.exe
#     bin\_pcbnew.dll  bin\_eeschema.dll      kifaces (KIFACE_SUFFIX is .dll on Windows);
#     bin\_cvpcb.dll                          KIWAY loads them from the executable's directory;
#                                            eeschema's ERC loads _cvpcb too
#     bin\ki*.dll                             KiCad's own shared libraries (kicommon, kigal,
#                                            kiapi, kicad_3dsg)
#     bin\*.dll                               every vcpkg runtime DLL + the MSVC runtime
#     share\kicad\schemas  share\kicad\template   GetStockDataPath() = <exe dir>\..\share\kicad
#     KICAD_COMMIT VERSION
#
# Usage: bundle.ps1 -BuildDir <build dir> -Out <staging dir>
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = "Stop"

$src = if ($env:KICAD_SRC) { (Resolve-Path $env:KICAD_SRC).Path } else { (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path }
$BuildDir = (Resolve-Path $BuildDir).Path
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
$bin = New-Item -ItemType Directory -Force -Path (Join-Path $Out "bin")
$share = New-Item -ItemType Directory -Force -Path (Join-Path $Out "share\kicad")
$Out = (Resolve-Path $Out).Path

function Find-One([string]$name) {
    $hits = @(Get-ChildItem -Path $BuildDir -Recurse -File -Filter $name |
        Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' -and $_.FullName -notmatch '\\vcpkg_installed\\' })
    if ($hits.Count -eq 0) { throw "no $name under $BuildDir" }
    if ($hits.Count -gt 1) {
        Write-Warning "$($hits.Count) copies of $name under $BuildDir; taking the newest"
        $hits = @($hits | Sort-Object LastWriteTime -Descending)
    }
    return $hits[0].FullName
}

foreach ($name in "kicad-cli.exe", "_pcbnew.dll", "_eeschema.dll", "_cvpcb.dll") {
    Copy-Item (Find-One $name) $bin
}

# KiCad's own shared libraries: kicommon, kigal, kiapi and kicad_3dsg (3d-viewer/3d_cache/sg,
# imported by _pcbnew).  Globbed like the Linux and macOS bundles do, so a new one cannot be
# forgotten; the import check below fails the build if anything is still missing.
$own = Get-ChildItem -Path $BuildDir -Recurse -File -Filter "ki*.dll" |
    Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' -and $_.FullName -notmatch '\\vcpkg_installed\\' } |
    Group-Object Name | ForEach-Object { $_.Group | Sort-Object LastWriteTime -Descending | Select-Object -First 1 }
if (-not $own) { throw "no ki*.dll under $BuildDir" }
foreach ($dll in $own) { Copy-Item $dll.FullName $bin }
Write-Host ("KiCad DLLs: {0}" -f (($own | ForEach-Object Name) -join ", "))

# Dependencies: every release DLL vcpkg installed (a superset of what the three binaries
# import, which keeps the walk out of this script) and the MSVC runtime from the toolset.
Copy-Item (Join-Path $BuildDir "vcpkg_installed\x64-windows\bin\*.dll") $bin
if ($env:VCToolsRedistDir) {
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
    if ($crt) { Copy-Item (Join-Path $crt.FullName "*.dll") $bin }
    else { Write-Warning "no Microsoft.VC*.CRT under $env:VCToolsRedistDir\x64; the MSVC runtime is not bundled" }
} else {
    Write-Warning "VCToolsRedistDir is not set; the MSVC runtime is not bundled"
}

# Import check (the Windows counterpart of the `ldd | grep 'not found'` on Linux): every
# static import of the executable, the kifaces and the DLLs must resolve in bin\ or System32.
& (Join-Path $PSScriptRoot "import-check.ps1") -Bin $bin
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Copy-Item -Recurse (Join-Path $src "api\schemas") (Join-Path $share "schemas")
Copy-Item -Recurse (Join-Path $src "resources\project_template") (Join-Path $share "template")

$sha = if ($env:NIGHTLY_SHA) { $env:NIGHTLY_SHA } else { (git -C $src rev-parse HEAD).Trim() }
$version = if ($env:NIGHTLY_VERSION) { $env:NIGHTLY_VERSION } else { (git -C $src describe --match "[0-9]*" --always).Trim() }
Set-Content -Path (Join-Path $Out "KICAD_COMMIT") -Value $sha
Set-Content -Path (Join-Path $Out "VERSION") -Value $version

$size = (Get-ChildItem -Recurse -File $Out | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Host ("staged {0}: {1} DLLs, {2:N0} MB" -f $Out, (Get-ChildItem $bin -Filter *.dll).Count, $size)

& (Join-Path $bin "kicad-cli.exe") version
exit $LASTEXITCODE
# The full smoke test (tools/nightly/smoke.sh: DRC + ERC through the kifaces) runs from a
# bash step in the workflow, the same script as on Linux and macOS.
