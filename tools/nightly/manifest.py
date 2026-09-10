#!/usr/bin/env python3
"""manifest.json for the nightly kicad-cli releases (see tools/nightly/README.md).

Subcommands:
  build       write a manifest for the archives in a directory
  is-current  exit 0 when a manifest carries the given commit for every listed platform
  notes       print release notes (Markdown) for a manifest

The manifest is what a consumer reads first: it maps a platform name to the archive to
download, its sha256 and the path of the executable inside the archive.
"""

import argparse
import hashlib
import json
import os
import re
import sys

SCHEMA = 1

# Archives produced by tools/nightly/archive.sh:  kicad-cli-<tag>-<platform>.<ext>
ARCHIVE_RE = re.compile(r"^kicad-cli-(?P<tag>nightly-\d{8}-[0-9a-f]{10})-(?P<platform>[a-z0-9]+-[a-z0-9_]+)\.(?P<ext>tar\.gz|zip)$")

# Path of the executable inside the archive, per OS.  The archive always unpacks into a
# top-level `kicad-cli/` directory.
ENTRYPOINTS = {
    "linux": "kicad-cli/kicad-cli",
    "macos": "kicad-cli/kicad-cli",
    "windows": "kicad-cli/bin/kicad-cli.exe",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def cmd_build(args):
    assets = {}
    for name in sorted(os.listdir(args.assets_dir)):
        m = ARCHIVE_RE.match(name)
        if not m:
            continue
        platform, ext = m.group("platform"), m.group("ext")
        os_name = platform.split("-", 1)[0]
        path = os.path.join(args.assets_dir, name)
        file_name = f"kicad-cli-{platform}.{ext}" if args.stable_names else name
        assets[platform] = {
            "file": file_name,
            "url": f"https://github.com/{args.repo}/releases/download/{args.release_tag}/{file_name}",
            "sha256": sha256(path),
            "size": os.path.getsize(path),
            "entrypoint": ENTRYPOINTS[os_name],
            "commit": args.sha,
            "version": args.version,
            "tag": args.tag,
            "date": args.date,
        }

    if args.merge and os.path.exists(args.merge):
        # Rolling release: keep the previous asset of every platform that did not build
        # this time, so `nightly` never loses a platform because one job failed.
        for platform, asset in load(args.merge).get("assets", {}).items():
            assets.setdefault(platform, asset)

    manifest = {
        "schema": SCHEMA,
        "repo": args.repo,
        "release": args.release_tag,
        "tag": args.tag,
        "commit": args.sha,
        "version": args.version,
        "date": args.date,
        "run_url": args.run_url,
        "assets": dict(sorted(assets.items())),
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"{args.out}: {', '.join(assets) or 'no assets'}")


def cmd_is_current(args):
    manifest = load(args.manifest)
    assets = manifest.get("assets", {})
    missing = [p for p in args.platforms if assets.get(p, {}).get("commit") != args.sha]
    if missing:
        print(f"not current for: {' '.join(missing)}")
        return 1
    return 0


def cmd_notes(args):
    m = load(args.manifest)
    lines = [
        f"Headless `kicad-cli` (+ pcbnew and eeschema kifaces) built from "
        f"`{m['commit']}` (`{m['version']}`).",
        "",
        "| Platform | Archive | Commit | SHA-256 |",
        "|---|---|---|---|",
    ]
    for platform, a in m["assets"].items():
        lines.append(f"| {platform} | [{a['file']}]({a['url']}) | `{a['commit'][:10]}` | `{a['sha256']}` |")
    lines += [
        "",
        "Each archive unpacks into `kicad-cli/`; run `kicad-cli/kicad-cli` "
        "(`kicad-cli\\bin\\kicad-cli.exe` on Windows), e.g.",
        "",
        "```",
        "kicad-cli/kicad-cli api-server board.kicad_pcb --socket /tmp/kicad/api.sock",
        "```",
        "",
        "`manifest.json` on this release lists the same assets for scripted downloads; "
        "the rolling `nightly` release always points at the latest build. "
        "Runtime requirements and the archive layout: `tools/nightly/README.md`.",
    ]
    if m.get("run_url"):
        lines += ["", f"Built by {m['run_url']}."]
    print("\n".join(lines))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("--assets-dir", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--repo", required=True, help="owner/name")
    b.add_argument("--release-tag", required=True, help="tag of the release the assets are attached to")
    b.add_argument("--tag", required=True, help="build tag (nightly-<date>-<sha10>)")
    b.add_argument("--sha", required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--date", required=True)
    b.add_argument("--run-url", default="")
    b.add_argument("--stable-names", action="store_true", help="name assets kicad-cli-<platform>.<ext>")
    b.add_argument("--merge", help="previous manifest whose assets fill the platforms not built now")
    b.set_defaults(func=cmd_build)

    c = sub.add_parser("is-current")
    c.add_argument("manifest")
    c.add_argument("sha")
    c.add_argument("platforms", nargs="+")
    c.set_defaults(func=cmd_is_current)

    n = sub.add_parser("notes")
    n.add_argument("manifest")
    n.set_defaults(func=cmd_notes)

    args = p.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
