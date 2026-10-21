"""Versioned scale policies; importable by the CLI without Blender."""

UNIT_SCALE = "unit-v1"
POSITIVE_AFFINE = "positive-affine-v1"
SCALE_PROFILES = (UNIT_SCALE, POSITIVE_AFFINE)

# Deliberate profile bounds, not inferred from whichever clip is being imported.
MIN_SCALE = 1e-4
MAX_SCALE = 100.0
TRS_RESIDUAL_LIMIT = 5e-4


def describe(profile):
    if profile not in SCALE_PROFILES:
        raise ValueError(f"Unknown scale profile: {profile}")
    if profile == UNIT_SCALE:
        return {"id": profile, "policy": "Unit source scales only; fixed target bind translations"}
    return {
        "id": profile,
        "policy": "Bind-relative source model linear transforms, including scale, mapped into target bone axes",
        "source_composition": "Affine parent @ local T*R*S, including unmapped source ancestors",
        "target_translation": "Bind translations retained except COM; effective lengths animate through inherited scale",
        "target_scale": "Explicit positive local GLB scale tracks; shared source mappings do not duplicate stretch",
        "scale_bounds": [MIN_SCALE, MAX_SCALE],
        "trs_residual_limit": TRS_RESIDUAL_LIMIT,
        "residual_metric": "max(abs(actual-reference)) / max(abs(reference)), over the 3x3 linear part",
        "shear_policy": "Bounded TRS approximation checked in both local and model space at every authored key; larger residuals reject",
        "scope": "Offline affine interpretation; no claim of exact Skyrim runtime nonuniform-scale evaluation or contact preservation",
    }
