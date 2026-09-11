# wasm-build status (2026-09-09)

**The module builds, links, loads and answers the API.**  `build/wasm/host/kicad_api.js`
(224 KB) + `kicad_api.wasm` (**37.4 MB**, no `.data`).  Under bun:
`createKiCadWasm()` → **init 104 ms**, **Ping 0.006 ms** (200 iterations, in-process — there is
no IPC), `GetVersion` → `10.99.0-3789-g8376472d4d`.  `packages/kicad-wasm` `bun test`:
**12 pass, 0 fail**.  Conformance under `KICAD_TRANSPORT=wasm`: **125/153 headless commands**
against stdio's 150/153, and the whole 25-command gap is one harness bug (§5), not the module.
The native gate is unchanged (§6).

## 1. The wx header question: option (a), and it cost ~25 minutes, not 90

A `--disable-gui` wx installs ~170 base headers and, with `wxUSE_GUI 0`, does not DECLARE
wxBitmap/wxFont/wxDC/wxWindow/wxImage/wxColour/wxGridTableBase/wxCommandEvent.  Chosen fix:
`host/wx_headless/wxgui/wx/setup.h` — the installed setup.h with `wxUSE_GUI` and 120 other
feature flags forced back on and the **wxGTK3** port selected, over the FULL 3.2.11 header set
(`build-deps.sh` now rsyncs the source `include/wx` beside the base-only install).

Why it was cheap: **wxGTK's public headers name no toolkit header** (only
`wx/gtk/webview_webkit.h`, never included).  A 60-header probe TU compiled clean first try.

ABI: `tools/wasm/check_wx_abi.sh` compiles the same `sizeof` table under both setup.h variants
and diffs — **23 classes, identical** (wxObject, wxEvtHandler, wxAppConsole, wxConsoleAppTraits,
wxLog, wxStandardPaths, wxFileName, wxFileConfig, wxVariant, wxEvent…).  The `wxUSE_GUI`
conditionals in those headers only add *separate derived* classes.  Option (b) was not needed.

## 2. Every Emscripten guard added

- `CMakeLists.txt`: `UNIX_NOT_APPLE` is now OFF under Emscripten (Wayland, libspnav, flatpak,
  link maps are desktop-Linux); `common/CMakeLists.txt`'s two spacenav blocks follow it.
- `include/kiway.h`: `__EMSCRIPTEN__` branch for `LIB_ENV_VAR`.
- `include/font/fontconfig.h`: no `<fontconfig/fontconfig.h>` under `KICAD_HEADLESS_API`.
- `common/eda_text.cpp`: `<wx/uri.h>`; `<wx/url.h>` needs wxUSE_SOCKETS.
- Three transitive includes a GUI wx used to supply: `<wx/log.h>` in `api_server_host.cpp`,
  `<ki_exception.h>` in `headless_symbol_context.cpp`, `<kiway_player.h>` in `pcbnew.cpp`.
- `host/wx_headless/wx_colour.cpp`: `#ifdef __WXOSX__` / `__WXGTK__` port split.
- `host/wasm/main_wasm.cpp`: `EM_JS` event sink → `Module.__kiapiEvent(HEAPU8.subarray(...))`,
  installed from `main()`; `setlocale(LC_ALL,"C.UTF-8")` in a `constructor(101)` so it beats the
  file-scope constructors (musl's "C" locale silently empties `wxString::Format`).
- Untouched and fine: mmap, flock, getpwuid, signals, std::thread, std::filesystem.
- Cosmetic only: `KICAD_BUILD_ARCH_X86` is defined (wasm32 has 4-byte pointers).  Its two users
  are `update_manager.cpp` and `kicad_curl_easy.cpp`; neither is built.

## 3. The two link problems, both structural

**(a) Undefined DATA.**  `-sERROR_ON_UNDEFINED_SYMBOLS=0` imports missing *functions* and gives
each a JS stub that throws with the symbol's name — the same diagnostic the native abort stubs
give, and it works (the BOM failure below aborts in `FIELDS_TABLE_DATA_MODEL_BASE::…C2Ev`,
named exactly).  wasm-ld cannot do that for data.  `tools/wasm/gen_wasm_link_stubs.py` takes
undefined-minus-defined from `llvm-nm` over the real link line, splits it with `llvm-cxxfilt`
(no parameter list ⇒ data) and emits a zeroed placeholder each: **71 data stubs, 792 functions
left to Emscripten**.  Regenerate with `tools/wasm/regen_wasm_link_stubs.sh` after any change
that adds a definition — a stale stub shadows a real one.
*The trap*: emcc appends libc++/libc++abi/compiler-rt/libc, which are NOT on the ninja command
line.  Without the `--defined-only` pass over the sysroot the generator stubs `std::cout`,
`std::nothrow` and every `std::*::id`, replacing the real ones.
Five C-linkage globals deserve real values instead and live in `host/wasm/wasm_missing_globals.cpp`
(`traceDatabase`, `traceHTTPLib`, `EDA_LANG_CHANGED`, `wxDefaultPosition`, `wxDefaultSize`) —
note the explicit `extern`, since a namespace-scope `const` has internal linkage.

**(b) The FreeType port has setjmp variants.**  `ftgrays.c` uses setjmp, so the port ships
`libfreetype.a` (JS longjmp, with `invoke_*` imports), `-wasmsjlj` and `-legacysjlj`.
`-sUSE_FREETYPE=1` picks correctly; KiCad's `find_package( Freetype )` puts an explicit path on
the link line and bypasses that, and `-fwasm-exceptions` implies `SUPPORT_LONGJMP=wasm`, so emcc
asserts *"invoke_ functions exported but exceptions and longjmp are both disabled"*.
`configure.sh` pins `FREETYPE_LIBRARY_RELEASE` to `-legacysjlj`.

## 4. Configure and link flags

`tools/wasm/configure.sh` (reproducible; `tools/wasm/env.sh` supplies the rest).  On top of
`$WASM_CMAKE_ARGS`: `CMAKE_FIND_ROOT_PATH=$WASM_PREFIX;$EM_SYSROOT;$GLM_PREFIX` (the emscripten
port sysroot and GLM must be named because we override the toolchain's), `GLM_INCLUDE_DIR`
(header-only, arch independent — the host copy is correct), `FREETYPE_LIBRARY_RELEASE`,
`LEMON_EXE=build/headless/thirdparty/lemon/lemon`, `Protobuf_PROTOC_EXECUTABLE`=brew 36.1,
`wxWidgets_INCLUDE_DIRS/_LIBRARIES/_DEFINITIONS`, `KICAD_HEADLESS_API=ON`.

Link (`host/wasm/CMakeLists.txt`): `-Wl,--whole-archive $<TARGET_FILE:kicad_api_host>
-Wl,--no-whole-archive` (the `-force_load` requirement is unchanged: the DRC/ERC providers
register from file-scope constructors), `-fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1
-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createKicadApi -sENVIRONMENT=web,worker,node
-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sSTACK_SIZE=16MB -sFORCE_FILESYSTEM=1
-sEXPORTED_FUNCTIONS=_main,_kiapi_init,_kiapi_dispatch,_kiapi_free,_kiapi_shutdown,_kiapi_last_error,_malloc,_free
-sEXPORTED_RUNTIME_METHODS=HEAPU8,FS,UTF8ToString,stringToNewUTF8,ccall,cwrap
-sERROR_ON_UNDEFINED_SYMBOLS=0 -sWARN_ON_UNDEFINED_SYMBOLS=1` (the last pair is
`KICAD_WASM_ALLOW_UNDEFINED`, default ON; flipping it off is the goal, not yet reachable).

No `--preload-file`: the fab_pcb harness mounts a share tree into MEMFS at run time
(`KICAD_WASM_SHARE`), and `template` + `schemas` from a KiCad install is 5.6 MB.

## 5. Conformance: 125/153, and the whole gap to stdio is one harness bug

`KICAD_TRANSPORT=wasm KICAD_WASM_DIR=<build>/wasm/host KICAD_WASM_SHARE=<template+schemas>`:

    168 commands: 125 pass, 15 skip (gui-only), 28 fail; headless 125/153;
    6 extra checks: 5 pass, 1 fail

against stdio's **150/153**.  All 28 failures are accounted for, and only three of them are the
module's:

- **3 permanent, identical to stdio** — `RunBoardJobExport3D` and `RunBoardJobExportRender` (no
  OpenCascade, no 3D viewer) and `RunSchematicJobExportBOM` (`FIELDS_TABLE_DATA_MODEL_BASE`
  derives from `wxGridTableBase`).  The BOM one is worth noting for a different reason: it
  surfaces as the Emscripten function stub trapping with the constructor's own mangled name in
  the stack, which proves the `-sERROR_ON_UNDEFINED_SYMBOLS=0` stub mechanism gives the same
  diagnostic the native abort stubs do.

- **25 are one harness bug: output files land in MEMFS and the tests stat the host.**  Every
  failing command is one that writes a file — the 15 `Run{Board,Schematic}JobExport*`,
  `NewProject`, `NewDocument`, `SaveCopyOfDocument`, `CreateLibrary`, `SaveLibraryItem`,
  `SyncSchematicToBoard`, `CreateItems(SCH_SHEET)` and both `roundtrip` cases.  Commands that
  only read or mutate in memory pass.  `packages/kicad-wasm/src/fs.ts` already has `exportDir`;
  the wasm branch of `packages/client/test/kicad-server.ts` has to copy job outputs back out
  after each command (and `tempProject()`'s directory has to be mounted writable).  This is a
  fab_pcb fix (`client:`) and is the single highest-value thing left — it should move the score
  from 125 to within the 3 permanent failures of stdio.

**One thing was fixed here already** (fab_pcb, `client: close the wasm transport when the module
aborts`).  An Emscripten `abort()` is an `unreachable` instruction, reaching JS as a
`WebAssembly.RuntimeError`, and it leaves the instance permanently dead.  `WasmTransport`
rejected that one request and carried on, so the BOM trap turned into a cascade: each of the 13
commands after it sat **~29.65 s** and failed.  Uniform 29.6 s across commands with nothing else
in common (board *and* schematic, trivial *and* expensive) was the tell.  The transport now
closes, and the harness's `ensureAlive()` restarts the module -- cheap, since `createKiCadWasm`
is **109 ms** and mounting the whole 5.6 MB share tree (185 files) into MEMFS is **16 ms**.

`roundtrip.kicad.test.ts` was run as part of the suite; its two cases are in the 25.

## 6. Native gate: GREEN, unchanged

Re-measured after every source change above.

- `kicad-api-host-native --selftest qa/data/pcbnew/api_kitchen_sink.kicad_pcb` — **13 checks, 0
  failures**; DRC still finds 1 error / 11 warnings on the kitchen sink.
- `KICAD_TRANSPORT=stdio` conformance — **168 commands: 150 pass, 15 gui-only skip, 3 fail;
  headless 150/153; 6/6 extra checks**.  The three failures are the documented ones
  (`RunSchematicJobExportBOM`, `RunBoardJobExport3D`, `RunBoardJobExportRender`).  Identical to
  the baseline in host/STATUS-wasm-prep.md.

Every change is inside `#ifndef KICAD_HEADLESS_API`, inside an Emscripten branch, or a plain
added `#include`, which is why nothing moved.

## 7. Two build-system traps, recorded because they cost hours

- **Never run two `ninja` processes in one build dir.**  Doing so corrupts `.ninja_deps`; ninja
  then warns *"premature end of file; recovering"* on **every** subsequent load and rebuilds all
  1100 translation units each time.  `rm build/wasm/.ninja_deps build/wasm/.ninja_log` fixes it,
  at the cost of one more full rebuild.  This cost four ~50-minute rebuilds.
- A full wasm rebuild is **~50 min at -j10**; the emcc link alone is ~4 min (wasm-ld + the
  post-link JS stage over 37 MB).  Changing `host/wx_headless/wxgui/wx/setup.h` invalidates
  every TU, so batch flag changes.
