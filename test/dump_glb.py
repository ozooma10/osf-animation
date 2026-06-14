#!/usr/bin/env python3
"""Dump a (possibly gzipped) NAF GLB: node tree with rest TRS, and the first/
last rotation keys of animated root-level nodes. Diagnostic for paired-clip
alignment issues."""

import gzip
import json
import math
import struct
import sys


def quat_to_euler_deg(q):
    x, y, z, w = q
    # ZYX intrinsic; good enough for eyeballing axis/yaw differences
    t0 = 2 * (w * x + y * z)
    t1 = 1 - 2 * (x * x + y * y)
    roll = math.degrees(math.atan2(t0, t1))
    t2 = max(-1.0, min(1.0, 2 * (w * y - z * x)))
    pitch = math.degrees(math.asin(t2))
    t3 = 2 * (w * z + x * y)
    t4 = 1 - 2 * (y * y + z * z)
    yaw = math.degrees(math.atan2(t3, t4))
    return roll, pitch, yaw


def load(path):
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    assert raw[:4] == b"glTF", "not a GLB"
    json_len = struct.unpack_from("<I", raw, 12)[0]
    doc = json.loads(raw[20:20 + json_len])
    bin_chunk = b""
    off = 20 + json_len
    while off < len(raw):
        clen, ctype = struct.unpack_from("<II", raw, off)
        if ctype == 0x004E4942:  # BIN
            bin_chunk = raw[off + 8:off + 8 + clen]
        off += 8 + clen
    return doc, bin_chunk


def read_accessor(doc, bin_chunk, idx):
    acc = doc["accessors"][idx]
    bv = doc["bufferViews"][acc["bufferView"]]
    comp_counts = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}
    n = comp_counts[acc["type"]]
    fmt = {5126: "f", 5123: "H", 5121: "B"}[acc["componentType"]]
    size = struct.calcsize(fmt) * n
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", size)
    out = []
    for i in range(acc["count"]):
        vals = struct.unpack_from("<" + fmt * n, bin_chunk, base + i * stride)
        out.append(vals[0] if n == 1 else vals)
    return out


def main(path):
    doc, bin_chunk = load(path)
    nodes = doc.get("nodes", [])
    is_child = set()
    for n in nodes:
        is_child.update(n.get("children", []))
    roots = [i for i in range(len(nodes)) if i not in is_child]

    print(f"=== {path}")
    print(f"nodes={len(nodes)} animations={len(doc.get('animations', []))} roots={roots}")

    def show(i, depth, max_depth=3):
        n = nodes[i]
        r = n.get("rotation")
        t = n.get("translation")
        desc = f"{'  ' * depth}[{i}] {n.get('name', '?')}"
        if r:
            e = quat_to_euler_deg(r)
            desc += f"  rot=({e[0]:.1f},{e[1]:.1f},{e[2]:.1f})deg"
        if t:
            desc += f"  pos=({t[0]:.2f},{t[1]:.2f},{t[2]:.2f})"
        print(desc)
        if depth < max_depth:
            for c in n.get("children", []):
                show(c, depth + 1, max_depth)

    for r in roots:
        show(r, 0)

    # animation channels on root + its direct children
    watch = set(roots)
    for r in roots:
        watch.update(nodes[r].get("children", []))
    for anim in doc.get("animations", []):
        print(f"anim '{anim.get('name', '')}': {len(anim.get('channels', []))} channels")
        for ch in anim.get("channels", []):
            tgt = ch.get("target", {})
            ni = tgt.get("node")
            if ni in watch:
                samp = anim["samplers"][ch["sampler"]]
                data = read_accessor(doc, bin_chunk, samp["output"])
                name = nodes[ni].get("name", "?")
                path_ = tgt.get("path")
                if path_ == "rotation":
                    e0 = quat_to_euler_deg(data[0])
                    e1 = quat_to_euler_deg(data[-1])
                    print(f"  node[{ni}] {name} rotation: {len(data)} keys, "
                          f"first=({e0[0]:.1f},{e0[1]:.1f},{e0[2]:.1f})deg "
                          f"last=({e1[0]:.1f},{e1[1]:.1f},{e1[2]:.1f})deg")
                else:
                    print(f"  node[{ni}] {name} {path_}: {len(data)} keys, "
                          f"first={tuple(round(v, 2) for v in data[0])} "
                          f"last={tuple(round(v, 2) for v in data[-1])}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
