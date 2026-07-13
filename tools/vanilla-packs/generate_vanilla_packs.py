#!/usr/bin/env python3
"""Generate OSF scene packs exposing the vanilla Starfield animation library.

Walks `Starfield - Animations.ba2`, filters to third-person human `.af` clips,
harvests per-clip metadata (duration from the .af header, display name / tag /
loop-vs-transition / root-motion flags from the .afx XML companion), and emits
one `*.osf.json` pack per top-level animation folder into `dist/OSF/vanilla/`.

Each leaf folder becomes one scene whose labeled stages are the clips — the
stage-as-browsable-animation model — so the whole library shows up in the
catalog and plays through the normal scene player with zero engine changes.
Every clip carries a pre-measured `sec` so the runtime never has to probe the
game archive for durations.

The output is static per game version: re-run after a game patch, diff, commit.

Usage (from the repo root):
    python tools/vanilla-packs/generate_vanilla_packs.py
    python tools/vanilla-packs/generate_vanilla_packs.py --include-combat
    python tools/vanilla-packs/generate_vanilla_packs.py --ba2 "D:\\path\\to\\Starfield - Animations.ba2" --out dist/OSF/vanilla
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

AF_FPS = 30.0  # Creation-Engine convention; mirrors AFImport::kAfFps
HUMAN_ANIMATED_BONES = 82  # human skeleton.rig boneCountAnimated (af_animation_skeleton_spec.md)
CLIP_ROOT = "meshes/actors/human/animations"
MAX_STAGES_PER_SCENE = 120  # oversized leaf folders get split into "<id>--2", "<id>--3", ...

# Subfolders under a furniture class that are variants/flavor, not a distinct class.
FURN_DECORATORS = {"animflavor", "female", "male"}
ANCHOR_MASTER = "Starfield.esm"  # every AnimFurn keyword is a base-game record

# Top-level folders under meshes/actors/human/animations/ shipped by default.
# gun/melee are weapon-pose layers (mostly meaningless standalone) — opt-in.
DEFAULT_TOPS = (
    "common",
    "furniture",
    "scenes",
    "chargen",
    "photomode",
    "playerinventory",
    "mq204a_030_deathscene",
)
COMBAT_TOPS = ("gun", "melee")


# ---------------------------------------------------------------------------
# BA2 (GNRL) reading — mirrors src/Util/Ba2.cpp
# ---------------------------------------------------------------------------

@dataclass
class Ba2Entry:
    name: str          # archive-relative path, backslashes, original case
    data_offset: int
    packed: int        # 0 = stored uncompressed
    unpacked: int


class Ba2Reader:
    def __init__(self, path: Path):
        self.path = path
        self.f = open(path, "rb")
        magic = self.f.read(4)
        if magic != b"BTDX":
            raise ValueError(f"{path}: not a BA2 (magic {magic!r})")
        (version,) = struct.unpack("<I", self.f.read(4))
        typ = self.f.read(4)
        if typ != b"GNRL":
            raise ValueError(f"{path}: not a GNRL archive ({typ!r})")
        (file_count,) = struct.unpack("<I", self.f.read(4))
        (name_table_offset,) = struct.unpack("<Q", self.f.read(8))
        rec_base = 0x20 if version >= 2 else 0x18

        self.f.seek(name_table_offset)
        blob = self.f.read()
        self.entries: list[Ba2Entry] = []
        p = 0
        for i in range(file_count):
            (nlen,) = struct.unpack_from("<H", blob, p)
            p += 2
            name = blob[p:p + nlen].decode("ascii", "replace").replace("/", "\\")
            p += nlen
            rec = rec_base + i * 36
            self.f.seek(rec + 16)
            data_offset, packed, unpacked = struct.unpack("<QII", self.f.read(16))
            self.entries.append(Ba2Entry(name, data_offset, packed, unpacked))
        self.by_name = {e.name.lower(): e for e in self.entries}

    def read(self, entry: Ba2Entry, prefix: int | None = None) -> bytes:
        """Read an entry, optionally only the first `prefix` decompressed bytes
        (partial inflate — the .af header pass never decompresses whole clips)."""
        self.f.seek(entry.data_offset)
        if entry.packed == 0:
            return self.f.read(entry.unpacked if prefix is None else min(prefix, entry.unpacked))
        if prefix is None:
            return zlib.decompress(self.f.read(entry.packed))
        d = zlib.decompressobj()
        out = b""
        remaining = entry.packed
        while len(out) < prefix and remaining > 0:
            chunk = self.f.read(min(65536, remaining))
            remaining -= len(chunk)
            out += d.decompress(chunk, prefix - len(out))
            if d.eof:
                break
        return out


# ---------------------------------------------------------------------------
# .af header / .afx companion
# ---------------------------------------------------------------------------

@dataclass
class AfxMeta:
    display: str = ""          # <filename> stem, authored casing
    tag: str = ""              # <tag>
    is_state: bool | None = None
    anim_driven: bool = False  # <flag>Animation Driven</flag> -> root motion


def parse_af_header(buf: bytes) -> tuple[int, int] | None:
    """(boneCount, frameCount) from the 64-byte .af header, or None if malformed."""
    if len(buf) < 0x2E:
        return None
    (bone_count,) = struct.unpack_from("<H", buf, 0x2A)
    (frame_count,) = struct.unpack_from("<H", buf, 0x2C)
    return bone_count, frame_count


# Real-coverage floor: a clip whose physically-present track count is below this
# fraction of the rig's animated-bone cap is a LAYER (partial-body clip meant to
# compose over another pose in the behavior graph). Played standalone, OSF stamps
# its untracked bones at bind pose -> T-pose limbs, so layers are excluded from the
# browsable library by default (--include-partial re-adds them, tagged "partial").
# 0.85 clears the ~9 skippable helper bones (AnimObject*/Camera*: the full-body
# idle_body tracks 73/82) while catching real layers (leanright: 5/82,
# relaxed_idlepartialbody_*: ~25/82). ~26% of human clips are layers by this rule.
PARTIAL_MIN_FRACTION = 0.85


def real_track_count(ba2: "Ba2Reader", e: "Ba2Entry") -> int | None:
    """Number of AnimationBlocks physically present = Σ(odd index-atlas runs).
    The header boneCount (0x2A) is only the atlas PREFIX length; even runs are
    identity/untracked bones (decoded to bind pose). Spec: OSF RE
    docs/af_animation_skeleton_spec.md §1-2. None if unparsable."""
    hdr = ba2.read(e, prefix=0x40)
    if len(hdr) < 0x40:
        return None
    (atlas_n,) = struct.unpack_from("<H", hdr, 0x2E)
    (preamble_off,) = struct.unpack_from("<H", hdr, 0x32)
    buf = ba2.read(e, prefix=0x40 + preamble_off + atlas_n)
    if len(buf) < 0x40 + preamble_off + atlas_n:
        return None
    atlas = buf[0x40 + preamble_off: 0x40 + preamble_off + atlas_n]
    return sum(atlas[j] for j in range(1, len(atlas), 2))


def parse_afx(raw: bytes) -> AfxMeta:
    """The .afx companions are tiny flat XML; regex keeps this stdlib-simple and
    tolerant of the occasional stray byte."""
    text = raw.decode("utf-8", "replace")
    m = AfxMeta()
    if g := re.search(r"<filename>([^<]+)</filename>", text, re.I):
        m.display = re.sub(r"\.af$", "", g.group(1).strip(), flags=re.I)
    if g := re.search(r"<tag>([^<]+)</tag>", text, re.I):
        m.tag = g.group(1).strip()
    if g := re.search(r"<is_state>(\d)</is_state>", text, re.I):
        m.is_state = g.group(1) == "1"
    if re.search(r"<flag>\s*Animation Driven\s*</flag>", text, re.I):
        m.anim_driven = True
    return m


# ---------------------------------------------------------------------------
# pack generation
# ---------------------------------------------------------------------------

@dataclass
class Clip:
    rel: str            # path below CLIP_ROOT, forward slashes, original case
    display: str
    sec: float
    tags: list[str] = field(default_factory=list)


def humanize(segment: str) -> str:
    """'mq101_010_barrettscene' -> 'Mq101 010 Barrettscene', 'animflavor' -> 'Animflavor'."""
    return " ".join(w.capitalize() for w in re.split(r"[_\s]+", segment) if w)


def collect_clips(ba2: Ba2Reader, tops: tuple[str, ...],
                  include_partial: bool = False) -> tuple[dict[str, list[Clip]], collections.Counter]:
    """Group human third-person clips by leaf folder (path below CLIP_ROOT)."""
    prefix = (CLIP_ROOT + "/").replace("/", "\\").lower()
    skipped = collections.Counter()
    scenes: dict[str, list[Clip]] = collections.defaultdict(list)

    for e in ba2.entries:
        low = e.name.lower()
        if not low.startswith(prefix) or not low.endswith(".af"):
            continue
        rel_low = low[len(prefix):]
        top = rel_low.split("\\")[0]
        if top not in tops:
            skipped["top:" + top] += 1
            continue

        hdr = parse_af_header(ba2.read(e, prefix=0x40))
        if hdr is None:
            skipped["malformed header"] += 1
            continue
        bone_count, frame_count = hdr
        if bone_count == 0 or bone_count > HUMAN_ANIMATED_BONES:
            skipped["bone count"] += 1
            continue
        if frame_count == 0:
            skipped["zero frames"] += 1
            continue
        real = real_track_count(ba2, e)
        partial = real is not None and real < PARTIAL_MIN_FRACTION * HUMAN_ANIMATED_BONES
        if partial and not include_partial:
            skipped["partial-coverage layer"] += 1
            continue

        meta = AfxMeta()
        afx = ba2.by_name.get(low[:-3] + ".afx")
        if afx:
            meta = parse_afx(ba2.read(afx))

        stem = e.name.rsplit("\\", 1)[-1][:-3]
        stem_low = stem.lower()
        if meta.tag.lower() == "camera" or stem_low.startswith("camera_"):
            skipped["camera clip"] += 1
            continue
        if "lookdirections" in stem_low:
            skipped["look-direction blend data"] += 1
            continue

        tags = []
        if partial:
            tags.append("partial")
        if meta.tag and meta.tag.lower() != stem_low:
            tags.append(meta.tag.lower())
        if meta.is_state is False:
            tags.append("transition")
        if meta.anim_driven:
            tags.append("rootmotion")
        if frame_count <= 2 and "pose" not in tags:
            tags.append("pose")

        rel = e.name[len(prefix):].replace("\\", "/")
        leaf = rel.rsplit("/", 1)[0] if "/" in rel else ""
        scenes[leaf].append(Clip(
            rel=rel,
            display=meta.display or stem,
            sec=round(max(frame_count - 1, 1) / AF_FPS, 3),
            tags=tags,
        ))
    return scenes, skipped


def stage_sort_key(c: Clip) -> tuple[int, str]:
    """Idles first, then enters, exits, the rest alphabetically — a browsable order."""
    low = c.display.lower()
    if "idle" in low:
        rank = 0
    elif "enter" in low:
        rank = 1
    elif "exit" in low:
        rank = 2
    else:
        rank = 3
    return rank, low


def furniture_class_key(segments: list[str]) -> str | None:
    """The furniture class a scene's folder belongs to (for anchor lookup), or None.
    'furniture/chair/animflavor' -> 'chair'; 'furniture/scenes/mq101_010_barrettscene'
    -> 'mq101_010_barrettscene'; non-furniture tops -> None."""
    if not segments or segments[0] != "furniture":
        return None
    rest = [s for s in segments[1:] if s not in FURN_DECORATORS]
    if not rest:
        return None
    if rest[0] == "scenes":
        return rest[1] if len(rest) > 1 else None
    return rest[0]


def build_scene(scene_id: str, name: str, tags: list[str], clips: list[Clip],
                anchor: dict | None = None) -> dict:
    stages = []
    for c in clips:
        stage = {"name": c.display, "loops": 0,
                 "clips": [{"file": c.rel, "sec": c.sec}]}
        if c.tags:
            stage["tags"] = c.tags
        stages.append(stage)
    scene = {"id": scene_id, "name": name, "tags": tags}
    if anchor:
        scene["anchor"] = anchor  # binds the scene to matching furniture (RequiresAnchor)
    scene["stages"] = stages
    return scene


def build_packs(scenes: dict[str, list[Clip]], furn_kw: dict) -> tuple[dict[str, dict], collections.Counter]:
    """One pack document per top folder, scenes sorted, oversized leaves split.
    Furniture scenes gain a keyword `anchor` when their class maps to an AnimFurn keyword."""
    packs: dict[str, dict] = {}
    anchor_stats = collections.Counter()
    for leaf in sorted(scenes):
        clips = sorted(scenes[leaf], key=stage_sort_key)
        segments = leaf.split("/") if leaf else ["misc"]
        top = segments[0]
        scene_id = "vanilla/" + "/".join(segments)
        name = "Vanilla · " + " / ".join(humanize(s) for s in segments)
        # Folder taxonomy lives in the id/name (the library view derives its tree from the
        # id); tags stay a small filterable set so they don't flood the catalog tag cloud.
        tags = ["vanilla", top.lower()]
        if "female" in (s.lower() for s in segments[1:]):
            tags.append("female")

        # Furniture scenes anchor to the class keyword so they only offer on matching furniture
        # (and the guided UI can count/surface them). Unmapped classes stay free-space.
        anchor = None
        cls = furniture_class_key(segments)
        if cls is not None:
            kw = furn_kw.get(cls.lower().replace("_", "").replace(" ", "").replace("-", ""))
            if kw:
                anchor = {"keyword": [f"{ANCHOR_MASTER}|{kw['formid']}"]}
                tags.append("anchored")
                anchor_stats["anchored"] += 1
            else:
                anchor_stats["unmapped:" + cls] += 1

        doc = packs.setdefault(top, {
            "schema": 1,
            "name": f"OSF Vanilla — {humanize(top)} (generated, do not hand-edit)",
            "section": "library",
            # Reference shelf, not matchmaking fodder: browse/play by direct id only;
            # the matchmaking pool belongs to custom mod scenes.
            "unlisted": True,
            "clipRoot": CLIP_ROOT,
            "scenes": [],
        })
        chunks = [clips[i:i + MAX_STAGES_PER_SCENE]
                  for i in range(0, len(clips), MAX_STAGES_PER_SCENE)]
        for n, chunk in enumerate(chunks, start=1):
            sid = scene_id if n == 1 else f"{scene_id}--{n}"
            snm = name if n == 1 else f"{name} ({n})"
            doc["scenes"].append(build_scene(sid, snm, tags, chunk, anchor))
    return packs, anchor_stats


# ---------------------------------------------------------------------------
# creature / alien packs — every non-human actor root under meshes\actors\
# ---------------------------------------------------------------------------
# Each non-human actor folder ships its OWN characterassets\skeleton.rig and its
# own animation set (bone counts run 6 -> 123, unlike human's 82). The engine now
# derives the rig to decode each .af against from the clip's own path (Util::Species),
# so these packs need no special engine wiring — they just declare a per-species
# clipRoot. `meshes\actors\human\animations` is owned by the human path above and
# excluded here; DLC wrappers like sfbgs004\human / sfbgs004\modela ARE included
# (they carry their own rig copy and map onto the human / modela skeleton).

HUMAN_SPECIES_ROOT = "meshes\\actors\\human"  # owned by the human collect/build path


def creature_species(path_low: str) -> tuple[str, str, int] | None:
    """(species_root, family, anim_index) for a non-human actor .af path, or None.
    species_root = the folder holding \\animations (backslashes, lowercase); family =
    the skeleton family = the last segment before \\animations (bipeda, modela, human…).
    'meshes\\actors\\bipeda\\animations\\x.af' -> ('meshes\\actors\\bipeda', 'bipeda', 3)
    'meshes\\actors\\sfbgs004\\human\\animations\\x.af' -> ('…\\sfbgs004\\human', 'human', 4)."""
    parts = path_low.split("\\")
    if len(parts) < 4 or parts[0] != "meshes" or parts[1] != "actors" or "animations" not in parts:
        return None
    ai = parts.index("animations")
    if ai < 3:
        return None
    species_root = "\\".join(parts[:ai])
    # base human is the human path's job; its nested folders (human\_1stperson = viewmodel
    # arms) are not full-body species and never bind to a real actor skeleton.
    if species_root == HUMAN_SPECIES_ROOT or species_root.startswith(HUMAN_SPECIES_ROOT + "\\"):
        return None
    return species_root, parts[ai - 1], ai


def rig_animated_bones(ba2: Ba2Reader, species_root_low: str) -> int | None:
    """boneCountAnimated (@0x3A) from a species' skeleton.rig header — the accept cap
    for that species' clips (mirrors AFImport::ParseRig). None if the rig is absent."""
    e = ba2.by_name.get(species_root_low + "\\characterassets\\skeleton.rig")
    if not e:
        return None
    buf = ba2.read(e, prefix=0x40)
    if len(buf) < 0x3C:
        return None
    (bc_animated,) = struct.unpack_from("<H", buf, 0x3A)
    return bc_animated or None


def collect_creature_clips(ba2: Ba2Reader,
                           include_partial: bool = False) -> tuple[dict[str, dict], collections.Counter]:
    """species_root -> {family, clip_root (fwd), cap, leaves: {leaf: [Clip]}}."""
    out: dict[str, dict] = {}
    skipped = collections.Counter()
    for e in ba2.entries:
        low = e.name.lower()
        if not low.endswith(".af"):
            continue
        info = creature_species(low)
        if info is None:
            continue
        species_root_low, family, ai = info
        sp = out.get(species_root_low)
        if sp is None:
            sp = out[species_root_low] = {
                "family": family,
                "clip_root": species_root_low.replace("\\", "/") + "/animations",
                "cap": rig_animated_bones(ba2, species_root_low),
                "leaves": collections.defaultdict(list),
            }

        hdr = parse_af_header(ba2.read(e, prefix=0x40))
        if hdr is None:
            skipped["malformed header"] += 1
            continue
        bone_count, frame_count = hdr
        cap = sp["cap"]
        if bone_count == 0 or (cap and bone_count > cap):
            skipped["bone count"] += 1
            continue
        if frame_count == 0:
            skipped["zero frames"] += 1
            continue
        real = real_track_count(ba2, e) if cap else None
        partial = real is not None and real < PARTIAL_MIN_FRACTION * cap
        if partial and not include_partial:
            skipped["partial-coverage layer"] += 1
            continue

        meta = AfxMeta()
        afx = ba2.by_name.get(low[:-3] + ".afx")
        if afx:
            meta = parse_afx(ba2.read(afx))

        stem = e.name.rsplit("\\", 1)[-1][:-3]
        stem_low = stem.lower()
        if meta.tag.lower() == "camera" or stem_low.startswith("camera_"):
            skipped["camera clip"] += 1
            continue

        tags = []
        if partial:
            tags.append("partial")
        if meta.tag and meta.tag.lower() != stem_low:
            tags.append(meta.tag.lower())
        if meta.is_state is False:
            tags.append("transition")
        if meta.anim_driven:
            tags.append("rootmotion")
        if frame_count <= 2 and "pose" not in tags:
            tags.append("pose")

        parts = e.name.split("\\")
        rel = "/".join(parts[ai + 1:])         # path below \animations (forward slashes)
        leaf = "/".join(parts[ai + 1:-1])      # subfolder(s) below \animations, "" if none
        sp["leaves"][leaf].append(Clip(
            rel=rel,
            display=meta.display or stem,
            sec=round(max(frame_count - 1, 1) / AF_FPS, 3),
            tags=tags,
        ))
    return out, skipped


def build_creature_packs(species_map: dict[str, dict]) -> dict[str, dict]:
    """One pack document per non-human species root; scenes are the leaf folders,
    tagged `species:<family>` so the browser can filter to the picked creature."""
    packs: dict[str, dict] = {}
    for species_root_low, sp in sorted(species_map.items()):
        species_id = "-".join(species_root_low.split("\\")[2:])  # bipeda | sfbgs004-human
        family = sp["family"]
        doc = {
            "schema": 1,
            "name": f"OSF Vanilla — {humanize(species_id)} (generated, do not hand-edit)",
            "section": "library",
            "unlisted": True,
            "clipRoot": sp["clip_root"],
            "scenes": [],
        }
        for leaf in sorted(sp["leaves"]):
            clips = sorted(sp["leaves"][leaf], key=stage_sort_key)
            leaf_segs = leaf.split("/") if leaf else []
            scene_id = "vanilla/creature/" + "/".join([species_id] + leaf_segs)
            name = "Vanilla · " + " / ".join(humanize(s) for s in [species_id] + leaf_segs)
            tags = ["vanilla", "creature", f"species:{family}"]
            if leaf_segs:
                tags.append(leaf_segs[0].lower())  # furniture / weapon / photomode …
            chunks = [clips[i:i + MAX_STAGES_PER_SCENE]
                      for i in range(0, len(clips), MAX_STAGES_PER_SCENE)]
            for n, chunk in enumerate(chunks, start=1):
                sid = scene_id if n == 1 else f"{scene_id}--{n}"
                snm = name if n == 1 else f"{name} ({n})"
                doc["scenes"].append(build_scene(sid, snm, tags, chunk))
        packs[species_id] = doc
    return packs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ba2", type=Path,
        default=Path(r"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Data\Starfield - Animations.ba2"))
    ap.add_argument("--out", type=Path,
        default=Path(__file__).resolve().parents[2] / "dist" / "OSF" / "vanilla")
    ap.add_argument("--include-combat", action="store_true",
        help="also generate gun/melee packs (weapon-pose layers; mostly odd standalone)")
    ap.add_argument("--no-creatures", action="store_true",
        help="skip the per-species creature/alien packs (every non-human actor root)")
    ap.add_argument("--include-partial", action="store_true",
        help="keep partial-coverage layer clips (real tracks < 85%% of the rig), tagged \"partial\" — "
             "these T-pose their untracked bones when played standalone")
    ap.add_argument("--furn-keywords", type=Path,
        default=Path(__file__).resolve().parent / "anim-furn-keywords.json",
        help="AnimFurn keyword table from extract_furn_anchors.py (furniture anchoring)")
    args = ap.parse_args()

    furn_kw = {}
    if args.furn_keywords.is_file():
        furn_kw = json.loads(args.furn_keywords.read_text(encoding="utf-8"))
    else:
        print(f"WARNING: {args.furn_keywords.name} not found — furniture scenes will be free-space "
              f"(run extract_furn_anchors.py first)")

    tops = DEFAULT_TOPS + (COMBAT_TOPS if args.include_combat else ())
    ba2 = Ba2Reader(args.ba2)
    scenes, skipped = collect_clips(ba2, tops, include_partial=args.include_partial)
    packs, anchor_stats = build_packs(scenes, furn_kw)

    args.out.mkdir(parents=True, exist_ok=True)
    total_clips = total_scenes = 0
    for top, doc in sorted(packs.items()):
        n_scenes = len(doc["scenes"])
        n_clips = sum(len(s["stages"]) for s in doc["scenes"])
        total_scenes += n_scenes
        total_clips += n_clips
        out_file = args.out / f"vanilla-{top}.osf.json"
        with open(out_file, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"{out_file.name}: {n_scenes} scene(s), {n_clips} clip(s)")

    # Per-species creature/alien packs (every non-human actor root).
    if not args.no_creatures:
        species_map, creature_skipped = collect_creature_clips(ba2, include_partial=args.include_partial)
        creature_packs = build_creature_packs(species_map)
        c_species = c_scenes = c_clips = 0
        for species_id, doc in sorted(creature_packs.items()):
            n_scenes = len(doc["scenes"])
            n_clips = sum(len(s["stages"]) for s in doc["scenes"])
            if n_scenes == 0:
                continue
            c_species += 1
            c_scenes += n_scenes
            c_clips += n_clips
            total_scenes += n_scenes
            total_clips += n_clips
            out_file = args.out / f"vanilla-creature-{species_id}.osf.json"
            with open(out_file, "w", encoding="utf-8", newline="\n") as f:
                json.dump(doc, f, indent=2, ensure_ascii=False)
                f.write("\n")
            print(f"{out_file.name}: {n_scenes} scene(s), {n_clips} clip(s)")
        print(f"creatures: {c_species} species, {c_scenes} scene(s), {c_clips} clip(s)")
        for k, v in creature_skipped.most_common():
            skipped[f"creature {k}"] += v

    print(f"\ntotal: {total_scenes} scene(s), {total_clips} clip(s)")
    anchored = anchor_stats.pop("anchored", 0)
    unmapped = sorted(k[len("unmapped:"):] for k in anchor_stats)
    print(f"furniture anchors: {anchored} scene(s) keyed to a class"
          + (f"; {len(unmapped)} class(es) left free-space: {', '.join(unmapped)}" if unmapped else ""))
    if skipped:
        print("skipped:", ", ".join(f"{k}={v}" for k, v in skipped.most_common()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
