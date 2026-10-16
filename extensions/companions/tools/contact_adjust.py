"""Small optional arm-contact adjustment for the private Embrace experiment.

Run inside Blender (mathutils is required). This module does not read, export,
or deploy animation files. It consumes one Starfield-rig local-TRS frame and
returns a copy with only the selected Biceps, Forearm, and Wrist rotations
changed. Positions/offsets use the same Z-up, metre, model-space coordinates as
the exported clip, before any OSF actor-placement transform.

The two limb segments retain their lengths. The original elbow is the default
pole reference; an explicit elbow hint can select a different bend plane. The authored
wrist world orientation and every finger's local pose are retained. Starfield's
sibling twist helpers retain their local TRS, so they follow their owning limb
segment without additional independent roll. This is a contact-position tool,
not a collision solver or a general retargeter.
"""
from __future__ import annotations

from copy import deepcopy
import math

from mathutils import Matrix, Quaternion, Vector


def _vector(values, name):
    if len(values) != 3 or any(not math.isfinite(float(v)) for v in values):
        raise ValueError(f"{name} must contain three finite coordinates")
    return Vector(values)


def _rotation(values):
    if len(values) != 4 or any(not math.isfinite(float(v)) for v in values):
        raise ValueError("Rotation must contain four finite XYZW values")
    result = Quaternion((values[3], values[0], values[1], values[2]))
    if abs(result.magnitude - 1.0) > 1e-4:
        raise ValueError("Input rotations must be normalized")
    result.normalize()
    return result


def _models(frame, parents):
    result = [None] * len(frame)
    pending = set(range(len(frame)))
    while pending:
        ready = []
        for index in sorted(pending):
            parent = parents[index]
            if parent < -1 or parent >= len(frame) or parent == index:
                raise ValueError("Invalid skeleton parent")
            if parent != -1 and result[parent] is None:
                continue
            row = frame[index]
            scale = _vector(row.get("scale", (1.0, 1.0, 1.0)), "Scale")
            if any(abs(v - 1.0) > 1e-4 for v in scale):
                raise ValueError("This contact helper requires unit-scale frames")
            local = Matrix.LocRotScale(
                _vector(row["translation"], "Translation"),
                _rotation(row["rotation"]), Vector((1.0, 1.0, 1.0)))
            result[index] = local if parent == -1 else result[parent] @ local
            ready.append(index)
        if not ready:
            raise ValueError("Skeleton contains a parent cycle")
        pending.difference_update(ready)
    return result


def _write_world_rotation(frame, models, parents, index, desired):
    parent = parents[index]
    local = desired if parent == -1 else models[parent].to_quaternion().inverted() @ desired
    local.normalize()
    # Retain the input hemisphere. The exporter also enforces continuity across
    # frames, but a single-frame helper should not introduce arbitrary sign flips.
    previous = _rotation(frame[index]["rotation"])
    if local.dot(previous) < 0:
        local.negate()
    frame[index]["rotation"] = [local.x, local.y, local.z, local.w]


def adjust_wrist(rig, frame, side, *, target_position=None, offset=None, elbow_hint=None,
                 endpoint_tolerance=2e-5):
    """Return ``(adjusted_frame, metrics)`` for one L/R arm.

    Supply exactly one of ``target_position`` or ``offset`` (three coordinates).
    The target must be strictly within the arm's reachable annulus. Unreachable
    An optional model-space elbow_hint selects the bend plane; otherwise the
    authored elbow is retained as the pole reference. Unreachable
    targets, degenerate poles, and non-unit scales raise ValueError instead of
    stretching a limb or guessing a new bend direction. Input data is untouched.
    """
    if side not in ("L", "R"):
        raise ValueError("Side must be L or R")
    if (target_position is None) == (offset is None):
        raise ValueError("Supply exactly one of target_position or offset")
    if not math.isfinite(endpoint_tolerance) or endpoint_tolerance <= 0:
        raise ValueError("Endpoint tolerance must be finite and positive")
    bones = rig.bones
    if len(frame) != len(bones):
        raise ValueError("Frame and rig bone counts differ")
    names = [bone.name for bone in bones]
    if len(set(names)) != len(names):
        raise ValueError("Rig contains duplicate bone names")
    parents = [bone.parent for bone in bones]
    indices = [names.index(f"{side}_{name}") for name in ("Biceps", "Forearm", "Wrist")]
    upper, lower, wrist = indices
    if parents[lower] != upper or parents[wrist] != lower:
        raise ValueError("Expected Starfield Biceps -> Forearm -> Wrist hierarchy")
    original = _models(frame, parents)
    shoulder, elbow, hand = [original[index].translation.copy() for index in indices]
    desired = (_vector(target_position, "Target position") if target_position is not None
               else hand + _vector(offset, "Offset"))
    displacement = desired - hand
    upper_length, lower_length = (elbow - shoulder).length, (hand - elbow).length
    if min(upper_length, lower_length) < 1e-6:
        raise ValueError("Degenerate arm segment")
    output = deepcopy(frame)
    metrics = {
        "side": side, "original_wrist": list(hand), "requested_wrist": list(desired),
        "requested_offset": list(displacement), "offset_length_m": displacement.length,
        "segment_lengths_before_m": [upper_length, lower_length],
        "changed_bones": [], "twist_policy": "preserve-authored-local-TRS",
        "pole_policy": "explicit-elbow-hint" if elbow_hint is not None else "authored-elbow",
    }
    if displacement.length > 1e-8:
        axis = desired - shoulder
        reach = axis.length
        if not abs(upper_length - lower_length) + 1e-6 < reach < upper_length + lower_length - 1e-6:
            raise ValueError(f"Wrist target is outside the nonsingular arm reach ({reach:.6f} m)")
        axis /= reach
        pole = (elbow if elbow_hint is None else _vector(elbow_hint, "Elbow hint")) - shoulder
        pole -= axis * pole.dot(axis)
        if pole.length < 1e-6:
            raise ValueError("Original elbow cannot define a stable pole for this target")
        pole.normalize()
        axial = (upper_length**2 - lower_length**2 + reach**2) / (2.0 * reach)
        radial = math.sqrt(max(0.0, upper_length**2 - axial**2))
        desired_elbow = shoulder + axis * axial + pole * radial

        upper_swing = (elbow - shoulder).rotation_difference(desired_elbow - shoulder)
        _write_world_rotation(output, original, parents, upper,
                              upper_swing @ original[upper].to_quaternion())
        after_upper = _models(output, parents)
        new_elbow = after_upper[lower].translation
        current_lower_vector = after_upper[wrist].translation - new_elbow
        lower_swing = current_lower_vector.rotation_difference(desired - new_elbow)
        _write_world_rotation(output, after_upper, parents, lower,
                              lower_swing @ after_upper[lower].to_quaternion())
        after_lower = _models(output, parents)
        _write_world_rotation(output, after_lower, parents, wrist, original[wrist].to_quaternion())
        metrics["changed_bones"] = [names[index] for index in indices if output[index] != frame[index]]

    result = _models(output, parents)
    resolved_shoulder, resolved_elbow, resolved_hand = [result[index].translation for index in indices]
    endpoint_error = (resolved_hand - desired).length
    metrics.update(
        resolved_wrist=list(resolved_hand), endpoint_error_m=endpoint_error,
        segment_lengths_after_m=[(resolved_elbow-resolved_shoulder).length,
                                 (resolved_hand-resolved_elbow).length],
        wrist_world_rotation_matrix_error=max(
            abs(result[wrist][r][c] - original[wrist][r][c])
            for r in range(3) for c in range(3)),
    )
    if displacement.length > 1e-8:
        resolved_pole = resolved_elbow - resolved_shoulder
        resolved_pole -= axis * resolved_pole.dot(axis)
        metrics["pole_alignment"] = resolved_pole.normalized().dot(pole)
    if endpoint_error > endpoint_tolerance:
        raise ValueError(f"IK endpoint residual {endpoint_error:.9f} m exceeds tolerance")
    return output, metrics
