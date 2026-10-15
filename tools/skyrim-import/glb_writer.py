"""Write OSF's bone-only GLB clips without exporter axis conversions.

Transforms are local to their parent, in target meters and Z-up coordinates;
quaternions use glTF's XYZW order. This deliberately preserves OSF's existing
GLB coordinate convention rather than producing a general-purpose Y-up asset.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import json
import math
from numbers import Real
from pathlib import Path
import struct
from typing import Any


def _sequence(value: Any, label: str) -> Sequence:
    if not isinstance(value, Sequence) or isinstance(value, (str, bytes, bytearray)):
        raise ValueError(f"{label} must be a sequence")
    return value


def _float32(value: Any, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a finite number")
    try:
        result = struct.unpack("<f", struct.pack("<f", float(value)))[0]
    except (OverflowError, struct.error) as error:
        raise ValueError(f"{label} is outside the finite float32 range") from error
    if not math.isfinite(result):
        raise ValueError(f"{label} must be finite")
    return result


def _vector(value: Any, size: int, label: str) -> list[float]:
    values = _sequence(value, label)
    if len(values) != size:
        raise ValueError(f"{label} must have {size} components")
    return [_float32(item, f"{label}[{index}]") for index, item in enumerate(values)]


def _rotation(value: Any, label: str) -> list[float]:
    result = _vector(value, 4, label)
    magnitude = math.hypot(*result)
    if magnitude == 0:
        raise ValueError(f"{label} must be a nonzero quaternion")
    return [_float32(component / magnitude, label) for component in result]


def _transform(value: Any, label: str, default_scale: list[float]) -> dict[str, list[float]]:
    if not isinstance(value, Mapping):
        raise ValueError(f"{label} must be a transform mapping")
    for key in ("translation", "rotation"):
        if key not in value:
            raise ValueError(f"{label} requires {key}")
    return {
        "translation": _vector(value["translation"], 3, f"{label}.translation"),
        "rotation": _rotation(value["rotation"], f"{label}.rotation"),
        "scale": _vector(value.get("scale", default_scale), 3, f"{label}.scale"),
    }


def write_glb(
    path: str | Path,
    bones: list[dict],
    times: list[float],
    samples: list[list[dict]],
    animation_name: str,
) -> None:
    """Write a hierarchy and LINEAR animation; reject invalid inputs before IO.

    Each bone requires ``name``, ``parent`` (index, -1, or None), ``translation``
    and ``rotation``. Optional ``scale`` defaults to unit scale. Every frame
    includes all bones in the same order; an omitted frame scale inherits its
    bone's rest scale. Quaternion magnitudes and signs are normalized without
    changing the represented orientation. Inputs are never modified.
    """
    bone_values = _sequence(bones, "bones")
    if not bone_values:
        raise ValueError("bones must not be empty")
    if not isinstance(animation_name, str) or not animation_name.strip():
        raise ValueError("animation_name must be a nonempty string")

    nodes: list[dict] = []
    parents: list[int | None] = []
    names: set[str] = set()
    for index, bone in enumerate(bone_values):
        label = f"bones[{index}]"
        if not isinstance(bone, Mapping):
            raise ValueError(f"{label} must be a bone mapping")
        name = bone.get("name")
        if not isinstance(name, str) or not name.strip() or name in names:
            raise ValueError(f"{label}.name must be nonempty and unique")
        names.add(name)
        if "parent" not in bone:
            raise ValueError(f"{label} requires parent")
        parent = bone["parent"]
        if parent is not None:
            if isinstance(parent, bool) or not isinstance(parent, int):
                raise ValueError(f"{label}.parent must be an integer index or None")
            if parent == -1:
                parent = None
            elif parent < 0 or parent >= len(bone_values) or parent == index:
                raise ValueError(f"{label}.parent is not a valid parent index")
        parents.append(parent)
        nodes.append({"name": name, **_transform(bone, label, [1.0, 1.0, 1.0])})

    # Validate first, including a cycle that has no root and would be unreachable
    # to a scene traversal. Parent order need not be topological.
    resolved: set[int] = set()
    for index in range(len(nodes)):
        chain: set[int] = set()
        current: int | None = index
        while current is not None and current not in resolved:
            if current in chain:
                raise ValueError("bone parent hierarchy contains a cycle")
            chain.add(current)
            current = parents[current]
        resolved.update(chain)
    roots = []
    for index, parent in enumerate(parents):
        if parent is None:
            roots.append(index)
        else:
            nodes[parent].setdefault("children", []).append(index)

    time_values = [_float32(value, f"times[{index}]")
                   for index, value in enumerate(_sequence(times, "times"))]
    if not time_values or time_values[0] < 0:
        raise ValueError("times must be nonempty and nonnegative")
    if any(after <= before for before, after in zip(time_values, time_values[1:])):
        raise ValueError("times must be strictly increasing after float32 conversion")
    sample_values = _sequence(samples, "samples")
    if len(sample_values) != len(time_values):
        raise ValueError("samples must contain one frame per time")

    frames: list[list[dict]] = []
    scale_tracks = ["scale" in bone for bone in bone_values]
    previous_rotations = [node["rotation"] for node in nodes]
    for frame_index, sample in enumerate(sample_values):
        label = f"samples[{frame_index}]"
        frame_values = _sequence(sample, label)
        if len(frame_values) != len(nodes):
            raise ValueError(f"{label} must contain exactly {len(nodes)} bones")
        frame = []
        for index, value in enumerate(frame_values):
            transform = _transform(value, f"{label}[{index}]", nodes[index]["scale"])
            rotation = transform["rotation"]
            if sum(a * b for a, b in zip(previous_rotations[index], rotation)) < 0:
                transform["rotation"] = rotation = [-component for component in rotation]
            previous_rotations[index] = rotation
            scale_tracks[index] |= "scale" in value
            frame.append(transform)
        frames.append(frame)

    binary = bytearray()
    buffer_views: list[dict] = []
    accessors: list[dict] = []

    def add_accessor(rows: list[list[float]], kind: str, *, bounds: bool = False) -> int:
        offset = len(binary)
        components = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[kind]
        packer = struct.Struct("<" + "f" * components)
        for row in rows:
            binary.extend(packer.pack(*row))
        buffer_views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(binary) - offset})
        accessor = {"bufferView": len(buffer_views) - 1, "byteOffset": 0,
                    "componentType": 5126, "count": len(rows), "type": kind}
        if bounds:
            accessor["min"] = [min(row[component] for row in rows) for component in range(components)]
            accessor["max"] = [max(row[component] for row in rows) for component in range(components)]
        accessors.append(accessor)
        return len(accessors) - 1

    time_accessor = add_accessor([[value] for value in time_values], "SCALAR", bounds=True)
    samplers = []
    channels = []
    for index in range(len(nodes)):
        properties = [("translation", "VEC3"), ("rotation", "VEC4")]
        if scale_tracks[index]:
            properties.append(("scale", "VEC3"))
        for property_name, kind in properties:
            accessor = add_accessor([frame[index][property_name] for frame in frames], kind)
            samplers.append({"input": time_accessor, "output": accessor, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": index, "path": property_name}})
    document = {
        "asset": {"version": "2.0", "generator": "OSF Skyrim prototype importer"},
        "scene": 0,
        "scenes": [{"nodes": roots}],
        "nodes": nodes,
        "animations": [{"name": animation_name, "samplers": samplers, "channels": channels}],
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    json_chunk = json.dumps(document, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * (-len(json_chunk) % 4)
    binary += b"\0" * (-len(binary) % 4)
    length = 12 + 8 + len(json_chunk) + 8 + len(binary)
    output = struct.pack("<4sII", b"glTF", 2, length)
    output += struct.pack("<I4s", len(json_chunk), b"JSON") + json_chunk
    output += struct.pack("<I4s", len(binary), b"BIN\0") + binary
    Path(path).write_bytes(output)
