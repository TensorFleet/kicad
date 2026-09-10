# KiCad as WebAssembly — status, 2026-09-09

KiCad's headless API core is compiled to WebAssembly and answers the same protobuf envelope the
`kicad-cli api-server` does. Under `KICAD_TRANSPORT=wasm` the conformance suite scores **151 of 153
headless commands — identical to the native host** — and the two failures are the two the native
host has (3D export and 3D render; there is no OpenCascade in this link). The bridge runs a session
as a module in a Worker; `apps/web` runs KiCad in a Web Worker in the tab, opens a real project and
runs DRC with no server anywhere.

Everything is on the branch `wasm` in both repos. Nothing is pushed and nothing is tagged — see
[Housekeeping](#housekeeping-for-you-to-decide).

## What exists

**Fork (`tensorfleet/kicad`, branch `wasm`, 41 commits on top of `web-api`)**

- `KICAD_HEADLESS_API` (CMake option, default OFF): splits the GUI out of `kicommon`/`common`/`gal`,
  links the pcbnew and eeschema kifaces statically into one image, runs the thread pool and the job
  registry inline, and drops nng, libgit2, Cairo, Fontconfig, OpenCascade, curl, ngspice and the
  renderers. With the option OFF the object set is byte-identical to today's build.
- `host/` — `KICAD_API_HOST` (the seven lifecycle hooks lifted out of `command_api_server.cpp`), the
  `kiapi_*` C ABI (`kicad_api_c.cpp`), the native stdio binary (`main_native.cpp`), the Emscripten
  entry point (`host/wasm/main_wasm.cpp`), and `host/wx_headless/` — real `wxColour` / `wxImage` /
  `wxPGChoices` for a wxBase-only link, plus the `wxUSE_GUI` header shim.
- **A BOM data model that is not a wxGrid table.** Under `KICAD_HEADLESS_API` the fields tables
  derive from a new `HEADLESS_GRID_TABLE_BASE` (`include/widgets/headless_grid_table.h`) instead of
  `wxGridTableBase`, so `RunSchematicJobExportBOM` works on both headless backends and writes a CSV
  byte-identical to `kicad-cli`'s. This was the one command that used to abort an instance.
  (`host/STATUS-bom-fix.md`)
- **Outline fonts from a manifest.** `common/font/fontconfig_manifest.cpp` replaces the old
  `fontconfig_stub.cpp`: it resolves `FindFont()` against a `manifest.json` (or a bare folder of
  `.ttf`/`.otf` it reads with FreeType) under `KICAD_FONTS_DIR`, matching `fontconfig.cpp`'s rules
  and `FF_RESULT` codes line for line. `KICAD_API_HOST_CONFIG::fonts` (`--fonts`, JSON `fonts`,
  default `/kicad/fonts` under Emscripten) exports it. `KICAD_WASM_PRELOAD_FONTS=ON` bundles Carlito
  Regular + Bold and a generated manifest into `kicad_api.data`, so **the module resolves outline
  text with no host font directory at all**. (`host/STATUS-fonts.md`)
- **`EDA_SHAPE( const SHAPE& )` no longer leaves its fill out of range** — it value-initialised
  `m_fill` and `FILL_T` starts at 1, so every glyph outline `handleGetTextAsShapes` wrapped tripped
  a `ToProtoEnum<FILL_T>` assertion. That is what filled the browser log; it is gone.
- **`-DwxDEBUG_LEVEL=0` for KiCad's own TUs** (`tools/wasm/env.sh`), matching the Homebrew wx the
  native build uses, and **`WASM_OPT_LEVEL`** (`tools/wasm/configure.sh`, default `-O2`): CMake used
  to append `-O3 -DNDEBUG` after `env.sh`'s `-O2`, so every KiCad TU was silently built at `-O3`.
- `tools/wasm/` — `build-deps.sh` (wxWidgets 3.2.11 base-only, protobuf 36.1 + abseil, zstd, and an
  81-check smoke test), `configure.sh`, `regen_wasm_link_stubs.sh` + `gen_wasm_link_stubs.py` (the
  65 data stubs wasm-ld cannot synthesise), `check_wx_abi.sh`, `size-report.sh`,
  `audit_undefined.py`, `package.sh`, `host-tools.sh`.
- **A release workflow.** `.github/workflows/wasm-release.yml` builds the module on Ubuntu on any
  `fp-pcb/*` tag (or `workflow_dispatch`) and attaches `kicad-wasm-<tag-slug>.tar.gz` — module,
  `kicad-wasm.json` provenance manifest and `SHA256SUMS` — to the release, so a consumer never needs
  the toolchain. Never run yet; the Linux path is the experiment. (`host/STATUS-release-artifact.md`)

**fab_pcb (branch `wasm`, 28 commits on top of `main`)**

- `@fp-pcb/client`: `StdioTransport`/`StdioSubscriber` and `WasmTransport`/`WasmSubscriber`. The
  `Transport` interface did not change.
- `@fp-pcb/kicad-wasm`: the loader over the `kiapi_*` ABI, MEMFS helpers (`mountProject`,
  `exportDir`, `DirMirror`), font mounting (`mountFonts`, `buildFontManifest`, an sfnt `name`-table
  parser), `bun run fetch` to copy a build into `dist/` and `fetch:release` to download, verify and
  unpack a fork release tarball instead.
- **The module runs in a Web Worker** (`worker.ts` / `worker-core.ts` / `worker-client.ts`): one
  instance per tab, the bridge's protocol plus `{id, fs}` for file operations, and `dispatchAsync`
  on the transport so in-process callers keep the straight-line path. `?wasm-main=1` keeps the old
  main-thread path for debugging.
- The conformance harness picks a backend with `KICAD_TRANSPORT`; the suites only see `RunningKiCad`.
- `@fp-pcb/bridge`: `SESSION_BACKEND=wasm` runs each session as a module in its own Bun `Worker`.
- `apps/web`: `?wasm=1` runs KiCad in the tab, with a file picker (and drop target) that imports a
  project into MEMFS.

## Building it

```sh
# --- the native gate (an hour the first time, minutes after) -----------------
cmake -S . -B build/headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DKICAD_HEADLESS_API=ON ...
ninja -C build/headless -j10 kicad-api-host-native      # -> build/native-host/kicad-api-host-native
./build/native-host/kicad-api-host-native --selftest qa/data/pcbnew/api_kitchen_sink.kicad_pcb

# --- the wasm module ---------------------------------------------------------
tools/wasm/build-deps.sh          # wx + protobuf + abseil + zstd for wasm32, ~30 min, idempotent
tools/wasm/configure.sh           # emcmake; needs build/headless for lemon and brew protoc 36.1
tools/wasm/regen_wasm_link_stubs.sh   # after ANY change that adds or removes a definition
ninja -C build/wasm -j10 kicad_api    # -> build/wasm/host/kicad_api.{js,wasm,data}, ~50 min clean
tools/wasm/size-report.sh build/wasm/host/kicad_api.wasm
tools/wasm/package.sh             # -> build/wasm-release/kicad-wasm-<tag>.tar.gz + SHA256SUMS
```

Pushing an `fp-pcb/*` tag runs `.github/workflows/wasm-release.yml`, which does all of the above on
an Ubuntu runner and attaches that tarball to the GitHub release, so fab_pcb can
`bun run --filter @fp-pcb/kicad-wasm fetch:release` instead of owning a toolchain. Nothing is pushed
or tagged yet — `host/STATUS-release-artifact.md` has the steps and the caveats (the Linux path is
untested; `tools/wasm/host-tools.sh` supplies the native `lemon` and `protoc` there).

Three traps that cost hours and are worth re-reading before touching the build: never run two
`ninja` processes in one build dir (it corrupts `.ninja_deps` and every later load rebuilds all 1100
TUs); `tools/wasm/regen_wasm_link_stubs.sh` must be re-run after any change that adds a definition,
or the link fails with `duplicate symbol` (the BOM fix defined six symbols the stub file still
carried, and that is exactly how this rebuild started); and the native
`tools/regen_link_stubs.sh` needs `WXLIBDIR=/opt/homebrew/lib` on this machine, plus `DEVSYMS_CACHE`
unless you want to wait out an `nm` pass over `build/dev`. Details in `kicad/host/STATUS-wasm-build.md`.

## Running it

```sh
# conformance, one line per backend
KICAD_CLI=../kicad/build/dev/kicad/KiCad.app/Contents/MacOS/kicad-cli bun run --cwd packages/client test:conformance
KICAD_TRANSPORT=stdio bun run --cwd packages/client test:conformance
KICAD_TRANSPORT=wasm KICAD_WASM_DIR=../kicad/build/wasm/host bun run --cwd packages/client test:conformance

# the module on its own (Ping/GetVersion through kiapi_dispatch) and two instances at once
KICAD_WASM_DIR=../kicad/build/wasm/host bun test --cwd packages/kicad-wasm

# outline fonts: the preloaded bundle needs no KICAD_FONTS_DIR, a host directory overrides it
KICAD_TRANSPORT=wasm KICAD_WASM_DIR=../kicad/build/wasm/host bun test --cwd packages/client test/fonts.kicad.test.ts

# the bridge, every session a module in a worker
SESSION_BACKEND=wasm KICAD_WASM_DIR=../kicad/build/wasm/host bun run --filter @fp-pcb/bridge start
KICAD_WASM_DIR=../kicad/build/wasm/host bun test --cwd packages/bridge test/session-wasm.kicad.test.ts

# KiCad in the tab
bun run --filter @fp-pcb/kicad-wasm fetch      # copies the build into packages/kicad-wasm/dist
bun run --filter @fp-pcb/app dev               # then open http://localhost:5173/?wasm=1

# the three backends side by side, by hand
bun packages/client/test/bench-drc.ts
```

`KICAD_CLI` matters: without it the suites silently use the stale `build/release` binary and the
integration tests fail for reasons that have nothing to do with this work.

## The numbers

Conformance is 168 commands (153 headless + 15 gui-only, which every headless backend skips) plus
6 extra checks. Measured on this machine, idle, against the module built from `78a0998b2a` + the
regenerated stubs.

|                                | `ipc` (kicad-cli)              | `stdio` (native host)           | `wasm`                                                                     |
| ------------------------------ | ------------------------------ | ------------------------------- | -------------------------------------------------------------------------- |
| Conformance                    | **177/177** (153/153 headless) | **151/153** headless, 6/6 extra | **151/153** headless, 6/6 extra                                            |
| Conformance wall time          | ~5 min                         | 61 s                            | **63 s**                                                                   |
| Open project + board           | 392 ms                         | 65 ms                           | **114 ms**                                                                 |
| DRC, kitchen sink (11 markers) | 69 ms                          | 64 ms                           | **78 ms** (first run 109 ms)                                               |
| Ping, mean of 200              | 0.052 ms                       | 0.031 ms                        | **0.006 ms** raw dispatch (0.19 ms through the test harness's MEMFS mirror) |

The module, `-O2` with `wxDEBUG_LEVEL=0` and the Carlito bundle:

| file                            |                    raw |         `brotli -q 11` |
| ------------------------------- | ---------------------: | ---------------------: |
| `kicad_api.wasm`                | 35 532 539 (33.89 MiB) |  6 752 286 (6.44 MiB)  |
| `kicad_api.js`                  |                212 054 |                 33 485 |
| `kicad_api.data` (fonts)        |  1 310 761 (1.25 MiB)  |                381 605 |
| **everything the browser pulls**| 37 055 354 (35.34 MiB) | **7 167 376 (6.84 MiB)** |

`gzip -9` on the wasm is 10 120 900 and `brotli -9` is 7 810 755, so the difference between "no
`Content-Encoding`" and "brotli at quality 11" is **35.3 MB against 6.4 MB** — still the largest
single win available, and still not done. Sections: `CODE` 29 903 647 (84.2%), `DATA` 5 488 887
(15.4%), 37 410 function bodies, 17 exports, **450 function imports** — 57 Emscripten/WASI runtime
and **393 KiCad symbols left undefined**, down from 444 (the BOM fix took 51 with it).
`tools/wasm/audit_undefined.py` classifies the rest.

Lifecycle:

|                                                 |                                                                                 |
| ----------------------------------------------- | ------------------------------------------------------------------------------- |
| `createKiCadWasm()`                             | **85-103 ms** under Bun                                                         |
| Mounting KiCad's share tree (185 files, 5.6 MB) | 16 ms                                                                           |
| Bridge `POST /sessions` → running and pinged    | **253 ms**                                                                      |
| RSS per instance                                | +327 MB for the first, +155 MB for the second; 679 MB with a board open in each |

Outline fonts, module-preloaded, **no `KICAD_FONTS_DIR` and nothing mounted from the host**
(`GetTextExtents("Wg1i- fp-pcb")` / `GetTextAsShapes("Wg")` at 2 mm, wasm transport) — the same four
numbers the native host gives with a host font directory:

| fontName          | extent     | shapes                |
| ----------------- | ---------- | --------------------- |
| `""` (stroke)     | 21.914 mm  | 21 segments           |
| `Carlito`         | 14.580 mm  | 2 polygons, 179 nodes |
| `Carlito` + bold  | 14.795 mm  | 2 polygons, 171 nodes |
| `No Such Family`  | 14.580 mm  | substituted to the default |

Isolation (`packages/kicad-wasm/test/isolation.kicad.test.ts`): two instances in one Bun process,
each with a different board at a different absolute path, answer `GetOpenDocuments` independently,
neither can `stat` the other's tree, and shutting one down leaves the other answering while the dead
one throws rather than touching a freed heap. 20/20 in that package against the real module.

Browser, driven by hand against the real module at `http://localhost:5178/?wasm=1` with the ecc83
fixture dropped onto the import target: the board opens and renders (59 tracks, 33 pads, 15
footprints, 4 graphic shapes, 1 zone), **Run DRC** reports 0 errors / 17 warnings, `kicad_api.js`,
`kicad_api.wasm` and `kicad_api.data` are each served **exactly once** per page session (counted at
the dev server), and the app log has **no `ToProtoEnum<FILL_T>` lines at all**.

## Known failures, with root causes

Two, both in the module, both also failing on the native host — so neither is an Emscripten or
MEMFS problem, and both are the same missing dependency:

1. **`RunBoardJobExport3D`** — "3D model export is not available in this build"; OpenCascade is not
   in the headless link.
2. **`RunBoardJobExportRender`** — "3D rendering is not available in this build"; the 3D viewer is
   not built.

Fixed since the last report: `RunSchematicJobExportBOM` (the wxGrid data model), the
`ToProtoEnum<FILL_T>` assertion storm (`EDA_SHAPE( const SHAPE& )`), and one wx assert per project
open (`wxString::Last(): index out of bounds`, compiled out by `-DwxDEBUG_LEVEL=0`).

Non-blocking noise: opening a project still logs one wx assert per instance
(`wxArrayString::Remove: bad index`, from `arrstr.cpp`). `-DwxDEBUG_LEVEL=0` applies to KiCad's TUs,
not to the wx library in `build/wasm-deps`, so this one still reports. It is the loudest unexplained
thing left in the log.

## Commits

`kicad`, `git log --oneline web-api..wasm` (41, newest first):

```
<this report>  docs: host/STATUS.md, rebuilt and re-measured
be56740b1c wasm: regenerate the link stubs against the BOM data model
78a0998b2a wasm: an -O3 control build, so the -O2 win is an isolated number
888b107163 wasm: measure the module, and build KiCad's TUs at -O2 instead of -O3
981109bbbc Headless: host/STATUS-bom-fix.md
c888753512 Headless: EDA_SHAPE( const SHAPE& ) left its fill out of range
a7dfdd5e6f Headless: a BOM data model that is not a wxGrid table
035db1f35e Headless: resolve outline fonts from a manifest instead of missing every lookup
4a9f583468 wasm: ship the module as a GitHub release artifact
afc30d7673 wasm: compile KiCad TUs with wxDEBUG_LEVEL=0
67a30a2661 docs: host/STATUS.md, the consolidated wasm status
8b8a65ebbc wasm: host/STATUS-wasm-build.md
d4719b5450 wasm: link the module -- the freetype port has setjmp variants and find_package picks the wrong one
8376472d4d wasm: real values for the five plain-data globals the link is missing
2e4163b854 wasm: generate the DATA half of the headless GUI stubs for wasm-ld
c23b19cbd3 wasm: three transitive-include fixes the GUI-enabled wx header chain used to hide
ea9a9423b5 wasm: compile KiCad against the full wx header set with a wxUSE_GUI shim
af1d93ff48 Host: qa_api is 162/162 on the wasm-prep branch, option-OFF tree rebuilt clean
4bd3e4387d Host: note where the qa_api rebuild stood at hand-off
b63dda574a Host: record the wasm-prep gate and what the Emscripten build still faces
19be4b8755 Headless: no wxSafeYield on the board plot path
efde1d3571 Headless: link against wxBase only, with real wxColour and wxImage
dccb95c31b Headless: an in-tree wxPGChoices for the wxBase-only build
3b439cc27c Headless: drop nng, libgit2, Cairo and Fontconfig from the headless link
656ad327a7 Host: qa_api is green against the job registry change (162 cases)
8c7ff0dd01 Host: record that qa_api still needs a run against the job registry change
2e9ffe9aec Host: note the option-OFF regression check in the gate status
9882935ed7 Headless: drop the DRC rule editor, and size the wx shim
84f8c8c4b6 Headless: deferred inline jobs, whole-archive linking, and two exporters lifted out of their dialogs
add9377a89 Headless: put the non-GUI half of the API path back in the build
f659f439e1 Host: an in-process API host, a C ABI, and the native stdio binary
12efcc0288 Headless: record the build status
f0e84ad8c0 Headless: let KIWAY find a KIFACE that is linked into the image
8b0e34f1e8 Headless: inline job execution, pgm_base guards, and cross-kiface ODR fixes
0a642082c6 Headless: run thread pool tasks inline
f9794962e0 Headless: keep the option-OFF build byte-identical
77fb4216dd API: Extract API_SERVER_HOST from the kicad-cli api-server command
3e875a479b API: Add an in-process dispatch seam to KICAD_API_SERVER
b83e3960f9 Headless: link the pcbnew and eeschema kifaces statically into one image
98ace7e983 wasm: dependency build script
20475e3aa2 Headless: add KICAD_HEADLESS_API and split the GUI out of the core libraries
```

`fab_pcb`, `git log --oneline main..wasm` (28, newest first):

```
<this report>  docs: the consolidated wasm status, rebuilt and re-measured
6e79169 client: an outline-font test for the fonts the module carries
e9013a6 kicad-wasm: resolve kicad_api.data next to the module, not against the cwd
627e472 docs: what the browser-worker agent changed
b2b18a2 web: KiCad in a Worker, and one module per tab
f978e78 kicad-wasm: run the module in a Web Worker
003f6bb kicad-wasm: mount outline fonts into the module
a640293 kicad-wasm: fetch the module from a fork release
22a88bf web: load @fp-pcb/kicad-wasm lazily so the package is optional
7426eee docs: one consolidated wasm status, with the numbers
de0c08c client: a hand-run benchmark for the three backends
2869437 web: replay imported files into every wasm instance, and stub node's rm
886ef24 bridge: an integration test for the wasm backend against the real module
6f177e8 kicad-wasm: two instances in one process, each with its own MEMFS
2ac6158 client: mirror the wasm backend's MEMFS around every conformance request
dea137f kicad-wasm: DirMirror, a two-way MEMFS/host mirror for one directory
d5d0837 client: close the wasm transport when the module aborts
b9702f0 bridge: record the kicad-wasm workspace deps in the lockfile
b62b92b docs: the bridge's wasm backend and the in-browser mode
bf9fb1b web: run KiCad in the tab as WebAssembly
abaabcb bridge: run a session's KiCad as wasm in a Worker
a6c0acd docs: conformance is 177/177 on an idle machine
7b1efbc ci: check the backend's own variable in integration mode
015e375 docs: wasm status after WT1-WT3
2515807 client: report the stdio events channel synchronously
326f307 conformance: pick the KiCad backend with KICAD_TRANSPORT (WT3)
d2355f6 kicad-wasm: new @fp-pcb/kicad-wasm loader package (WT2)
e175cd7 client: add StdioTransport and WasmTransport (WT1)
```

Three commits carry content that belongs to a neighbour. The content is right in every case; only
the attribution is wrong, which matters if any of this is ever proposed upstream:

1. The move of `PROJECT_TEMPLATE` to `include/project_template.h` + `common/project_template.cpp`
   and the `COMMON_SRCS` additions belong to the seams work (`77fb4216dd`), but a `git add -A` swept
   them into `20475e3aa2` "Headless: add KICAD_HEADLESS_API…". Recorded in
   `kicad/host/STATUS-seams.md`.
2. The **deletion** of `common/font/fontconfig_stub.cpp` belongs to the fonts commit
   (`035db1f35e`, which added `fontconfig_manifest.cpp` and repointed `common/CMakeLists.txt`), but
   it landed in `4a9f583468` "wasm: ship the module as a GitHub release artifact" instead. Neither
   commit is buildable on its own as a result.
3. `a7dfdd5e6f` "Headless: a BOM data model…" regenerated `host/headless_link_stubs.cpp` against a
   working tree that already had the (then uncommitted) font changes, so part of that 1601-line
   diff is index renumbering caused by the fonts work rather than by the BOM fix.
   (`host/STATUS-bom-fix.md`)

## Follow-ups, in priority order

1. **Serve the module Brotli-compressed.** 6.44 MB against 35.3 MB, today, with no build change —
   worth more than every compile-flag experiment combined, and more than the four size items below
   put together. Nothing in fab_pcb or the Vite dev middleware sets `Content-Encoding` yet.
2. **The ~120 frame/tool/dialog stubs** among the 393 still-undefined symbols — `SCH_EDIT_FRAME`
   (11), `PCB_EDIT_FRAME` (7), `PCB_SELECTION_TOOL` (5), the `DIALOG_*` vtables the job handlers
   name. These are KiCad's own classes, named by code that does run, so a command that takes a "we
   have a frame" branch hits a trap that kills the instance instead of a clean error. Each is an
   `#ifdef KICAD_HEADLESS_API` guard plus an error return. Keep `KICAD_WASM_ALLOW_UNDEFINED=ON`
   until they are done; the remaining ~280 are wx GUI classes the header shim declares and nothing
   constructs. `tools/wasm/audit_undefined.py` prints the list, sorted, with the referencing files.
3. **A cancel button in the UI.** `terminate()` is on the worker client and nothing calls it, so a
   wedged command still needs a page reload.
4. **Project download.** MEMFS is only reachable through the session; `readFile` is already in the
   worker protocol, so "save my project back out of the tab" is UI work, not plumbing.
5. **pthreads.** DRC and zone fill are the two things a user waits on and both are parallel
   natively. `-pthread` needs COOP/COEP headers on the app and a real thread pool instead of the
   inline façade.
6. **Two size seams worth real work**, both needing a code change rather than a flag:
   **exclude the ngspice model tables** (3.4 MB, 10.9% of `CODE`, reachable only through the SPICE
   netlist exporter — `RunSchematicJobExportNetlist` must stay green) and a
   **`KICAD_HEADLESS_MINIMAL_IO`** option dropping the 14 foreign-format importers (~6.4 MB, 20%).
   `-Os` gives 17.5% for 28% of DRC, so it stays opt-in as `WASM_OPT_LEVEL=-Os`; `-flto` is the one
   untried candidate that might get close without the DRC cost. All measurements, and the six
   link-flag experiments that were **not** worth adopting, are in `kicad/host/STATUS-module-size.md`.
7. **A real PNG decoder for reference images.** `host/wx_headless/wx_image.cpp` reads geometry out
   of the container header (PNG `IHDR`, JPEG `SOFn`, BMP, GIF) and leaves the raster blank. Boards
   round-trip byte for byte because `BITMAP_BASE` keeps the undecoded bytes, but nothing can render
   or plot the image. Emscripten's libpng port is the cheap route.
8. **The pre-existing `format:check` failures** — 159 files, none of them touched by this work.
   Either run `bun run format` once across the repo or narrow the prettier glob; today the check is
   useless because it is always red.
9. **The `wxArrayString::Remove` assert** on every project open (see above). It is inside wx itself,
   so finding it means a `wxDEBUG_LEVEL=1` wx and a backtrace, not a KiCad rebuild.
10. **KiCad's stock fonts are still not installed anywhere the module can find.** Carlito comes from
    `thirdparty/libwmf/fonts/`; a board naming any other family gets the substitution, not the real
    face. `mountFonts()` is the escape hatch, and nothing in the API returns `ListFonts()`.

## Housekeeping (for you to decide)

- **Push both branches.** They are still local: `kicad` is 41 commits ahead of `web-api`, `fab_pcb`
  28 ahead of `main`. `git push origin wasm` in each.
- **Tag `fp-pcb/2026-09-09-wasm` on both repos.** The alignment scheme wants a tag on both at a
  change set this size, and on the fork the tag push is also what triggers the release workflow.
- **Then cut the first release run.** The tag starts **kicad-wasm release**; from cold caches expect
  100-140 minutes. When it finishes, set `packages/proto/KICAD_TAG` to that tag and
  `GITHUB_TOKEN=<pat> bun run --filter @fp-pcb/kicad-wasm fetch:release` — after which the wasm
  suites need no toolchain at all. Optionally uncomment the `wasm` job in fab_pcb's
  `.github/workflows/ci.yml` and add a `KICAD_FORK_TOKEN` secret (a repository's own `GITHUB_TOKEN`
  cannot read another private repository's release assets). Steps and caveats:
  `kicad/host/STATUS-release-artifact.md`.
- `packages/kicad-wasm/dist/` holds a 35 MB `kicad_api.wasm` plus the 1.3 MB `kicad_api.data` (from
  `bun run fetch`). Both are ignored by git; decide whether the app should fetch them from a CDN,
  which is also where the Brotli follow-up wants to live.

---

# Log

The notes below are what each agent wrote as it finished; they are kept for the detail the summary
above drops.

## ts-side

TypeScript side (branch `wasm`, agent `ts-side`), 2026-09-09.

- **WT1 done** — `StdioTransport`/`StdioSubscriber` (spawn the native host, `uint32be` frames on
  stdin/stdout, events on fd 3, replies matched by order) and `WasmTransport`/`WasmSubscriber`
  (in-process, microtask-queued dispatch, events raised during a dispatch flushed after that reply).
  21 unit tests against a fake stdio host and a fake instance.
- **WT2 done** — `@fp-pcb/kicad-wasm`: `createKiCadWasm()` (the `kiapi_*` ABI, `__kiapiEvent` wired
  before init, heap copies, `kiapi_last_error` surfaced), MEMFS helpers (`mountProject`/`mountPath`/
  `exportDir`), `bun run fetch` into `dist/`. 11 unit tests against a JS mock of the ABI;
  `test/wasm.kicad.test.ts` runs Ping/GetVersion and skips until a build exists.
- **WT3 done** — `KICAD_TRANSPORT=ipc|stdio|wasm` selects the backend inside `startKiCad()`; the
  suites use `RunningKiCad` only (`subscribe()`, `secondTransport()`, `server.eventsUrl`). A missing
  backend binary skips with the variable to set. Contract for the C++ side: `docs/08-wasm.md`.
- **Verified** — `bun run test:unit` 10/10 packages green; `KICAD_TRANSPORT=ipc` conformance with the
  dev `kicad-cli`: **177/177**, 153/153 headless commands, 15 gui-only skips, 6/6 extra checks (a
  first run scored 176/177 with 3 event-sequence gaps while another agent was building KiCad at load
  100+ — nng pub/sub drops frames behind a stalled subscriber; it is green on an idle machine). The
  `stdio` and `wasm` paths of the harness were smoke-tested against fake hosts answering a canned
  `AS_OK`: both load, mount, connect and run until the fixtures' real content is needed.
- **Blocked on the C++ side** — nothing exists yet at `kicad/build/native-host/kicad-api-host-native`
  or `kicad/build/wasm/host/kicad_api.js`, so neither backend has run against real KiCad. Change
  `docs/08-wasm.md` if the build lands with different names, paths, or an `--events-fd` fallback.
- **Known wasm divergence** — files KiCad writes land in MEMFS, so conformance checks that
  `existsSync()` a job output on the host will fail until the suite exports those directories.
  _(Fixed by `finish`: `DirMirror` + `MirroringWasmTransport`.)_

## bridge-app

Bridge and app sides (branch `wasm`, agent `bridge-app`), 2026-09-09.

- **WT4a done** — `SESSION_BACKEND=wasm` (and `POST /sessions {backend}`) runs a session's KiCad as
  `kicad_api.wasm` in one Bun `Worker`: `packages/bridge/src/session-wasm.ts` (the `SessionLike`
  surface, a `Transport` over the worker port), `wasm-worker.ts` (the module, MEMFS, the flush
  policy) and `wasm-protocol.ts` (`{id,req}`/`{id,res}`/`{event}`/`{state}`, buffers transferred).
  `SessionLike` is extracted so `server.ts`, SSE and the route/compile jobs never see the backend.
- **Isolation and teardown** — the worker exists because `kiapi_dispatch` is synchronous: on the
  main thread one wedged command would freeze every session. `KICAD_REQUEST_TIMEOUT_MS` terminates
  the worker, fails everything in flight and marks the session `failed`.
- **MEMFS policy** (documented in `docs/08-wasm.md`): the project dir is mounted at the same
  absolute path from inside the module factory, i.e. before `kiapi_init` sees `preload`; it is
  exported back after `Save*` / `CloseDocument`, after a `DocumentSaved` event, on `{flush}` and on
  stop. `EndCommit` is not a trigger — a commit does not touch the file.
- **WT4b done** — `?wasm=1` / `VITE_KICAD_WASM` runs KiCad in the tab: `WasmTransport` +
  `WasmSubscriber` passed to the document service as the explicit `events` option (the
  `instanceof WebSocketTransport` checks were not widened), `bridgeless`, a file picker that writes
  the project into MEMFS under `/project`, and `stat`/`listFiles` answered from MEMFS. **Main
  thread for now** — a Worker is the follow-up, behind `WasmModeOptions.createInstance`.
- **Vite** — the build is served at `/kicad-wasm/` (dev middleware, copied into `dist/` on build);
  `node:path` and `node:fs/promises` are aliased to browser stubs because the loader's entry point
  re-exports host-disk helpers; `worker.format: "es"`, no COOP/COEP (single-threaded).
- **Verified** — `bun run typecheck`, `bun run test:unit` 10/10 (8 new bridge tests through the real
  HTTP + WebSocket path, 4 new app tests), and `vite build` green, all against the JS mock of the
  `kiapi_*` ABI. Nothing here has met a real `kicad_api.js` yet: when one lands,
  `SESSION_BACKEND=wasm KICAD_WASM_DIR=… bun run --filter @fp-pcb/bridge start` and `?wasm=1` are
  the two things to try. _(Both tried by `finish`; the app needed one fix, below.)_

## finish

Overnight wrap-up (branch `wasm`, agent `finish`), 2026-09-09.

- **Conformance 125 → 150/153.** `DirMirror` (`packages/kicad-wasm/src/fs.ts`) mirrors one directory
  between the host and MEMFS with a size+mtime signature per entry, directories included, deletions
  included. `MirroringWasmTransport` in `packages/client/test/kicad-server.ts` brackets every
  request with it — host wins going in, MEMFS wins coming out — for the directories the suite owns
  (only under the OS temp dir, so KiCad's QA data stays read-only). `CreateLibrary` was the one case
  that needed directory mirroring: it creates an empty `conf_fp.pretty/`.
- **Isolation test** — `packages/kicad-wasm/test/isolation.kicad.test.ts`, numbers above.
- **Bridge against the real module** — `packages/bridge/test/session-wasm.kicad.test.ts`: session
  ready in 255 ms, 6 footprints over the WebSocket through the ordinary client model, `SaveDocument`
  flushed back to the workspace (400 780 → 401 542 bytes), `DELETE` stops the worker. The existing
  `session-wasm.test.ts` keeps running against the JS mock; its assertions are mock-specific, so it
  is not worth re-pointing at a real build.
- **App** — two fixes were needed before `?wasm=1` could open anything (`2869437`): the browser stub
  for `node:fs/promises` had no `rm`, so `vite build` failed; and the file picker wrote the project
  into the _current_ module's MEMFS, which `connect()` then replaced with a fresh module — imports
  are now replayed into each new instance. After that, the real 37 MB module renders the kitchen
  sink in the tab. No Playwright harness exists, so this was driven by hand in a browser.
- **`bun test --cwd packages/bridge`** shows 15 failures without `KICAD_CLI` pointing at
  `build/dev`; with it, `bridge.kicad.test.ts` is 16/16. That is the stale `build/release` binary,
  not this work.

## browser-worker

Follow-ups 2 and 10 (branch `wasm`, agent `browser-worker`), 2026-09-09.

- **The module runs in a Web Worker.** `packages/kicad-wasm/src/worker.ts` (entry) +
  `worker-core.ts` (the logic, written against a port interface) own one instance and its MEMFS;
  `worker-client.ts` is the page side. Same protocol as the bridge's backend, minus `{flush}` (a tab
  has no disk to mirror to) and plus `{id, fs}` for writeFiles / readFile / exists / stat /
  listFiles / mkdir — which is why `stat()` and `listMemfs()` on the session are now async.
- **`dispatchAsync`.** `KiCadWasmInstance.dispatch` is synchronous because the ABI is, so it cannot
  cross a thread. `WasmTransport` now prefers an optional `dispatchAsync` when the instance has one;
  in-process callers (Bun, the bridge, conformance) keep the straight-line path unchanged. An abort
  on the worker thread comes back as an error named `KiCadWasmAbort`, which the transport treats
  like the `WebAssembly.RuntimeError` it would have caught itself, so it still closes.
- **One module per tab.** `ownsInstance: false` on the transport; a teardown that is really a
  reconnect keeps the module, `disconnect()` shuts it down, and only an abort forces a reload (the
  import replay stays for that case). `?wasm=1` used to fetch 37 MB twice — once for the startup
  `GetVersion`, once for the project the user then picked.
- **Measured in the tab, real module, ecc83:** a worker round trip is **0.037 ms**; fifteen awaited
  KiCad calls in a row give the main thread **0** macrotask turns on the main-thread path and
  **2 507** through the worker; `kicad_api.wasm` is fetched **once** per page session (verified in
  the network log); the board renders and DRC reports its 17 warnings. `?wasm-main=1` (or
  `WasmModeOptions.inWorker: false`) keeps the old path for debugging.
- **Verified** — `bun run typecheck`; `bun test` 177/177 in apps/web (two new tests: the reuse, and
  the whole worker protocol through an in-process fake Worker), 18/18 unit + 20/20 with the real
  module in packages/kicad-wasm, 135/135 unit in packages/client; `vite build` green (`worker.ts`
  comes out as its own 6.6 KB ES-module chunk). The `.kicad.test.ts` failures in packages/client are
  the usual missing `KICAD_CLI`, not this work.
- **Still open here:** no cancel button — `terminate()` is available to the client but nothing in the
  UI calls it, so a wedged command still needs a reload; and MEMFS is only reachable through the
  session, so a future "download my project" needs `readFile` wired to the UI (the message is there).

## rebuild

The rebuild after everything above landed (branch `wasm`, agent `rebuild`), 2026-09-09.

- **The link failed first, exactly as documented.** The BOM data model defines six symbols
  (`FIELDS_TABLE_DATA_MODEL_BASE`'s typeinfo and two statics, `wxGridTableBase`'s typeinfo, the two
  editor grid model vtables) that `host/wasm/wasm_link_stubs.cpp` still carried.
  `tools/wasm/regen_wasm_link_stubs.sh` took the file from 71 data stubs to **65** and the link went
  through. The compile pass was ~40 minutes because `-DwxDEBUG_LEVEL=0` and `WASM_OPT_LEVEL=-O2`
  invalidated every KiCad TU.
- **`kicad_api.data` was not being found.** `--preload-file` makes Emscripten ask for the package by
  bare name, and its two fallbacks are both wrong here: `readFileSync("kicad_api.data")` relative to
  the cwd under Bun, and a `fetch` against the *document* URL in a browser. `createKiCadWasm()` now
  installs a `locateFile` that resolves every sidecar against the module's own URL (and hands back a
  plain path for `file:` ones, which the data loader — unlike the wasm loader — cannot parse), and
  both workers pass their `moduleUrl` through for it. Without this the module aborted at load in
  every backend.
- **Numbers above are all from this run**: conformance on `stdio` and `wasm`, `bench-drc.ts` on both,
  `bun test` in `packages/kicad-wasm` (20/20) and the bridge's real-module test (4/4, session ready
  in 253 ms), `tools/wasm/size-report.sh` plus a `brotli -q 11` pass, and the browser session with
  the ecc83 fixture.
- **Fonts have a wasm-preloaded variant now.** `packages/client/test/fonts.kicad.test.ts` keyed its
  whole suite off a host `KICAD_FONTS_DIR`, which is precisely the thing the preloaded bundle
  removes the need for; a second `describe` runs under `KICAD_TRANSPORT=wasm` with no fonts
  directory and checks that `Carlito` comes back as polygons where the stroke font gives segments.
- **Left alone deliberately:** `apps/web/vite.config.ts` was patched to log each sidecar request
  while the browser check ran, and restored afterwards — the "fetched exactly once" figure comes
  from that log, not from the page's resource timings, because the fetches happen inside the Worker.
