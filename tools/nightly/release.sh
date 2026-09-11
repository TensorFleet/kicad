#!/usr/bin/env bash
# Publish the nightly archives as GitHub Releases (run by .github/workflows/nightly.yml).
#
#   1. a pinnable prerelease `$NIGHTLY_TAG` (nightly-<date>-<sha10>) with the archives,
#      SHA256SUMS and manifest.json
#   2. the rolling prerelease `nightly`: the same archives under stable names
#      (kicad-cli-<platform>.<ext>), a merged manifest.json that keeps the previous asset of
#      any platform that did not build tonight, and the `nightly` tag moved to the commit
#   3. prune dated nightlies published more than $KEEP_DAYS days ago (default 90).  This is
#      the retention guarantee consumers pin against (tools/nightly/README.md): retention is
#      by age, never by count, so it does not depend on how often the branch changes.
#
# Usage: release.sh <dir with kicad-cli-<tag>-<platform>.* archives>
# Env:   GH_TOKEN GH_REPO NIGHTLY_TAG NIGHTLY_SHA NIGHTLY_VERSION NIGHTLY_DATE
#        NIGHTLY_PLATFORMS (requested, space separated) NIGHTLY_RUN_URL KEEP_DAYS
set -euo pipefail

mkdir -p "$1"   # download-artifact creates nothing when every build failed
ASSETS="$(cd "$1" && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEEP_DAYS="${KEEP_DAYS:-90}"
SHORT="${NIGHTLY_SHA:0:10}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

shopt -s nullglob
archives=("$ASSETS"/kicad-cli-"$NIGHTLY_TAG"-*.tar.gz "$ASSETS"/kicad-cli-"$NIGHTLY_TAG"-*.zip)
if [ ${#archives[@]} -eq 0 ]; then
  echo "::error::no archives for $NIGHTLY_TAG in $ASSETS (every platform failed?)"
  exit 1
fi
echo "archives:"; printf '  %s\n' "${archives[@]##*/}"

manifest() {
  python3 "$HERE/manifest.py" build --assets-dir "$ASSETS" --repo "$GH_REPO" \
    --tag "$NIGHTLY_TAG" --sha "$NIGHTLY_SHA" --version "$NIGHTLY_VERSION" --date "$NIGHTLY_DATE" \
    --run-url "${NIGHTLY_RUN_URL:-}" "$@"
}

release_exists() { gh release view "$1" --json tagName >/dev/null 2>&1; }

# ------------------------------------------------------------------ 1. dated release
DATED="$WORK/dated"
mkdir -p "$DATED"
cp "${archives[@]}" "$DATED/"
(cd "$DATED" && sha256sum -- * > SHA256SUMS)
manifest --release-tag "$NIGHTLY_TAG" --out "$DATED/manifest.json"
python3 "$HERE/manifest.py" notes "$DATED/manifest.json" > "$WORK/dated-notes.md"

title="kicad-cli nightly $NIGHTLY_DATE ($SHORT)"
if release_exists "$NIGHTLY_TAG"; then
  # Re-run of the same commit on the same day: replace the assets.
  gh release upload "$NIGHTLY_TAG" --clobber "$DATED"/*
  gh release edit "$NIGHTLY_TAG" --prerelease --title "$title" --notes-file "$WORK/dated-notes.md"
else
  gh release create "$NIGHTLY_TAG" --prerelease --target "$NIGHTLY_SHA" \
    --title "$title" --notes-file "$WORK/dated-notes.md" "$DATED"/*
fi
echo "published $NIGHTLY_TAG"

# ------------------------------------------------------------------ 2. rolling release
ROLLING="$WORK/rolling"
mkdir -p "$ROLLING"
for a in "${archives[@]}"; do
  name="$(basename "$a")"
  cp "$a" "$ROLLING/${name/-$NIGHTLY_TAG-/-}"   # kicad-cli-<tag>-<platform>.ext -> kicad-cli-<platform>.ext
done

previous="$WORK/previous-manifest.json"
if release_exists nightly; then
  gh release download nightly --pattern manifest.json --output "$previous" 2>/dev/null || true
fi

# A build of a commit *behind* the one the rolling release carries is a backfill for a
# consumer's pin (workflow_dispatch with an old `ref`), not the latest: it gets its dated
# release above but must not move `nightly` backwards.
update_rolling=true
if [ -s "$previous" ]; then
  prev_sha="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["commit"])' "$previous")"
  if [ "$prev_sha" != "$NIGHTLY_SHA" ]; then
    status="$(gh api "repos/$GH_REPO/compare/$prev_sha...$NIGHTLY_SHA" --jq .status 2>/dev/null || echo unknown)"
    if [ "$status" = "behind" ]; then
      echo "built $NIGHTLY_SHA is behind the rolling release's $prev_sha (backfill); leaving nightly as it is"
      update_rolling=false
    fi
  fi
fi

if [ "$update_rolling" = true ]; then
  manifest --release-tag nightly --stable-names --merge "$previous" --out "$ROLLING/manifest.json"

  # SHA256SUMS of the rolling release covers every listed asset, tonight's and the kept ones.
  python3 - "$ROLLING/manifest.json" > "$ROLLING/SHA256SUMS" <<'PYEOF'
import json, sys
m = json.load(open(sys.argv[1]))
for a in m["assets"].values():
    print(f"{a['sha256']}  {a['file']}")
PYEOF
  python3 "$HERE/manifest.py" notes "$ROLLING/manifest.json" > "$WORK/rolling-notes.md"

  rolling_title="kicad-cli nightly (latest: $NIGHTLY_DATE, $SHORT)"
  if release_exists nightly; then
    gh release upload nightly --clobber "$ROLLING"/*
    gh release edit nightly --prerelease --title "$rolling_title" --notes-file "$WORK/rolling-notes.md"
    # Move the tag; `gh release edit --target` only applies to releases whose tag does not exist yet.
    gh api -X PATCH "repos/$GH_REPO/git/refs/tags/nightly" -f sha="$NIGHTLY_SHA" -F force=true >/dev/null
  else
    gh release create nightly --prerelease --target "$NIGHTLY_SHA" \
      --title "$rolling_title" --notes-file "$WORK/rolling-notes.md" "$ROLLING"/*
  fi
  echo "updated rolling release nightly -> $NIGHTLY_SHA"
fi

# ------------------------------------------------------------------ 3. prune (by age only)
cutoff="$(date -u -d "-${KEEP_DAYS} days" +%Y-%m-%dT%H:%M:%SZ)"
# (gh's --jq takes a bare expression, no --arg; the timestamps compare as ISO-8601 strings)
gh release list --limit 500 --json tagName,createdAt \
  --jq ".[] | select(.tagName | startswith(\"nightly-\")) | select(.createdAt < \"$cutoff\") | .tagName" \
  | while read -r old; do
      [ -n "$old" ] || continue
      echo "pruning $old (older than $KEEP_DAYS days)"
      gh release delete "$old" --cleanup-tag --yes
    done

{
  echo "### kicad-cli nightly"
  echo
  echo "- release: https://github.com/$GH_REPO/releases/tag/$NIGHTLY_TAG"
  echo "- rolling: https://github.com/$GH_REPO/releases/tag/nightly"
  echo
  cat "$WORK/rolling-notes.md" 2>/dev/null || cat "$WORK/dated-notes.md"
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
