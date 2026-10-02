#!/usr/bin/env python3
"""Verify matching 3D loader ABI and actual VRML/STEP bodies in the native PCB renderer."""
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def pixels(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "renderer did not write PNG"
    offset, packed = 8, bytearray()
    while offset < len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        chunk = data[offset + 8:offset + 8 + size]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            assert depth == 8 and color in (2, 6) and interlace == 0, "unsupported renderer PNG"
        elif kind == b"IDAT":
            packed.extend(chunk)
        offset += size + 12
    channels = 3 if color == 2 else 4
    stride = width * channels
    raw, rows, prior = zlib.decompress(packed), [], bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        mode, row = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = prior[x]
            upper_left = prior[x - channels] if x >= channels else 0
            if mode == 1:
                correction = left
            elif mode == 2:
                correction = up
            elif mode == 3:
                correction = (left + up) // 2
            elif mode == 4:
                estimate = left + up - upper_left
                distances = [abs(estimate - v) for v in (left, up, upper_left)]
                correction = (left, up, upper_left)[distances.index(min(distances))]
            else:
                assert mode == 0, "unsupported PNG filter"
                correction = 0
            row[x] = (row[x] + correction) & 255
        rows.extend(tuple(row[x:x + 3]) for x in range(0, stride, channels))
        prior = row
    return rows


def verify_loaders(cli):
    # CLI is the launcher at the bundle root, or the actual binary in Contents/MacOS.
    root = cli.parent / "KiCad.app/Contents" if cli.parent.joinpath("KiCad.app").is_dir() else cli.parent.parent
    for loader in ("oce", "vrml", "idf"):
        path = root / "PlugIns/3d" / f"libs3d_plugin_{loader}.so"
        assert path.is_file(), f"missing matching model loader: {path}"
        library = ctypes.CDLL(str(path))
        library.GetKicadPluginClass.restype = ctypes.c_char_p
        assert library.GetKicadPluginClass() == b"PLUGIN_3D"
        library.CheckClassVersion.argtypes = [ctypes.c_ubyte] * 4
        library.CheckClassVersion.restype = ctypes.c_bool
        assert library.CheckClassVersion(1, 0, 0, 0), f"incompatible model loader: {path}"
        subprocess.run(["codesign", "--verify", "--strict", str(path)], check=True)



def verify_render(cli, source):
    with tempfile.TemporaryDirectory(prefix="kicad-model-smoke-") as directory:
        temp = Path(directory)
        env = {**os.environ, "KICAD_CONFIG_HOME": str(temp / "settings")}
        model = temp / "cube.wrl"
        model.write_text('#VRML V2.0 utf8\nTransform { translation 0 0 2 children [ Shape { appearance Appearance { material Material { diffuseColor 1 0 0 } } geometry Box { size 3 3 3 } } ] }\n')
        step = source / "qa/data/pcbnew/step_model_colors/TO-252-2.step"
        assert step.is_file(), f"missing real STEP smoke fixture: {step}"
        header = '(kicad_pcb (version 20240108) (generator pcbnew) (general (thickness 1.6)) (paper "A4") (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user)) (setup (pad_to_mask_clearance 0)) (net 0 "")\n'
        outline = '(gr_rect (start -15 -15) (end 15 15) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))\n'
        board = temp / "board.kicad_pcb"
        def render(label, model_path=None):
            footprint = ''
            if model_path:
                # A local fixture path only; no external library/install discovery.
                path = json.dumps(str(model_path))
                footprint = f'(footprint "SmokeModel" (layer "F.Cu") (at 0 0) (model {path} (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))\n'
            board.write_text(header + outline + footprint + ')\n')
            output = temp / f"{label}.png"
            result = subprocess.run([str(cli), "pcb", "render", "--width", "320", "--height", "240", "--side", "top", "--quality", "high", "--background", "opaque", "--zoom", "0.7", "--output", str(output), str(board)], env=env, capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            assert "implemented in both" not in result.stderr, "duplicate wxWidgets runtime images"
            return pixels(output)
        baseline = render("without-model")
        for label, fixture in (("vrml", model), ("step", step)):
            rendered = render(label, fixture)
            changed = sum(max(abs(a - b) for a, b in zip(left, right)) > 30 for left, right in zip(baseline, rendered))
            assert changed > 100, f"{label} body absent: only {changed} changed pixels"
            print(f"{label} native model render: {changed} changed pixels; passed")


def main():
    cli = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve()
    verify_loaders(cli)
    verify_render(cli, source)


if __name__ == "__main__":
    main()
