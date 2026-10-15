"""Compare a saved Blender preview against serialized GLB keys and midpoints.

This is a skeleton transform check, not a visual/body-contact acceptance test.
Run inside Blender; scene and clip files are read from the private output folder.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import struct

import bpy
from mathutils import Matrix, Quaternion, Vector

from target_rig import validate_glb


def _clip(path):
    validate_glb(path)
    raw = path.read_bytes()
    json_size = struct.unpack_from("<I", raw, 12)[0]
    doc = json.loads(raw[20:20 + json_size])
    binary = raw[28 + json_size:]
    if len(doc["animations"]) != 1:
        raise ValueError("Preview audit expects one animation per clip")
    def values(index):
        accessor = doc["accessors"][index]
        view = doc["bufferViews"][accessor["bufferView"]]
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[accessor["type"]]
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        stride = view.get("byteStride", width * 4)
        return [struct.unpack_from(f"<{width}f", binary, start + i * stride) for i in range(accessor["count"])]
    animation = doc["animations"][0]
    times = None
    channels = {}
    for channel in animation["channels"]:
        sampler = animation["samplers"][channel["sampler"]]
        grid = [v[0] for v in values(sampler["input"])]
        if times is not None and grid != times:
            raise ValueError("Preview audit expects a shared time grid")
        times = grid
        channels[channel["target"]["node"], channel["target"]["path"]] = values(sampler["output"])
    parents = [-1] * len(doc["nodes"])
    for index, node in enumerate(doc["nodes"]):
        for child in node.get("children", []):
            parents[child] = index
    return doc["nodes"], parents, times, channels


def _models(nodes, parents, channels, frame, fraction):
    local = []
    for index, node in enumerate(nodes):
        pose = {}
        for field, default in (("translation", (0, 0, 0)), ("rotation", (0, 0, 0, 1)), ("scale", (1, 1, 1))):
            track = channels.get((index, field))
            a = track[frame] if track else node.get(field, default)
            b = track[frame + 1] if track and fraction else a
            if field == "rotation":
                qa, qb = Quaternion((a[3], *a[:3])), Quaternion((b[3], *b[:3]))
                pose[field] = qa.slerp(qb, fraction)
            else:
                pose[field] = Vector(a).lerp(Vector(b), fraction)
        local.append(Matrix.LocRotScale(pose["translation"], pose["rotation"], pose["scale"]))
    models = {}
    def resolve(index):
        if index not in models:
            models[index] = local[index] if parents[index] == -1 else resolve(parents[index]) @ local[index]
        return models[index]
    return [resolve(i) for i in range(len(nodes))]


def audit_preview(path, scene, data_root, stage_index, tolerance=2e-4):
    """Reopen the file, verify LINEAR curves, then compare placed bone matrices."""
    path, data_root = Path(path), Path(data_root).resolve()
    bpy.ops.wm.open_mainfile(filepath=str(path), load_ui=False, use_scripts=False)
    clips = scene["stages"][stage_index]["clips"]
    if len(clips) != len(scene["roles"]):
        raise ValueError("Preview audit role/clip count mismatch")
    report = {"schema": 1, "stage": stage_index + 1, "status": "passed", "actors": [],
              "preview_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
              "matrix_element_tolerance": tolerance,
              "scope": "Saved skeleton transforms at every GLB key and interval midpoint; no body/contact or in-game review"}
    for role, clip in zip(scene["roles"], clips):
        resource = clip if isinstance(clip, str) else clip["file"]
        source = (data_root / resource).resolve()
        if not source.is_relative_to(data_root):
            raise ValueError("Preview clip escapes output Data folder")
        nodes, parents, times, channels = _clip(source)
        obj = bpy.data.objects.get(role["name"])
        if obj is None or obj.type != "ARMATURE" or not obj.animation_data or not obj.animation_data.action:
            raise ValueError(f"Missing animated preview role: {role['name']}")
        points = [key for layer in obj.animation_data.action.layers for strip in layer.strips
                  for bag in strip.channelbags for curve in bag.fcurves for key in curve.keyframe_points]
        if not points or any(key.interpolation != "LINEAR" for key in points):
            raise ValueError(f"Preview role {role['name']} has missing or non-LINEAR keys")
        if set(obj.pose.bones.keys()) != {node["name"] for node in nodes}:
            raise ValueError(f"Preview role {role['name']} bone inventory differs from GLB")
        offset = clip.get("offset", {}) if isinstance(clip, dict) else {}
        placement = Matrix.Translation(Vector([offset.get(k, 0) for k in ("x", "y", "z")])) @ Matrix.Rotation(
            math.radians(offset.get("heading", 0)), 4, "Z")
        maximum, count = 0., 0
        for index, time in enumerate(times):
            for fraction in ((0., .5) if index + 1 < len(times) else (0.,)):
                seconds = time + fraction * (times[index + 1] - time) if fraction else time
                frame = 1 + seconds * bpy.context.scene.render.fps / bpy.context.scene.render.fps_base
                bpy.context.scene.frame_set(math.floor(frame), subframe=frame - math.floor(frame))
                expected = _models(nodes, parents, channels, index, fraction)
                for node, model in zip(nodes, expected):
                    actual = obj.matrix_world @ obj.pose.bones[node["name"]].matrix
                    wanted = placement @ model
                    error = max(abs(actual[r][c] - wanted[r][c]) for r in range(4) for c in range(4))
                    if not math.isfinite(error) or error > tolerance:
                        raise ValueError(f"Preview mismatch: {role['name']} / {node['name']} at {seconds:.6f}s (error {error:.6g})")
                    maximum = max(maximum, error)
                    count += 1
        report["actors"].append({"role": role["name"], "clip": resource, "keys": len(times),
                                 "clip_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                 "bone_matrix_checks": count, "max_matrix_error": maximum})
    return report
