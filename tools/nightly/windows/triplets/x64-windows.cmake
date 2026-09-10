# Nightly overlay triplet: the repository's x64-windows triplet (tools/custom_vcpkg_triplets)
# with release-only builds of the dependencies, which halves the cold vcpkg build.  The same
# overlay directory must be passed to `vcpkg install` and to CMake (VCPKG_OVERLAY_TRIPLETS)
# so the ABI hashes, and therefore the binary cache, line up.
# Kept inline rather than include()d from tools/custom_vcpkg_triplets so that this overlay
# does not depend on where the tooling checkout sits relative to the source checkout.
include("${VCPKG_ROOT_DIR}/triplets/x64-windows.cmake")

set(VCPKG_DISABLE_COMPILER_TRACKING ON)
set(VCPKG_BUILD_TYPE release)
