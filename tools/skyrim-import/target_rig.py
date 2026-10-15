"""Read the Starfield v5 rig layout used by AFImport and check prototype GLBs.

No Blender or third-party dependency is needed. The stored bind transforms are
absolute *local* transforms: translation is in meters in the rig's Z-up basis,
and rotation is returned as glTF/ozz XYZW (the rig stores WXYZ). OSF reads GLB
local TRS without glTF Y-up conversion and stamps it by bone name. Consequently
the prototype deliberately writes the rig basis, not a general glTF scene basis.
See src/Serialization/{AFImport,GLTFImport}.cpp and src/Animation/Graph.cpp.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
from pathlib import Path
import struct


MAX_BONES = 4096
MAX_DEPTH = 512
MAX_RIG_BYTES = 4 * 1024 * 1024
MAX_GLB_BYTES = 512 * 1024 * 1024


@dataclass(frozen=True)
class Bone:
    name: str
    parent: int
    rotation_xyzw: tuple[float, float, float, float]
    translation: tuple[float, float, float]


@dataclass(frozen=True)
class Rig:
    bones: tuple[Bone, ...]
    animated_bone_count: int

    @property
    def by_name(self) -> dict[str, Bone]:
        return {bone.name: bone for bone in self.bones}


def _unit_rotation(values: list | tuple, context: str) -> None:
    if len(values) != 4 or not all(math.isfinite(v) for v in values):
        raise ValueError(f"{context}: invalid quaternion")
    if abs(sum(v * v for v in values) - 1.0) > 0.002:
        raise ValueError(f"{context}: quaternion is not normalized")


def _check_parents(parents: list[int]) -> None:
    """Bound hierarchy traversal, including disconnected cycles."""
    count = len(parents)
    for index, parent in enumerate(parents):
        if parent < -1 or parent >= count or parent == index:
            raise ValueError(f"bone {index}: invalid parent {parent}")
    for index in range(count):
        visited: set[int] = set()
        current = index
        while current >= 0:
            if current in visited:
                raise ValueError("bone hierarchy contains a cycle")
            visited.add(current)
            if len(visited) > MAX_DEPTH:
                raise ValueError("bone hierarchy exceeds depth limit")
            current = parents[current]


def parse_rig(data: bytes) -> Rig:
    """Parse the bounded 80-byte-header/96-byte-bone Starfield v5 layout."""
    if not 80 <= len(data) <= MAX_RIG_BYTES:
        raise ValueError("rig size is outside supported bounds")
    version, file_size, header_size = struct.unpack_from("<III", data)
    if version != 5 or header_size != 80:
        raise ValueError(f"unsupported rig header: version={version}, size={header_size}")
    if file_size != len(data):
        raise ValueError("rig declared size differs from actual size")
    count, animated_count = struct.unpack_from("<HH", data, 56)
    if not 0 < count <= MAX_BONES or animated_count > count:
        raise ValueError("rig bone counts are invalid")
    table_end = 80 + count * 96
    if table_end > len(data):
        raise ValueError("rig bone table is truncated")
    bones: list[Bone] = []
    names: set[str] = set()
    for index in range(count):
        offset = 80 + index * 96
        rw, rx, ry, rz = struct.unpack_from("<4f", data, offset)
        translation = struct.unpack_from("<3f", data, offset + 32)
        name_offset, parent = struct.unpack_from("<Qi", data, offset + 48)
        if not table_end <= name_offset < len(data):
            raise ValueError(f"bone {index}: name offset is outside string data")
        end = data.find(b"\0", name_offset, min(len(data), name_offset + 1025))
        if end < 0:
            raise ValueError(f"bone {index}: name is unterminated or too long")
        try:
            name = data[name_offset:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"bone {index}: name is not UTF-8") from exc
        if not name or any(ord(char) < 32 for char in name) or name.lower() in names:
            raise ValueError(f"bone {index}: empty, invalid, or duplicate name")
        names.add(name.lower())
        rotation = (rx, ry, rz, rw)
        _unit_rotation(rotation, name)
        if not all(math.isfinite(value) for value in translation):
            raise ValueError(f"{name}: non-finite translation")
        bones.append(Bone(name, parent, rotation, translation))
    _check_parents([bone.parent for bone in bones])
    return Rig(tuple(bones), animated_count)


def read_rig(path: str | Path) -> Rig:
    path = Path(path)
    if path.stat().st_size > MAX_RIG_BYTES:
        raise ValueError("rig exceeds supported size")
    return parse_rig(path.read_bytes())


def validate_glb(path: str | Path, rig: Rig | None = None) -> dict:
    """Check the narrow output contract for this importer's self-contained GLB.

    This checks structure and animation data, not visual retargeting quality or
    successful loading by the native engine. Only float TRS tracks with LINEAR
    interpolation are supported, because OSF imports those as ordinary keys.
    """
    path = Path(path)
    if path.stat().st_size > MAX_GLB_BYTES:
        raise ValueError("GLB exceeds supported size")
    data = path.read_bytes()
    if len(data) < 20 or struct.unpack_from("<III", data) != (0x46546C67, 2, len(data)):
        raise ValueError("invalid GLB header")
    chunks: list[tuple[int, bytes]] = []
    cursor = 12
    while cursor < len(data):
        if cursor + 8 > len(data):
            raise ValueError("truncated GLB chunk header")
        size, kind = struct.unpack_from("<II", data, cursor)
        cursor += 8
        if size % 4 or size > len(data) - cursor:
            raise ValueError("invalid GLB chunk bounds")
        chunks.append((kind, data[cursor:cursor + size]))
        cursor += size
    if [kind for kind, _ in chunks] != [0x4E4F534A, 0x004E4942]:
        raise ValueError("expected one JSON and one BIN chunk")
    try:
        doc = json.loads(chunks[0][1])
    except (ValueError, UnicodeDecodeError) as exc:
        raise ValueError("invalid GLB JSON") from exc
    binary = chunks[1][1]
    if doc.get("asset", {}).get("version") != "2.0":
        raise ValueError("expected glTF version 2.0")
    buffers = doc.get("buffers", [])
    if len(buffers) != 1 or "uri" in buffers[0]:
        raise ValueError("expected one embedded GLB buffer")
    byte_length = buffers[0].get("byteLength", -1)
    if not isinstance(byte_length, int) or not 0 <= len(binary) - byte_length <= 3:
        raise ValueError("invalid embedded buffer size")
    nodes = doc.get("nodes", [])
    if not 0 < len(nodes) <= MAX_BONES:
        raise ValueError("invalid GLB node count")
    names: dict[str, int] = {}
    parents = [-1] * len(nodes)
    for index, node in enumerate(nodes):
        name = node.get("name", "")
        if not name or name.lower() in names:
            raise ValueError("GLB node names must be nonempty and unique")
        names[name.lower()] = index
        if "matrix" in node:
            raise ValueError(f"{name}: use TRS nodes, not matrix nodes")
        _unit_rotation(node.get("rotation", [0, 0, 0, 1]), name)
        for key, default in (("translation", [0, 0, 0]), ("scale", [1, 1, 1])):
            values = node.get(key, default)
            if len(values) != 3 or not all(math.isfinite(v) for v in values):
                raise ValueError(f"{name}: invalid {key}")
        for child in node.get("children", []):
            if not isinstance(child, int) or not 0 <= child < len(nodes) or parents[child] != -1:
                raise ValueError("invalid GLB child or multiple parents")
            parents[child] = index
    _check_parents(parents)
    if rig is not None:
        for bone in rig.bones:
            index = names.get(bone.name.lower())
            if index is None:
                continue  # A deliberately sparse body-only export is supported.
            expected = names.get(rig.bones[bone.parent].name.lower()) if bone.parent >= 0 else -1
            if expected is None or parents[index] != expected:
                raise ValueError(f"{bone.name}: GLB parent differs from target rig")
        if not any(bone.name.lower() in names for bone in rig.bones):
            raise ValueError("GLB contains no target-rig bones")

    accessors = doc.get("accessors", [])
    views = doc.get("bufferViews", [])
    cache: dict[int, list[tuple[float, ...]]] = {}

    def values(index: int, shape: str) -> list[tuple[float, ...]]:
        if not isinstance(index, int) or not 0 <= index < len(accessors):
            raise ValueError("invalid animation accessor index")
        accessor = accessors[index]
        if accessor.get("type") != shape or accessor.get("componentType") != 5126 or "sparse" in accessor:
            raise ValueError("expected dense float animation accessor")
        if index in cache:
            return cache[index]
        view_index = accessor.get("bufferView", -1)
        if not isinstance(view_index, int) or not 0 <= view_index < len(views):
            raise ValueError("invalid animation bufferView")
        view = views[view_index]
        if view.get("buffer") != 0:
            raise ValueError("animation references an external buffer")
        components = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[shape]
        size = components * 4
        count = accessor.get("count", 0)
        start = accessor.get("byteOffset", 0)
        base = view.get("byteOffset", 0)
        length = view.get("byteLength", -1)
        stride = view.get("byteStride", size)
        if not all(isinstance(v, int) for v in (count, start, base, length, stride)):
            raise ValueError("invalid accessor dimensions")
        if count <= 0 or min(start, base, length) < 0 or stride < size or stride % 4 or (base + start) % 4:
            raise ValueError("invalid accessor dimensions or alignment")
        if base + length > byte_length or start + (count - 1) * stride + size > length:
            raise ValueError("animation accessor exceeds its bufferView")
        result = [struct.unpack_from(f"<{components}f", binary, base + start + i * stride) for i in range(count)]
        if not all(math.isfinite(v) for row in result for v in row):
            raise ValueError("animation accessor contains non-finite values")
        cache[index] = result
        return result

    animations = doc.get("animations", [])
    if not animations:
        raise ValueError("GLB has no animations")
    duration = 0.0
    channel_count = 0
    for animation in animations:
        samplers = animation.get("samplers", [])
        channels = animation.get("channels", [])
        if not channels:
            raise ValueError("animation has no channels")
        used: set[tuple[int, str]] = set()
        for channel in channels:
            target = channel.get("target", {})
            node_index = target.get("node", -1)
            path_name = target.get("path", "")
            if not isinstance(node_index, int) or not 0 <= node_index < len(nodes) or path_name not in ("translation", "rotation", "scale"):
                raise ValueError("invalid animation target")
            if (node_index, path_name) in used:
                raise ValueError("duplicate animation channel")
            used.add((node_index, path_name))
            sampler_index = channel.get("sampler", -1)
            if not isinstance(sampler_index, int) or not 0 <= sampler_index < len(samplers):
                raise ValueError("invalid animation sampler")
            sampler = samplers[sampler_index]
            if sampler.get("interpolation", "LINEAR") != "LINEAR":
                raise ValueError("OSF prototype requires LINEAR animation keys")
            times = [row[0] for row in values(sampler.get("input", -1), "SCALAR")]
            output = values(sampler.get("output", -1), "VEC4" if path_name == "rotation" else "VEC3")
            if len(times) != len(output) or times[0] < 0 or any(a >= b for a, b in zip(times, times[1:])):
                raise ValueError("animation times must be nonnegative and strictly increasing with matching outputs")
            if path_name == "rotation":
                for rotation in output:
                    _unit_rotation(rotation, "animation")
            if path_name == "translation" and nodes[node_index]["name"].lower() in ("root", "humanexportroot"):
                if any(abs(v) > 1e-6 for row in output for v in row):
                    raise ValueError("OSF discards Root/HumanExportRoot translation; put placement on COM or scene roles")
            duration = max(duration, times[-1])
            channel_count += 1
    if duration <= 0:
        raise ValueError("animation must have positive duration")
    return {"nodes": len(nodes), "animations": len(animations), "channels": channel_count, "duration_seconds": duration}
