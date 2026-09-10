# Linux gate for the `wasm` branch — **PASS** (option-OFF build built and smoke-tested)

Date: 2026-09-11. Tree built: `wasm` @ `da0e31f585` (git worktree copy, not the live checkout).
Machine: Docker Desktop on Apple Silicon, **linux/arm64**, 6 CPU / 7.7 GB VM, `-j3`.

Everything below is measured unless a sentence says otherwise. (The previous revision of this
file was a static source audit written while no Docker daemon was available; its predictions are
kept only where a build confirmed them.)

## 1. Result summary

| gate | result |
|---|---|
| `docker build` of `packages/kicad-patches/docker/Dockerfile`, `KICAD_SOURCE=context` | **pass**, 3985 s (66 min) for the cmake+ninja layer, ~68 min wall |
| `kicad-cli version` in the runtime image | **pass** — `10.99.0` |
| `packages/kicad-patches/docker/smoke.sh` (api-server + `tooling/m0/ping.ts`) | **pass** — Ping / GetVersion / OpenDocument / GetOpenDocuments all `AS_OK` |
| `-DKICAD_HEADLESS_API=ON` configure | **pass** |
| `ninja kicad-api-host-native` compile (1109 edges) | **pass**, 0 compile errors |
| …its link | **fails**: one duplicate symbol; with `--allow-multiple-definition` it links |
| …the linked binary runs | **no** — `ld.so` rejects it before `main` (see §4.3) |
| source fixes required for the OFF build | **none** |

`host/linux-fixes.patch` contains **one optional, cosmetic hunk** (a `-Wreorder` warning, §3.2).
Nothing in it is required for the build to succeed; the orchestrator may skip it.

## 2. The option-OFF build (what CI compiles) — green

Command actually run (the worktree copy stands in for the fork checkout):

    KICAD_SRC=<worktree> JOBS=3 packages/kicad-patches/build-linux.sh

* Configure and all 2336 ninja edges: **0 errors**. `kicad-cli`, `_pcbnew.kiface`,
  `_eeschema.kiface` and the `libki*.so` set all produced.
* The Dockerfile's own guards passed: no `ldd … not found`, the runtime-package list came out
  non-empty, and the runtime stage's `kicad-cli version` check succeeded as user `kicad`.
* Final image `fp-pcb/kicad-cli:da0e31f585`, **1.18 GB**.
* No apt package had to be added. The dependency list in the Dockerfile is sufficient as written.

Smoke (`smoke.sh` with the image and the worktree's `qa/data/pcbnew`), verbatim:

    SP handshake ok (peer is REP0)
    Ping:            AS_OK (3.87 ms)
    GetVersion:      AS_OK (1.36 ms)   KiCad 10.99.0  "10.99.0-unknown"
    OpenDocument:    AS_OK (121.39 ms) opened: api_kitchen_sink.kicad_pcb
    GetOpenDocuments:AS_OK (1.60 ms)
    server log: KiCad API server listening at ipc:///tmp/kicad/api.sock
                KiCad API events published at ipc:///tmp/kicad/api-events.sock

So the branch's in-process dispatch seam, the event socket and the exporter lift all survive a
GCC 14 / libstdc++ / Debian trixie build. The three unguarded source changes the old audit worried
about (`common/eda_shape.cpp`, `common/eda_text.cpp`, `libs/kiplatform/os/unix/io.cpp`) are
confirmed harmless.

### 2.1 Build-time caveat for planning CI

The ccache in the BuildKit cache mount was **49 % warm** on this run (`Hits: 519 / 1062`), so 66
min is *not* a cold number. A GitHub `ubuntu-24.04` runner has no persisted ccache and 4 CPUs, so
budget closer to the job's 120-minute timeout for the first build after a `KICAD_COMMIT` repoint.
The job is `continue-on-error: true`, so a timeout will not redden the workflow, but it will also
not publish the image, and the next run repeats the whole build.

## 3. Warnings worth knowing about

13 distinct warning lines. All but one are pre-existing KiCad noise (`-Wshadow`,
`-Wmaybe-uninitialized` inside `basic_string.h`).

### 3.1 (informational) `-Wmaybe-uninitialized` in `include/api/api_handler_editor.h:160`

`'changes.API_HANDLER_EDITOR::REVISION_CHANGES::Revision' may be used uninitialized`. GCC-only,
in this branch's own header. Not investigated; likely a false positive from inlining, but it is
the one warning that points at new API code doing something GCC cannot prove.

### 3.2 `-Wreorder` in `common/api/api_server.cpp:117` — the contents of `linux-fixes.patch`

`KICAD_API_SERVER::KICAD_API_SERVER` initialises `m_serverHandler` / `m_fallbackHandler` before
`m_eventSequence`, but `include/api/api_server.h` declares `m_eventSequence` (line 251) first.
Harmless — the three initialisers are independent — but it is a warning in new code, and the fix
is to move `m_eventSequence( 0 )` to the head of the init list. That single hunk is
`host/linux-fixes.patch`. **Optional.**

## 4. `KICAD_HEADLESS_API=ON` on Linux — compiles, links only with a flag, does not run

Measured in a dev container (`debian:trixie` + the Dockerfile's own apt list) with the source and
a build dir bind-mounted, so this is iterable without re-running `docker build`.

Configure with `-DKICAD_HEADLESS_API=ON` **succeeds** on Linux. The old audit's §4.3 prediction
holds: the wx `find_package` is unguarded, so the full `libwxgtk3.2-dev` +
`libwxgtk-webview3.2-dev` must be installed, but libsecret / poppler / OCCT / ngspice / nng /
libgit2 / cairo / pixman / curl / spnav / GL are genuinely skipped. The wxBase-only narrowing
works: the link line ends `-lwx_baseu-3.2 -lwx_baseu_xml-3.2`, no `wx_gtk3u_*` at all.

All **1109** compile edges succeed. There are **no compile errors** — no `__APPLE__`-only code
path breaks the Linux compile.

### 4.1 Link blocker #1 (real, fixable): duplicate `wxAnyValueTypeImpl<PCB_LAYER_ID>::sm_instance`

    /usr/bin/ld: common/libkicommon.a(property_value_converter.cpp.o):
        (.bss._ZN18wxAnyValueTypeImplI12PCB_LAYER_IDE11sm_instanceE[…]+0x0):
        multiple definition of `wxAnyValueTypeImpl<PCB_LAYER_ID>::sm_instance';
        common/libpcbcommon.a(board_item.cpp.o):(.bss+0x0): first defined here

`pcbnew/board_item.cpp:503` has `IMPLEMENT_ENUM_TO_WXANY( PCB_LAYER_ID )`, which expands to
`WX_IMPLEMENT_ANY_VALUE_TYPE(...)` and emits a **strong** `sm_instance` into `libpcbcommon.a`.
`common/diff_merge/property_value_converter.cpp:179` calls `tryAs<PCB_LAYER_ID>`, which
*implicitly instantiates* the same static into `libkicommon.a` — as a COMDAT group symbol, which
ELF still counts as a global definition, so GNU ld errors. Mach-O marks the implicit one
`weak_definition` and ld64 silently lets the strong one win; that is the whole platform
difference.

It only bites with the headless option because `KICAD_LIB_TYPE` becomes `STATIC` there — in the
OFF build `kicommon` is a shared library and the two never meet in one link.

Two ways out, neither attempted beyond the workaround: add
`-Wl,--allow-multiple-definition` next to the existing `--unresolved-symbols=ignore-all` in the
non-Apple branch of `host/CMakeLists.txt`, or (cleaner) give
`common/diff_merge/property_value_converter.cpp` the `DECLARE_ENUM_TO_WXANY( PCB_LAYER_ID )`
declaration so it stops instantiating its own copy. **With `--allow-multiple-definition` the link
completes** and produces a 59 MB `kicad-api-host-native`, so this is the only duplicate in the
tree.

### 4.2 The macOS stub file is, as expected, absent

`host/headless_link_stubs.cpp` is one `#if defined( __APPLE__ )` block, so on Linux it is an empty
translation unit. Relinking with `--unresolved-symbols=ignore-all` removed gives the real size of
the hole: **923 distinct undefined symbols** (3017 references), of which **91** are
`_ZT*` vtable / typeinfo / VTT entries and 2 are C-linkage wx event ids
(`EDA_EVT_UNITS_CHANGED`, `EDA_LANG_CHANGED`). The list is not checked in — regenerating it is one
relink away (§5).

### 4.3 `--unresolved-symbols=ignore-all` does not work on ELF — the binary cannot start

This is the finding that changes the plan. With the flag, the link succeeds, but:

    $ ./kicad-api-host-native
    error while loading shared libraries: unexpected PLT reloc type 0x00

`readelf -r` shows **574 `R_AARCH64_NONE` entries inside `.rela.plt`**, with symbol index 0 — that
is what `ld` writes for a call it was told to ignore. glibc's dynamic loader walks `.rela.plt`
eagerly at startup and aborts on any entry that is not `JUMP_SLOT`/`IRELATIVE`, so the process
dies before `main`, before any lazy binding could have saved a call that is never made. (Measured
on aarch64; x86-64 writes `R_X86_64_NONE` into the same section and glibc rejects it the same way,
so do not expect an x86 runner to behave differently.)

Consequence: on Linux the headless host is not "link now, abort at the call site later". Without a
real stub object the binary is unrunnable, full stop. `KICAD_HEADLESS_ALLOW_UNDEFINED=ON` is a
macOS-only convenience and should probably be forced OFF on Linux so the failure is a link error
rather than a mysterious loader message.

### 4.4 What a Linux backend for `tools/gen_link_stubs.py` would need

Not attempted, per the brief. The concrete shape, now that the real Linux link log exists:

1. **Getting the symbol list.** GNU ld's diagnostic is line-oriented, not the ld64
   `Undefined symbols for architecture …:` block, so `tools/regen_link_stubs.sh:16`
   (`sed -n '/Undefined symbols/,$p'`) matches nothing. The Linux shape is

       /usr/bin/ld: <archive>(<obj>.o): in function `<caller>':
       <file>.cpp:(.text+0x684): undefined reference to `<symbol>'

   with the `/usr/bin/ld: <obj>:` prefix dropped on repeats within one object. A parser is
   `grep -o "undefined reference to \`[^']*'"` plus dedupe — 4 lines, simpler than the ld64 path.
2. **Mangling comes for free.** Add `-Wl,--no-demangle` to the probe link and ld prints the raw
   Itanium names (`_ZN11ACTION_MENU…`, no leading underscore). That removes the entire reason
   `gen_link_stubs.py` needs `devsyms.txt` for *mangling*: 923 mangled names straight out of the
   linker, and the `EXTRA` hand-mangled table (`gen_link_stubs.py:24-25`, whose entries carry the
   ld64 `__Z` double underscore) is not needed on Linux at all.
3. **Function-vs-data still needs `nm`.** `_ZT*` and the wx event ids are obviously data, but class
   statics such as `wxGDIObject::ms_classInfo` mangle like anything else. Keep the
   option-OFF-build symbol harvest, but note GNU `nm` has no `-U`: `regen_link_stubs.sh:29,45`
   must use `nm -g --defined-only` instead of `nm -gU`.
4. **Emission.** libstdc++ spellings (`std::__cxx11::basic_string`,
   no `std::__1::`) fall out of step 2 automatically. The function stubs port as-is —
   `__asm__("mangled")` on an `extern "C"` definition works the same in GCC. The data block needs
   ELF directives instead of the current bare `.globl`/`.p2align`/`.space`: add
   `.type <sym>, @object` and `.size <sym>, 64`, and drop the Mach-O-only `.data` section naming
   assumptions. `backtrace()`/`backtrace_symbols_fd()` exist in glibc via `<execinfo.h>`, same
   spelling, so the abort thunk needs no change.
5. **The `#if defined( __APPLE__ )` wrapper** (`gen_link_stubs.py:67`) becomes a per-platform
   choice: generate `host/headless_link_stubs_linux.cpp` guarded on `#if defined( __linux__ )` and
   select it in `host/CMakeLists.txt`, rather than trying to make one file serve both — the symbol
   *sets* differ too (923 on Linux vs ~1400 on macOS), because the two toolchains inline and devirtualise
   differently.
6. **Do §4.1 first.** The duplicate-symbol error fires before the undefined-symbol pass, so nobody
   can even see the stub list on Linux until `sm_instance` is resolved.

This remains a separate work item, gated on a Linux box (or this container recipe) to regenerate on.

## 5. Reproducing the headless experiment without a full `docker build`

The iteration loop used here, ~1 min per relink:

    # dev image = FROM debian:trixie + the Dockerfile's own apt-get line (layer-cache hit)
    docker build -t kicad-dev:trixie <dir with that 3-line Dockerfile>
    docker run --rm -v <src>:/src -v <builddir>:/build kicad-dev:trixie \
      cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKICAD_HEADLESS_API=ON \
        -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF -DKICAD_USE_SENTRY=OFF \
        -DKICAD_UPDATE_CHECK=OFF -DKICAD_INSTALL_DEMOS=OFF -DKICAD_USE_PCH=ON
    docker run --rm -v <src>:/src -v <builddir>:/build kicad-dev:trixie ninja -C /build -j3 kicad-api-host-native
    # to re-probe the undefined set:
    docker run … sh -c 'cd /build && ninja -t commands kicad-api-host-native | tail -1' > link.sh
    #   then edit link.sh: -Wl,--unresolved-symbols=ignore-all  ->  -Wl,--allow-multiple-definition -Wl,--no-demangle

Full headless compile from cold at `-j3`: about 80 minutes.

## 6. `tools/wasm/host-tools.sh lemon`

Unchanged from the audit: run on the host, it passes in a couple of seconds and produces a 125 KB
`bin/lemon`. The stage is one `cc -O2 -w -o bin/lemon thirdparty/lemon/lemon.c` with no platform
assumptions, and everything around it is portable to GNU coreutils. Not re-run inside Linux; there
is nothing left in it that could differ.

## 7. CI action items

1. **Repoint `fab_pcb/packages/proto/KICAD_COMMIT`** once `wasm-clean` is rewritten and pushed.
   It currently pins `0c45443da6168edfba53f51a1195b54d7b708b45`; the `kicad-integration` job feeds
   that SHA to `git fetch --depth 1 origin <sha>` inside the `src-git` stage, which fails outright
   if the commit no longer exists on the fork.
2. **Expect a full image build on the first post-rewrite CI run.** The GHCR tag is
   `ghcr.io/<owner>/kicad-cli:<first 10 chars of KICAD_COMMIT>`, so repointing the pin guarantees a
   `docker pull` miss. See §2.1 for why that build is closer to the 120-minute timeout than to the
   66 minutes measured here.
3. **Nothing else needs to change for CI.** The OFF build needs no source fix and no new apt
   package. This gate was run on linux/arm64; CI runs x86-64, which is the one axis this
   measurement does not cover, though nothing that failed or passed here was arch-specific except
   the reloc *names* in §4.3.
4. **Do not enable `KICAD_HEADLESS_API` in any Linux CI job** until §4.1 and §4.4 land.
