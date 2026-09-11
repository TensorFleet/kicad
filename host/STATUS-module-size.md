# Module size — what `kicad_api.wasm` is made of, and what actually shrinks it

Agent `module-size`, 2026-09-09.  Follow-ups 5 and 6 in `host/STATUS.md`.

**`build/wasm` was not touched.**  Every experiment ran in `build/wasm-size`; the link-only ones
read `build/wasm`'s objects and archives and wrote their output under `build/wasm-size/relink/`.
No `ninja` process ever ran in `build/wasm`.

## The short version

- **The build was never `-O2`.**  `tools/wasm/env.sh` puts `-O2` in `CMAKE_CXX_FLAGS`; CMake then
  appends `CMAKE_CXX_FLAGS_RELEASE`, which is `-O3 -DNDEBUG`, and the last `-O` wins.  Every KiCad
  TU in the 37.4 MB module was compiled at **`-O3`**.
- **`-O2` is now the default.**  `tools/wasm/configure.sh` sets `CMAKE_{C,CXX}_FLAGS_RELEASE` from a
  new `WASM_OPT_LEVEL`, default `-O2`: the module drops to **35.53 MB**, which is −3.7% against an
  `-O3` control built from the same tree (−5.1% against the 37.4 MB reference, the rest being
  `-DwxDEBUG_LEVEL=0` and source changes).  It costs **2.5% on DRC** and nothing on project open,
  and conformance is unchanged.  `WASM_OPT_LEVEL=-O3` puts it back.
- **`-Os` is the only large win, and it costs 28% of DRC.**  30.43 MB (−17.5% against the same-tree
  `-O3` control), conformance clean, but DRC on the kitchen sink goes 81 ms → 105 ms and project
  open 115 ms → 131 ms.  Outside the 20% budget, so it stays opt-in as `WASM_OPT_LEVEL=-Os`.
- **Compression beats all of it.**  The unmodified 37.4 MB module is **6.65 MB with `brotli -q 11`**
  — an 81% reduction for a `Content-Encoding` header and no build change whatsoever.  At `-O2` it
  is 6.44 MB and at `-Os` 5.77 MB.  Serving the module compressed is worth more than every
  compile-flag change combined, and it is the one thing not yet done.
- **Five of the six link-time experiments are worth nothing**, and one of them is actively harmful:
  post-link `wasm-opt -Oz` shrinks the raw file 3.6% but makes it *less compressible* (gzip goes
  **up** 1.5%), `emmalloc` saves 7 KB, selective `--whole-archive` saves 5 KB,
  `-sASSERTIONS=0`/`-sSTACK_OVERFLOW_CHECK=0` are already the defaults at `-O1`+, and
  `--closure 1` **breaks the module**.
- The "792 functions still imported as throwing JS stubs" figure in `host/STATUS-wasm-build.md` is a
  link-line count, not what shipped.  The module imports **501 functions**: 57 Emscripten/WASI
  runtime and **444 KiCad symbols left undefined**.  `tools/wasm/audit_undefined.py` classifies
  them; `-sERROR_ON_UNDEFINED_SYMBOLS=1` is not reachable, and why is below.

## Baseline

`build/wasm/host/kicad_api.wasm`, `-O3`, measured with `tools/wasm/size-report.sh`:

| | bytes | |
| --- | ---: | --- |
| `kicad_api.wasm` raw | **37 438 919** | 35.70 MiB |
| gzip -9 | 10 545 677 | 10.06 MiB |
| brotli -9 | 8 075 855 | 7.70 MiB |
| **brotli -q 11** | **6 975 395** | **6.65 MiB** |
| `kicad_api.js` raw / gzip -9 | 224 437 / 43 971 | |

Sections: `CODE` 31 669 481 (84.6%), `DATA` 5 626 428 (15.0%), everything else 143 kB (0.4%).
37 422 function bodies, 17 exports, 501 function imports.  There is no name, producers or debug
custom section — the module is twelve standard sections and nothing else, so `--strip-debug` and
`--strip-producers` have nothing to remove.

### Where the code actually is

emcc strips the name section at `-O1` and above, so `wasm-objdump`, `twiggy` and `wasm-opt
--metrics` can attribute nothing.  Relinking the same objects with `--emit-symbol-map` gives a name
for every function body.  Grouped by owning class, as a share of the 31.7 MB `CODE` section:

| subsystem | bytes | share |
| --- | ---: | ---: |
| **ngspice model tables** (`SIM_MODEL_*`, `NGSPICE_MODEL_INFO_MAP`) | 3 446 271 | 10.9% |
| protobuf + abseil | 3 421 604 | 10.8% |
| settings / JSON (`*_SETTINGS`, nlohmann) | 1 488 090 | 4.7% |
| **foreign-format importers, all 14 together** | **~6 400 000** | **~20.2%** |
| — PADS | 1 377 551 | 4.4% |
| — CADSTAR | 993 377 | 3.1% |
| — OrCAD | 759 378 | 2.4% |
| — EasyEDA / EasyEDA Pro | 699 064 | 2.2% |
| — Altium | 602 598 | 1.9% |
| — Allegro, Eagle, IPC2581, DipTrace, PCAD, ODB++, Fabmaster, Sprint, AutoTrax | ~1 578 000 | 5.0% |
| wxWidgets | 819 247 | 2.6% |
| harfbuzz + freetype | 741 443 | 2.3% |
| PNS router | 592 331 | 1.9% |
| plot / gerber | 563 110 | 1.8% |
| DRC test providers | 512 278 | 1.6% |
| jobs | 437 170 | 1.4% |
| ERC | 258 943 | 0.8% |
| connectivity | 184 755 | 0.6% |
| zone filler | 151 443 | 0.5% |
| unclassified | 12 504 536 | 39.5% |

The three largest single functions are all ngspice model-parameter tables:
`SIM_MODEL_NGSPICE::ModelInfo` at **1 610 902 bytes**, `NGSPICE_MODEL_INFO_MAP::addHSIM()` at
717 507 and `addB3SOI()` at 474 351.  ngspice itself is not linked; the tables are, because the
SPICE netlist exporter reaches `SIM_MODEL`.  Fourth is `__wasm_call_ctors` at 374 669 bytes — the
static-initialiser chain, which is what keeps most of the above alive.

## Every experiment, with the numbers

Compile-flag changes need a full rebuild (~1 h at `-j4`); link-flag changes are a **50 s relink** of
the same objects (5 min if `wasm-opt -Oz` is involved), which is why the link rows are dense.
Wire sizes are what a CDN would actually serve.

| # | change | scope | wasm raw | Δ raw | gzip -9 | brotli -9 | brotli -q11 | `.js` |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | baseline `-O3`, whole-archive, dlmalloc | — | 37 438 919 | — | 10 545 677 | 8 075 855 | 6 975 395 | 224 437 |
| 0′ | `-O3` control, same tree as a1/a2 | compile | 36 895 435 | −1.5% | 10 473 695 | 8 044 795 | 6 945 715 | 208 209 |
| a1 | **`-O2` for KiCad TUs — now the default** | compile | **35 532 518** | **−5.1%** | 10 120 825 | 7 812 691 | **6 753 624** | 208 209 |
| a2 | **`-Os` for KiCad TUs** | compile | **30 429 154** | **−18.7%** | 8 789 231 | 6 920 935 | **6 054 127** | 208 209 |
| b1 | post-link `wasm-opt -Oz` on the `-O3` objects | link | 36 073 652 | −3.6% | 10 703 933 ⚠ | 8 115 560 ⚠ | — | 202 057 |
| b2 | `-Os` + post-link `-Oz` | both | 29 367 248 | −21.6% | 8 908 248 ⚠ | 6 915 774 | — | 188 330 |
| c | `-flto` | compile | compatible, not measured at scale | | | | | |
| d | `-sASSERTIONS=0 -sSTACK_OVERFLOW_CHECK=0` | link | no-op — already the default at `-O1`+ | | | | | |
| e | selective `--whole-archive` (DRC providers only) | link | 37 433 454 | −0.015% | — | — | — | 224 437 |
| f | `-sMALLOC=emmalloc` | link | 37 431 541 | −0.02% | — | — | — | 224 437 |
| g | `--closure 1` | link | unchanged (JS-only) | — | — | — | — | **79 081** ❌ |

⚠ compresses *worse* than the baseline despite a smaller raw file.  ❌ does not work; see (g).

### Speed and correctness of each candidate

`packages/client/test/bench-drc.ts` under `KICAD_TRANSPORT=wasm`, three rounds round-robin on an
idle machine (the four modules interleaved, so any drift hits all of them equally); DRC is the
steady-state figure, the first of the three DRC runs is always ~30 ms slower.  Conformance is
`bun run test:conformance`.

| module | open | DRC (steady) | ping | headless conformance |
| --- | ---: | ---: | ---: | --- |
| baseline `-O3` (`build/wasm`) | 115 ms | 80–82 ms | 0.181 ms | 150/153, 6/6 extra |
| **`-O2`** | **114 ms** | **81–84 ms** | 0.182 ms | **151/153, 6/6 extra** |
| `-Os` | 130–133 ms | 102–108 ms | 0.184 ms | **151/153, 6/6 extra** |
| `-O3` + post-link `-Oz` | 116–118 ms | 80–83 ms | 0.183 ms | not run |

All four report 11 DRC markers on the kitchen sink, which is the check that the DRC test-provider
registrations survived.

`-O2` costs **+2.5% on DRC** and nothing on open — inside the 20% budget by a wide margin.  `-Os`
costs **+28% on DRC** and **+14% on open** — outside it, so it does not become the default.

The 150 → 151 improvement is **not** caused by these flags: it is another agent's
`FIELDS_TABLE_DATA_MODEL_BASE` fix landing in the tree between `build/wasm` being built and these
builds (`RunSchematicJobExportBOM` now passes).  Both candidates were run against the same tree and
both score 151; the two remaining failures are the documented `RunBoardJobExport3D` and
`RunBoardJobExportRender`.

### (a) optimisation level

The knob that works is `CMAKE_{C,CXX}_FLAGS_RELEASE`, **not** `CMAKE_CXX_FLAGS`: CMake appends the
per-configuration flags after the general ones, so anything `env.sh` puts in `CMAKE_CXX_FLAGS` is
overridden by the `-O3` in `CMAKE_CXX_FLAGS_RELEASE`.  `configure.sh` now sets both from
`WASM_OPT_LEVEL` (default `-O2`).

`-Os` moves `CODE` from 31 669 481 to 24 805 895 (−21.7%); `DATA` barely moves
(5 626 428 → 5 478 460), which is why the whole-module saving is 18.7% rather than 21.7%.

The rows above compare against `build/wasm`, which also predates `-DwxDEBUG_LEVEL=0` in `env.sh` and
several source changes other agents landed in the meantime.  So a third full build was done at
`-O3` **on the same tree, through the same `configure.sh`**, to separate the two effects:

| same tree, same everything but `-O` | wasm raw | gzip -9 | brotli -q11 | open | DRC (steady) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `-O3` control | 36 895 435 | 10 473 695 | 6 945 715 | 113–116 ms | 80–82 ms |
| **`-O2`** | **35 532 518** | 10 120 825 | 6 753 624 | 114–115 ms | 82–84 ms |
| `-Os` | 30 429 154 | 8 789 231 | 6 054 127 | 129–134 ms | 103–106 ms |

So the honest, isolated figures are:

- **`-O3` → `-O2`: −3.7% raw, −2.8% on the wire, +2.5% on DRC, no change to project open.**  The
  remaining 1.4% of the 5.1% headline is `-DwxDEBUG_LEVEL=0` plus the tree moving on.
- **`-O3` → `-Os`: −17.5% raw, −12.8% on the wire, +28% on DRC, +14% on project open.**

`-O2`'s 2.5% DRC cost is consistent across all three rounds rather than noise, so it is a genuine
(small) trade rather than a free win: 1.36 MB of module against 2 ms of a 82 ms DRC run.  It is
inside the 20% budget by a wide margin and it is one variable away from being undone
(`WASM_OPT_LEVEL=-O3`), which is why it is the default.  If a future workload makes DRC latency the
binding constraint rather than download size, flip it back.

### (b) post-link `wasm-opt -Oz` — shrinks the file, grows the download

emcc already runs `wasm-opt` at the link's `-O` level, which is `-O2` here.  Raising it to `-Oz`
takes the link from 50 s to 5 min and buys 3.6% of raw size — and the result **compresses worse**:
gzip -9 goes 10 545 677 → 10 703 933 (+1.5%), brotli -9 8 075 855 → 8 115 560 (+0.5%).  Binaryen's
size passes trade repeated byte patterns for fewer bytes, and the compressor was living on those
patterns.  On top of `-Os` the same thing happens to gzip (+1.4%) while brotli is a wash (−0.07%).
It is perf-neutral (DRC 80–83 ms), so it is not harmful — it just does not help the number that
matters.  **Not adopted.**

### (c) `-flto` — compatible, unmeasured

A two-TU probe with `-O2 -flto -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1` compiles to bitcode,
links, and throws and catches across the TU boundary correctly, so LTO and the wasm EH proposal do
work together in emscripten 6.0.9 — the obvious worry was unfounded.  It was not measured at scale:
every prebuilt dependency in `build/wasm-deps/prefix` (wx, protobuf, abseil, zstd) is an object
archive rather than bitcode, so LTO would cover only KiCad's own ~1100 TUs, and an LTO link over
those is a multi-hour, many-gigabyte step that has to follow a full rebuild.  It is the obvious
next experiment for an idle overnight machine, and the one most likely to beat `-Os` without
`-Os`'s speed cost.

### (d) assertions — already off

`-sASSERTIONS` and `-sSTACK_OVERFLOW_CHECK` default to 0 at `-O1` and above.  The baseline
`kicad_api.js` contains no `assert(`, no `"Assertion failed"` and no `checkStackCookie`, and a
trivial `emcc -O2` probe produces the same.  `-DNDEBUG` already comes from
`CMAKE_CXX_FLAGS_RELEASE`.  `-DwxDEBUG_LEVEL=0` **is** in effect for KiCad's TUs in any build
configured after `env.sh` gained it (confirmed on `build/wasm-size`'s compile lines).

### (e) selective `--whole-archive` — no win, and the reason is worth knowing

The hypothesis was that `-Wl,--whole-archive libkicad_api_host.a` (67 MB, 328 members) drags in
everything, and that only the DRC/ERC registrations actually need forcing.  Tested by extracting
the 31 `drc_test_provider_*.o` members, putting them on the link line as plain objects, and
dropping the `--whole-archive` pair so the archive resolves on demand:

    37 438 919  ->  37 433 454        (-5 465 bytes, 0.015%)

**The whole-archive link costs almost nothing**: nearly every member is pulled by ordinary symbol
resolution anyway, because the two statically linked kifaces reference them.  The 31 DRC providers
are the only members that needed forcing, which is exactly what the native gate found
(`host/STATUS-native-gate.md`: "DRC found 4 markers where kicad-cli finds 13").  There is no
dead-code win hiding behind the link flag, so `host/wasm/CMakeLists.txt`'s link options are
unchanged.

The 3.4 MB of ngspice tables and the 6.4 MB of foreign-format importers are **not** whole-archive
artefacts either — they are genuinely referenced, by `SIM_MODEL` from the SPICE netlist exporter and
by `PCB_IO_MGR`'s / `SCH_IO_MGR`'s plugin factories respectively.  Removing them needs a
source-level `KICAD_HEADLESS_API` exclusion, not a linker flag.  See the recommendations.

### (f) `-sMALLOC=emmalloc` — 7 KB

37 438 919 → 37 431 541.  Below the noise floor of anything else here, and it changes allocator
behaviour under a workload (DRC, zone fill) that allocates hard.  Not worth the risk for 0.02%.

### (g) `--closure 1` — breaks the module

It does what it advertises for the JS — `kicad_api.js` goes 224 437 → **79 081** bytes (−65%,
24 527 gzipped) and the `.wasm` is byte-identical — but the module then fails on the first project
open with

    TypeError: fs.isDir is not a function. (In 'fs.isDir(mode)', 'fs.isDir' is undefined)

Closure minifies the members of the `FS` object, and `FS` is part of this module's public contract:
`-sEXPORTED_RUNTIME_METHODS=...,FS` exists precisely so fab_pcb's `mountProject` / `exportDir` /
`DirMirror` can call `FS.isDir`, `FS.readdir` and `FS.stat` from outside.  Adopting closure would
mean an externs file naming every `FS` member the loader touches, or moving all MEMFS traffic
behind wrappers.  **Not viable as-is**, and the 145 KB it saves is 0.4% of the download.

## Undefined symbols (follow-up 6)

`tools/wasm/audit_undefined.py build/wasm/host/kicad_api.wasm` reads the module's own import
section — so it reports what survived the link, not what the link line asked for — demangles with
`llvm-cxxfilt`, groups by owning class, and marks the classes that headless handler and job sources
name:

    501 function imports total
     57 emscripten/wasi runtime imports (supplied by kicad_api.js)
    444 KiCad symbols left undefined -- each one throws if reached

    128 stubs sit in classes the headless API sources name; the rest are GUI-only.

Run against the current tree's `-O2` module instead of the reference, the count is already down to
**393** — the `FIELDS_TABLE_DATA_MODEL_BASE` fix landing took 51 stubs (that class plus most of
`wxGridTableBase`) with it, which is a good sign for how the rest of the list behaves once the
classes behind it are given headless definitions.

The reference module's 444 break down as:

- **~280 in wx GUI classes** — `wxWindow` (62), `wxWindowBase` (54), `wxTopLevelWindowGTK` (28),
  `wxGridTableBase` (23), `wxDialogBase` + `wxDialog` (14), and a long tail.  These are the price of
  the `wxUSE_GUI=1` header shim: KiCad's kept TUs *declare* GUI types and a wxBase-only link has no
  definitions.  Nothing headless constructs a window, so none is reachable — but they are also why
  `-sERROR_ON_UNDEFINED_SYMBOLS=1` cannot simply be turned on.
- **23 in `FIELDS_TABLE_DATA_MODEL_BASE`** — the BOM-export failure, `STATUS.md` follow-up 1.  This
  is being fixed in the same tree right now, and the fix started defining five symbols that
  `host/wasm/wasm_link_stubs.cpp` still stubs, so the module link failed with `duplicate symbol` —
  the documented stale-stub trap.  **`tools/wasm/regen_wasm_link_stubs.sh` must be re-run** before
  the next module build.  The measurements here were taken with a locally patched *copy* of the
  stub TU (five `KI_HEADLESS_DATA` lines commented out, compiled into the scratch dir); the shared
  file was never edited.
- **~90 in editor frames and tools** — `SCH_EDIT_FRAME` (11), `PCB_EDIT_FRAME` (7),
  `PCB_SELECTION_TOOL` (5), `PCB_BASE_FRAME` (4), `GLOBAL_EDIT_TOOL` (3), `SCH_SELECTION_TOOL` (3),
  `EDA_DRAW_FRAME` (3) and friends.  **These are the ones worth guarding first**: the API handlers
  name them by hand (`pcbnew/api/pcb_context.cpp`, `eeschema/api/sch_context.cpp`,
  `pcbnew/api/api_handler_board.cpp`), so a command that takes a "we have a frame" branch reaches a
  throwing stub instead of a clean error.
- **~30 one- and two-symbol `DIALOG_*` classes named by the job handlers** — `DIALOG_PLOT`,
  `DIALOG_GENDRILL`, `DIALOG_EXPORT_STEP`, `DIALOG_DRC_JOB_CONFIG`, `DIALOG_ERC_JOB_CONFIG`,
  `DIALOG_EXPORT_2581`, `DIALOG_EXPORT_ODBPP` and so on.  `pcbnew_jobs_handler.cpp` and
  `eeschema_jobs_handler.cpp` reference each job's configuration dialog for the interactive path;
  the headless path never opens one, but the reference keeps the vtable undefined.

**`-sERROR_ON_UNDEFINED_SYMBOLS=1` is not reachable today** — it needs real headless definitions for
all 444, which is the wx-shim problem rather than a flag.  The useful intermediate step is the ~120
frame/tool/dialog symbols: they are KiCad's own classes, they are named by code that does run, and
each is a `#ifdef KICAD_HEADLESS_API` guard plus an error return away from being a clean
"not available in this build" instead of an instance-killing trap.  Keep
`KICAD_WASM_ALLOW_UNDEFINED=ON` until then.

## What to do next, in value order

1. **Serve the module compressed.**  6.65 MB brotli against 37.4 MB raw, today, with no build
   change — bigger than everything below put together.  Nothing in fab_pcb or the Vite dev
   middleware sets `Content-Encoding` yet.
2. **`-flto`.**  Compatibility is proved; the measurement is not.  It is the only remaining
   candidate that could approach `-Os`'s 18.7% without `-Os`'s 27% DRC cost.
3. **Exclude the ngspice model tables from `KICAD_HEADLESS_API`.**  3.4 MB, 10.9% of the code, for
   three functions that exist to describe simulator model parameters.  They are pulled in by the
   SPICE netlist exporter, so this needs a real seam (a `SIM_MODEL` that reports "no model
   database" headlessly), not a flag — and `RunSchematicJobExportNetlist` must stay green.
4. **A `KICAD_HEADLESS_MINIMAL_IO` option** dropping the 14 foreign-format importers: ~6.4 MB,
   20% of the code.  They are reachable only through `PCB_IO_MGR`/`SCH_IO_MGR` factories, and the
   API's own commands open KiCad files.  Needs the factories to answer "unsupported format" for the
   excluded plugins, and a check that no conformance case imports one.
5. **The ~120 frame/tool/dialog stubs** above, so that a command that reaches one gets an error
   rather than an `abort()` that kills the instance.
6. **Re-run `tools/wasm/regen_wasm_link_stubs.sh`** — required now, see above.

## Tools added

- **`tools/wasm/size-report.sh <module.wasm> [...]`** — raw / gzip -9 / brotli -9 sizes, every wasm
  section with its byte count, import and export counts (imports grouped by module), and, with a
  `kicad_api.js.symbols` beside the module from a `--emit-symbol-map` link, the largest functions by
  name.  Reads only, so it is safe against a build tree another process owns.  `TOP_FUNCS=n` sets
  how many functions to list.
- **`tools/wasm/audit_undefined.py <module.wasm>`** — the import-section audit above.  `--list`
  prints every symbol, `--json` is machine-readable, `--api-dirs` changes what counts as handler
  code.

Both parse the wasm binary directly.  Neither `wasm-objdump` (wabt) nor `twiggy` is installed on
this machine, and neither would have helped: without a name section they can only report section
sizes, which `llvm-objdump -h` already does.

## Reproducing

```sh
# a size-experiment build tree of its own; configure.sh honours WASM_BUILD_DIR and WASM_OPT_LEVEL
source tools/wasm/env.sh
WASM_BUILD_DIR=$PWD/build/wasm-size tools/wasm/configure.sh          # -O2, the new default
WASM_OPT_LEVEL=-Os WASM_BUILD_DIR=$PWD/build/wasm-size tools/wasm/configure.sh   # the small one
ninja -C build/wasm-size -j4 kicad_api

tools/wasm/size-report.sh build/wasm-size/host/kicad_api.wasm
tools/wasm/audit_undefined.py build/wasm-size/host/kicad_api.wasm

# from fab_pcb/packages/client
KICAD_TRANSPORT=wasm KICAD_WASM_DIR=<kicad>/build/wasm-size/host bun test/bench-drc.ts
KICAD_TRANSPORT=wasm KICAD_WASM_DIR=<kicad>/build/wasm-size/host bun run test:conformance
```

The three finished modules are kept side by side for comparison, each a `kicad_api.js` +
`kicad_api.wasm` pair that `KICAD_WASM_DIR` can be pointed straight at:

    build/wasm-size/modules/o3/     -O3, same tree as the other two (the control)
    build/wasm-size/modules/o2/     -O2, the new default
    build/wasm-size/modules/os/     -Os
    build/wasm-size/relink/*/       the six link-flag experiments

`build/wasm-size` itself is left configured at `-O3` because that is what its object files are; a
`configure.sh` with no `WASM_OPT_LEVEL` plus a full rebuild returns it to the `-O2` default.

Run the benchmark on an **idle** machine.  A concurrent `ninja -j4` or a `wasm-opt` inflates open
and DRC by a factor of two to three; two rounds of measurements here had to be thrown away for
exactly that reason, and the ones in the tables above were taken with nothing else running and the
four modules interleaved.
