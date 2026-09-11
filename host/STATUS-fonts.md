# fonts status (2026-09-09)

Follow-up #3 in `host/STATUS.md`: outline fonts in the headless/wasm build.
`common/font/fontconfig_stub.cpp` is replaced by `common/font/fontconfig_manifest.cpp`, which
reads `KICAD_FONTS_DIR` (a `:`-separated list) and resolves `FindFont()` against a
`manifest.json` — `{"default":"Carlito","fonts":[{family,style,bold,italic,file}]}` — or, with no
manifest, against a bare folder of `.ttf`/`.otf` whose family/style it reads with FreeType.
Embedded font files are folded in as candidates, so a document's own fonts now resolve too.
Matching and the `FF_RESULT` codes mirror `fontconfig.cpp` line for line (name-says-bold rule,
substitution through the default family, `FF_MISSING_BOLD*` for synthetic styles, REPORTER
warnings), so `OUTLINE_FONT::LoadFont()` cannot tell the two apart. `Version()` is `"manifest"`.
No fonts dir, or an empty one, means every lookup misses and text falls back to the stroke font
— exactly the old stub behaviour.

Host: `KICAD_API_HOST_CONFIG::fonts` (JSON `fonts`, `--fonts <dir>`, default `/kicad/fonts`
under Emscripten) is exported as `KICAD_FONTS_DIR`. `host/wasm/CMakeLists.txt` preloads
Carlito Regular + Bold (1.3 MB, SIL OFL, already in `thirdparty/libwmf/fonts/`) at
`/kicad/fonts` with a generated manifest, behind `KICAD_WASM_PRELOAD_FONTS=ON`; the bundle
lands in `kicad_api.data`, which fab_pcb's `fetch.ts` already copies. **The wasm module has not
been rebuilt** (`build/wasm` is another agent's); the preload is untested end to end.

fab_pcb (`wasm` branch): `packages/kicad-wasm/src/fonts.ts` — `mountFonts()`,
`buildFontManifest()`, `readFontNames()` (an sfnt `name`-table parser, so a generated manifest
carries the same family names FreeType reports), `fontsEnv()`; one export line in `index.ts`.
`packages/client/test/kicad-server.ts` passes `--fonts` (stdio) or mounts the directory (wasm)
when `KICAD_FONTS_DIR` is set. `packages/client/test/fonts.kicad.test.ts` is the focused test,
skipped without `KICAD_FONTS_DIR`.

## Numbers

Built in `build/headless-fonts` (`-DKICAD_HEADLESS_HOST_LINK=OFF`, a new option so a secondary
tree does not repoint `build/native-host`); `--selftest` 13/13.

`GetTextExtents("Wg1i- fp-pcb")` / `GetTextAsShapes("Wg")` at 2 mm, stdio transport:

| fontName         | with `KICAD_FONTS_DIR` | without                 |
| ---------------- | ---------------------- | ----------------------- |
| `""` (stroke)    | 21.914 mm, 21 segments | 21.914 mm, 21 segments  |
| `Carlito`        | 14.580 mm, 2 polygons / 179 nodes | 21.914 mm, 21 segments |
| `Carlito` + bold | 14.795 mm, 2 polygons / 171 nodes | 22.274 mm, 21 segments |
| `No Such Family` | 14.580 mm (substituted to the default) | 21.914 mm, 21 segments |

Both the manifest path and the bare-folder scan give the same four numbers; a `KICAD_FONTS_DIR`
that does not exist degrades to the stroke column with no error.

Conformance (`KICAD_TRANSPORT=stdio`, 177 tests): **151/153 headless green with and without
fonts**, the same two failures either way (`RunBoardJobExport3D`, `RunBoardJobExportRender` —
no OCC/3D in this build), and the per-command detail lines are identical apart from run ids.

## Open

- Rebuild `build/wasm` and check `kicad_api.data` loads at `/kicad/fonts`.
- Nothing in the API returns `ListFonts()`, so the family list is only exercised indirectly.
- KiCad's stock fonts are not installed anywhere the module can find; Carlito comes from
  `thirdparty/libwmf/fonts/`. A board naming any other family gets the substitution, not the
  real face.
