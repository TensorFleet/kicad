# native-gate status (2026-09-09)

Branch `wasm`, build dir `build/headless`, binary `build/native-host/kicad-api-host-native`
(a symlink the build creates; `build/headless/host/` is the real output).

## Gate: PASSES

- `kicad-api-host-native --selftest <board>` -- 13 checks, 0 failures (Ping, GetVersion,
  GetServerInfo == `inproc://kicad`, OpenDocument, GetItems, RunAction zoneFillAll,
  RunBoardJobDrc, CloseDocument).  Registered as the CTest `api.host.selftest` when
  `KICAD_BUILD_QA_TESTS` is on.
- fab_pcb conformance over `KICAD_TRANSPORT=stdio`: **174/177, 150/153 headless commands**,
  15 gui-only skips, 6/6 extra checks; `roundtrip.kicad.test.ts` green.  The ipc baseline is
  177/177.  Three failures, all understood:
  - `RunBoardJobExport3D` / `RunBoardJobExportRender` -- **permanent**.  OpenCascade and the
    3D viewer are not in this build; both jobs now report that instead of crashing.
  - `RunSchematicJobExportBOM` -- `FIELDS_TABLE_DATA_MODEL_BASE` (common/dialogs/) derives
    from `wxGridTableBase`, and the CLI BOM path instantiates it directly.  Making BOM
    headless means splitting the data model off its grid base; nothing else is in the way.
- DRC on the kitchen sink finds 11 markers (2 errors / 9 warnings) against `kicad-cli`'s 13
  (2/11).  The test passes; the two missing warnings are not chased down.
- Option-OFF regression: `ninja -C build/dev pcbnew_kiface eeschema_kiface` links clean after
  the dialog/exporter split.  fab_pcb needed no changes at all -- `StdioTransport` and
  `docs/08-wasm.md` describe the host exactly as built.
- `qa_api` (`build/qa`, rebuilt for the `api_job_registry.h` change): **162 cases, no errors**,
  and `--run_test=ApiJobs,ApiInProcess` on its own is 13/13.  Inline mode is off there, so the
  deferred queue is only exercised by the stdio host.

## What is in host/

`kicad_api_host.{h,cpp}` (config + `Init`/`Dispatch`/`Shutdown`), `kicad_api_c.{h,cpp}` (the
`kiapi_*` ABI the wasm module will export, with an `AS_BUSY` re-entrancy guard),
`static_kifaces.h`, `cvpcb_headless_kiface.cpp` (ERC asks KIWAY for FACE_CVPCB; cvpcb/ is not
built, so its two `IfaceOrAddress` functions live here), `main_native.cpp` (stdio framing;
stdout is dup'ed aside and fd 1 pointed at stderr so no stray printf can corrupt a frame) and
`headless_link_stubs.cpp`.

## Three things the next agent must know

1. **`-force_load` is load-bearing.**  `kicad_api_host` archives the kiface object sets, and a
   plain archive link drops members nothing references -- which silently loses every DRC/ERC
   test provider, since they register from file-scope constructors.  That was the "DRC finds no
   errors" bug.  The wasm link needs the same treatment (`--whole-archive`).
2. **`host/headless_link_stubs.cpp` is generated, never edited.**  682 GUI entry points that
   kept sources still reference are defined there as functions that print their own name plus a
   backtrace and abort.  `tools/regen_link_stubs.sh` relinks with no stubs and regenerates from
   what is missing; a stale stub silently shadows a real definition.  Every remaining stub is a
   place a headless guard is still missing -- an abort names it, which is how the 18 conformance
   failures of the first run were found and fixed one at a time.
3. **Inline jobs are now deferred, not immediate.**  `API_JOB_REGISTRY::SetInlineMode` holds an
   async job and `RunDeferred()` runs it; the host calls that between requests.  Without it a
   client is told JS_RUNNING after the job has already published all its JobProgress events.

## Still to do: the four dependencies (NOT started)

Each is harder than the earlier note assumed; the gate would regress if they were rushed.

- **nng** -- two users now: `api_server.cpp` (guardable; `StartInProcess`/`DispatchBytes` are
  already socket-free) and `api/cross_probe_client.cpp`, which serves the `CrossProbeAnnounce`
  API command and is *back in* the headless build.  Dropping nng means that command answers
  unimplemented: -1 conformance command.
- **Cairo** -- `PNG_PLOTTER` is reached from `plot_board_layers.cpp`, `sch_plotter.cpp` and
  `common_plot_functions.cpp`, and `plotter_png.h` names `cairo_surface_t`.  Wants a
  `PNG_plotter_stub.cpp` (same shape as `font/fontconfig_stub.cpp`) plus a forward-declared
  cairo type in the header, and the PNG plot format reporting unsupported.
- **Fontconfig** -- not a stray link line: `thirdparty/nanosvg/nanosvg.cpp` really calls
  `FcConfig`/`FcWeightFromOpenType`, and `emf2svg` links it too.  Needs nanosvg's text path
  patched (or the SVG importer excluded) as well as dropping emf2svg/libwmf and the OrCAD/EMF
  importers.
- **libgit2** -- unchanged from headless-build's note; `KICOMMON_VCS_SRCS` is the seam.

## Reproducing

```
cmake -S . -B build/headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DKICAD_HEADLESS_API=ON \
  -DKICAD_HEADLESS_WX_BASE_ONLY=OFF ...          # (see STATUS-headless-build.md for the rest)
                                                 # OFF is what the gate below was run with; the
                                                 # option's default is ON now -- see the note in
                                                 # "WP6f: the wx shim, sized".
ninja -C build/headless -j10 kicad-api-host-native
./build/native-host/kicad-api-host-native --selftest qa/data/pcbnew/api_kitchen_sink.kicad_pcb
cd ../fab_pcb/packages/client && KICAD_TRANSPORT=stdio bun run test:conformance
```

## WP6f: the wx shim, sized

`KICAD_HEADLESS_WX_BASE_ONLY` (**default ON since the wx-base-only link landed**; it was OFF when
this section was written, and the numbers below are from that state) drops the toolkit halves of wx
from the link line, keeping wxBase, wxXML, the -L and the macOS frameworks.  The gate above runs
with it off,
i.e. wxCore linked, so that the KiCad-level blockers could be found first.  Turning it on links
and reports **480 undefined wx symbols** (681 before the DRC rule editor was excluded), and they
are *not* value types: the list is led by `wxWindow` (90), `wxWindowBase` (78), `wxImage` (32),
`wxGrid`/`wxGridTableBase` (51), `wxStyledTextCtrl` (35), `wxDialog`, `wxMenu`, `wxDataViewModel`.
Per the plan's own rule that means more GUI survived the split, and the referencing objects say
exactly where:

    393 zone_filler.cpp      182 local_history.cpp    73 zone_settings.cpp
     55 router_tool.cpp       41 PDF_plotter.cpp      40 pcb_tuning_pattern.cpp
     38 rc_item.cpp           35 pcbnew_jobs_handler  35 bitmap_base.cpp
     32 sheet.cpp             30 kicad_clipboard.cpp  29 eeschema_jobs_handler.cpp
     26 sch_field.cpp         26 drc_rule_editor_utils.cpp

`zone_filler.cpp` and `local_history.cpp` alone are 575 of the 480 references (they repeat), and
both are progress-dialog/`wxWindow*` parameters on functions that never show anything headless --
guard those two and the surface roughly halves.  `PDF_plotter`/`bitmap_base`/`gfx_import_utils`
want `wxImage`, which is the one place a real value-type shim is called for.
