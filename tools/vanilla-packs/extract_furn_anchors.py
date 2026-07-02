#!/usr/bin/env python3
"""Extract the AnimFurn* keyword table from Starfield.esm -> anim-furn-keywords.json.

Furniture animation classes in Starfield are declared by keywords named `AnimFurn<Class>`
(e.g. `AnimFurnChair`, `AnimFurnSitTable_Write`), carried in each FURN record's keyword
array. The vanilla animation folders (meshes/actors/human/animations/furniture/<class>/)
line up 1:1 with these class names, so an anchor keyword resolved from this table pins a
generated vanilla furniture scene to every furniture instance of that class in the game
(RE::TESObjectREFR::HasKeyword, the same check OSF's AnchorAccepts runs).

Output (committed, regenerate only on a game update):
  { "<normalized class>": { "name": "AnimFurn<Class>", "formid": "0xNNNNNN" }, ... }
normalized = lowercased AnimFurn-prefix-stripped name, underscores/spaces/dashes removed.

Bethesda plugin format notes: 24-byte record header, 6-byte subrecord header, per-record
zlib compression (flag 0x00040000), XXXX subrecord = uint32 real-size override for the
next subrecord, top-level GRUPs (type 0) labelled with the record signature.

Usage:
    python extract_furn_anchors.py [--esm <path>] [--out anim-furn-keywords.json]
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib

COMPRESSED = 0x00040000
DEFAULT_ESM = r"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Data\Starfield.esm"


def norm(s: str) -> str:
    return s.lower().replace("_", "").replace(" ", "").replace("-", "")


def subrecords(data: bytes):
    """Yield (sig, payload) from a record's field data, honoring the XXXX size override."""
    i, n, override = 0, len(data), None
    while i + 6 <= n:
        sig = data[i:i + 4]
        size = struct.unpack_from("<H", data, i + 4)[0]
        i += 6
        if sig == b"XXXX":
            override = struct.unpack_from("<I", data, i)[0]
            i += size
            continue
        if override is not None:
            size, override = override, None
        yield sig, data[i:i + size]
        i += size


def zstr(b: bytes) -> str:
    z = b.find(b"\x00")
    return b[: z if z >= 0 else len(b)].decode("cp1252", "replace")


def walk_records(f, group_end, want_sig, on_record):
    """Walk records (recursing into nested GRUPs) within [pos, group_end)."""
    while f.tell() < group_end:
        pos = f.tell()
        hdr = f.read(4)
        if len(hdr) < 4:
            break
        if hdr == b"GRUP":
            grp_size = struct.unpack("<I", f.read(4))[0]
            f.read(16)
            walk_records(f, pos + grp_size, want_sig, on_record)
            f.seek(pos + grp_size)
            continue
        data_size, flags, _formid = struct.unpack("<III", f.read(12))
        f.read(8)  # timestamp(2)+vc(2)+internalVersion(2)+unknown(2)
        raw = f.read(data_size)
        if hdr == want_sig:
            if flags & COMPRESSED:
                raw = zlib.decompress(raw[4:])  # first u32 = decompressed size
            on_record(_formid, raw)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--esm", default=DEFAULT_ESM)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "anim-furn-keywords.json"))
    args = ap.parse_args()

    fsize = os.path.getsize(args.esm)
    table: dict[str, dict] = {}
    with open(args.esm, "rb") as f:
        assert f.read(4) == b"TES4", "not a Starfield/Bethesda plugin"
        ds = struct.unpack("<I", f.read(4))[0]
        f.read(16)
        f.read(ds)
        while f.tell() < fsize:
            pos = f.tell()
            if f.read(4) != b"GRUP":
                break
            grp_size = struct.unpack("<I", f.read(4))[0]
            label = f.read(4)
            f.read(12)
            end = pos + grp_size
            if label == b"KYWD":
                def on_kywd(fid, d):
                    edid = next((zstr(p) for s, p in subrecords(d) if s == b"EDID"), "")
                    if edid.lower().startswith("animfurn"):
                        table[norm(edid[8:])] = {"name": edid, "formid": f"0x{fid & 0xFFFFFF:06X}"}
                walk_records(f, end, b"KYWD", on_kywd)
            f.seek(end)

    with open(args.out, "w", encoding="utf-8", newline="\n") as w:
        json.dump(dict(sorted(table.items())), w, indent=2)
        w.write("\n")
    print(f"wrote {len(table)} AnimFurn keyword(s) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
