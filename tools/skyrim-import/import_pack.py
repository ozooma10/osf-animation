"""Run the experimental Skyrim human animation importer in background Blender."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_plan(plan):
    if (not isinstance(plan, dict) or plan.get("schema") != 1 or not isinstance(plan.get("id"), str)
            or not re.fullmatch(r"[A-Za-z0-9_.-]+", plan["id"])):
        raise ValueError("Plan needs schema:1 and a filesystem-safe scene id")
    if plan["id"] in (".", ".."):
        raise ValueError("Invalid scene id")
    roles = plan.get("roles", [])
    if (not isinstance(roles, list) or not 1 <= len(roles) <= 5
            or not isinstance(plan.get("stages"), list) or not plan["stages"]):
        raise ValueError("Plan needs 1-5 roles and at least one stage")
    if any(not isinstance(plan.get(k), str) or not plan[k].strip() for k in ("name", "pack")):
        raise ValueError("Plan needs nonempty name and pack strings")
    tags = plan.get("tags", [])
    if not isinstance(tags, list) or any(not isinstance(tag, str) for tag in tags):
        raise ValueError("Plan tags must be an array of strings")
    source = plan.get("source", {})
    if not isinstance(source, dict) or source.get("offset_units", "skyrim") != "skyrim":
        raise ValueError("Plan offsets must use Skyrim source units")
    if len(plan["stages"]) > 100:
        raise ValueError("Prototype supports at most 100 stages")
    role_names = set()
    for role in roles:
        if not isinstance(role, dict) or not isinstance(role.get("name"), str) or not role["name"].strip():
            raise ValueError("Each role needs a nonempty name")
        if role["name"] in role_names:
            raise ValueError("Role names must be unique")
        role_names.add(role["name"])
        if role.get("gender") not in ("male", "female", None):
            raise ValueError("Prototype supports human roles only")
        skeleton = Path(role.get("skeleton", ""))
        if not skeleton.is_file():
            raise ValueError(f"Source skeleton missing: {skeleton}")
        role["skeleton"] = str(skeleton.resolve())
    for stage in plan["stages"]:
        if (not isinstance(stage, dict) or not isinstance(stage.get("clips"), list)
                or len(stage["clips"]) != len(roles)):
            raise ValueError("Every stage needs exactly one HKX per role")
        for key in ("timer", "loops"):
            if key in stage:
                value = stage[key]
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                    raise ValueError(f"Invalid stage {key}")
                if key == "timer" and value == 0:
                    raise ValueError("Use loops:0 for a stage with no timer")
                if key == "loops" and value != int(value):
                    raise ValueError("Stage loops must be an integer")
        if "offsets" in stage:
            if not isinstance(stage["offsets"], list) or len(stage["offsets"]) != len(roles):
                raise ValueError("Stage offsets must align with roles")
            for offset in stage["offsets"]:
                if not isinstance(offset, dict) or set(offset) - {"x", "y", "z", "heading"}:
                    raise ValueError("Unknown offset fields")
                if not all(isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x) for x in offset.values()):
                    raise ValueError("Non-finite stage offsets")
        for i, clip in enumerate(stage["clips"]):
            path = Path(clip)
            if not path.is_file() or path.suffix.lower() != ".hkx":
                raise ValueError(f"Missing HKX clip: {path}")
            stage["clips"][i] = str(path.resolve())
    return plan


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    inputs = p.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--manifest", type=Path, help="Explicit schema-1 conversion plan")
    inputs.add_argument("--slal-json", type=Path, help="Generated SLAL JSON (source scripts are never executed)")
    p.add_argument("--pack-root", type=Path, help="Installed SLAL pack folder with meshes and FNIS lists")
    p.add_argument("--scene", help="SLAL animation id")
    p.add_argument("--list", action="store_true", help="List SLAL metadata candidates and unsupported scenes without converting")
    p.add_argument("--skeleton", type=Path, help="Default source skeleton HKX")
    p.add_argument("--female-skeleton", type=Path, help="Optional female source skeleton")
    p.add_argument("--target-rig", type=Path)
    p.add_argument("--pynifly", type=Path, help="Existing io_scene_nifly folder; read-only dependency")
    p.add_argument("--blender", type=Path)
    p.add_argument("--output", type=Path, help="New output directory; existing directories are never overwritten")
    p.add_argument("--native-validator", type=Path, help="Existing osf-import-test.exe to decode every output GLB")
    p.add_argument("--save-blend", action="store_true", help="Save target skeleton playback for the middle stage")
    return p


def run(args):
    if args.slal_json:
        from slal import load_scenes, list_scene_summaries
        if args.list:
            for scene in list_scene_summaries(args.slal_json):
                print(f"{scene['id']}: {scene['name']} ({scene['roles']} roles, {scene['stages']} stages) "
                      f"[{scene['status']}] {scene['reason']}")
            return 0
        if not args.pack_root or not args.scene:
            raise ValueError("Conversion requires --pack-root and one --scene from --list")
        scenes = load_scenes(args.slal_json, args.pack_root, scene_id=args.scene)
        selected = [s for s in scenes if s["id"] == args.scene]
        if len(selected) != 1:
            raise ValueError("Choose exactly one supported --scene from --list")
        plan = selected[0]
    else:
        if args.list:
            raise ValueError("--list applies to --slal-json")
        plan = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    if not isinstance(plan, dict) or not isinstance(plan.get("roles"), list):
        raise ValueError("Plan needs an object with a roles array")
    for role in plan["roles"]:
        if not isinstance(role, dict):
            raise ValueError("Each role must be an object")
        override = args.female_skeleton if role.get("gender") == "female" and args.female_skeleton else args.skeleton
        if override:
            role["skeleton"] = str(override.resolve())
    plan = validate_plan(plan)
    for key in ("blender", "target_rig"):
        path = getattr(args, key)
        if not path or not path.is_file():
            raise ValueError(f"Supply an existing --{key.replace('_', '-')} file")
    if not args.pynifly or not (args.pynifly / "hkx" / "anim_skyrim.py").is_file():
        raise ValueError("--pynifly must point to an existing io_scene_nifly installation")
    if not args.output:
        raise ValueError("Supply --output")
    output = args.output.resolve()
    if output.exists():
        raise ValueError(f"Output already exists; choose a new directory: {output}")
    if args.native_validator and not args.native_validator.is_file():
        raise ValueError("Native validator is missing")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.with_name(f".{output.name}.partial-{uuid.uuid4().hex[:8]}")
    staging.mkdir()
    job = {"plan": plan, "target_rig": str(args.target_rig.resolve()),
           "pynifly": str(args.pynifly.resolve()), "output": str(staging), "save_blend": args.save_blend}
    job_path = staging / "job.json"
    job_path.write_text(json.dumps(job, indent=2), encoding="utf-8")
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", TEMP=str(staging), TMP=str(staging))
    command = [str(args.blender.resolve()), "--background", "--factory-startup", "--disable-autoexec",
               "--python-exit-code", "1", "--python", str(HERE / "worker.py"), "--", str(job_path)]
    print(f"Converting {plan['name']} in background Blender...", flush=True)
    try:
        with (staging / "blender.log").open("w", encoding="utf-8") as log:
            process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=900)
        if process.returncode:
            raise RuntimeError(f"Blender exited with {process.returncode}; see {staging / 'blender.log'}")
        report_path = staging / "report.json"
        report = json.loads(report_path.read_text(encoding="utf-8"))
        from target_rig import read_rig, validate_glb
        rig = read_rig(args.target_rig)
        native = []
        for clip in sorted((staging / "Data").rglob("*.glb")):
            validate_glb(clip, rig)
            if args.native_validator:
                result = subprocess.run([str(args.native_validator.resolve()), str(clip)], capture_output=True,
                                        text=True, timeout=60)
                native.append({"clip": str(clip.relative_to(staging)), "exit_code": result.returncode,
                               "output": result.stdout + result.stderr})
                if result.returncode:
                    raise RuntimeError(f"OSF native importer rejected {clip}: {result.stdout} {result.stderr}")
        report["native_validation"] = native
        if args.native_validator:
            report["native_validator"] = {"path": str(args.native_validator.resolve()),
                                          "sha256": sha256(args.native_validator)}
        report["validation"] = {"glb_structure": "passed", "native_decode_and_sampling": "passed" if native else "not_run",
                                "scene_registry": "not_run", "in_game": "not_run", "visual_contact_quality": "not_reviewed"}
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        # Input plan is reviewable; temporary worker path has no place in the finished package.
        job_path.unlink()
        (staging / "input-plan.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
        staging.rename(output)
    except Exception:
        print(f"Incomplete output retained for diagnosis: {staging}", file=sys.stderr)
        raise
    print(f"Created {output}")
    print("Structure/decoding checks completed. In-game and body-contact review remain pending.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run(parser().parse_args()))
    except (ValueError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Import failed: {error}", file=sys.stderr)
        raise SystemExit(1)
