#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Write a large synthetic glTF scene for measuring the importer, the cook step and asset uploads.

The scene stands in for a converted station model: many tessellated parts (tori, tubes, plates), each with an
"_LOD1" coarser sibling node, many materials with base-colour, metallic-roughness, normal and emissive PNG textures,
and a few hundred placed nodes. Everything is computed here (procedural geometry and procedural textures);
nothing is third-party. The output goes wherever you point it (keep it out of the repository).

    python -I tools/gen_stress_gltf.py OUTDIR [--parts 48] [--instances 240] [--texture 2048] [--materials 8]

Stdlib only (zlib for PNG). Prints the triangle, node and byte counts.
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib


def png(path, w, h, pixel_row):
    """pixel_row(y) returns w*4 bytes of RGBA."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pixel_row(y)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 1)) + chunk(b"IEND", b""))
    with open(path, "wb") as fh:
        fh.write(data)
    return len(data)


def textures(outdir, m, size):
    """Four maps for material m: panel lines, grime and lights from integer patterns."""
    files = {}
    hue = [(200, 205, 215), (150, 160, 175), (210, 180, 120), (120, 135, 150), (190, 70, 60), (80, 90, 110),
           (230, 230, 225), (100, 120, 90)][m % 8]
    cell = max(size // 16, 4)

    def base(y):
        row = bytearray()
        for x in range(size):
            line = (x % cell) < 2 or (y % cell) < 2
            g = ((x * 7 + y * 13 + m * 31) % 23) - 11
            k = 0.55 if line else 1.0
            row += bytes((max(0, min(255, int(hue[0] * k) + g)), max(0, min(255, int(hue[1] * k) + g)),
                          max(0, min(255, int(hue[2] * k) + g)), 255))
        return bytes(row)

    def mr(y):
        row = bytearray()
        for x in range(size):
            line = (x % cell) < 2 or (y % cell) < 2
            row += bytes((255, 200 if line else 90 + (x * y + m) % 60, 40 if line else 220, 255))
        return bytes(row)

    def nrm(y):
        row = bytearray()
        for x in range(size):
            dx = 40 if (x % cell) == 0 else (-40 if (x % cell) == 1 else 0)
            dy = 40 if (y % cell) == 0 else (-40 if (y % cell) == 1 else 0)
            row += bytes((128 + dx, 128 + dy, 255, 255))
        return bytes(row)

    def emit(y):
        row = bytearray()
        lit_row = (y // cell) % 3 == 1 and (y % cell) in (cell // 2, cell // 2 + 1)
        for x in range(size):
            on = lit_row and ((x // cell + m) % 4) != 0 and (x % cell) > 2
            row += bytes((255, 220, 160, 255) if on else (0, 0, 0, 255))
        return bytes(row)

    total = 0
    for name, fn in (("base", base), ("mr", mr), ("normal", nrm), ("emit", emit)):
        f = "m%d_%s.png" % (m, name)
        total += png(os.path.join(outdir, f), size, size, fn)
        files[name] = f
    return files, total


def torus(R, r, seg, sides):
    pos, nor, uv, idx = [], [], [], []
    for i in range(seg + 1):
        a = 2 * math.pi * i / seg
        for j in range(sides + 1):
            b = 2 * math.pi * j / sides
            n = (math.cos(a) * math.cos(b), math.sin(a) * math.cos(b), math.sin(b))
            pos.append((math.cos(a) * R + n[0] * r, math.sin(a) * R + n[1] * r, n[2] * r))
            nor.append(n)
            uv.append((i / seg * 8.0, j / sides))
    for i in range(seg):
        for j in range(sides):
            a0 = i * (sides + 1) + j
            a1 = (i + 1) * (sides + 1) + j
            idx += [a0, a1, a1 + 1, a0, a1 + 1, a0 + 1]
    return pos, nor, uv, idx


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("--parts", type=int, default=48)
    ap.add_argument("--instances", type=int, default=240)
    ap.add_argument("--texture", type=int, default=2048)
    ap.add_argument("--materials", type=int, default=8)
    a = ap.parse_args(argv)
    os.makedirs(a.outdir, exist_ok=True)
    bin_data = bytearray()
    views, accessors, meshes = [], [], []

    def add_view(data, target):
        while len(bin_data) % 4:
            bin_data.append(0)
        views.append({"buffer": 0, "byteOffset": len(bin_data), "byteLength": len(data), "target": target})
        bin_data.extend(data)
        return len(views) - 1

    def add_acc(data, comp, count, typ, target, mn=None, mx=None):
        acc = {"bufferView": add_view(data, target), "componentType": comp, "count": count, "type": typ}
        if mn is not None:
            acc["min"], acc["max"] = mn, mx
        accessors.append(acc)
        return len(accessors) - 1

    tris = 0
    for p in range(a.parts):
        prims = []
        for lod, (seg, sides) in enumerate(((96 + (p % 5) * 16, 24), (24, 8))):
            pos, nor, uv, idx = torus(6.0 + (p % 7) * 2.5, 0.6 + (p % 3) * 0.3, seg, sides)
            mn = [min(v[k] for v in pos) for k in range(3)]
            mx = [max(v[k] for v in pos) for k in range(3)]
            ap_ = add_acc(b"".join(struct.pack("<3f", *v) for v in pos), 5126, len(pos), "VEC3", 34962, mn, mx)
            an = add_acc(b"".join(struct.pack("<3f", *v) for v in nor), 5126, len(nor), "VEC3", 34962)
            au = add_acc(b"".join(struct.pack("<2f", *v) for v in uv), 5126, len(uv), "VEC2", 34962)
            ai = add_acc(struct.pack("<%dI" % len(idx), *idx), 5125, len(idx), "SCALAR", 34963)
            meshes.append({"name": "part%d_lod%d" % (p, lod), "primitives": [
                {"attributes": {"POSITION": ap_, "NORMAL": an, "TEXCOORD_0": au}, "indices": ai,
                 "material": p % a.materials}]})
            if lod == 0:
                tris += len(idx) // 3
    materials, images, textures_json = [], [], []
    tex_bytes = 0
    for m in range(a.materials):
        files, nbytes = textures(a.outdir, m, a.texture)
        tex_bytes += nbytes
        base = len(images)
        for k in ("base", "mr", "normal", "emit"):
            images.append({"uri": files[k], "name": "m%d_%s" % (m, k)})
            textures_json.append({"source": len(images) - 1})
        materials.append({"name": "mat%d" % m,
                          "pbrMetallicRoughness": {"baseColorTexture": {"index": base}, "metallicFactor": 1.0,
                                                   "roughnessFactor": 1.0,
                                                   "metallicRoughnessTexture": {"index": base + 1}},
                          "normalTexture": {"index": base + 2}, "emissiveTexture": {"index": base + 3},
                          "emissiveFactor": [1.0, 1.0, 1.0],
                          "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}}})
    nodes = []
    roots = []
    for i in range(a.instances):
        p = i % a.parts
        ang = 2 * math.pi * i / a.instances
        rad = 80.0 + (i % 11) * 9.0
        t = [math.cos(ang) * rad, ((i * 37) % 60) - 30.0, math.sin(ang) * rad]
        q = [0.0, math.sin(ang * 0.5), 0.0, math.cos(ang * 0.5)]
        for lod in (0, 1):
            nodes.append({"name": "piece%d_LOD%d" % (i, lod), "mesh": 2 * p + lod, "translation": t, "rotation": q})
            roots.append(len(nodes) - 1)
    gltf = {"asset": {"version": "2.0", "generator": "sutekh tools/gen_stress_gltf.py"},
            "extensionsUsed": ["KHR_materials_emissive_strength"],
            "buffers": [{"byteLength": len(bin_data), "uri": "stress.bin"}], "bufferViews": views,
            "accessors": accessors, "meshes": meshes, "materials": materials, "images": images,
            "textures": textures_json, "nodes": nodes, "scenes": [{"nodes": roots}], "scene": 0}
    with open(os.path.join(a.outdir, "stress.bin"), "wb") as fh:
        fh.write(bin_data)
    with open(os.path.join(a.outdir, "stress.gltf"), "w", encoding="utf-8") as fh:
        json.dump(gltf, fh)
    print("wrote %s: %d parts (LOD0 %d triangles in all parts), %d placed nodes (%d with LOD1 siblings), %d materials, "
          "%d PNG textures of %d px (%d bytes), buffer %d bytes" %
          (os.path.join(a.outdir, "stress.gltf"), a.parts, tris, a.instances, a.instances, a.materials,
           4 * a.materials, a.texture, tex_bytes, len(bin_data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
