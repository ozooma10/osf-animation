#!/usr/bin/env python3
"""Generate OSF Animation's template-IDLE master (dist/OSF.esm).

The IdlePlayer system (src/Animation/IdlePlayer.*) plays a modder-supplied loose `.af` by holding a
template `TESIdleForm`, repointing its GNAM/animFileName to the target file per play, and calling the
engine's PlayIdle. This plugin ships a small POOL of identical template IDLE records (one is mutated
per play, so the pool must be >= the max concurrent plays) for the engine's dynamic-idle door.

The byte layout is cloned EXACTLY from the in-game-proven test plugin
`MO2/mods/OSF Idle Test/IdleTest.esm` (OSF_TestIdle, cloned in xEdit from the vanilla DynamicIdle
ReadDataSlate_M01), only scaled to N records with corrected header counts. Each record is a vanilla
DynamicIdle shape: ENAM=DynamicIdle (routes to NT_DYNAMIC_ANIMATION), DNAM=Human_Root.agx (the human
behaviour graph), FNAM=0x01 (Loose), GNAM=a placeholder path that IdlePlayer overwrites at runtime.

Run from the repo root:  python packaging/generate_template_esm.py
Output:                  dist/OSF.esm   (xmake's after_build copies it to the mod root)
"""

import struct
import sys
from pathlib import Path

# --- tunables ----------------------------------------------------------------------------------
POOL_SIZE = 4                 # template IDLE records; bump if you need more concurrent plays
FIRST_LOCAL_ID = 0x800        # first user FormID (0x000-0x7FF are engine-reserved)
SELF_INDEX = 0x01             # load-order byte for this plugin's own records (1 master = index 01;
                              # the engine remaps at load, and OSF's ComposeFormID ignores it)
EDID_FMT = "OSF_DynamicIdle_{}"
MASTER = "Starfield.esm"
AUTHOR = "OSF"
GRAPH = r"AnimTextData\Tables\Graphs\Human_Root.agx"   # DNAM (donor behaviour graph)
EVENT = "DynamicIdle"                                   # ENAM (the dynamic-idle event)
GNAM_PLACEHOLDER = r"actors\human\animations\osf\template.af"  # repointed by IdlePlayer per play
# Stop idle (FormID FIRST_LOCAL_ID + POOL_SIZE). IdlePlayer PlayIdle()s this BEFORE a new clip to
# interrupt whatever idle is running — the only way to deliver "IdleStop" (a Graph Event the engine
# rejects when pushed externally via NotifyAnimationGraph) is through the idle subsystem, as AAF does
# with its LooseIdleStop form. No GNAM (it plays no clip, just sends the stop event).
STOP_EDID = "OSF_IdleStop"
STOP_EVENT = "IdleStop"
HEDR_VERSION = 0x3F75C28F     # float 0.96, the value the proven file carries (kept as raw bits)
TES4_INTERNAL_VERSION = 576
IDLE_INTERNAL_VERSION = 581
# -----------------------------------------------------------------------------------------------


def zstr(s: str) -> bytes:
    return s.encode("ascii") + b"\x00"


def subrecord(sig: str, body: bytes) -> bytes:
    return sig.encode("ascii") + struct.pack("<H", len(body)) + body


def record(sig: str, form_id: int, internal_version: int, body: bytes) -> bytes:
    # 24-byte record header: sig(4) dataSize(4) flags(4) formID(4) ts(2) vc(2) iv(2) unk(2)
    header = (sig.encode("ascii") + struct.pack("<I", len(body)) + struct.pack("<I", 0) +
              struct.pack("<I", form_id) + struct.pack("<HHHH", 0, 0, internal_version, 0))
    return header + body


def grup(label: str, payload: bytes) -> bytes:
    # 24-byte GRUP header: sig(4) groupSize(4, incl. header) label(4) type(4) ts(2) vc(2) iv(2) unk(2)
    size = 24 + len(payload)
    header = (b"GRUP" + struct.pack("<I", size) + label.encode("ascii") + struct.pack("<I", 0) +
              struct.pack("<HHHH", 0, 0, 0, 0))
    return header + payload


def build() -> bytes:
    # IDLE records ---------------------------------------------------------------------------
    idle_records = []
    for i in range(POOL_SIZE):
        form_id = (SELF_INDEX << 24) | (FIRST_LOCAL_ID + i)
        body = (subrecord("EDID", zstr(EDID_FMT.format(i))) +
                subrecord("DNAM", zstr(GRAPH)) +
                subrecord("ENAM", zstr(EVENT)) +
                subrecord("ANAM", b"\x00" * 8) +      # parent/prev idle form-ids, both null
                subrecord("FNAM", b"\x01") +          # flags: Loose
                subrecord("GNAM", zstr(GNAM_PLACEHOLDER)))
        idle_records.append(record("IDLE", form_id, IDLE_INTERNAL_VERSION, body))

    # Stop idle (FormID FIRST_LOCAL_ID + POOL_SIZE): ENAM=IdleStop, no GNAM, FNAM=0.
    stop_form_id = (SELF_INDEX << 24) | (FIRST_LOCAL_ID + POOL_SIZE)
    stop_body = (subrecord("EDID", zstr(STOP_EDID)) +
                 subrecord("DNAM", zstr(GRAPH)) +
                 subrecord("ENAM", zstr(STOP_EVENT)) +
                 subrecord("ANAM", b"\x00" * 8) +
                 subrecord("FNAM", b"\x00"))
    idle_records.append(record("IDLE", stop_form_id, IDLE_INTERNAL_VERSION, stop_body))

    idle_group = grup("IDLE", b"".join(idle_records))

    # TES4 header ----------------------------------------------------------------------------
    # numRecords counts groups + records (excludes TES4 itself), mirroring the proven file
    # (1 IDLE + 1 GRUP -> 2). nextObjectID is the first free local id above all records.
    record_count = POOL_SIZE + 1  # pool + the stop idle
    num_records = record_count + 1  # + the GRUP
    next_object_id = FIRST_LOCAL_ID + record_count
    hedr = struct.pack("<I", HEDR_VERSION) + struct.pack("<II", num_records, next_object_id)
    tes4_body = (subrecord("HEDR", hedr) +
                 subrecord("CNAM", zstr(AUTHOR)) +
                 subrecord("MAST", zstr(MASTER)) +
                 subrecord("INCC", struct.pack("<I", 0)))
    # TES4 flags: 0x01 = ESM/master
    tes4_header = (b"TES4" + struct.pack("<I", len(tes4_body)) + struct.pack("<I", 0x01) +
                   struct.pack("<I", 0) + struct.pack("<HHHH", 0, 0, TES4_INTERNAL_VERSION, 0))
    tes4 = tes4_header + tes4_body

    return tes4 + idle_group


def main() -> int:
    out = Path(__file__).resolve().parent.parent / "dist" / "OSF.esm"
    data = build()
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes, {POOL_SIZE} template IDLE records "
          f"0x{FIRST_LOCAL_ID:X}-0x{FIRST_LOCAL_ID + POOL_SIZE - 1:X} + stop idle "
          f"0x{FIRST_LOCAL_ID + POOL_SIZE:X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
