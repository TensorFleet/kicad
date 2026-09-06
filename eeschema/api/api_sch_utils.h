/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KICAD_API_SCH_UTILS_H
#define KICAD_API_SCH_UTILS_H

#include <memory>
#include <unordered_map>
#include <tl/expected.hpp>
#include <core/typeinfo.h>
#include <api/common/envelope.pb.h>
#include <api/schematic/schematic_types.pb.h>
#include <pin_map.h>

class EDA_ITEM;
class LIB_SYMBOL;
class SCH_SYMBOL;
class SCH_SHEET;
class SCH_SHEET_PATH;
class SCHEMATIC;

std::unique_ptr<EDA_ITEM> CreateItemForType( KICAD_T aType, EDA_ITEM* aContainer );

bool PackSymbol( kiapi::schematic::types::SchematicSymbolInstance* aOutput, const SCH_SYMBOL* aInput,
                 const SCH_SHEET_PATH& aPath );

/**
 * Pack a library symbol definition, including its pins, into a SchematicSymbol message.  This is
 * the library view of the symbol (as served by a DOCTYPE_SYMBOL document); the instance-specific
 * view is produced by PackSymbol.
 */
void PackLibSymbol( kiapi::schematic::types::SchematicSymbol* aOutput, const LIB_SYMBOL* aInput );

/**
 * Build a library symbol from its SchematicSymbol message (the inverse of PackLibSymbol).
 * @param aPinAlternates receives the active alternate of every pin that has one, keyed by the
 *                       pin's id, for the caller to apply to a placed symbol
 *
 * Since 11.0.
 */
std::unique_ptr<LIB_SYMBOL> UnpackLibSymbol( const kiapi::schematic::types::SchematicSymbol& aInput,
                                             std::unordered_map<KIID, wxString>* aPinAlternates = nullptr );

/**
 * Unpack the geometry, the library definition, fields, and the default-variant attributes that
 * are shared between every placement. Single-placement data is handled by #ApplySymbolInstance.
 */
bool UnpackSymbol( SCH_SYMBOL* aOutput, const kiapi::schematic::types::SchematicSymbolInstance& aInput );

/**
 * If the library definition carried by @a aInput is the one @a aExisting already has, give
 * @a aTarget (the unpacked replacement for aExisting) a copy of aExisting's library symbol and
 * aExisting's pin order instead of the definition rebuilt from the message.  The rebuilt
 * definition is not byte-for-byte what was loaded (draw item order, pin name offset, ...), so
 * an unchanged symbol would otherwise rewrite the sheet's lib_symbols cache under a new name.
 *
 * Since 11.0.
 *
 * @return true if the library symbol was reused
 */
bool ReuseUnchangedLibSymbol( SCH_SYMBOL* aTarget, SCH_SYMBOL* aExisting,
                              const kiapi::schematic::types::SchematicSymbolInstance& aInput,
                              const SCH_SHEET_PATH& aPath );

/**
 * Apply placement-specific data to an @a aSymbol at @a aPath: reference, unit, and
 * the per-placement attribute and field differentials.
 *
 * Variant names are registered with @a aSchematic so that the UI offers them for selection.
 * @a aSymbol must already be in the schematic; the other placements of the symbol are untouched.
 */
void ApplySymbolInstance( SCH_SYMBOL* aSymbol,
                          const kiapi::schematic::types::SchematicSymbolInstance& aInput,
                          const SCH_SHEET_PATH& aPath, SCHEMATIC* aSchematic );

/// Pack/unpack a pin-to-pad map instance override to/from its protobuf form (issue #2282).
void PackPinMapOverride( kiapi::schematic::types::PinMapInstanceOverride* aOutput,
                         const PIN_MAP_INSTANCE_OVERRIDE&                 aOverride );

PIN_MAP_INSTANCE_OVERRIDE UnpackPinMapOverride( const kiapi::schematic::types::PinMapInstanceOverride& aInput );

bool PackSheet( kiapi::schematic::types::SheetSymbol* aOutput, const SCH_SHEET* aInput,
                const SCH_SHEET_PATH& aPath );

/**
 * Unpack the every placement data from the input. Placement data is applied separately by #ApplySheetInstance.
 */
tl::expected<bool, kiapi::common::ApiResponseStatus> UnpackSheet( SCH_SHEET* aOutput, const kiapi::schematic::types::SheetSymbol& aInput );

/**
 * Apply the placement data in a sheet message to @a aSheet: page number and the variants the
 * message carries.
 *
 * @a aParentPath is the path of the sheet that contains @a aSheet, which is how a sheet's
 * placement records are keyed.
 */
void ApplySheetInstance( SCH_SHEET* aSheet, const kiapi::schematic::types::SheetSymbol& aInput,
                         const SCH_SHEET_PATH& aParentPath, SCHEMATIC* aSchematic );

#endif //KICAD_API_SCH_UTILS_H
