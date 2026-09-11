# wasm-prep status (2026-09-09)

Branch `wasm`, build dir `build/headless` (now configured with
`-DKICAD_HEADLESS_WX_BASE_ONLY=ON`, which is also the new default under the option).

## Gate: PASSES, unchanged numbers

- `kicad-api-host-native --selftest qa/data/pcbnew/api_kitchen_sink.kicad_pcb` --
  **13 checks, 0 failures**, exit 0.
- fab_pcb conformance over `KICAD_TRANSPORT=stdio`: **174/177, 150/153 headless
  commands**, 15 gui-only skips, 6/6 extra checks; `roundtrip.kicad.test.ts` green
  (72 items, 13 types, `SaveDocumentToString: identical`).  Same three failures as
  before: `RunBoardJobExport3D`, `RunBoardJobExportRender` (no OpenCascade / 3D
  viewer) and `RunSchematicJobExportBOM` (`FIELDS_TABLE_DATA_MODEL_BASE` derives from
  `wxGridTableBase`).
- `qa_api`: **162 cases, no errors**, rebuilt and run; see the last section.

## The link line: only what the wasm toolchain has

`otool -L build/native-host/kicad-api-host-native`, abseil elided:

```
/usr/lib/libz.1.dylib
/opt/homebrew/opt/freetype/lib/libfreetype.6.dylib
/opt/homebrew/opt/harfbuzz/lib/libharfbuzz.0.dylib
/opt/homebrew/opt/zstd/lib/libzstd.1.dylib
/opt/homebrew/opt/protobuf/lib/libprotobuf.36.1.0.dylib
/opt/homebrew/opt/protobuf/lib/libutf8_validity.36.1.0.dylib
/opt/homebrew/opt/abseil/lib/libabsl_*.dylib          (89 of them)
/opt/homebrew/opt/wxwidgets@3.2/lib/libwx_baseu-3.2.dylib
/opt/homebrew/opt/wxwidgets@3.2/lib/libwx_baseu_xml-3.2.dylib
/usr/lib/libSystem.B.dylib   /usr/lib/libc++.1.dylib
CoreFoundation Security IOKit Carbon Cocoa QuartzCore AudioToolbox OpenGL   (frameworks)
```

Gone: cairo, pixman, fontconfig, git2, nng, jpeg, png, iconv, and every wxCore
library.  The macOS **frameworks are wx-config's own list**, not a KiCad dependency --
`nm -u` on the binary finds zero `gl*` symbols, and Emscripten has no frameworks at
all, so they are noise here.

## What changed (four commits)

1. **`Headless: drop nng, libgit2, Cairo and Fontconfig from the headless link`**
   - *nng*: `KICAD_API_SERVER`'s socket half (`Start`, `onApiRequest`,
     `handleApiEvent`, `handleApiRequestString`, the publisher members) compiles out;
     `StartInProcess`/`DispatchBytes`/`WaitForRequest` untouched.  `libs/kinng` is not
     built.  `CROSS_PROBE_CLIENT` keeps `RegisterPeer`/`IsOnStandardSocketPath`, so
     **`CrossProbeAnnounce` still answers** -- only the outbound half is a no-op, and
     no conformance command was lost.
   - *libgit2*: `KICOMMON_VCS_SRCS` now also names `local_history.cpp`,
     `history_lock.cpp` and `text_eval/text_eval_vcs.cpp`, and the whole group is
     subtracted.  `common/headless/vcs_stub.cpp` is the "not under version control"
     spelling of `LOCAL_HISTORY` and the `${VCS_*}` text variables; `${VCSHASH}`
     answers `no hash`; the merge drivers (`pcbnew/git/kigit_pcb_merge.cpp` and the
     two header-only eeschema ones) are excluded at their call sites.  **Saving is
     unaffected** -- local history only ever ran after the file was written.
   - *Cairo/Pixman*: `PNG_plotter.cpp` -> `common/plotters/PNG_plotter_stub.cpp`.
     `PLOT_FORMAT::PNG` still exists and constructs, `OpenFile`/`StartPlot` report
     "PNG output is not available in this build" and fail; `plotter_png.h`
     forward-declares the two cairo types.
   - *Fontconfig*: `thirdparty/nanosvg` is compiled with `NANOSVG_NO_FONTCONFIG` (it
     carries its own UTF-8 decoder now and skips text in an imported SVG);
     `libemf2svg` and `libwmf` are not built and eeschema's OLE preview decoders
     become `sch_io/ole_image_stub.cpp` (the OrCAD reader itself is untouched, it just
     gets no bitmap preview).
   - Also: `KIWAY::KiFACE`'s dlopen path is guarded (the Emscripten wxBase is built
     `wxUSE_DYNAMIC_LOADER=OFF`, so `wxDynamicLibrary` does not exist), and
     `zone_filler.cpp`'s three `KIDIALOG` prompts are guarded -- 393 of the wxCore
     references and unreachable with no frame.
   - The `find_package` calls are inside `if( NOT KICAD_HEADLESS_API )` with an
     `unset( ... CACHE )` in the else branch: a plain `set( VAR )` **unsets the normal
     variable and lets the cache entry show through**, which is how libgit2 stayed on
     the line for one iteration.

2. **`Headless: an in-tree wxPGChoices for the wxBase-only build`**
   `wxPGChoices` is wxPropertyGrid's, and its entries carry a `wxPGCell` holding a
   `wxBitmapBundle`, a `wxColour` and a `wxFont`.  KiCad uses it as a plain
   label/value list from *file-scope* registration constructors, so a stub aborted
   before `main()`.  `include/properties/pg_choices_headless.h` defines
   `wxPGChoiceEntry`/`wxPGChoicesData`/`wxPGChoices` with the same names and the
   subset of the API the headless sources use, over `std::vector`; `property.h`
   includes it instead of `wx/propgrid/property.h` under the option.  No call site
   changed.  (`lib_symbol.cpp` needed an explicit `<wx/tokenzr.h>` that propgrid used
   to drag in.)

3. **`Headless: link against wxBase only, with real wxColour and wxImage`**
   462 undefined wxCore symbols, split three ways:
   - **Abort stubs** for the GUI ones (`wxWindow` 65 + `wxWindowBase` 66,
     `wxGridTableBase` 26, `wxDialog`, `wxDataViewModel`, `wxStyledTextCtrl`, `wxDC`,
     `wxMenu`...).  424 of the references come from `eeschema/sheet.cpp` alone.
     `tools/regen_link_stubs.sh` now also `nm`s the wx dylibs for the mangled spelling
     and the T/D nature of a symbol -- the role `build/dev` plays for KiCad's own --
     and `DEVSYMS_CACHE=<path>` keeps that scan across runs (it is minutes otherwise).
     **1062 stubs** now, up from 682.
   - **Real implementations** in `host/wx_headless/` (static lib `wx_headless_shim`,
     linked into `kicad_api_host` when the option is on or under Emscripten), for the
     value types the headless paths actually execute.  These supply the *out-of-line*
     half of wx's own class declarations, so there is no second definition and no ABI
     of our own; defining all the virtuals in one TU is also what makes the compiler
     emit the vtable.
     - `wx_colour.cpp`: `wxColour` (COLOR4D conversions, the file-scope colour tables
       in `stackup_predefined_prms.cpp`, colour settings parsing) plus
       `wxColourBase::FromString`/`GetAsString` -- `#RRGGBB[AA]`, `rgb()/rgba()`, and
       a 16-entry name table in place of the wxCore colour database.
     - `wx_image.cpp`: `wxImage` -- `BITMAP_BASE` builds one *while the board file is
       being parsed*.  Full pixel container (RGB plane, alpha plane, mask, options,
       Scale/Rotate90/Paste/ConvertToGreyscale).  **No codecs**: `LoadFile` reads the
       geometry out of the container header instead -- PNG `IHDR` + `pHYs`, JPEG
       `SOFn` + JFIF `APP0`, BMP, GIF -- so size and resolution are right and the
       raster is blank.  Because `BITMAP_BASE` keeps the undecoded bytes, a board with
       a reference image still round-trips byte for byte (the roundtrip test proves
       it).  `SaveFile` always fails.
   - **Call sites that should not have been there**: `wxBitmap` in `BITMAP_BASE` (the
     device-side copy only `DrawBitmap` reads), `wxInitAllImageHandlers` in
     `PGM_BASE::InitPgm`, and `wxSafeYield` in `PCB_PLOTTER::Plot` (commit 4 -- it
     took all four board plot jobs down).
   `main_native` now leaves through `_exit()` after `kiapi_shutdown`: the file-scope
   teardown left at `exit()` walks a `wxAnyValueType` registration whose type is a
   stub, and segfaults with everything already done.

4. **`Headless: no wxSafeYield on the board plot path`**

## For the wasm-build agent: what will bite

- **`wx/bitmap.h`, `wx/font.h`, `wx/dc.h`, `wx/window.h` do not declare their classes
  in a `--disable-gui` wxBase.**  This is the big one and it is *not* visible from the
  native gate, where the headers come from a GUI-enabled install and only the
  libraries were narrowed.  Every KiCad TU that names `wxBitmap`, `wxFont`, `wxDC`,
  `wxWindow`, `wxImage` or `wxColour` will fail to **compile**, not just to link.
  Plan on an in-tree header shim on the include path ahead of wx's (the plan's
  Decision 2), reusing `host/wx_headless/*.cpp` as the implementations, or on
  compiling the wasm wxBase with `wxUSE_GUI=0` but the GUI headers still installed.
  `host/wx_headless` is deliberately implementation-only so it can move either way.
- **`std::thread` in the linked set**: `common/api/api_job_registry.cpp` (already
  inert with inline mode on), `eeschema/sch_io/database/sch_io_database.cpp` and
  `sch_io/http_lib/sch_io_http_lib.cpp` (background library refresh -- only
  constructed when such a library is opened, so it throws rather than links badly),
  `pcbnew/pcbnew.cpp` / `eeschema/eeschema.cpp` `std::async` (already guarded).
- **`wxExecute`/`wxProcess`**: `common/gestfich.cpp` and
  `eeschema/netlist_exporters/netlist_generator.cpp` are still built.  Neither is on
  the API path (`api_plugin_manager` and `python_manager` are excluded), but
  `netlist_generator` is reachable from a *custom* netlist exporter job.  Guard or
  accept a runtime failure.
- **`flock` / `sys/file.h`**: only in `KICAD_API_SERVER::Start()`, which is now inside
  the `#ifndef KICAD_HEADLESS_API` block.  Nothing else uses it.
- **`mmap`**: `libs/kiplatform/os/unix/io.cpp` `MAPPED_FILE`.  Emscripten supports
  read-only `mmap` on MEMFS, so this should be fine; `posix_fadvise` is already
  guarded.
- **`wxGetHomeDir`**: `common/jobs/jobs_output_archive.cpp`,
  `jobs_output_folder.cpp`, `eeschema/sch_io/geda/sch_io_geda.cpp` expand a leading
  `~`.  Set `HOME` in the module's environment (the host config already does) and
  this is harmless; nothing calls `getpwuid`.
- **Signal handlers**: only `common/navlib_safe_init.cpp`, which is in
  `KICOMMON_GUI_SRCS` and not built.
- `libz` is on the native line from the system; the wasm build uses the zlib port.
- The `-force_load` / `--whole-archive` requirement from STATUS-native-gate.md is
  unchanged and still load-bearing.

## Reproducing

```
cmake -S . -B build/headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DKICAD_HEADLESS_API=ON \
  -DKICAD_HEADLESS_WX_BASE_ONLY=ON -DKICAD_HEADLESS_ALLOW_UNDEFINED=OFF ...
ninja -C build/headless -j10 kicad-api-host-native
# after any change that adds or removes a definition the stubs shadow:
WXLIBDIR=/opt/homebrew/lib DEVSYMS_CACHE=/tmp/devsyms.cache \
  tools/regen_link_stubs.sh build/headless build/dev
./build/native-host/kicad-api-host-native --selftest qa/data/pcbnew/api_kitchen_sink.kicad_pcb
cd ../fab_pcb/packages/client && KICAD_TRANSPORT=stdio bun run test:conformance
```

**A stale stub silently shadows a real definition** -- that is how the wxPGChoices and
wxImage implementations appeared to do nothing for one cycle each.  Regenerate after
every commit that adds a definition.

## qa_api: green, and the option-OFF regression with it

`ninja -C build/qa qa_api` then `./build/qa/qa/tests/api/qa_api`:
**162 test cases, no errors detected.**

That rebuild is also the option-OFF regression check: `property.h` is included nearly
everywhere, so it recompiled ~1300 translation units of the default tree -- all of
common/, pcbnew/ and eeschema/ -- with **zero compile failures**.  Every guard added
on this branch is inside `#ifndef KICAD_HEADLESS_API` or an
`if( NOT KICAD_HEADLESS_API )`, and no entry moved in the default build's source or
link lists.
