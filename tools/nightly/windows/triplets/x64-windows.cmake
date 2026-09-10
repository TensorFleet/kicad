# Nightly overlay triplet: the repository's x64-windows triplet (tools/custom_vcpkg_triplets)
# with release-only builds of the dependencies, which halves the cold vcpkg build.  The same
# overlay directory must be passed to `vcpkg install` and to CMake (VCPKG_OVERLAY_TRIPLETS)
# so the ABI hashes, and therefore the binary cache, line up.
include("${CMAKE_CURRENT_LIST_DIR}/../../../custom_vcpkg_triplets/x64-windows.cmake")

set(VCPKG_BUILD_TYPE release)
