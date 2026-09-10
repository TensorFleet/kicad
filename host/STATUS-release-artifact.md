# wasm release artifact — status

**The wasm module can now be shipped as a GitHub release asset instead of being rebuilt by every
consumer.** Nothing has been pushed, tagged or released: the workflow and the scripts are on the
`wasm` branch of both repos and the first release is a deliberate act by a human.

## What was added

**Fork (`tensorfleet/kicad`)**

- `.github/workflows/wasm-release.yml` — Ubuntu 24.04, `timeout-minutes: 180`. Triggers on a push
  of any `fp-pcb/*` tag, and on `workflow_dispatch` (inputs: `ref`, `attach_to_release`). It
  installs emsdk **6.0.9** (a real emsdk release tag; Homebrew's build of the same source tag
  reports itself as `6.0.9-git`, and the workflow accepts either spelling), builds the two native
  host tools, builds the wasm dependency prefix, configures, links `kicad_api`, packages, uploads
  the tarball as a run artifact on **every** run, and on a tag attaches it to the GitHub release
  (created with `gh release create --verify-tag` if missing, asset `--clobber`ed on a re-run).
  Every step is a call into `tools/wasm/*.sh`, so the same sequence runs locally.
- `tools/wasm/package.sh` — packages `build/wasm/host` into
  `build/wasm-release/kicad-wasm-<tag-slug>.tar.gz`: `kicad_api.js`, `kicad_api.wasm`,
  `kicad_api.data` (if a build ever preloads one), `kicad-wasm.json` and `SHA256SUMS`, all flat at
  the tarball root. The **tag slug is the tag with `/` replaced by `-`** — release asset names
  cannot contain a slash — so `fp-pcb/2026-09-09-wasm` produces
  `kicad-wasm-fp-pcb-2026-09-09-wasm.tar.gz`. `kicad-wasm.json` records the fork commit (and
  whether the tree was dirty), the tag, emscripten and host-protoc versions, the protobuf / abseil /
  zstd / wx pins read straight out of `build-deps.sh`, the build date, and each file's size and
  sha256.
- `tools/wasm/host-tools.sh` — the two **native** tools the wasm build needs, for machines without
  Homebrew: `lemon` (one `cc` of `thirdparty/lemon/lemon.c`) and `protoc` at the pinned protobuf
  version (native abseil + protobuf, ~5-8 min, cached in CI). It reuses an already-matching system
  `protoc` and the tarballs `build-deps.sh` already downloaded.

**fab_pcb**

- `packages/kicad-wasm/scripts/fetch.ts` gained a release mode: `KICAD_WASM_RELEASE=<tag>` (default
  `packages/proto/KICAD_TAG`), `KICAD_WASM_RELEASE_FILE=<path>` for a tarball already on disk,
  `KICAD_WASM_REPO` to point at a different fork. It downloads through the releases API with
  `Accept: application/octet-stream` and `GITHUB_TOKEN` / `GH_TOKEN` (the fork is private), falls
  back to `gh release download`, verifies every file against `SHA256SUMS` **before** anything
  reaches `dist/`, and copies the manifest alongside the module. A local `KICAD_WASM_DIR` build tree
  is still the default whenever it exists. New script: `bun run --filter @fp-pcb/kicad-wasm
  fetch:release`.
- `packages/kicad-wasm/README.md`, `docs/08-wasm.md` (new section "Getting the module without
  building it" + four new environment-variable rows), and a **commented-out** `wasm` conformance job
  in `.github/workflows/ci.yml` — commented because a job depending on a release that does not exist
  yet would fail every run.

## Cutting the first release

1. Push the branch and the alignment tag to `TensorFleet/kicad`:
   `git push origin wasm && git push origin fp-pcb/<yyyy-mm-dd>-<name>`.
2. The tag push starts **kicad-wasm release**. From cold caches expect roughly 100-140 minutes:
   ~10 min host protoc, ~30 min dependencies, ~50-80 min for `kicad_api` itself. Later runs restore
   the emsdk, dependency-prefix and ccache caches and only rebuild the module.
3. The run attaches `kicad-wasm-<tag-slug>.tar.gz` to the release for that tag, creating the release
   with generated notes if it does not exist. The same tarball is also a downloadable run artifact.
4. In fab_pcb, set `packages/proto/KICAD_TAG` to that tag (it usually already is) and run
   `GITHUB_TOKEN=<pat> bun run --filter @fp-pcb/kicad-wasm fetch:release`. Then the wasm suites run
   with no toolchain: `KICAD_TRANSPORT=wasm bun run --cwd packages/client test:conformance`.
5. Optionally uncomment the `wasm` job in fab_pcb's `.github/workflows/ci.yml` and add a
   `KICAD_FORK_TOKEN` secret — a repository's own `GITHUB_TOKEN` cannot read a *different* private
   repository's release assets.

To build without cutting a release: run the workflow from the Actions tab (`workflow_dispatch`) on
any ref and take the run artifact; leave `attach_to_release` unticked.

## Caveats

- **The Linux path has never been run.** Everything to date was built on macOS with Homebrew's
  emscripten. The workflow is the same sequence with apt + emsdk, and the pieces that are known to
  differ are handled — `GLM_PREFIX=/usr` instead of `brew --prefix glm`, `LEMON_EXE` from
  `host-tools.sh` instead of `build/headless`, a from-source `protoc` because no apt release ships
  protobuf 36.1 — but the first run is still the experiment. Nothing else can catch, for instance, a
  wx configure probe that answers differently under Linux's Python or a different `ninja`.
- **The native `build/headless` tree is deliberately not built in CI.** `configure.sh` needs exactly
  one thing from it, `lemon`, and reproducing the full native dependency set (wxGTK, OCCT, ngspice,
  …) for one 6k-line C file is not worth an hour of runner time. If a future change makes
  `configure.sh` need something else from `build/headless`, the workflow has to grow a real native
  configure.
- **Two pins have to move together.** The host `protoc` and the wasm `libprotobuf` must be the same
  protobuf version or generated `.pb.cc` aborts at static-init; both come from
  `PROTOBUF_VERSION` in `tools/wasm/build-deps.sh`, which `host-tools.sh` and `package.sh` read
  rather than duplicate. Emscripten is pinned at 6.0.9 in the workflow *and* relied on by every
  library in `build/wasm-deps` (exception ABI, `-sWASM_LEGACY_EXCEPTIONS=1`): bumping it means
  invalidating the dependency cache, which the cache key does automatically since it includes the
  version.
- **The freetype port variant.** `configure.sh` hands `find_package( Freetype )` an explicit path to
  `libfreetype-legacysjlj.a`, which only exists once something has linked with the port flags. The
  workflow has a "Warm the emscripten ports" step for that, so a partially restored cache cannot
  leave the file missing.
- **The asset name is a contract.** `package.sh` writes `kicad-wasm-<tag with / → ->.tar.gz` and
  fab_pcb's `fetch.ts` derives the same string. Changing one without the other breaks the download
  with a "release has no asset" error listing what is actually there.
- The tarball is ~10 MB (37 MB of wasm, gzipped), far below GitHub's per-asset limit, and the
  release API path is the only one that works for a private repo — `browser_download_url` needs a
  session cookie.

## Verified locally

`actionlint` clean on both workflows, both files parse as YAML, `shellcheck -S warning` clean on the
new scripts. `tools/wasm/package.sh` was run against the existing `build/wasm/host` output
(`kicad_api.wasm` 35.70 MiB → 10.16 MiB tarball, manifest valid JSON, `shasum -a 256 -c SHA256SUMS`
passes), and fab_pcb's `fetch.ts` was run against that tarball through `KICAD_WASM_RELEASE_FILE`:
the extraction and checksum verification path installs the module into `dist/`, a deliberately
corrupted tarball is rejected before anything is copied, and `bun test` in
`packages/kicad-wasm` passes 13/13 against the module unpacked from the tarball. The full wasm build
was **not** re-run, and no GitHub Actions run has happened.
