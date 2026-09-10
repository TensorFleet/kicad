# Real headless implementations of the few wxCore *value* types the API path executes.
#
# Everything else wxCore declares that the kept sources still reference is served by the
# generated abort stubs in host/headless_link_stubs.cpp -- those are code paths that only run
# with a frame.  What lands here is the opposite: types that are constructed and read on the
# headless path (COLOR4D conversions, the file-scope colour tables), where a stub would abort
# in a static initializer before main() ever runs.
#
# The class declarations come from wx's own headers; these files supply the out-of-line half
# only, so there is no second definition of anything and no ABI of our own.

add_library( wx_headless_shim STATIC
    wx_colour.cpp
    wx_image.cpp
    )

target_include_directories( wx_headless_shim SYSTEM PRIVATE
    ${wxWidgets_INCLUDE_DIRS}
    )

target_compile_definitions( wx_headless_shim PRIVATE ${wxWidgets_DEFINITIONS} )

set_target_properties( wx_headless_shim PROPERTIES
    COMPILE_FLAGS "${wxWidgets_CXX_FLAGS}"
    )
