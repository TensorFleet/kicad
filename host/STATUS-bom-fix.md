# bom-fix status (2026-09-09)

Branch `wasm`, build dir `build/headless` (which is `KICAD_HEADLESS_WX_BASE_ONLY=ON` now, not OFF as
STATUS-native-gate.md still says), option-OFF regression dir `build/check-off`.  Two commits, both
prefixed "Headless:", nothing pushed or tagged.

## Task 1 -- `RunSchematicJobExportBOM` passes (`a7dfdd5e6f`)

Under `KICAD_HEADLESS_API` the fields tables' data model derives from `HEADLESS_GRID_TABLE_BASE`
(new `include/widgets/headless_grid_table.h`) instead of `WX_GRID_TABLE_BASE`/`wxGridTableBase`: an
abstract base with the virtuals the model overrides and nothing else.  The grid-facing half -- cell
attributes, the striped/resolved-text renderers, `wxGridTableMessage`, every `GetView()` block -- is
`#ifndef`'d out.  With the option OFF the class definition and the object set are unchanged
(`ninja -C build/check-off eeschema_kiface_objects pcbnew_kiface_objects common` is clean).
`common/dialogs/fields_table_data_model.cpp`, `eeschema/symbol_fields_data_model.cpp` and
`pcbnew/footprint_fields_data_model.cpp` go back into the headless source lists, so both `bom` jobs
work, and `tools/regen_link_stubs.sh` dropped their 30 stubs (1062 -> 1035).

**Conformance over stdio: 175/177, 151/153 headless commands** (was 150/153); only Export3D and
ExportRender remain.  The CSV the headless host writes is byte-identical to `kicad-cli`'s.

## Task 2 -- the `ToProtoEnum<FILL_T>` assertion (`c888753512`)

`EDA_SHAPE( const SHAPE& )` value-initialized `m_fill`, and `FILL_T` starts at 1, so the fill was 0.
Its one caller is `API_HANDLER_COMMON::handleGetTextAsShapes`, which wraps each glyph outline in a
proxy `EDA_SHAPE` -- hence one assert per glyph in the wasm log whenever the app renders text.  Now
`FILL_T::NO_FILL`, plus `m_fillColor( COLOR4D::UNSPECIFIED )` (the default `COLOR4D` is opaque
black, so the proxy also carried a fill colour).  Confirmed by a temporary backtrace in the
unhandled branch: three hits for one "A" before, none after; the instrumentation is removed.

## Two things worth knowing

1. `tools/regen_link_stubs.sh` needs `WXLIBDIR=/opt/homebrew/lib` on this machine.  Its default is
   `wx-config --libs base`, and only `wx-config-3.2` exists here, so without it the wx half of the
   undefined set cannot be mangled and the run emits a stub file that does not link.  Set
   `DEVSYMS_CACHE` too; the `nm` pass over `build/dev` takes minutes.
2. The regenerated `host/headless_link_stubs.cpp` is shared state.  It was regenerated against the
   working tree as it stood, which included another agent's uncommitted font changes; the only
   semantic difference from the previous file is the 30 fields-table symbols, everything else is
   index renumbering.
