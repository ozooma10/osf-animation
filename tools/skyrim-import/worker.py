"""Background Blender worker. Invoked by import_pack.py, never by game runtime."""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bpy
from glb_writer import write_glb
from target_rig import read_rig, validate_glb
from retarget import HumanoidRetargeter, matrix, model_matrices


def fingerprint(path):
    path = Path(path)
    return {"path": str(path.resolve()), "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def save_preview(path, rig, roles, samples, times, placements):
    """Save an editable target-skeleton preview, without launching the UI."""
    # Background conversion needs no desktop thumbnail cache writes.
    bpy.context.preferences.filepaths.file_preview_type = "NONE"
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    rest_locals = [matrix(b.translation, b.rotation_xyzw) for b in rig.bones]
    parents = [b.parent for b in rig.bones]
    rest_models = model_matrices(rest_locals, parents)
    bpy.context.scene.render.fps = 30
    for actor, (role, frames) in enumerate(zip(roles, samples)):
        armature = bpy.data.armatures.new(role["name"])
        obj = bpy.data.objects.new(role["name"], armature)
        bpy.context.collection.objects.link(obj)
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        obj.show_in_front = True
        placement = placements[actor]
        obj.location = [placement.get(k, 0) for k in ("x", "y", "z")]
        obj.rotation_euler.z = math.radians(placement.get("heading", 0))
        bpy.ops.object.mode_set(mode="EDIT")
        for i, bone in enumerate(rig.bones):
            edit = armature.edit_bones.new(bone.name)
            edit.matrix = rest_models[i]
            edit.length = .06
        for bone in rig.bones:
            if bone.parent >= 0:
                armature.edit_bones[bone.name].parent = armature.edit_bones[rig.bones[bone.parent].name]
        bpy.ops.object.mode_set(mode="POSE")
        # Blender's bone Y-axis convention is held in its own actual rest matrices.
        for frame_id, transforms in enumerate(frames):
            models = model_matrices([matrix(t["translation"], t["rotation"]) for t in transforms], parents)
            for i, bone in enumerate(rig.bones):
                pb = obj.pose.bones[bone.name]
                kwargs = {}
                if bone.parent >= 0:
                    kwargs = {"parent_matrix": models[bone.parent],
                              "parent_matrix_local": armature.bones[rig.bones[bone.parent].name].matrix_local}
                pb.matrix_basis = pb.bone.convert_local_to_pose(models[i], pb.bone.matrix_local, invert=True, **kwargs)
                pb.rotation_mode = "QUATERNION"
                time = 1 + times[frame_id] * 30
                pb.keyframe_insert(data_path="location", frame=time, group=bone.name)
                pb.keyframe_insert(data_path="rotation_quaternion", frame=time, group=bone.name)
        # keyframe_insert does not honor the new-key interpolation preference
        # in the tested Blender version. Set the saved layered-action keys.
        for layer in obj.animation_data.action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    for curve in bag.fcurves:
                        for key in curve.keyframe_points:
                            key.interpolation = "LINEAR"
        bpy.ops.object.mode_set(mode="OBJECT")
        obj.select_set(False)
    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = math.ceil(1 + times[-1] * 30)
    bpy.context.scene.frame_set(1)
    bpy.ops.wm.save_as_mainfile(filepath=str(path))


def run(job):
    output = Path(job["output"])
    plan = job["plan"]
    decoder_dir = Path(job["pynifly"]) / "hkx"
    from decoder import load_decoder
    decoder = load_decoder(Path(job["pynifly"]))
    load_skyrim_animation = decoder.load_skyrim_animation
    load_skyrim_skeleton = decoder.load_skyrim_skeleton
    rig = read_rig(job["target_rig"])
    skeletons = [load_skyrim_skeleton(r["skeleton"]) for r in plan["roles"]]
    converters = [HumanoidRetargeter(skeletons[0], rig)]
    converters += [HumanoidRetargeter(s, rig, converters[0].unit_scale) for s in skeletons[1:]]
    bones = [{"name": b.name, "parent": b.parent, "translation": list(b.translation),
              "rotation": list(b.rotation_xyzw)} for b in rig.bones]
    resource_root = Path("OSF") / "Imported" / plan["id"]
    clips_dir = output / "Data" / resource_root
    clips_dir.mkdir(parents=True)
    mapped = set.intersection(*(set(c.mapped_names) for c in converters))
    # A mapped child's local rotation assumes its whole parent chain uses our
    # baked pose. In particular the unmapped Cup bones parent mapped Pinkies.
    driven = set(mapped)
    for i, bone in enumerate(rig.bones):
        if bone.name in mapped:
            parent = bone.parent
            while parent >= 0:
                driven.add(rig.bones[parent].name)
                parent = rig.bones[parent].parent
    preserve = [b.name for b in rig.bones if b.name not in driven]
    scene = {"schema": 1, "id": plan["id"], "name": plan["name"], "pack": plan["pack"],
             "tags": list(dict.fromkeys(["imported", "prototype", *plan.get("tags", [])])), "hideApparel": False,
             "roles": [{"name": r["name"], **({"gender": r["gender"]} if r.get("gender") else {}),
                        "preserveBones": preserve} for r in plan["roles"]], "stages": []}
    report = {"schema": 1, "status": "prototype", "profile": "skyrim-human-to-starfield-v1",
              "blender": bpy.app.version_string, "source": plan.get("source", {}),
              "hkx_block_policy": decoder.osf_block_policy,
              "inputs": {"target_rig": fingerprint(job["target_rig"]),
                         "skeletons": [fingerprint(r["skeleton"]) for r in plan["roles"]],
                         "decoder": [fingerprint(decoder_dir / f) for f in ("anim_skyrim.py", "anim_fo4.py")],
                         "converter": [fingerprint(HERE / f) for f in
                                       ("import_pack.py", "worker.py", "retarget.py", "decoder.py", "glb_writer.py", "target_rig.py", "slal.py")]},
              "mapped_target_bones": sorted(mapped), "preserved_target_bones": preserve,
              "source_to_target_scale": converters[0].unit_scale,
              "source_to_target_basis": [list(row) for row in converters[0].source_to_target],
              "limitations": ["No contact/IK correction", "No facial, physics, sound or prop conversion",
                              "Only unit-scale source clips are accepted; target bone lengths remain fixed",
                              "No external HKX extracted-motion conversion", "Only this humanoid skeleton profile is supported"],
              "clips": [], "warnings": [*plan.get("warnings", []), *plan.get("source", {}).get("warnings", [])]}
    preview_stage = len(plan["stages"]) // 2
    for stage_id, stage in enumerate(plan["stages"]):
        animations = [load_skyrim_animation(p) for p in stage["clips"]]
        first = animations[0]
        if any(a.num_frames != first.num_frames or abs(a.duration - first.duration) > 1e-4 for a in animations):
            raise ValueError(f"Stage {stage_id + 1}: participant clip lengths differ; automatic resampling is not implemented")
        if first.num_frames > 18000 or first.duration > 600:
            raise ValueError("Clip exceeds prototype duration/frame limit")
        bindings = [c.binding(a) for c, a in zip(converters, animations)]
        times = [i * first.duration / (first.num_frames - 1) for i in range(first.num_frames)]
        stage_out = {k: stage[k] for k in ("timer", "loops") if k in stage}
        stage_out["clips"] = []
        stage_samples = []
        placements = []
        for actor_id, (anim, converter, source_path) in enumerate(zip(animations, converters, stage["clips"])):
            binding = bindings[actor_id]
            samples = [converter.convert_frame(anim, binding, frame) for frame in range(anim.num_frames)]
            filename = f"stage{stage_id + 1:02d}_actor{actor_id + 1:02d}.glb"
            clip_path = clips_dir / filename
            write_glb(clip_path, bones, times, samples, f"stage{stage_id + 1}_actor{actor_id + 1}")
            validation = validate_glb(clip_path, rig)
            resource = (resource_root / filename).as_posix()
            offsets = stage.get("offsets", [{} for _ in plan["roles"]])[actor_id]
            if offsets:
                offset = converter.convert_offset(offsets)
                stage_out["clips"].append({"file": resource, "offset": offset})
                placements.append(offset)
            else:
                stage_out["clips"].append(resource)
                placements.append({})
            report["clips"].append({"input": fingerprint(source_path), "output": resource,
                                    "sha256": hashlib.sha256(clip_path.read_bytes()).hexdigest(),
                                    "validation": validation, "source_frames": anim.num_frames,
                                    "binding_mode": "explicit" if anim.track_to_bone_indices else
                                                    "named" if any(anim.bone_names) else "verified-vanilla-human-prefix",
                                    "source_scale_range": [min(v for t in anim.tracks for s in t.scales for v in s),
                                                           max(v for t in anim.tracks for s in t.scales for v in s)],
                                    "source_annotations": [{"time": a.time, "text": a.text} for a in anim.annotations]})
            if job["save_blend"] and stage_id == preview_stage:
                stage_samples.append(samples)
            print(f"Converted stage {stage_id + 1}, actor {actor_id + 1}: {anim.num_frames} frames", flush=True)
        scene["stages"].append(stage_out)
        if stage_samples:
            save_preview(output / "preview.blend", rig, plan["roles"], stage_samples, times, placements)
    (output / "Data" / "OSF" / f"{plan['id']}.osf.json").write_text(json.dumps(scene, indent=2), encoding="utf-8")
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    arguments = sys.argv[sys.argv.index("--") + 1:]
    if len(arguments) != 1:
        raise ValueError("Worker expects a single job.json path")
    run(json.loads(Path(arguments[0]).read_text(encoding="utf-8")))
