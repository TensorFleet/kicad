# Nightly `kicad-cli` releases

`.github/workflows/nightly.yml` builds the headless server of this fork — `kicad-cli` plus
the `_pcbnew` and `_eeschema` kifaces, the three targets fab_pcb's
`packages/kicad-patches` builds — for Linux, macOS and Windows every night and publishes
them as GitHub Releases of this repository:

| Release | What it is |
|---|---|
| `nightly-<YYYYMMDD>-<sha10>` | one prerelease per build; pin to it. The oldest are pruned (`KEEP_NIGHTLIES`, 14) |
| `nightly` | rolling prerelease: the latest build of every platform under stable asset names |

A build is skipped when the rolling release already carries the branch head for every
platform, so an idle branch costs one short job a night. `workflow_dispatch` takes a `ref`,
a `force` flag and a `platforms` subset.

## Assets

```
kicad-cli-<tag>-linux-x86_64.tar.gz      (nightly: kicad-cli-linux-x86_64.tar.gz)
kicad-cli-<tag>-macos-arm64.tar.gz       (nightly: kicad-cli-macos-arm64.tar.gz)
kicad-cli-<tag>-macos-x86_64.tar.gz      (nightly: kicad-cli-macos-x86_64.tar.gz)
kicad-cli-<tag>-windows-x86_64.zip       (nightly: kicad-cli-windows-x86_64.zip)
SHA256SUMS
manifest.json
```

`manifest.json` is what a consumer reads first:

```json
{
  "schema": 1,
  "repo": "TensorFleet/kicad",
  "release": "nightly",
  "tag": "nightly-20260910-0c45443da6",
  "commit": "0c45443da6168edfba53f51a1195b54d7b708b45",
  "version": "10.99.0-3760-g0c45443da6",
  "date": "20260910",
  "run_url": "https://github.com/TensorFleet/kicad/actions/runs/…",
  "assets": {
    "linux-x86_64": {
      "file": "kicad-cli-linux-x86_64.tar.gz",
      "url": "https://github.com/TensorFleet/kicad/releases/download/nightly/kicad-cli-linux-x86_64.tar.gz",
      "sha256": "…", "size": 123456789,
      "entrypoint": "kicad-cli/kicad-cli",
      "commit": "0c45443da6168edfba53f51a1195b54d7b708b45",
      "version": "10.99.0-3760-g0c45443da6",
      "tag": "nightly-20260910-0c45443da6", "date": "20260910"
    },
    "macos-arm64": { "…": "entrypoint kicad-cli/kicad-cli" },
    "windows-x86_64": { "…": "entrypoint kicad-cli/bin/kicad-cli.exe" }
  }
}
```

On the rolling release each asset carries its own `commit`: when one platform fails to
build, `nightly` keeps that platform's previous archive rather than dropping it, and the
top-level `commit` is the newest build. The dated releases only ever contain the platforms
that built that night.

The repository is private, so downloads need a token with `contents:read`
(`Authorization: Bearer …`) and, for the archive bytes, the asset's API URL with
`Accept: application/octet-stream` — `browser_download_url` does not work for private
repositories. fab_pcb's `tooling/kicad-cli/fetch.ts` is a dependency-free reference
downloader (manifest → platform → sha256 check → unpack → print the entrypoint).

## Archive layout

Every archive unpacks into a `kicad-cli/` directory:

```
kicad-cli/kicad-cli                  Linux, macOS: launcher; run this
kicad-cli/bin/kicad-cli.exe          Windows: run this
kicad-cli/KICAD_COMMIT  VERSION      provenance

Linux    bin/{kicad-cli,_pcbnew.kiface,_eeschema.kiface}  lib/*.so*  share/kicad/{schemas,template}
macOS    KiCad.app/Contents/{MacOS/kicad-cli, PlugIns/*.kiface, Frameworks/*.dylib, SharedSupport/}
Windows  bin/{kicad-cli.exe,_pcbnew.dll,_eeschema.dll,ki*.dll,<vcpkg + MSVC runtime>.dll}  share/kicad/
```

```
kicad-cli/kicad-cli api-server board.kicad_pcb --socket /tmp/kicad/api.sock
```

Runtime requirements:

- **Linux** (`x86_64`, built on Ubuntu 24.04): glibc ≥ 2.39 and libstdc++ from GCC 13 or
  newer, plus GTK 3, WebKitGTK 4.1, OpenGL, X11/Wayland client libraries and the
  fontconfig/freetype/harfbuzz stack — i.e. `linux/runtime-packages.txt`, which the
  workflow proves sufficient by running the smoke test on a clean `ubuntu:24.04`
  container. Everything else is in `lib/` (RUNPATH `$ORIGIN/../lib`). The launcher sets
  `KICAD_STOCK_DATA_HOME` because KiCad compiles its data path in (`/opt/kicad/share/kicad`).
  No display is needed: `kicad-cli` is a `wxAppConsole`.
- **macOS** (`arm64` and `x86_64`, built on macOS 15 with Homebrew): self-contained
  (`Frameworks/`), ad-hoc signed, not notarised. Started from a terminal or by another
  process it runs as is; a copy that picked up the quarantine attribute (browser download)
  needs `xattr -dr com.apple.quarantine kicad-cli`. An app that ships it should sign the
  Mach-O files inside with its own identity, as electron-builder does for nested binaries.
- **Windows** (`x86_64`, MSVC + vcpkg): self-contained, including the MSVC runtime. Not
  signed.

## Layout of this directory

| File | Purpose |
|---|---|
| `matrix.sh` | the platform list (add a runner here) |
| `manifest.py` | `build` / `is-current` / `notes` for manifest.json |
| `archive.sh` | `kicad-cli/` staging tree → tar.gz / zip |
| `smoke.sh` | `version`, a DRC and an ERC through the kifaces, on files from `qa/data` |
| `release.sh` | dated + rolling releases, tag move, pruning (`gh`) |
| `linux/install-deps.sh`, `build.sh`, `bundle.py`, `smoke-container.sh`, `runtime-packages.txt` | Ubuntu build; `bundle.py` walks DT_NEEDED and copies every non-platform library, `patchelf`s RUNPATHs |
| `macos/install-deps.sh`, `build.sh`, `bundle.sh` | Homebrew build (same flags as fab_pcb's `build-macos.sh`); `bundle.sh` walks `otool -L`, rewrites load commands to `@rpath`, re-signs |
| `windows/setup-vcpkg.ps1`, `install-deps.ps1`, `build.ps1`, `bundle.ps1`, `triplets/` | vcpkg at the manifest's baseline with a files binary cache on the Actions cache; the overlay triplet builds release-only ports |

All of it runs by hand too, e.g. on Linux:

```bash
sudo tools/nightly/linux/install-deps.sh
BUILD_DIR=build/nightly tools/nightly/linux/build.sh
python3 tools/nightly/linux/bundle.py --build-dir build/nightly --out dist/kicad-cli
tools/nightly/linux/smoke-container.sh dist/kicad-cli        # needs docker
tools/nightly/archive.sh dist/kicad-cli linux-x86_64 nightly-$(date +%Y%m%d)-$(git rev-parse --short=10 HEAD) out
```

## First run and cost

The first Windows run has to build every vcpkg port (OpenCASCADE, wxWidgets, Boost, ICU,
Python, …) from source. The `vcpkg install` step has a 250-minute budget of its own and
does not fail the job when it runs out: the ports it finished are saved to the Actions
cache regardless, and the next run continues from there. Until it completes, the Windows
build step fails with a message saying so, the other platforms publish normally, and the
rolling release simply has no Windows asset yet. After that, cached ports restore in
minutes and a Windows build is about an hour.

Linux builds in roughly an hour, macOS in one to two (the Apple-silicon runners have three
cores); `ccache` is kept on the Actions cache for both. On a private repository macOS
minutes are billed at a multiple of Linux ones, so drop `macos-x86_64` from `matrix.sh`
if Intel Macs are not a target.
