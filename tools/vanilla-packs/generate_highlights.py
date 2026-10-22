#!/usr/bin/env python3
"""Generate the curated 'Highlights' pack from the generated vanilla packs.

Applies highlights-manifest.json include rules (scene-id globs, stage-name regex, tag
filters, caps) to the vanilla-*.osf.json packs and emits one highlights.osf.json.
It has no `section` key, so it lands in the main catalog lane, not the library.

One output scene per source scene (anchors are per-scene); `--N` chunks fold back into
their source leaf, and `"fold": true` folds prefix sub-clusters into the glob's prefix.

Usage (from the repo root, after generate_vanilla_packs.py):
    python tools/vanilla-packs/generate_highlights.py
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from pathlib import Path

DEFAULT_EXCLUDE_TAGS = ["transition"]
VANILLA_PREFIX = "vanilla/"
CHUNK_RE = re.compile(r"--\d+$")


def load_vanilla_scenes(packs_dir: Path) -> tuple[dict[str, dict], str]:
    """id -> scene from the human vanilla-*.osf.json packs, plus their shared clipRoot."""
    scenes: dict[str, dict] = {}
    clip_root = None
    files = [f for f in sorted(packs_dir.glob("vanilla-*.osf.json"))
             if not f.name.startswith("vanilla-creature-")]
    if not files:
        sys.exit(f"no vanilla-*.osf.json under {packs_dir} — run generate_vanilla_packs.py first")
    for f in files:
        doc = json.loads(f.read_text(encoding="utf-8"))
        root = doc.get("clipRoot", "")
        if clip_root is None:
            clip_root = root
        elif clip_root != root:
            sys.exit(f"{f.name}: clipRoot {root!r} differs from {clip_root!r} — packs out of sync")
        for s in doc.get("nodes", []):
            scenes[s["id"]] = s
    return scenes, clip_root or ""


def stage_passes(stage: dict, exclude_tags: list[str], require_tags: list[str],
                 name_re: re.Pattern | None, exclude_names: list[str]) -> bool:
    tags = set(stage.get("tags", []))
    if tags & set(exclude_tags):
        return False
    if not set(require_tags) <= tags:
        return False
    name = stage.get("name", "")
    if name_re and not name_re.search(name):
        return False
    low = name.lower()
    return not any(x.lower() in low for x in exclude_names)


def short_slug(collection_id: str) -> str:
    """'highlights/bar-life' -> 'bar-life'."""
    return collection_id.rsplit("/", 1)[-1]


def build_collection(coll: dict, scenes: dict[str, dict],
                     warnings: list[str]) -> dict[str, dict]:
    """Returns output-scene-id -> scene draft {srcId, stages, ...} for one collection."""
    exclude_tags = coll.get("excludeTags", DEFAULT_EXCLUDE_TAGS)
    require_tags = coll.get("requireTags", [])
    exclude_names = coll.get("excludeNames", [])
    slug = short_slug(coll["id"])

    out: dict[str, dict] = {}
    seen_files: set[str] = set()  # dedupe clips across the collection

    for rule in coll["include"]:
        name_re = re.compile(rule["namePattern"], re.I) if "namePattern" in rule else None
        cap = rule.get("max")
        r_exclude_tags = rule.get("excludeTags", exclude_tags)
        r_require_tags = rule.get("requireTags", require_tags)
        r_exclude_names = rule.get("excludeNames", exclude_names)
        taken = 0
        matched_any = False
        for glob in rule["scenes"]:
            fold_base = re.split(r"[*?\[]", glob, 1)[0].rstrip("/") if rule.get("fold") else None
            for src_id in sorted(scenes):
                if not fnmatch.fnmatchcase(src_id, glob):
                    continue
                matched_any = True
                src = scenes[src_id]
                for stage in src["stages"]:
                    if cap is not None and taken >= cap:
                        break
                    if not stage_passes(stage, r_exclude_tags, r_require_tags, name_re, r_exclude_names):
                        continue
                    file_key = stage["clips"][0]["file"].lower()
                    if file_key in seen_files:
                        continue
                    seen_files.add(file_key)
                    taken += 1

                    base_id = CHUNK_RE.sub("", src_id)
                    src_name = CHUNK_RE.sub("", re.sub(r"\s*\(\d+\)$", "", src.get("name", base_id)))
                    if fold_base and base_id.startswith(fold_base + "/"):
                        extra = base_id[len(fold_base):].count("/")
                        parts = src_name.split(" / ")
                        if extra < len(parts):
                            src_name = " / ".join(parts[:-extra])
                        base_id = fold_base
                    rel = base_id[len(VANILLA_PREFIX):] if base_id.startswith(VANILLA_PREFIX) else base_id
                    out_id = f"{coll['id']}/{rel}"
                    dst = out.setdefault(out_id, {
                        "srcId": base_id,
                        "srcName": src_name,
                        "srcTags": src.get("tags", []),
                        "anchor": src.get("anchor"),
                        "stages": [],
                    })
                    if dst["anchor"] != src.get("anchor"):
                        warnings.append(f"{out_id}: chunk anchors differ — kept the first")
                    dst["stages"].append(stage)
            if cap is not None and taken >= cap:
                break
        if not matched_any:
            warnings.append(f"{coll['id']}: no scenes match {rule['scenes']} — curation drift?")
        elif taken == 0:
            warnings.append(f"{coll['id']}: rule {rule['scenes']} matched scenes but selected 0 stages")

    result: dict[str, dict] = {}
    for out_id, d in out.items():
        src_disp = d["srcName"]
        src_disp = src_disp[len("Vanilla · "):] if src_disp.startswith("Vanilla · ") else src_disp
        # Drop the top folder segment; the collection gives the context.
        if " / " in src_disp:
            src_disp = src_disp.split(" / ", 1)[1]
        tags = ["highlights", slug]
        for t in ("female", "anchored"):
            if t in d["srcTags"]:
                tags.append(t)
        scene = {"id": out_id, "name": f"{coll['name']} — {src_disp}", "tags": tags}
        if d["anchor"]:
            scene["anchor"] = d["anchor"]
        scene["stages"] = d["stages"]
        result[out_id] = scene
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    here = Path(__file__).resolve()
    ap.add_argument("--manifest", type=Path, default=here.parent / "highlights-manifest.json")
    ap.add_argument("--packs-dir", type=Path, default=here.parents[2] / "dist" / "OSF" / "vanilla")
    ap.add_argument("--out", type=Path, default=None,
                    help="output file (default: <packs-dir>/highlights.osf.json)")
    args = ap.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    scenes, clip_root = load_vanilla_scenes(args.packs_dir)

    warnings: list[str] = []
    doc = {
        "schema": 2,
        "name": manifest.get("packName", "OSF Highlights") + " (generated, do not hand-edit)",
        # Browsing shelf only: keep it out of the matchmaker pool.
        "unlisted": True,
        # Outside the library section, so the library's no-strip default isn't inherited.
        "hideApparel": False,
        "clipRoot": clip_root,
        "nodes": [],
    }
    total = 0
    for coll in manifest["collections"]:
        built = build_collection(coll, scenes, warnings)
        n = sum(len(s["stages"]) for s in built.values())
        total += n
        doc["nodes"].extend(built[k] for k in sorted(built))
        print(f"{coll['id']}: {len(built)} scene(s), {n} clip(s)")

    out_file = args.out or (args.packs_dir / "highlights.osf.json")
    with open(out_file, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"\n{out_file.name}: {len(doc['nodes'])} scene(s), {total} clip(s) total")
    for w in warnings:
        print("WARNING:", w)
    return 0


if __name__ == "__main__":
    sys.exit(main())
