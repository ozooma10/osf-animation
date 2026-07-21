#!/usr/bin/env python3
"""Generate the standalone dev view's library snapshot offline.

Replicates UIBridge.cpp BuildCatalog(a_library=true) over the generated packs in
dist/OSF (section:"library"), writing the card array the in-game view would
receive to ui/animation-browser/fixtures/live/library.json. The library lane is
fully static — pack-authored clip durations, no pins, no probe cache — so the
runtime and this script produce the same payload; the runtime PersistSnapshot
path was removed in favor of this.

The card math mirrors the C++ exactly (see BuildCatalog / ParseOsfStageList /
DesugarLinear in SceneRegistry.cpp), including float32 rounding, so durations
serialize with the same values the DLL would emit. Anchor keyword form ids are
prettified via tools/vanilla-packs/anim-furn-keywords.json (the ESM extractor
output) — the offline stand-in for GetFormEditorID.

The script is deliberately STRICT: an unknown scene/stage/clip key aborts, so a
pack-schema change forces this generator to be updated instead of silently
drifting from what the runtime serializes.

Usage: python tools/generate-library-snapshot.py
"""

import glob
import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK_DIR = os.path.join(REPO, "dist", "OSF")
KEYWORD_MAP = os.path.join(REPO, "tools", "vanilla-packs", "anim-furn-keywords.json")
OUT = os.path.join(REPO, "ui", "animation-browser", "fixtures", "live", "library.json")

# How many loops an open-ended hold stage is assumed to run (kHoldLoopEstimate).
HOLD_LOOP_ESTIMATE = 2.0

# Keys the generator understands; anything else aborts (see module docstring).
SCENE_KEYS = {"id", "name", "tags", "stages", "anchor", "clipRoot", "unlisted"}
STAGE_KEYS = {"name", "tags", "loops", "timer", "clips"}
CLIP_KEYS = {"file", "sec", "anim", "offset"}
ANCHOR_KEYS = {"keyword", "base"}
FILE_KEYS = {"schema", "name", "pack", "section", "unlisted", "clipRoot", "scenes", "stripActors", "fade", "anchor"}


def f32(x):
    """Round-trip through IEEE float32 — the C++ side stores all durations as float."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def load_jsonc(path):
    """Pack JSONs may carry // comments (the registry parses with ignore_comments)."""
    text = open(path, encoding="utf-8-sig").read()
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            out.append(c)
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    return json.loads("".join(out))


def check_keys(obj, allowed, what):
    extra = set(obj) - allowed
    if extra:
        sys.exit(f"error: {what} has key(s) this generator does not model: {sorted(extra)} — "
                 "update tools/generate-library-snapshot.py to match BuildCatalog")


def keyword_label(edid):
    """KeywordLabel: strip the AnimFurn/Anim prefix, split camel-case into words."""
    for prefix in ("AnimFurn", "Anim"):
        if edid.startswith(prefix):
            edid = edid[len(prefix):]
            break
    out = []
    for i, c in enumerate(edid):
        if i > 0 and c.isupper() and (not edid[i - 1].isupper() or
                                      (i + 1 < len(edid) and edid[i + 1].islower())):
            out.append(" ")
        out.append(" " if c == "_" else c)
    return "".join(out)


def normalize_clip_root(root):
    root = root.lower()
    if not root or root == "naf":
        return root
    root = root.replace("\\", "/").rstrip("/")
    if ":" in root:
        sys.exit(f"error: clipRoot '{root}' may not contain ':'")
    return root


def apply_clip_root(file, clip_root):
    low = file.lower()
    if not clip_root or not file:
        return file
    if low.startswith(("naf:", "naf/", "naf\\")) or (len(file) > 1 and file[1] == ":"):
        return file
    if clip_root == "naf":
        return "naf:" + file
    return clip_root + "/" + file


def species_from_path(path):
    """SpeciesFromAnimPath: the segment before an 'animations' folder that itself
    follows an 'actors' segment (mirrors Util::Species MarkerIndex — a path like
    SAF/Animations/... names no species and falls back to human downstream)."""
    segs = [s for s in path.lower().replace("\\", "/").split("/") if s]
    have_actors = False
    for i, s in enumerate(segs):
        if s == "actors":
            have_actors = True
        elif have_actors and i > 0 and s == "animations":
            return segs[i - 1]
    return ""


def sec_or_null(x):
    return x if x is not None and x >= 0.0 else None


def build_card(scene, file_defaults, kw_by_formid):
    check_keys(scene, SCENE_KEYS, f"scene '{scene.get('id', '?')}'")
    sid = scene["id"]
    clip_root = (normalize_clip_root(scene["clipRoot"]) if "clipRoot" in scene
                 else file_defaults["clipRoot"])

    # Anchor: scene-level block, else the file-level default.
    anchor = scene.get("anchor", file_defaults["anchor"])
    anchor_names = []
    if anchor:
        check_keys(anchor, ANCHOR_KEYS, f"scene '{sid}' anchor")
        for entry in anchor.get("keyword", []):
            formid = entry.split("|", 1)[1].lower() if "|" in entry else None
            edid = kw_by_formid.get(formid) if formid else entry
            if edid:
                anchor_names.append(keyword_label(edid))
            else:
                print(f"warn: scene '{sid}': anchor keyword {entry} not in anim-furn-keywords.json — label dropped")
        for entry in anchor.get("base", []):
            # Runtime falls back to the form id when the base form has no edid.
            formid = int(entry.split("|", 1)[1], 16) if "|" in entry else int(entry, 16)
            anchor_names.append(f"{formid:#010x}")

    # Stages: ParseOsfStageList defaults + DesugarLinear node shapes + StageCard math.
    stages = []
    est_sec = -1.0
    est_partial = False
    open_ended = False
    actor_count = 0
    for i, st in enumerate(scene["stages"]):
        if not isinstance(st, dict):
            sys.exit(f"error: scene '{sid}': bare-array stage shorthand not modeled here")
        check_keys(st, STAGE_KEYS, f"scene '{sid}' stage {i}")
        timer = float(st.get("timer", 0.0))
        loops = int(st.get("loops", 0))
        if "timer" not in st and "loops" not in st:
            loops = 1  # untimed -> play once

        files = []
        loop_sec = None
        for j, clip in enumerate(st["clips"]):
            if isinstance(clip, str):
                clip = {"file": clip}
            check_keys(clip, CLIP_KEYS, f"scene '{sid}' stage {i} clip {j}")
            file = apply_clip_root(clip["file"], clip_root)
            files.append(file)
            if j == 0 and clip.get("sec", 0) > 0:
                loop_sec = f32(clip["sec"])
        if actor_count == 0:
            actor_count = len(files)

        # DesugarLinear: timer>0 -> hold+timer (or count if loops>0 too); loops>0 -> count; else hold.
        sc_est = None
        sc_open = False
        if timer > 0.0 and loops > 0:
            out_loops, out_timer = loops, timer
            if loop_sec is not None:
                sc_est = f32(loops * loop_sec)
                sc_est = min(sc_est, timer)
            else:
                sc_est = timer
        elif timer > 0.0:
            out_loops, out_timer = 0, timer
            sc_est = timer  # timed hold: exact
        elif loops > 0:
            out_loops, out_timer = loops, 0.0
            if loop_sec is not None:
                sc_est = f32(loops * loop_sec)
        else:
            out_loops, out_timer = 0, 0.0
            sc_open = True  # runs until advanced — assume a couple of loops
            if loop_sec is not None:
                sc_est = f32(HOLD_LOOP_ESTIMATE * loop_sec)

        if sc_est is not None:
            est_sec = f32((0.0 if est_sec < 0.0 else est_sec) + sc_est)
        else:
            est_partial = True
        open_ended = open_ended or sc_open

        stages.append({
            "index": i,
            "name": st.get("name", ""),
            "tags": st.get("tags", []),
            "clipCount": len(files),
            "sig": "".join(f + "\n" for f in files),
            "loopSec": sec_or_null(loop_sec),
            "timerSec": f32(out_timer) if out_timer > 0.0 else None,
            "loops": out_loops if out_loops >= 0 else None,
            "openEnded": sc_open,
            "estSec": sec_or_null(sc_est),
        })

    species = ""
    for st in stages:
        for f in st["sig"].splitlines():
            species = species_from_path(f)
            if species:
                break
        if species:
            break

    unlisted = scene.get("unlisted", file_defaults["unlisted"])
    return {
        "id": sid,
        "title": scene.get("name") or sid,
        "pack": file_defaults["pack"],
        "sourceFile": file_defaults["sourceFile"],
        "species": species or "human",
        "tags": scene.get("tags", []),
        "actorCount": actor_count,
        "genders": ["any"] * actor_count,  # library packs author no roles -> anonymous any-gender slots
        "requiresFurniture": bool(anchor),
        "anchors": anchor_names,
        "unlisted": unlisted,
        "pinned": 0,  # the library lane never pins
        "stageCount": len(stages),
        "stages": stages,
        "estSec": sec_or_null(est_sec),
        "estPartial": est_partial,
        "openEnded": open_ended,
    }


def main():
    kw_raw = json.load(open(KEYWORD_MAP, encoding="utf-8"))
    kw_by_formid = {v["formid"].lower(): v["name"] for v in kw_raw.values()}

    cards = []
    packs = 0
    for path in sorted(glob.glob(os.path.join(PACK_DIR, "**", "*.osf.json"), recursive=True)):
        data = load_jsonc(path)
        if data.get("section") != "library":
            continue
        check_keys(data, FILE_KEYS, os.path.basename(path))
        packs += 1
        defaults = {
            "clipRoot": normalize_clip_root(data.get("clipRoot", "")),
            "unlisted": data.get("unlisted", False),
            "anchor": data.get("anchor"),
            "pack": data.get("pack", ""),
            "sourceFile": os.path.basename(path),
        }
        for scene in data["scenes"]:
            cards.append(build_card(scene, defaults, kw_by_formid))

    cards.sort(key=lambda c: (c["title"].lower(), c["id"]))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="") as f:
        json.dump(cards, f, ensure_ascii=False, separators=(",", ":"))

    n_stages = sum(c["stageCount"] for c in cards)
    n_anchored = sum(1 for c in cards if c["requiresFurniture"])
    species = sorted({c["species"] for c in cards})
    print(f"{packs} library pack(s) -> {len(cards)} card(s), {n_stages} stage(s), "
          f"{n_anchored} anchored, {len(species)} species -> {os.path.relpath(OUT, REPO)}")


if __name__ == "__main__":
    main()
