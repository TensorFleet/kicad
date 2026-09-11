# headless-build status (2026-09-09)

Branch `wasm`, build dir `build/headless`, option-OFF regression dir `build/check-off`.

## Gate: all three items pass

1. Configure + build succeed:
   ```
   cmake -S . -B build/headless -G Ninja -DCMAKE_BUILD_TYPE=Release \
     -DKICAD_HEADLESS_API=ON -DCMAKE_PREFIX_PATH=$(brew --prefix) \
     -DwxWidgets_CONFIG_EXECUTABLE=$(brew --prefix)/bin/wx-config-3.2 \
     -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF -DKICAD_USE_SENTRY=OFF \
     -DKICAD_UPDATE_CHECK=OFF -DKICAD_INSTALL_DEMOS=OFF -DKICAD_USE_PCH=ON
   ninja -C build/headless -j8 kiapi kicommon gal common pcbcommon connectivity \
     pcbnew_kiface_objects eeschema_kiface_objects
   ```
   kiapi/kicommon/gal are STATIC under the option (`KICAD_LIB_TYPE`); the kifaces
   are object libraries as before.  `pcbnew.cpp` / `eeschema.cpp` are compiled
   *into* their object sets instead of separate MODULEs.
2. `nm -m` over both object sets: **0 duplicate strong symbols**.  `KifacePcb`,
   `KifaceSch`, `KifaceGetterPcb`, `KifaceGetterSch` all present; no plain
   `Kiface()` or `KIFACE_1`.  The only cross-*archive* overlaps (pcbnew objects
   vs pcbcommon, common vs pcbcommon) are pre-existing upstream: 9 sources are
   listed in both PCB_COMMON_SRCS and PCBNEW_CLASS_SRCS (padstack.cpp,
   drc_test_provider.cpp, pcbnew_settings.cpp, footprint_editor_settings.cpp,
   convert_shape_list_to_polygon.cpp, the four component_classes/*), and
   netlist_reader_obj is in both common and pcbcommon.  Objects win over archive
   members, so this links; it is not something the split introduced.
3. Option-OFF build in `build/check-off` is byte-identical to the pre-change
   baseline (`baseline-targets.txt`, `baseline-cmd-*.txt` saved there) apart from
   the two files the `seams` agent added.  Link and source lists are assembled in
   pieces rather than reordered, precisely so this stays true.

## What is excluded, and how

`KICOMMON_SRCS` / `COMMON_SRCS` / `PCB_COMMON_SRCS` / `PCBNEW_SRCS` /
`EESCHEMA_SRCS` keep their flat upstream lists.  Named groups
(`KICOMMON_GUI_SRCS`, `KICOMMON_NET_SRCS`, `KICOMMON_VCS_SRCS`, `COMMON_GUI_SRCS`,
`PCB_COMMON_GUI_SRCS`, `PCBNEW_GUI_SRCS`, `EESCHEMA_GUI_SRCS`) are subtracted with
`list( REMOVE_ITEM )` under the option, and a `list( FIND )` loop raises
FATAL_ERROR if a name stops matching, so the lists cannot silently drift when
upstream renames a file.  Read those groups in the CMakeLists for the exact
contents; the shape is:

- kicommon: dialogs/, widgets/, kicad_gl/, startwizard/, bitmap*/confirm/kidialog/
  dialog_shim/grid_tricks/scintilla_tricks/app_monitor/navlib, plus the NET group
  (kicad_curl/, oauth/, remote_provider_*, api_plugin*, python_manager,
  cross_probe_client, api_client).  `font/fontconfig.cpp` is swapped for
  `font/fontconfig_stub.cpp`.
- gal: `cursors`, `hidpi_gl_*`, `3d/camera`, all of `opengl/` and `cairo/`, and the
  shader generation.  The view/painter/font half stays.
- common: COMMON_ABOUT_DLG/DLG/WIDGET/PREVIEW_ITEMS wholesale, the frames, the
  interactive tools, `properties/pg_*`, `database_connection`, `http_lib/`,
  `diff_renderer_gal`.  Tool framework core, plotters, io, drawing sheet,
  eda_item/shape/text/group, commit, property_mgr, bitmap_base all stay.
- pcbnew: PCBNEW_DIALOGS, ZONE_MANAGER_SRCS, microwave, exporters/step + u3d, the
  board_stackup_manager panels, frames/menubars/toolbars, widgets/, and the
  interactive tools.  Kept: DRC, zone filler + `zone_filler_tool`,
  `global_edit_tool`, `drc_tool`, `pcb_tool_base`, `pcb_selection*`, api/, the
  pcb_io plugins, plot/exporters, jobs handler.
- eeschema: EESCHEMA_DLGS, LIBEDIT, SIM, WIDGETS, PRINTING, SYNC_SHEET_PIN, the
  frames/toolbars/menubar and the interactive tools.  Kept: schematic model,
  sch_io, ERC, netlist exporters, sch_plotter, api/, jobs handler.

kiplatform gains `os/headless/` (app, drivers, environment, policy, secrets,
printing -- environment resolves the XDG paths itself, no GLib/GIO) and
`port/none/` (touchpad, ui, webview no-ops).  `os/unix/io.cpp` is reused for POSIX
file IO; its `posix_fadvise` call is now guarded because macOS has none.

## Things on the API path that had to be guarded (`#ifndef KICAD_HEADLESS_API`)

- `pcbnew.cpp`: the frame/dialog/panel includes, the whole `CreateKiWindow` body,
  `pcbnewOpenDiffDialogExport`, the focus-window lookup in `IfaceOrAddress`, the
  `EDA_3D_VIEWER_SETTINGS` registration, and `std::async` in `PreloadLibraries`
  (now runs inline and returns a satisfied future).
- `pcbnew_jobs_handler.cpp`: `JobExportRender` reports "3D rendering is not
  available in this build" -- it goes through 3d-viewer's OpenGL board adapter.
- `tools/global_edit_tool.cpp`: `Migrate3DModels` is a no-op (same reason).
- `pgm_base.cpp` / `pgm_base.h`: curl init/cleanup, sentry, `wxPGInitResourceModule`,
  `API_PLUGIN_MANAGER` (member and accessor), `PYTHON_MANAGER::FindPythonInterpreter`,
  `ReloadPlugins`, and the sentry calls in `HandleException`.
- `build_version.cpp`: OCC, curl and ngspice version strings.
- `singleton.cpp`: `GL_CONTEXT_MANAGER`.
- `coroutine.h`: selects `coroutine_sync.h` under `KICAD_SYNC_COROUTINE` (defined by
  the option).  Same `COROUTINE<R,A>` API; `Call()` runs to completion,
  `KiYield()`/`RunMainStack()` throw.  Safe because the RunAction allowlist in
  `api_handler_board.cpp` admits only ZONE_FILLER_TOOL and GLOBAL_EDIT_TOOL,
  neither of which yields.
- `kiway.h`/`kiway.cpp`: `KIWAY::RegisterStaticKiface()` + `startKiface()`.  A host
  registers `KifaceGetterPcb`/`KifaceGetterSch` and `KiFACE()` uses them instead of
  dlopen, through the same `OnKifaceStart`/`HandleException` path.
- `api_job_registry`: `SetInlineMode(true)` runs async jobs on the caller's thread,
  still answering JS_RUNNING + job id, and `WaitForIdle()` returns immediately.
- Thread pool: `compat/inline-thread-pool/bs_thread_pool.hpp` replaces BS's header
  via the `thread-pool` INTERFACE target's include dir.  All ~50 call sites compile
  against it unchanged.

Eight cross-kiface ODR duplicates fixed regardless of the option: `allowedActions`,
`checkOverwriteDb`, `g_excludedLayers` are now `static`; `FOOTPRINT_INFO_GENERATOR`,
`LOCK_CONTEXT_MENU`, `RECTANGLE_POINT_EDIT_BEHAVIOR`, `TEXTBOX_POINT_EDIT_BEHAVIOR`
are in anonymous namespaces.

## Dependencies deliberately NOT dropped (next agent's work)

The find_package calls for these are still unconditional; each has a comment
saying why:

- **libgit2** -- `local_history.cpp` (2600 lines), `history_lock.cpp`, `project.cpp`
  and `text_eval_vcs.cpp` are written directly against it and are on the document
  save / `${VCS_*}` text-variable path.  `KICOMMON_VCS_SRCS` already names the git
  sources so they can be lifted out behind a backend seam.
- **nng** -- `common/api/api_server.cpp` is written against kinng, and
  `KICAD_API_SERVER*` appears in the KIFACE virtuals.  The `seams` agent's
  `api_server_host.*` is the seam this should land on.
- **Cairo/Pixman** -- only the Cairo *GAL renderer* is dropped.  `PNG_PLOTTER`
  rasterizes through Cairo and is reached from `plot_board_layers.cpp`,
  `sch_plotter.cpp` and `diff_renderer_plotter.cpp`.
- **Fontconfig** -- KiCad's own wrapper is stubbed (no FcInit, no cache scan), but
  thirdparty `emf2svg` and `nanosvg` link `Fontconfig::Fontconfig` directly.

Dropped cleanly: CURL, SPNAV, ngspice, OpenCascade, OpenGL, Boost::locale, nanodbc,
libcontext, glad.  `resources/`, `3d-viewer/`, `gerbview/`, `pagelayout_editor/`,
`bitmap2component/`, `pcb_calculator/`, `plugins/`, `cvpcb/`, `kicad/`, `tools/`,
`utils/` are not added as subdirectories at all.

## Known remaining work before a link succeeds

Not attempted tonight (the gate was compile-only):

- `PGM_BASE` still references `BACKGROUND_JOBS_MONITOR`, `NOTIFICATIONS_MANAGER`,
  `ShowSplash()`, `DisplayErrorMessage()` and `bitmaps.h`, whose translation units
  are excluded.  Either those come back or `PGM_BASE` needs a headless variant.
- Nothing calls `KIWAY::RegisterStaticKiface()` yet -- that belongs in the host
  executable, alongside `API_JOB_REGISTRY::Instance().SetInlineMode( true )`.

## Emscripten notes

An early `if( EMSCRIPTEN )` block in the top-level CMakeLists forces
`KICAD_HEADLESS_API` on, turns off PCH/i18n/sentry/QA/update-check/demos, sets
`KICAD_WX_PORT` to "none", and skips the wx `find_package` and port-detection
FATAL_ERROR in favour of caller-supplied `wxWidgets_INCLUDE_DIRS` /
`wxWidgets_LIBRARIES` / `wxWidgets_DEFINITIONS`.  `UNIX_NOT_APPLE` is true under
Emscripten, so the Linux-only branches (SPNAV, Wayland, KiCadAppNames, libsecret,
Poppler, GTK3, X11) still need auditing -- kiplatform's is already handled because
`KICAD_HEADLESS_API` is checked ahead of the `elseif( UNIX )`.
