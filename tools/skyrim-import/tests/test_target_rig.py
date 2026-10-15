"""Synthetic rig/GLB fixtures; no Bethesda assets are copied into tests."""

import copy
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from target_rig import parse_rig, validate_glb


def rig_fixture():
    names = b"Root\0COM\0C_Hips\0"
    data = bytearray(80 + 3 * 96) + names
    struct.pack_into("<III", data, 0, 5, len(data), 80)
    struct.pack_into("<HH", data, 56, 3, 3)
    for index, (parent, name_offset) in enumerate(((-1, 368), (0, 373), (1, 377))):
        offset = 80 + index * 96
        rotation = (1, 0, 0, 0) if index != 2 else (math.sqrt(0.5), 0, 0, math.sqrt(0.5))
        struct.pack_into("<4f", data, offset, *rotation)
        struct.pack_into("<3f", data, offset + 32, 0, 0, 1.05 if index == 1 else 0)
        struct.pack_into("<Qi", data, offset + 48, name_offset, parent)
    return data


def glb_fixture():
    binary = struct.pack("<2f8f", 0, 1, 0, 0, 0, 1, 0, 0, 0, 1)
    doc = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": len(binary)}],
        "bufferViews": [{"buffer": 0, "byteLength": 8}, {"buffer": 0, "byteOffset": 8, "byteLength": 32}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "type": "SCALAR", "count": 2},
                      {"bufferView": 1, "componentType": 5126, "type": "VEC4", "count": 2}],
        "nodes": [{"name": "Root", "children": [1]}, {"name": "COM", "children": [2]}, {"name": "C_Hips"}],
        "animations": [{"samplers": [{"input": 0, "output": 1}], "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}]}],
    }
    return doc, binary


def pack_glb(doc, binary):
    payload = json.dumps(doc).encode()
    payload += b" " * (-len(payload) % 4)
    binary += b"\0" * (-len(binary) % 4)
    return (struct.pack("<III", 0x46546C67, 2, 28 + len(payload) + len(binary))
            + struct.pack("<II", len(payload), 0x4E4F534A) + payload
            + struct.pack("<II", len(binary), 0x004E4942) + binary)


class RigTests(unittest.TestCase):
    def test_preserves_local_units_hierarchy_and_quaternion_order(self):
        rig = parse_rig(rig_fixture())
        self.assertEqual(rig.animated_bone_count, 3)
        self.assertEqual([bone.parent for bone in rig.bones], [-1, 0, 1])
        self.assertAlmostEqual(rig.by_name["COM"].translation[2], 1.05, places=6)
        self.assertAlmostEqual(rig.by_name["C_Hips"].rotation_xyzw[2], math.sqrt(0.5), places=6)
        self.assertAlmostEqual(rig.by_name["C_Hips"].rotation_xyzw[3], math.sqrt(0.5), places=6)

    def test_bad_sizes_and_name_offsets(self):
        for change in (lambda b: struct.pack_into("<I", b, 4, 80),
                       lambda b: struct.pack_into("<I", b, 0, 6),
                       lambda b: struct.pack_into("<H", b, 56, 4097),
                       lambda b: struct.pack_into("<Q", b, 128, 80),
                       lambda b: b.__setitem__(-1, 65)):
            with self.subTest(change=change):
                data = rig_fixture()
                change(data)
                with self.assertRaises(ValueError):
                    parse_rig(data)

    def test_rejects_cycles_invalid_transforms_and_duplicate_names(self):
        for change in (lambda b: struct.pack_into("<i", b, 136, 2),
                       lambda b: struct.pack_into("<i", b, 232, 99),
                       lambda b: struct.pack_into("<f", b, 112, float("nan")),
                       lambda b: struct.pack_into("<4f", b, 80, 0, 0, 0, 0),
                       lambda b: struct.pack_into("<Q", b, 224, 368)):
            with self.subTest(change=change):
                data = rig_fixture()
                change(data)
                with self.assertRaises(ValueError):
                    parse_rig(data)


class GlbTests(unittest.TestCase):
    def validate(self, doc, binary):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "fixture.glb"
            path.write_bytes(pack_glb(doc, binary))
            return validate_glb(path, parse_rig(rig_fixture()))

    def test_valid_output(self):
        self.assertEqual(self.validate(*glb_fixture()), {"nodes": 3, "animations": 1, "channels": 1, "duration_seconds": 1.0})

    def test_accessor_overrun_and_unsupported_interpolation(self):
        doc, binary = glb_fixture()
        for change in (lambda d: d["accessors"][1].update(count=3),
                       lambda d: d["animations"][0]["samplers"][0].update(interpolation="CUBICSPLINE"),
                       lambda d: d["nodes"][2].update(matrix=[1] * 16),
                       lambda d: d["nodes"][0].update(children=[1, 2])):
            with self.subTest(change=change):
                changed = copy.deepcopy(doc)
                change(changed)
                with self.assertRaises(ValueError):
                    self.validate(changed, binary)

    def test_rejects_bad_animation_times_and_rotations(self):
        doc, binary = glb_fixture()
        for offset, value in ((4, 0), (4, float("nan")), (20, 0)):
            with self.subTest(offset=offset, value=value):
                changed = bytearray(binary)
                struct.pack_into("<f", changed, offset, value)
                with self.assertRaises(ValueError):
                    self.validate(doc, bytes(changed))

    def test_sparse_export_keeps_necessary_ancestors(self):
        doc, binary = glb_fixture()
        doc["nodes"] = [{"name": "Root", "children": [1]}, {"name": "COM"}]
        doc["animations"][0]["channels"][0]["target"]["node"] = 1
        self.assertEqual(self.validate(doc, binary)["nodes"], 2)
        doc["nodes"] = [{"name": "Root", "children": [1]}, {"name": "C_Hips"}]
        with self.assertRaisesRegex(ValueError, "parent differs"):
            self.validate(doc, binary)


if __name__ == "__main__":
    unittest.main()
