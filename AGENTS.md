# Working in this repository

This is TensorFleet's fork of KiCad. It carries the API patch series that
[fab_pcb](https://github.com/TensorFleet/fab_pcb) and fabdesk build on, plus the nightly
release pipeline in `tools/nightly/` and `.github/workflows/nightly.yml`.

## Branches

- **`main` is the only long-lived branch.** Always open new branches against `main` and
  target `main` with every pull request. There is no `web-api` or `master` any more; the
  API series that used to live on `web-api` is on `main`.
- Merge with a merge commit (the repository's convention), never rebase or force-push a
  branch someone else may have checked out.
- Upstream KiCad is merged into `main` through fab_pcb's `tooling/upstream-sync` scripts,
  which open a sync PR here (`upstream-sync-<date>` → `main`) and a bindings PR on fab_pcb.
  Review and merge both; never push `main` directly.
- Alignment tags `fp-pcb/<date>-<name>` mark the fork commit a fab_pcb change set was built
  against; they are created by the sync tooling, not by hand.

## Nightly releases

The nightly workflow builds `main` and publishes `kicad-cli` for Linux, macOS and Windows as
GitHub Releases (`nightly-<date>-<sha10>` and the rolling `nightly`). Consumers pin the dated
releases by the `-<sha10>` suffix, and a dated release stays for at least 90 days. The full
contract, the archive layout and how to build any commit by hand are in
`tools/nightly/README.md`; read it before changing anything under `tools/nightly/` or the
workflow, and keep the contract section true.

## Code

Follow the [KiCad coding style](https://dev-docs.kicad.org/en/rules-guidelines/code-style/).
API changes come as one commit per feature: proto + handler + a `qa/tests/api` test, written
to be upstreamable. MSVC is a supported compiler (the nightly builds Windows), so avoid the
GCC-only idioms it rejects: mixing a string literal and `wxString` in one conditional
expression, assigning a `UTF8` straight to a `wxString` (use `.wx_str()`), and marking a
class `KICOMMON_API` unless its source is compiled into the `kicommon` shared library.
