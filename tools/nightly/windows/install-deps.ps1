#!/usr/bin/env pwsh
# Build/restore the vcpkg dependencies of vcpkg.json into $BUILD_DIR\vcpkg_installed.
# Run after setup-vcpkg.ps1.  Ports come from the binary cache when they were built before;
# --clean-after-build keeps the runner's disk from filling with build trees.
$ErrorActionPreference = "Stop"

$root = if ($env:GITHUB_WORKSPACE) { $env:GITHUB_WORKSPACE } else { (Get-Location).Path }
$buildDir = if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $root "build\nightly" }
$vcpkg = Join-Path $env:VCPKG_ROOT "vcpkg.exe"

$installRoot = Join-Path $buildDir "vcpkg_installed"

& $vcpkg install `
    "--triplet=x64-windows" `
    "--x-manifest-root=$root" `
    "--x-install-root=$installRoot" `
    "--overlay-triplets=$env:NIGHTLY_TRIPLETS" `
    --x-abi-tools-use-exact-versions `
    --clean-after-build
exit $LASTEXITCODE
