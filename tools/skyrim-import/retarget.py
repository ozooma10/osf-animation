"""Conservative humanoid retargeting, executed in Blender's Python runtime.

Model-space rest-relative rotations cross differing bone hierarchies. Target
limb lengths stay fixed; pelvis translation carries source scene placement.
This initial profile does not solve contacts or reconstruct physics/expressions.
"""
from __future__ import annotations

import math
import hashlib
import json
from mathutils import Matrix, Quaternion, Vector

# Ordered names + parent indices from vanilla Skyrim SE male/female animation
# skeletons, including the two terminal camera nodes. This limits implicit
# 97-track legacy human clips to the verified layout, not arbitrary named rigs.
VANILLA_HUMAN_LAYOUT = "f7555999b3f0e5b64cdd93057b6c0620a09eb8a0926f00bef438603aea4759c6"


def quat(xyzw):
    if len(xyzw) != 4 or not all(math.isfinite(x) for x in xyzw):
        raise ValueError("Invalid quaternion")
    q = Quaternion((xyzw[3], *xyzw[:3]))
    if q.magnitude < 1e-8:
        raise ValueError("Zero quaternion")
    q.normalize()
    return q


def matrix(translation, rotation, scale=(1, 1, 1)):
    values = (*translation, *scale)
    if not all(math.isfinite(x) for x in values):
        raise ValueError("Non-finite transform")
    return Matrix.LocRotScale(Vector(translation), quat(rotation), Vector(scale))


def decompose(m):
    t, r, s = m.decompose()
    r.normalize()
    return {"translation": list(t), "rotation": [r.x, r.y, r.z, r.w], "scale": list(s)}


def model_matrices(locals_, parents):
    result = [None] * len(parents)
    pending = set(range(len(parents)))
    while pending:
        progressed = False
        for i in sorted(pending):
            p = parents[i]
            if p < -1 or p >= len(parents):
                raise ValueError("Skeleton parent index outside bone table")
            if p == -1 or result[p] is not None:
                result[i] = locals_[i] if p == -1 else result[p] @ locals_[i]
                pending.remove(i)
                progressed = True
        if not progressed:
            raise ValueError("Skeleton hierarchy contains a cycle")
    return result


def model_rotations(locals_, parents):
    """Compose rotations separately so source scale cannot introduce shear."""
    rotation_locals = [m.decompose()[1].to_matrix().to_4x4() for m in locals_]
    return [m.to_quaternion() for m in model_matrices(rotation_locals, parents)]


def profile_mapping(source_names, target_names):
    """Explicit vanilla Skyrim human to Starfield human v1 mapping."""
    mapping = {
        "COM": "NPC COM [COM ]", "C_Hips": "NPC Pelvis [Pelv]",
        "C_Spine": "NPC Spine [Spn0]", "C_Spine1": "NPC Spine1 [Spn1]",
        "C_Spine2": "NPC Spine2 [Spn2]", "C_Chest": "NPC Spine2 [Spn2]",
        "C_Neck": "NPC Neck [Neck]", "C_Neck1": "NPC Neck [Neck]",
        "C_Head": "NPC Head [Head]", "C_Waist": "NPC Pelvis [Pelv]",
    }
    for side in ("L", "R"):
        for target, source, code in (
            ("Thigh", "Thigh", "Thg"), ("Calf", "Calf", "Clf"),
            ("Foot", "Foot", "ft "), ("Toe", "Toe0", "Toe"),
            ("Clavicle", "Clavicle", "Clv"), ("Biceps", "UpperArm", "Uar"),
            ("Forearm", "Forearm", "Lar"), ("Wrist", "Hand", "Hnd"),
        ):
            mapping[f"{side}_{target}"] = f"NPC {side} {source} [{side}{code}]"
        for finger_id, finger in enumerate(("Thumb", "Index", "Middle", "Ring", "Pinky")):
            for joint in range(3):
                target = f"{side}_{finger}{joint if joint else ''}"
                mapping[target] = f"NPC {side} Finger{finger_id}{joint} [{side}F{finger_id}{joint}]"
    source_index = {name: i for i, name in enumerate(source_names)}
    required = ("NPC Pelvis [Pelv]", "NPC Head [Head]", "NPC L Thigh [LThg]",
                "NPC R Thigh [RThg]", "NPC L Hand [LHnd]", "NPC R Hand [RHnd]")
    missing = [n for n in required if n not in source_index]
    if missing:
        raise ValueError(f"Unsupported source humanoid skeleton: missing {missing}")
    return {i: source_index[mapping[name]] for i, name in enumerate(target_names)
            if name in mapping and mapping[name] in source_index}


class HumanoidRetargeter:
    def __init__(self, skeleton, rig, unit_scale=None):
        if not skeleton or not skeleton.bones:
            raise ValueError("Source HKX contains no skeleton bones")
        if len({n.casefold() for n in skeleton.bones}) != len(skeleton.bones):
            raise ValueError("Duplicate source bone names")
        if len(skeleton.parents) != len(skeleton.bones) or len(skeleton.reference_pose) != len(skeleton.bones):
            raise ValueError("Incomplete source skeleton")
        self.source = skeleton
        self.rig = rig
        self.source_rest_local = [matrix(p.translation, p.rotation, p.scale) for p in skeleton.reference_pose]
        self.source_rest = model_matrices(self.source_rest_local, skeleton.parents)
        self.source_rest_rotations = model_rotations(self.source_rest_local, skeleton.parents)
        self.target_rest_local = [matrix(b.translation, b.rotation_xyzw) for b in rig.bones]
        self.target_parents = [b.parent for b in rig.bones]
        self.target_rest = model_matrices(self.target_rest_local, self.target_parents)
        self.mapping = profile_mapping(skeleton.bones, [b.name for b in rig.bones])
        self.source_pelvis = skeleton.bones.index("NPC Pelvis [Pelv]")
        self.target_com = [b.name for b in rig.bones].index("COM")
        self.source_to_target = self._basis(self.target_rest, [b.name for b in rig.bones],
                                          "L_Thigh", "R_Thigh") @ self._basis(
            self.source_rest, skeleton.bones, "NPC L Thigh [LThg]", "NPC R Thigh [RThg]").transposed()
        # One common scale must be used across participants, preserving relative placement.
        self.unit_scale = unit_scale if unit_scale is not None else self.estimate_scale()
        if not math.isfinite(self.unit_scale) or not 0 < self.unit_scale < 100:
            raise ValueError("Invalid source-to-target unit scale")

    @staticmethod
    def _basis(models, names, left, right):
        pos = lambda n: models[names.index(n)].translation
        # Both supported game profiles use world +Z as up. Head/pelvis bind
        # positions include posture lean, which must not pitch scene offsets or
        # turn horizontal root motion into vertical motion.
        up = Vector((0, 0, 1))
        lateral = pos(left) - pos(right)
        lateral.z = 0
        lateral.normalize()
        forward = up.cross(lateral).normalized()
        if min(lateral.length, forward.length, up.length) < .9:
            raise ValueError("Degenerate humanoid reference pose")
        return Matrix((lateral, forward, up)).transposed()

    def estimate_scale(self):
        def length(models, names, chain):
            points = [models[names.index(n)].translation for n in chain]
            return sum((b - a).length for a, b in zip(points, points[1:]))
        source = length(self.source_rest, self.source.bones,
                        ("NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]"))
        target = length(self.target_rest, [b.name for b in self.rig.bones],
                        ("L_Thigh", "L_Calf", "L_Foot"))
        if source <= 1e-6 or target <= 1e-6:
            raise ValueError("Degenerate reference leg length")
        return target / source

    def convert_offset(self, offset):
        position = self.source_to_target @ Vector([offset.get(k, 0) for k in ("x", "y", "z")])
        position *= self.unit_scale
        result = dict(zip(("x", "y", "z"), position))
        # SexLab's positive heading turns +Y toward +X; OSF uses the opposite
        # positive-Z rotation convention (Scene.h PlacementToWorld).
        result["heading"] = -offset.get("heading", 0)
        return result

    def binding(self, anim):
        if anim.blend_hint != 0:
            raise ValueError("Additive HKX clips are outside the prototype profile")
        if anim.num_frames < 2 or not math.isfinite(anim.duration) or anim.duration <= 0:
            raise ValueError("Invalid animation duration/frame count")
        if len(anim.tracks) != anim.num_tracks:
            raise ValueError("Decoder returned incomplete animation tracks")
        indices = list(anim.track_to_bone_indices)
        if not indices:
            names = anim.bone_names
            by_name = {n.casefold(): i for i, n in enumerate(self.source.bones)}
            layout = hashlib.sha256(json.dumps([self.source.bones, self.source.parents], separators=(",", ":")).encode()).hexdigest()
            if (not any(names) and layout == VANILLA_HUMAN_LAYOUT and anim.num_tracks in (97, 99)
                    and getattr(anim, "original_skeleton_name", "") == self.source.name):
                indices = list(range(anim.num_tracks))
            elif len(names) == anim.num_tracks and all(n.casefold() in by_name for n in names):
                indices = [by_name[n.casefold()] for n in names]
            else:
                raise ValueError("HKX needs explicit track indices or complete matching bone names")
        if len(indices) != anim.num_tracks or len(set(indices)) != len(indices):
            raise ValueError("Invalid or duplicate HKX track binding")
        if any(i < 0 or i >= len(self.source.bones) for i in indices):
            raise ValueError("Animation targets bones outside the supplied skeleton")
        for track in anim.tracks:
            for values in (track.translations, track.rotations, track.scales):
                if len(values) not in (1, anim.num_frames):
                    raise ValueError("Decoder returned incomplete per-frame channels")
        return indices

    def convert_frame(self, anim, binding, frame):
        locals_ = list(self.source_rest_local)
        for track, bone_id in zip(anim.tracks, binding):
            sample = lambda v: v[0] if len(v) == 1 else v[frame]
            scale = sample(track.scales)
            if any(not math.isfinite(s) or s <= 0 or s > 100 for s in scale):
                raise ValueError("Invalid source bone scale")
            if any(abs(s - 1) > 1e-3 for s in scale):
                raise ValueError(f"Non-unit source scale at {self.source.bones[bone_id]}; "
                                 "this clip needs a separate Skyrim scale-interpretation profile")
            locals_[bone_id] = matrix(sample(track.translations), sample(track.rotations), scale)
        source_models = model_matrices(locals_, self.source.parents)
        source_rotations = model_rotations(locals_, self.source.parents)
        c = self.source_to_target.to_quaternion()
        models, local_out = [None] * len(self.rig.bones), [None] * len(self.rig.bones)
        pending = set(range(len(self.rig.bones)))
        while pending:
            for i in sorted(pending):
                p = self.target_parents[i]
                if p != -1 and models[p] is None:
                    continue
                parent = Matrix.Identity(4) if p == -1 else models[p]
                local = self.target_rest_local[i].copy()
                if i in self.mapping:
                    s = self.mapping[i]
                    delta = source_rotations[s] @ self.source_rest_rotations[s].inverted()
                    desired = c @ delta @ c.inverted() @ self.target_rest[i].to_quaternion()
                    local_rotation = parent.to_quaternion().inverted() @ desired
                    local = Matrix.LocRotScale(local.translation, local_rotation, Vector((1, 1, 1)))
                if i == self.target_com:
                    displacement = source_models[self.source_pelvis].translation - self.source_rest[self.source_pelvis].translation
                    pos = self.target_rest[i].translation + self.source_to_target @ displacement * self.unit_scale
                    local.translation = parent.inverted() @ pos
                # OSF owns anchor placement; export Root stays at its identity/rest origin.
                local_out[i] = decompose(local)
                models[i] = parent @ local
                pending.remove(i)
        return local_out

    @property
    def mapped_names(self):
        return [self.rig.bones[i].name for i in self.mapping]
