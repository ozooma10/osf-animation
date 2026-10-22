"""Original FURN interaction markers, rebased onto the audition's first role.

SNAM stores xyz + clockwise marker rotation in radians. Starfield's row-vector
root pin is active Rz(-heading), so retain marker heading differences. Position
differences are rotated by the inverse of the first marker's active rotation.
The installed SNAM/NNAM records are checked independently by verify.py.
"""
from __future__ import annotations

import math
import struct
from pathlib import Path


SOURCES = {
    "hunter-walter": {
        "form_id": 0x0038A70D,
        "editor_id": "Scenes_MQ204_HunterWalter",
        "roles": ["Hunter", "Walter"],
        "markers": {"Hunter": (0, -0.72, 0, 0), "Walter": (0, 0, 0, 0)},
    },
    "survivor-victim": {
        "form_id": 0x0020FFAD,
        "equivalent_form_id": 0x00212FD8,
        "editor_id": "Scenes_MQ204A_030_FemaleVictim",
        "roles": ["Survivor", "Victim"],
        "markers": {"Survivor": (1.1867, -0.0821, 0, 270), "Victim": (0, 0, 0, 0)},
    },
    "key": {
        "form_id": 0x000E6A6A,
        "editor_id": "Scenes_DialogueCFTheKey_EstablishingScene",
        "roles": ["Marauder", "Reaver"],
        # The vanilla record orders Naeva, Reaver, Marauder. Select by NNAM name,
        # not index: the audition deliberately omits Naeva and starts with Marauder.
        "markers": {"Marauder": (0.45, -0.30, 0, 75), "Reaver": (-0.50, -0.15, 0, 90)},
    },
}


def rebase(markers, roles):
    """Return role offsets in OSF units/degrees, leaving role zero at the anchor."""
    x0, y0, z0, clockwise0 = markers[roles[0]]
    # Inverse of the first actor's Rz(-clockwise0).
    angle = math.radians(clockwise0)
    c, s = math.cos(angle), math.sin(angle)
    offsets = []
    for role in roles:
        x, y, z, clockwise = markers[role]
        dx, dy = x - x0, y - y0
        values = [c * dx - s * dy, s * dx + c * dy, z - z0,
                  (clockwise - clockwise0 + 180) % 360 - 180]
        offsets.append(dict(zip(("x", "y", "z", "heading"), [round(v, 6) + 0.0 for v in values])))
    return offsets


def pair_placements(family):
    source = SOURCES[family]
    return rebase(source["markers"], source["roles"])


def read_installed_markers(esm: Path):
    """Read the four small source records without loading the entire plugin."""
    # verify.py adds the repository's vanilla-packs tooling to sys.path.
    from extract_furn_anchors import subrecords, walk_records, zstr

    wanted = {source["form_id"] for source in SOURCES.values()}
    wanted.add(SOURCES["survivor-victim"]["equivalent_form_id"])
    found = {}

    def record(fid, raw):
        if fid not in wanted:
            return
        fields = dict(subrecords(raw))
        snam = fields[b"SNAM"]
        strings = fields[b"NNAM"].split(b"\0")
        if len(snam) % 28 or len(strings) != len(snam) // 28 * 2 + 1:
            raise ValueError(f"Unexpected furniture marker layout: {fid:08X}")
        markers = {}
        for index in range(len(snam) // 28):
            x, y, z, radians, keyword, entry, exit_ = struct.unpack_from("<4f3I", snam, index * 28)
            name = strings[index * 2].decode("ascii")
            markers[name] = (x, y, z, math.degrees(radians))
        found[fid] = {"editor_id": zstr(fields[b"EDID"]), "markers": markers, "snam_hex": snam.hex()}

    with esm.open("rb") as stream:
        while True:
            start = stream.tell()
            header = stream.read(24)
            if not header:
                break
            if len(header) != 24:
                raise ValueError("Truncated ESM header")
            size = struct.unpack_from("<I", header, 4)[0]
            if header[:4] != b"GRUP":
                stream.seek(size, 1)
                continue
            end = start + size
            if header[8:12] == b"FURN":
                walk_records(stream, end, b"FURN", record)
                break
            stream.seek(end)
    if found.keys() != wanted:
        raise ValueError("Missing source furniture records")
    return found
