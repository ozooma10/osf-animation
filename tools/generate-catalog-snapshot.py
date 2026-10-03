#!/usr/bin/env python3
"""Generate the dev view's main-catalog snapshot (BuildCatalog(a_library=false)).

Sources: this repo's dist/OSF, the sibling `OSF Compatibility Packs` repo's */OSF
outputs, and any extra directories passed as arguments. Library-section packs
belong to generate-library-snapshot.py.

Writes fixtures/live/catalog.local.json: git-ignored (compat packs are adult
content), and preferred by the Vite dev server over the committed catalog.json.

Tolerant: scenes the offline model can't represent are warned about and skipped;
runtime-only fields come out null/raw.

Usage: python tools/generate-catalog-snapshot.py [extra-pack-dir ...]
"""

import glob
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT = os.path.join(REPO, "ui", "animation-browser", "fixtures", "live", "catalog.local.json")

# Reuse the library generator's card math; its dashed filename needs importlib.
_spec = importlib.util.spec_from_file_location(
    "libsnap", os.path.join(HERE, "generate-library-snapshot.py"))
libsnap = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(libsnap)

# Scene keys the shared build_card rejects but main-lane packs legitimately
# carry — stripped before the call, folded back into the card afterwards.
SCENE_SIDE_KEYS = {"roles", "stripActors", "lockPlayer", "fade", "camera",
                   "inPlace", "priority", "weight", "playerControl", "equip"}


def default_sources():
    dirs = [os.path.join(REPO, "dist", "OSF")]
    compat = os.path.join(os.path.dirname(REPO), "OSF Compatibility Packs")
    if os.path.isdir(compat):
        dirs.extend(sorted(glob.glob(os.path.join(compat, "*", "OSF"))))
    return dirs


def build_file(path, kw_by_formid, cards, seen):
    data = libsnap.load_jsonc(path)
    if not isinstance(data, dict) or "scenes" not in data:
        print(f"warn: {os.path.basename(path)}: bare single-scene file not modeled — skipped")
        return 0
    if data.get("schema") != 1:
        print(f"warn: {os.path.basename(path)}: schema != 1 — skipped")
        return 0
    if data.get("section") == "library":
        return 0  # the library lane belongs to generate-library-snapshot.py

    defaults = {
        "clipRoot": libsnap.normalize_clip_root(data.get("clipRoot", "")),
        "unlisted": data.get("unlisted", False),
        "anchor": data.get("anchor"),
        "pack": data.get("pack", ""),
        "sourceFile": os.path.basename(path),
    }
    pack_roles = data.get("roles")

    added = 0
    for scene in data["scenes"]:
        sid = scene.get("id", "?")
        side = {k: scene.pop(k) for k in list(scene) if k in SCENE_SIDE_KEYS}
        try:
            card = libsnap.build_card(scene, defaults, kw_by_formid)
        except (SystemExit, Exception) as e:  # noqa: BLE001 — tolerant dev tool
            print(f"warn: scene '{sid}' in {os.path.basename(path)}: {e} — skipped")
            continue
        roles = side.get("roles") or pack_roles
        if roles:
            card["actorCount"] = len(roles)
            card["roles"] = [
                {
                    "name": r.get("name", "") if isinstance(r, dict) else "",
                    "gender": (
                        r.get("gender", r.get("filters", {}).get("gender", "any"))
                        if isinstance(r, dict) else "any"
                    ),
                }
                for r in roles
            ]
        key = card["id"].lower()
        if key in seen:
            print(f"warn: duplicate scene id '{card['id']}' in {os.path.basename(path)} — keeping the first")
            continue
        seen.add(key)
        cards.append(card)
        added += 1
    return added


def main():
    kw_raw = json.load(open(libsnap.KEYWORD_MAP, encoding="utf-8"))
    kw_by_formid = {v["formid"].lower(): v["name"] for v in kw_raw.values()}

    roots = default_sources() + sys.argv[1:]
    cards, seen = [], set()
    files = 0
    for root in roots:
        if not os.path.isdir(root):
            print(f"warn: {root} not found — skipped")
            continue
        for path in sorted(glob.glob(os.path.join(root, "**", "*.osf.json"), recursive=True)):
            n = build_file(path, kw_by_formid, cards, seen)
            if n:
                files += 1

    cards.sort(key=lambda c: (c["title"].lower(), c["id"]))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="") as f:
        json.dump(cards, f, ensure_ascii=False, separators=(",", ":"))

    packs = sorted({c["pack"] or c["sourceFile"] for c in cards})
    print(f"{files} pack file(s) -> {len(cards)} card(s) in {len(packs)} group(s) -> {os.path.relpath(OUT, REPO)}")
    for p in packs:
        n = sum(1 for c in cards if (c["pack"] or c["sourceFile"]) == p)
        print(f"  {p}: {n}")


if __name__ == "__main__":
    main()
