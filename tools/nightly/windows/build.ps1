#!/usr/bin/env pwsh
# Build kicad-cli + the pcbnew and eeschema kifaces with MSVC and the vcpkg dependencies
# installed by install-deps.ps1.  Needs an MSVC developer environment (ilammy/msvc-dev-cmd
# in CI, or a "x64 Native Tools" prompt locally) and VCPKG_ROOT/NIGHTLY_TRIPLETS from
# setup-vcpkg.ps1.
#
# Env: BUILD_DIR (default build\nightly), BUILD_TYPE (Release)
$ErrorActionPreference = "Stop"

$root = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$buildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $root "build\nightly" }
$buildType = if ($env:BUILD_TYPE) { $env:BUILD_TYPE } else { "Release" }
$toolchain = Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake"

Write-Host "source : $root"
Write-Host "build  : $buildDir ($buildType)"

cmake -S $root -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$buildType" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    "-DVCPKG_OVERLAY_TRIPLETS=$env:NIGHTLY_TRIPLETS" `
    -DVCPKG_MANIFEST_INSTALL=OFF `
    "-DVCPKG_INSTALLED_DIR=$buildDir\vcpkg_installed" `
    -DKICAD_BUILD_QA_TESTS=OFF `
    -DKICAD_BUILD_I18N=OFF `
    -DKICAD_USE_SENTRY=OFF `
    -DKICAD_UPDATE_CHECK=OFF `
    -DKICAD_INSTALL_DEMOS=OFF `
    -DKICAD_WIN32_INSTALL_PDBS=OFF `
    -DKICAD_USE_PCH=ON
if ($LASTEXITCODE) { exit $LASTEXITCODE }

cmake --build $buildDir --target kicad-cli pcbnew_kiface eeschema_kiface
exit $LASTEXITCODE
