#!/usr/bin/env bash
# Print the build matrix for .github/workflows/nightly.yml as two $GITHUB_OUTPUT lines:
#   matrix=<JSON array of {platform, os, runner}>
#   platforms=<space separated platform names>
#
# Usage: matrix.sh [comma-separated subset]      (empty = every platform)
#
# To add a platform, add a line to ALL below.  `os` selects the build steps in the workflow
# (linux | macos | windows); `runner` is the GitHub-hosted runner label.
set -euo pipefail

ALL=(
  "linux-x86_64   linux   ubuntu-24.04"
  "macos-arm64    macos   macos-15"
  "macos-x86_64   macos   macos-15-intel"
  "windows-x86_64 windows windows-2022"
)

requested="${1:-}"
requested="${requested//,/ }"

want() {
  [ -z "$requested" ] && return 0
  for r in $requested; do [ "$r" = "$1" ] && return 0; done
  return 1
}

json="["
names=""
for line in "${ALL[@]}"; do
  read -r platform os runner <<<"$line"
  want "$platform" || continue
  [ "$json" != "[" ] && json+=","
  json+="{\"platform\":\"$platform\",\"os\":\"$os\",\"runner\":\"$runner\"}"
  names+="${names:+ }$platform"
done
json+="]"

if [ -z "$names" ]; then
  echo "no platform matches '$requested'" >&2
  exit 1
fi

echo "matrix=$json"
echo "platforms=$names"
