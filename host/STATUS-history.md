# How this branch was rewritten

The headless/wasm work was done as 41 commits on a branch called `wasm`, in the order the
problems turned up: a change, then the fix for what it broke, then a status note, then the
same file touched again three commits later. That history is a record of an investigation,
not something anyone should have to bisect or review.

This branch is the same tree, rebuilt as 15 commits that each do one thing and build on the
one before. The final tree is identical to `wasm` apart from this file. The old branch is
kept at `f3663f2c55..wasm` if anyone wants the blow-by-blow.

## The new series

| # | Commit | What it is |
|---|--------|------------|
| 1 | API: Add an in-process dispatch seam to KICAD_API_SERVER | `StartInProcess`/`Dispatch`, no sockets |
| 2 | API: Extract API_SERVER_HOST from the kicad-cli api-server command | the reusable half of `api-server` |
| 3 | Common: Fix EDA_SHAPE( const SHAPE& ) leaving its fill out of range | plain bug, found headless, applies everywhere |
| 4 | Kicad: Give the symbols pcbnew and eeschema both define internal linkage | ODR fixes, prerequisite for one image |
| 5 | Pcbnew: Lift the IPC-2581 and ODB++ exporters out of their dialogs | exporters usable without a frame |
| 6 | Build: Add KICAD_HEADLESS_API and split the GUI out of the core libraries | the option, the source groups, the dependency gating |
| 7 | Headless: Link the pcbnew and eeschema kifaces statically | `RegisterStaticKiface`, per-target `Kiface`/`KIFACE_GETTER` |
| 8 | Headless: Portability shims for a build with no threads and no stack switching | coroutine, thread pool, inline jobs, `wxSafeYield` |
| 9 | Headless: Resolve outline fonts from a manifest | `fontconfig_manifest.cpp`, `KICAD_FONTS_DIR` |
| 10 | Headless: BOM data model without wxGridTableBase | `HEADLESS_GRID_TABLE_BASE`, the three fields models |
| 11 | Host: In-process host library, C ABI and the native stdio executable | `host/`, wxBase-only link, generated stubs |
| 12 | wasm: Emscripten toolchain, dependency builds and configure | `tools/wasm/`, the `if(EMSCRIPTEN)` branch |
| 13 | wasm: Build kicad_api.js and kicad_api.wasm | `host/wasm/`, the wxUSE_GUI header shim, data stubs |
| 14 | wasm: GitHub release workflow and packaging | the tag workflow, `package.sh`, `host-tools.sh` |
| 15 | docs: host/STATUS.md and the per-agent status files | this file and the status notes |

Commits 1-5 were rewritten in an earlier pass; 6-15 in this one.

## Old to new

| Old commit | Subject | Lands in |
|---|---|---|
| `3e875a479b` | API: Add an in-process dispatch seam to KICAD_API_SERVER | 1 |
| `77fb4216dd` | API: Extract API_SERVER_HOST from the kicad-cli api-server command | 2 |
| `c888753512` | Headless: EDA_SHAPE( const SHAPE& ) left its fill out of range | 3 |
| `8b0e34f1e8` | Headless: inline job execution, pgm_base guards, and cross-kiface ODR fixes | 4, 6, 8 |
| `84f8c8c4b6` | Headless: deferred inline jobs, whole-archive linking, and two exporters lifted out of their dialogs | 5, 8, 11 |
| `20475e3aa2` | Headless: add KICAD_HEADLESS_API and split the GUI out of the core libraries | 6 |
| `f9794962e0` | Headless: keep the option-OFF build byte-identical | 6 |
| `add9377a89` | Headless: put the non-GUI half of the API path back in the build | 6 |
| `9882935ed7` | Headless: drop the DRC rule editor, and size the wx shim | 6 |
| `3b439cc27c` | Headless: drop nng, libgit2, Cairo and Fontconfig from the headless link | 6, 9 |
| `dccb95c31b` | Headless: an in-tree wxPGChoices for the wxBase-only build | 6 |
| `b83e3960f9` | Headless: link the pcbnew and eeschema kifaces statically into one image | 7 |
| `f0e84ad8c0` | Headless: let KIWAY find a KIFACE that is linked into the image | 7 |
| `0a642082c6` | Headless: run thread pool tasks inline | 8 |
| `19be4b8755` | Headless: no wxSafeYield on the board plot path | 8 |
| `035db1f35e` | Headless: resolve outline fonts from a manifest instead of missing every lookup | 9 |
| `a7dfdd5e6f` | Headless: a BOM data model that is not a wxGrid table | 10 |
| `f659f439e1` | Host: an in-process API host, a C ABI, and the native stdio binary | 11 |
| `efde1d3571` | Headless: link against wxBase only, with real wxColour and wxImage | 11 |
| `c23b19cbd3` | wasm: three transitive-include fixes the GUI-enabled wx header chain used to hide | 11 |
| `98ace7e983` | wasm: dependency build script | 12 |
| `ea9a9423b5` | wasm: compile KiCad against the full wx header set with a wxUSE_GUI shim | 13 |
| `2e4163b854` | wasm: generate the DATA half of the headless GUI stubs for wasm-ld | 13 |
| `8376472d4d` | wasm: real values for the five plain-data globals the link is missing | 13 |
| `d4719b5450` | wasm: link the module (freetype setjmp variants) | 13 |
| `afc30d7673` | wasm: compile KiCad TUs with wxDEBUG_LEVEL=0 | 13 |
| `888b107163` | wasm: measure the module, and build KiCad's TUs at -O2 instead of -O3 | 13 |
| `78a0998b2a` | wasm: an -O3 control build | 13 (result only; the control build left no tree change) |
| `be56784b1c` | wasm: regenerate the link stubs against the BOM data model | 13 |
| `4a9f583468` | wasm: ship the module as a GitHub release artifact | 14 |
| `12efcc0288`, `2e9ffe9aec`, `8c7ff0dd01`, `656ad327a7`, `b63dda574a`, `4bd3e4387d`, `af1d93ff48`, `8b8a65ebbc`, `67a30a2661`, `981109bbbc`, `da0e31f585` | status notes | 15 |

Three old commits exist only because an earlier one was wrong, and have no separate
identity here: `f9794962e0` (restoring the option-OFF build after the first split),
`add9377a89` (putting back sources the first split removed too eagerly) and
`be56784b1c` (regenerating stubs after the BOM change). Their end state is folded into the
commit that should have had it.

## What the split was allowed to be sloppy about

Files touched by several of the new commits -- the CMakeLists, `pgm_base.cpp`,
`pcbnew.cpp`, `host/CMakeLists.txt` -- were split hunk by hunk. Two places are deliberate
approximations:

- Commit 6 removes the `find_package( Fontconfig )` but leaves `common/font/fontconfig.cpp`
  in the source list; commit 9 is what takes it out and puts the manifest reader in its
  place. So the headless build does not compile at commit 6-8. The option-OFF build does.
- `host/CMakeLists.txt` at commit 11 guards the native executable with `NOT EMSCRIPTEN`
  rather than the `EMSCRIPTEN / else()` pair it ends up with, so that adding `host/wasm` in
  commit 13 is a three-line change instead of a re-indent of the whole block.

The rule applied throughout: every commit compiles for the option-OFF build where it touches
option-OFF files, and the headless and wasm builds are only guaranteed at the commit that
completes them.

## Verified on the rebased tip

- `ninja -C build/check-off kicad-cli pcbnew_kiface eeschema_kiface` -- clean, and this is
  the build that must not regress. Also compile-checked incrementally after commits 6, 7
  and 8.
- `ninja -C build/headless kicad-api-host-native` -- clean.
  `kicad-api-host-native --selftest` -- 6 checks, 0 failures.
- The stdio conformance suite from `fab_pcb/packages/client`, against that binary:
  **175 pass, 2 fail** out of 177. The two failures are `RunBoardJobExport3D` and
  `RunBoardJobExportRender`, which report "not available in this build" because
  OpenCascade and the 3D renderer are not part of the headless core. That is the expected
  headless result.

The wasm module itself was not rebuilt for this rewrite: commits 12-14 only move files that
the `wasm` branch already built and measured, and the final tree is byte-identical to it.
See `host/STATUS.md` for those numbers.
