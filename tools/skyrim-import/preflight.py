"""Decoded input checks shared by preflight and conversion; no output baking.

Compatibility here means the current retarget profile can accept the decoded
channels. It does not establish body/contact quality or engine acceptance.
"""
from __future__ import annotations

import hashlib
import math
from numbers import Real
from pathlib import Path
from scale_profile import UNIT_SCALE, POSITIVE_AFFINE, MIN_SCALE, MAX_SCALE, describe


def inspect_animation(anim, converter):
    profile = getattr(converter, "scale_profile", UNIT_SCALE)
    result = {"frames": anim.num_frames,
              "duration_seconds": anim.duration if math.isfinite(anim.duration) else None,
              "tracks": anim.num_tracks, "errors": []}
    errors = result["errors"]
    try:
        binding = converter.binding(anim)
    except ValueError as error:
        errors.append(str(error))
        binding = []
    if anim.num_frames > 18000 or anim.duration > 600:
        errors.append("Clip exceeds prototype duration/frame limit")
    result["binding_mode"] = ("explicit" if anim.track_to_bone_indices else
                              "named" if any(anim.bone_names) else "verified-vanilla-human-prefix") if binding else None
    scale_min, scale_max = math.inf, -math.inf
    non_unit = non_positive = non_uniform = out_of_bounds = 0
    examples = []
    malformed = set()
    for index, track in enumerate(anim.tracks):
        bone = converter.source.bones[binding[index]] if binding else f"track {index}"
        for field, width in (("translations", 3), ("rotations", 4), ("scales", 3)):
            for frame, sample in enumerate(getattr(track, field)):
                if len(sample) != width or any(isinstance(v, bool) or not isinstance(v, Real)
                                               or not math.isfinite(v) for v in sample):
                    malformed.add(field)
                    continue
                if field == "rotations" and math.hypot(*sample) < 1e-8:
                    malformed.add("zero quaternion")
                if field != "scales":
                    continue
                scale_min = min(scale_min, *sample)
                scale_max = max(scale_max, *sample)
                invalid = any(v <= 0 or v > 100 for v in sample)
                changed = any(abs(v - 1) > 1e-3 for v in sample)
                non_positive += any(v <= 0 for v in sample)
                non_unit += changed
                non_uniform += max(sample) - min(sample) > 1e-5
                out_of_bounds += any(not MIN_SCALE <= v <= MAX_SCALE for v in sample)
                if (invalid or changed) and len(examples) < 8:
                    examples.append({"bone": bone, "track": index, "sample": frame, "scale": list(sample)})
    if malformed:
        errors.append("Malformed decoded channels: " + ", ".join(sorted(malformed)))
    if non_unit and profile == UNIT_SCALE:
        errors.append("Non-unit source scales require a separate scale-interpretation profile; no scales were discarded")
    if non_positive:
        errors.append("Non-positive source scales are unsupported")
    if profile == POSITIVE_AFFINE and out_of_bounds:
        errors.append(f"Source scales outside {profile} bounds [{MIN_SCALE}, {MAX_SCALE}]")
    result["scales"] = {"range": [scale_min, scale_max] if scale_min != math.inf else None,
                        "non_unit_samples": non_unit, "non_positive_samples": non_positive,
                        "non_uniform_samples": non_uniform,
                        "examples": examples}
    if profile == POSITIVE_AFFINE and not errors:
        from retarget import scale_diagnostics
        metrics = scale_diagnostics()
        result["affine_scale_check"] = metrics
        try:
            for frame in range(anim.num_frames):
                converter.convert_frame(anim, binding, frame, diagnostics=metrics)
        except ValueError as error:
            errors.append(str(error))
    return result


def inspect_plan(plan, decoder, converters):
    """Inspect every stage/role, retaining individual failures for pack review."""
    clips, errors = [], []
    for stage_index, stage in enumerate(plan["stages"], 1):
        stage_clips = []
        for actor, (source, converter) in enumerate(zip(stage["clips"], converters), 1):
            path = Path(source)
            row = {"stage": stage_index, "actor": actor, "path": str(path), "errors": []}
            try:
                row["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
                row.update(inspect_animation(decoder.load_skyrim_animation(str(path)), converter))
            except Exception as error:
                row["errors"].append(f"{type(error).__name__}: {error}")
            clips.append(row)
            stage_clips.append(row)
        decoded = [row for row in stage_clips if "frames" in row and row.get("duration_seconds") is not None]
        if len(decoded) == len(stage_clips) and any(
                row["frames"] != decoded[0]["frames"] or
                abs(row["duration_seconds"] - decoded[0]["duration_seconds"]) > 1e-4 for row in decoded[1:]):
            errors.append(f"Stage {stage_index}: participant clip lengths differ; automatic resampling is not implemented")
    return {"schema": 1, "scene_id": plan["id"], "profile": "skyrim-human-to-starfield-v1",
            "scale_interpretation": describe(plan.get("scale_profile", UNIT_SCALE)),
            "status": "unsupported" if errors or any(row["errors"] for row in clips) else "compatible",
            "clips": clips, "errors": errors,
            "scope": "Decoded input compatibility only; no baking, visual/contact or engine acceptance"}
