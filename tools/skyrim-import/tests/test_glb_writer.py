"""Binary-contract checks for the direct OSF GLB writer (no Blender needed)."""

import copy
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from glb_writer import write_glb


def transform(translation=(0, 0, 0), rotation=(0, 0, 0, 1), **extra):
    return {"translation": list(translation), "rotation": list(rotation), **extra}


class GlbWriterTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "clip.glb"
        # Child before parent exercises index preservation independent of tree order.
        self.bones = [
            {"name": "Spine", "parent": 1, **transform((0.01, -0.02, 0.9))},
            {"name": "Root", "parent": -1, **transform()},
        ]
        self.times = [0, 0.25, 0.5]
        self.samples = [
            [transform((0.1, -0.2, 0.9)), transform()],
            [transform((0.1, -0.2, 1.0), (0, 0, -2, -2)), transform()],
            [transform((0.1, -0.2, 1.1), (0, 0, 1, 1)), transform()],
        ]

    def write_and_read(self):
        write_glb(self.path, self.bones, self.times, self.samples, "Test motion")
        payload = self.path.read_bytes()
        self.assertEqual(struct.unpack_from("<4sII", payload), (b"glTF", 2, len(payload)))
        json_length, json_type = struct.unpack_from("<I4s", payload, 12)
        self.assertEqual(json_type, b"JSON")
        self.assertEqual(json_length % 4, 0)
        document = json.loads(payload[20:20 + json_length])
        bin_offset = 20 + json_length
        bin_length, bin_type = struct.unpack_from("<I4s", payload, bin_offset)
        self.assertEqual(bin_type, b"BIN\0")
        self.assertEqual(bin_length % 4, 0)
        binary = payload[bin_offset + 8:]
        self.assertEqual(len(binary), bin_length)
        self.assertEqual(document["buffers"], [{"byteLength": bin_length}])
        # Every declared region must lie in its buffer, and must not overlap.
        end = 0
        for view in document["bufferViews"]:
            self.assertEqual(view["buffer"], 0)
            self.assertEqual(view["byteOffset"] % 4, 0)
            self.assertGreaterEqual(view["byteOffset"], end)
            end = view["byteOffset"] + view["byteLength"]
            self.assertLessEqual(end, bin_length)
        self.assertEqual(end, bin_length)
        return document, binary

    def accessor_rows(self, document, binary, index):
        accessor = document["accessors"][index]
        view = document["bufferViews"][accessor["bufferView"]]
        self.assertEqual(accessor["componentType"], 5126)
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}[accessor["type"]]
        offset = view["byteOffset"] + accessor.get("byteOffset", 0)
        self.assertLessEqual(accessor.get("byteOffset", 0) + accessor["count"] * width * 4, view["byteLength"])
        return [list(struct.unpack_from("<" + "f" * width, binary, offset + row * width * 4))
                for row in range(accessor["count"])]

    def tracks(self, document, binary):
        animation = document["animations"][0]
        result = {}
        for channel in animation["channels"]:
            sampler = animation["samplers"][channel["sampler"]]
            self.assertEqual(sampler["interpolation"], "LINEAR")
            self.assertEqual(self.accessor_rows(document, binary, sampler["input"]), [[t] for t in self.times])
            result[(channel["target"]["node"], channel["target"]["path"])] = self.accessor_rows(document, binary, sampler["output"])
        return result

    def test_roundtrip_preserves_z_up_trs_hierarchy_and_time_bounds(self):
        document, binary = self.write_and_read()
        self.assertEqual(document["asset"]["version"], "2.0")
        self.assertEqual(document["scenes"], [{"nodes": [1]}])
        self.assertEqual(document["nodes"][1]["children"], [0])
        self.assertNotIn("children", document["nodes"][0])
        self.assertEqual(document["nodes"][1]["translation"], [0, 0, 0])
        self.assertNotIn("skins", document)
        self.assertNotIn("meshes", document)
        self.assertEqual(document["animations"][0]["name"], "Test motion")
        self.assertEqual(document["accessors"][0]["min"], [0])
        self.assertEqual(document["accessors"][0]["max"], [0.5])
        tracks = self.tracks(document, binary)
        self.assertEqual(set(tracks), {(0, "translation"), (0, "rotation"), (1, "translation"), (1, "rotation")})
        for actual, expected in zip(tracks[(0, "translation")], [frame[0]["translation"] for frame in self.samples]):
            for a, b in zip(actual, expected):
                self.assertAlmostEqual(a, b, places=6)
        self.assertEqual(tracks[(1, "translation")], [[0, 0, 0]] * 3)

    def test_normalizes_quaternions_and_preserves_hemisphere_without_mutation(self):
        original = copy.deepcopy((self.bones, self.samples))
        document, binary = self.write_and_read()
        rotations = self.tracks(document, binary)[(0, "rotation")]
        previous = document["nodes"][0]["rotation"]
        for rotation in rotations:
            self.assertAlmostEqual(math.hypot(*rotation), 1, places=6)
            self.assertGreaterEqual(sum(a * b for a, b in zip(previous, rotation)), 0)
            previous = rotation
        self.assertEqual(rotations[1], rotations[2])
        self.assertGreater(rotations[1][2], 0)
        self.assertEqual((self.bones, self.samples), original)

    def test_optional_scale_defaults_to_rest_and_writes_scale_track(self):
        self.bones[0]["scale"] = [1, 2, 3]
        self.samples[1][0]["scale"] = [1, 2, 4]
        self.samples[0][1]["scale"] = [2, 2, 2]
        document, binary = self.write_and_read()
        tracks = self.tracks(document, binary)
        self.assertEqual(tracks[(0, "scale")], [[1, 2, 3], [1, 2, 4], [1, 2, 3]])
        self.assertEqual(tracks[(1, "scale")], [[2, 2, 2], [1, 1, 1], [1, 1, 1]])

    def test_rejects_invalid_inputs_before_overwriting_output(self):
        invalid = [
            ("bones", []),
            ("bones", [{**self.bones[0], "parent": 0}, self.bones[1]]),
            ("bones", [self.bones[0], {**self.bones[1], "parent": 0}]),
            ("bones", [{**self.bones[0], "parent": 9}, self.bones[1]]),
            ("bones", [{**self.bones[0], "parent": True}, self.bones[1]]),
            ("bones", [self.bones[0], {**self.bones[1], "name": "Spine"}]),
            ("times", []),
            ("times", [-1, 0, 1]),
            ("times", [0, 0.25, 0.25]),
            ("times", [0, 1, 1 + 1e-9]),
            ("times", [0, 0.5, 0.25]),
            ("times", [0, float("nan"), 0.5]),
            ("samples", self.samples[:2]),
            ("samples", [self.samples[0][:1], *self.samples[1:]]),
            ("animation_name", ""),
        ]
        for property_name, bad_value in invalid:
            with self.subTest(property=property_name, value=bad_value):
                arguments = dict(bones=self.bones, times=self.times, samples=self.samples, animation_name="Test")
                arguments[property_name] = bad_value
                self.path.write_bytes(b"existing clip")
                with self.assertRaises(ValueError):
                    write_glb(self.path, **arguments)
                self.assertEqual(self.path.read_bytes(), b"existing clip")

    def test_rejects_malformed_transforms_and_nonfinite_float32(self):
        invalid = [
            {"translation": [0, 0], "rotation": [0, 0, 0, 1]},
            transform(rotation=(0, 0, 0, 0)),
            transform(rotation=(0, 0, float("inf"), 1)),
            transform(translation=(0, float("nan"), 0)),
            transform(translation=(0, 1e100, 0)),
            transform(translation=(0, True, 0)),
            transform(scale=[1, 2]),
            {"translation": [0, 0, 0]},
            None,
        ]
        for value in invalid:
            with self.subTest(transform=value):
                samples = copy.deepcopy(self.samples)
                samples[0][0] = value
                with self.assertRaises(ValueError):
                    write_glb(self.path, self.bones, self.times, samples, "Test")
                self.assertFalse(self.path.exists())


if __name__ == "__main__":
    unittest.main()
