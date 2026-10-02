#!/usr/bin/env python3
"""Preserve wxWidgets aliases without loading the same relocated library twice."""
from collections import defaultdict
from pathlib import Path
import struct
import sys


def identity(path):
    data = path.read_bytes()
    if len(data) < 32:
        return None
    magic, cpu, subtype, kind, commands = struct.unpack_from("<5I", data)
    if magic != 0xFEEDFACF or kind != 6:
        return None  # Preserve unsupported/fat binaries rather than guessing equivalence.
    offset = 32
    for _ in range(commands):
        if offset + 8 > len(data):
            break
        command, size = struct.unpack_from("<2I", data, offset)
        if size < 8 or offset + size > len(data):
            break
        if command == 0x1B and size == 24:
            return path.name.split("-")[0], cpu, subtype, data[offset + 8:offset + 24]
        offset += size
    return None


def deduplicate(frameworks):
    groups = defaultdict(list)
    for path in frameworks.glob("libwx*.dylib"):
        if path.is_symlink():
            continue
        key = identity(path)
        if key is not None:
            groups[key].append(path)
    for paths in groups.values():
        paths.sort(key=lambda path: (-len(path.name), path.name))
        canonical = paths[0]
        for alias in paths[1:]:
            alias.unlink()
            alias.symlink_to(canonical.name)
            print(f"library alias {alias.name} -> {canonical.name}")


if __name__ == "__main__":
    deduplicate(Path(sys.argv[1]))
