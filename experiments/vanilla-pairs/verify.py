"""Check the audition artifact against installed assets; package only after success."""
from __future__ import annotations

import hashlib
import json
import math
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "build/vanilla-pairs-audition"
ARCHIVE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Data\Starfield - Animations.ba2")
PROBE = ROOT / "build/windows/x64/releasedbg/osf-af-import-test.exe"
sys.path.insert(0, str(ROOT / "tools/vanilla-packs"))
from generate_vanilla_packs import Ba2Reader, parse_af_header
from placements import SOURCES, read_installed_markers, rebase


def check(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    check(PROBE.is_file(), "Build the existing native AF import probe before validation.")
    OUT.mkdir(parents=True, exist_ok=True)
    furniture = read_installed_markers(ARCHIVE.parent / "Starfield.esm")
    expected_placements = {}
    for family, source in SOURCES.items():
        installed = furniture[source["form_id"]]
        check(installed["editor_id"] == source["editor_id"], "Wrong furniture source: " + family)
        for role, authored in source["markers"].items():
            check(all(abs(a - b) < 1e-5 for a, b in zip(authored, installed["markers"][role])),
                  "Original marker changed: " + family + "/" + role)
        expected_placements[family] = rebase(installed["markers"], source["roles"])
    check(furniture[SOURCES["survivor-victim"]["form_id"]]["markers"] ==
          furniture[SOURCES["survivor-victim"]["equivalent_form_id"]]["markers"],
          "Male/female victim furniture placements differ.")
    archive = Ba2Reader(ARCHIVE)
    files = sorted((HERE / "Data/OSF/Auditions/VanillaPairs").glob("*.osf.json"))
    check(len(files) == 3, "Expected exactly three audition content files.")
    scenes, clips, stage_count = {}, {}, 0
    try:
        for file in files:
            pack = json.loads(file.read_text(encoding="utf-8"))
            check(pack["schema"] == 1 and pack["pack"] == "OSF Vanilla Pairs - Audition", str(file))
            check(pack["unlisted"] is True and pack["hideApparel"] is False, "Audition policies changed.")
            check(pack["clearHeldItems"] is True and pack["playerInputLock"] is True, "Ownership policies changed.")
            check(pack["placement"] == "anchorAndPin" and pack["camera"] == "scene_orbit", "Placement/camera mismatch.")
            check(pack["fade"] is False, "Audition start should remain visible.")
            check(set(pack) == {"schema", "name", "pack", "folder", "tags", "unlisted", "hideApparel", "clearHeldItems", "playerInputLock", "placement", "camera", "fade", "scenes"}, "Unexpected pack field.")
            for scene in pack["scenes"]:
                sid = scene["id"]
                check(sid.startswith("osf.audition.vanilla-pairs.") and "#" not in sid and sid not in scenes, sid)
                check(set(scene) == {"id", "name", "roles", "stages"}, "Unexpected scene field.")
                check(len(scene["roles"]) == 2 and len({r["name"] for r in scene["roles"]}) == 2, sid)
                family = sid.removeprefix("osf.audition.vanilla-pairs.").split(".", 1)[0]
                check([r["name"] for r in scene["roles"]] == SOURCES[family]["roles"], "Wrong marker/cast order: " + sid)
                for role, expected in zip(scene["roles"], expected_placements[family]):
                    check(role["gender"] == "any" and set(role["offset"]) == set(expected), sid)
                    check(all(type(role["offset"][key]) in (int, float) and math.isfinite(role["offset"][key]) and
                              abs(role["offset"][key] - value) < 1e-5 for key, value in expected.items()),
                          "Placement differs from the original furniture marker: " + sid)
                check(bool(scene["stages"]), sid)
                scenes[sid] = {"name": scene["name"], "roles": [r["name"] for r in scene["roles"]], "stages": len(scene["stages"])}
                for stage in scene["stages"]:
                    stage_count += 1
                    check(set(stage) <= {"name", "clips", "hold", "loops"}, "Unexpected stage field.")
                    check(len(stage["clips"]) == 2, "Stage must contain two clips.")
                    if "hold" in stage:
                        check("loops" not in stage, "Frozen poses cannot loop.")
                        check(type(stage["hold"]) in [int, float] and math.isfinite(stage["hold"]) and 0 <= stage["hold"] <= 1, "Invalid freeze position.")
                    if "loops" in stage:
                        check(type(stage["loops"]) is int and stage["loops"] == 0, "Only inspection idles should loop.")
                    counts = []
                    for clip in stage["clips"]:
                        check(set(clip) == {"file", "sec"}, "Unexpected clip field.")
                        path = clip["file"]
                        check(path.startswith("meshes/actors/human/animations/") and ".." not in Path(path).parts, path)
                        key = path.replace("/", "\\").lower()
                        check(key in archive.by_name, "Missing base-game clip: " + path)
                        entry = archive.by_name[key]
                        raw = archive.read(entry)
                        bones, frames = parse_af_header(raw[:64])
                        counts.append(frames)
                        seconds = (frames - 1) / 30
                        check(bones == 82 and abs(clip["sec"] - seconds) < 1e-6, "Wrong bone count/duration: " + path)
                        clips[path] = {"frames": frames, "seconds": seconds, "sha256": hashlib.sha256(raw).hexdigest()}
                        target = OUT / "source-clips" / path
                        target.parent.mkdir(parents=True, exist_ok=True)
                        if not target.exists() or digest(target) != clips[path]["sha256"]:
                            target.write_bytes(raw)
                    check(counts[0] == counts[1], "Actor source lengths differ: " + sid)
        check(len(scenes) == 10, "Expected ten audition scenes.")
        rig = OUT / "skeleton.rig"
        rig.write_bytes(archive.read(archive.by_name[r"meshes\actors\human\characterassets\skeleton.rig"]))
    finally:
        archive.f.close()

    for path, details in clips.items():
        run = subprocess.run([str(PROBE), "--json", str(OUT / "source-clips" / path), str(rig)], capture_output=True, text=True)
        check(run.returncode == 0, "Native import failed: " + path + "\n" + run.stdout + run.stderr)
        result = json.loads(run.stdout)
        check(result["joints"] == 97 and len(result["samples"]) == 5, "Unexpected probe result: " + path)
        check(abs(result["duration"] - details["seconds"]) < 1e-4, "Decoded duration mismatch: " + path)
        check(all(math.isfinite(value) for sample in result["samples"] for matrix in sample["locals"] for value in matrix), "Nonfinite pose: " + path)
        details["native_import_and_five_samples"] = "passed"

    package = OUT / "OSF Vanilla Pairs - Audition.zip"
    payloads = {f.relative_to(HERE / "Data").as_posix(): digest(f) for f in files}
    with zipfile.ZipFile(package, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for file in files:
            z.write(file, file.relative_to(HERE / "Data").as_posix())
        z.write(HERE / "README.md", "README.md")
    with zipfile.ZipFile(package) as z:
        check(z.testzip() is None and len(z.namelist()) == 4, "Invalid ZIP.")
        for relative, expected in payloads.items():
            check(hashlib.sha256(z.read(relative)).hexdigest() == expected, "ZIP payload mismatch.")
    report = {
        "status": "passed",
        "scope": "Structural pack checks, installed FURN marker placements, archive paths/frame counts, and offline native decoding/sampling. Production registry/game acceptance not run.",
        "furniture_sources": {f"{fid:08X}": value for fid, value in furniture.items()},
        "role_placements": expected_placements,
        "scene_count": len(scenes), "stage_count": stage_count, "unique_clips": len(clips),
        "archive": str(ARCHIVE), "archive_sha256": digest(ARCHIVE),
        "probe": str(PROBE), "probe_sha256": digest(PROBE),
        "payload_sha256": payloads, "scenes": scenes, "clips": clips,
        "zip": str(package), "zip_sha256": digest(package),
    }
    (OUT / "validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(scenes)} scenes, {stage_count} stages, {len(clips)} source clips; native import and 5 samples each.")
    print(package)


if __name__ == "__main__":
    main()
