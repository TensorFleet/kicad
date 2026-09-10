#!/usr/bin/env bash
# Smoke test a staged/unpacked kicad-cli tree: `version`, then a DRC and an ERC run, which
# load the pcbnew and eeschema kifaces (the api-server uses the same two) on files from qa/.
#
# Usage: smoke.sh <kicad-cli executable> <kicad source root>
set -euo pipefail

CLI="$1"
SRC="$2"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# KiCad writes its settings on first start; never touch the caller's real config.
export HOME="$TMP/home"
export XDG_CONFIG_HOME="$TMP/home/.config"
mkdir -p "$HOME"

echo "== $CLI version"
"$CLI" version

echo "== pcb drc (loads _pcbnew kiface)"
"$CLI" pcb drc --format json -o "$TMP/drc.json" "$SRC/qa/data/pcbnew/api_kitchen_sink.kicad_pcb"
test -s "$TMP/drc.json"

echo "== sch erc (loads _eeschema kiface)"
"$CLI" sch erc --format json -o "$TMP/erc.json" "$SRC/qa/data/eeschema/api_kitchen_sink.kicad_sch"
test -s "$TMP/erc.json"

echo "smoke test passed"
