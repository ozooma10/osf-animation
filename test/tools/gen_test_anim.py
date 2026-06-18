#!/usr/bin/env python3
"""Generate a short, visibly-moving, cleanly-looping test clip for OSF Animation.

The baked-in test clips (StandSurrender / StandCover) are each a single pose held for
20 s — useless for *seeing* that playback / looping / blending / sync work. This builds a
clip that oscillates between two existing valid rig poses (A -> B -> A) on a short loop, so
the upper body visibly moves. Both endpoints are real rig poses, so there is no risky
procedural quaternion math and the result always maps onto the Starfield skeleton.

Reuses pose A's skeleton (node hierarchy + names), so OSF's bone mapping is unchanged.
Validate with:  xmake build osf-import-test  &&  osf-import-test.exe <out.glb>

Usage:
    python tools/gen_test_anim.py            # default: surrender<->cover, 1.2s -> TestSway01.glb
    python tools/gen_test_anim.py --a StandCover01.glb --b StandSurrender01.glb \
        --out TestNod01.glb --loop 0.8
"""
import argparse, gzip, json, struct, os

ANIM_DIR = os.path.join(os.path.dirname(__file__), "..", "dist", "OSF", "Animations", "OSF_Test")


def load(path):
    raw = gzip.open(path, "rb").read()  # the baked clips are gzipped (NAF-style) GLBs
    assert raw[:4] == b"glTF", "not a GLB"
    clen, _ = struct.unpack_from("<II", raw, 12)
    j = json.loads(raw[20:20 + clen])
    boff = 20 + clen
    blen, _ = struct.unpack_from("<II", raw, boff)
    return j, raw[boff + 8:boff + 8 + blen]


def elem0(j, bin_, ai):
    a = j["accessors"][ai]
    bv = j["bufferViews"][a["bufferView"]]
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    n = {"VEC4": 4, "VEC3": 3, "SCALAR": 1}[a["type"]]
    return list(struct.unpack_from("<" + "f" * n, bin_, off))


def pose(j, bin_):
    """name -> {rotation:[x,y,z,w], translation:[x,y,z]} from the clip's first keyframe."""
    a = j["animations"][0]
    nm = {i: nd.get("name") for i, nd in enumerate(j["nodes"])}
    out = {}
    for ch in a["channels"]:
        p = ch["target"]["path"]
        if p in ("rotation", "translation"):
            out.setdefault(nm[ch["target"]["node"]], {})[p] = elem0(j, bin_, a["samplers"][ch["sampler"]]["output"])
    return out


def build(a_path, b_path, out_path, loop):
    jA, bA = load(a_path)
    jB, bB = load(b_path)
    A, B = pose(jA, bA), pose(jB, bB)
    name2idx = {nd.get("name"): i for i, nd in enumerate(jA["nodes"])}
    times = [0.0, round(loop / 2, 4), loop]  # A -> B -> A so the first and last keyframe match (seamless loop)

    blob = bytearray()
    accs, samplers, channels = [], [], []

    def add_acc(values, comp, vmin=None, vmax=None):
        off = len(blob)
        for v in values:
            blob.extend(struct.pack("<" + "f" * comp, *v))
        a = {"bufferView": 0, "byteOffset": off, "componentType": 5126,
             "count": len(values), "type": {4: "VEC4", 3: "VEC3", 1: "SCALAR"}[comp]}
        if vmin:
            a["min"], a["max"] = vmin, vmax
        accs.append(a)
        return len(accs) - 1

    t_acc = add_acc([[t] for t in times], 1, [times[0]], [times[-1]])
    for n in A:
        if not n or n not in name2idx:
            continue
        b = B.get(n, {})
        for path, comp in (("rotation", 4), ("translation", 3)):
            if path not in A[n]:
                continue
            a0 = A[n][path]
            a1 = b.get(path, a0)
            out = add_acc([a0, a1, a0], comp)
            samplers.append({"input": t_acc, "output": out, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": name2idx[n], "path": path}})

    while len(blob) % 4:
        blob.append(0)
    out_j = dict(jA)  # keep A's asset/scene/scenes/nodes (the skeleton); replace all buffer data
    out_j["buffers"] = [{"byteLength": len(blob)}]
    out_j["bufferViews"] = [{"buffer": 0, "byteOffset": 0, "byteLength": len(blob)}]
    out_j["accessors"] = accs
    out_j["animations"] = [{"name": "TestLoop", "channels": channels, "samplers": samplers}]

    jb = json.dumps(out_j, separators=(",", ":")).encode()
    while len(jb) % 4:
        jb += b" "
    total = 12 + 8 + len(jb) + 8 + len(blob)
    glb = bytearray(b"glTF") + struct.pack("<I", 2) + struct.pack("<I", total)
    glb += struct.pack("<I", len(jb)) + b"JSON" + jb
    glb += struct.pack("<I", len(blob)) + b"BIN\x00" + blob
    open(out_path, "wb").write(gzip.compress(bytes(glb)))
    print(f"wrote {out_path}  bones={len(channels)//2} dur={loop}s  glb={total}B gz={os.path.getsize(out_path)}B")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--a", default="StandSurrender01.glb")
    p.add_argument("--b", default="StandCover01.glb")
    p.add_argument("--out", default="TestSway01.glb")
    p.add_argument("--loop", type=float, default=1.2, help="full loop seconds (A->B->A)")
    args = p.parse_args()
    build(os.path.join(ANIM_DIR, args.a), os.path.join(ANIM_DIR, args.b),
          os.path.join(ANIM_DIR, args.out), args.loop)
