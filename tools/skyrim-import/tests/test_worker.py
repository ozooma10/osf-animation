"""Verify the actual saved Blender preview, independently of GLB serialization."""
from pathlib import Path
import tempfile
import unittest

try:
    import bpy
except ImportError as exc:
    raise unittest.SkipTest("Preview tests require Blender") from exc

from target_rig import Bone, Rig
from worker import save_preview
from glb_writer import write_glb
from preview_audit import audit_preview


class PreviewTests(unittest.TestCase):
    def test_saved_preview_retains_animated_scale_and_inherited_segment_length(self):
        identity = (0, 0, 0, 1)
        rig = Rig((Bone("Root", -1, identity, (0, 0, 0)),
                   Bone("COM", 0, identity, (0, 0, 1)),
                   Bone("Hand", 1, identity, (0, .5, 0))), 3)
        bones = [{"name": b.name, "parent": b.parent, "translation": b.translation,
                  "rotation": b.rotation_xyzw} for b in rig.bones]
        frames = [[{"translation": b.translation, "rotation": identity,
                    "scale": (scale, scale, scale) if b.name == "COM" else (1, 1, 1)}
                   for b in rig.bones] for scale in (1., 1.2, .8)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_glb(root / "clip.glb", bones, [0, .5, 1], frames, "scaled")
            save_preview(root / "preview.blend", rig, [{"name": "actor"}], [frames], [0, .5, 1], [{}])
            report = audit_preview(root / "preview.blend", {"roles": [{"name": "actor"}],
                                  "stages": [{"clips": ["clip.glb"]}]}, root, 0)
            self.assertEqual(report["status"], "passed")
            bpy.context.scene.frame_set(16)
            actor = bpy.data.objects["actor"]
            self.assertAlmostEqual(actor.pose.bones["Hand"].matrix.translation.y, .6, places=5)

    def test_saved_preview_matches_serialized_pair_and_rejects_changed_placement(self):
        import math
        from mathutils import Quaternion
        identity = (0, 0, 0, 1)
        rig = Rig((Bone("Root", -1, identity, (0, 0, 0)),
                   Bone("COM", 0, identity, (0, 0, 1)),
                   Bone("Hand", 1, identity, (0, .5, 0))), 3)
        bones = [{"name": b.name, "parent": b.parent, "translation": b.translation,
                  "rotation": b.rotation_xyzw} for b in rig.bones]
        samples = []
        for angle in (170, 190, 210):
            q = Quaternion((0, 0, 1), math.radians(angle))
            samples.append([{"translation": b.translation,
                             "rotation": (q.x, q.y, q.z, q.w) if b.name == "COM" else identity}
                            for b in rig.bones])
        roles = [{"name": "actor1"}, {"name": "actor2"}]
        placements = [{"x": 1, "heading": 90}, {"y": -1, "heading": -90}]
        scene = {"roles": roles, "stages": [{"clips": [
            {"file": "clip.glb", "offset": placement} for placement in placements]}]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_glb(root / "clip.glb", bones, [0, .5, 1], samples, "example")
            save_preview(root / "preview.blend", rig, roles, [samples, samples], [0, .5, 1], placements)
            audit = audit_preview(root / "preview.blend", scene, root, 0)
            self.assertEqual(audit["status"], "passed")
            self.assertEqual([a["bone_matrix_checks"] for a in audit["actors"]], [15, 15])
            scene["stages"][0]["clips"][1]["offset"]["y"] = -2
            with self.assertRaisesRegex(ValueError, "Preview mismatch: actor2"):
                audit_preview(root / "preview.blend", scene, root, 0)

    def test_saved_preview_has_linear_keys_and_matches_keyed_pose(self):
        identity = (0, 0, 0, 1)
        rig = Rig((Bone("Root", -1, identity, (0, 0, 0)),
                   Bone("COM", 0, identity, (0, 0, 1))), 2)
        samples = [[{"translation": [0, 0, 0], "rotation": identity},
                    {"translation": [0, y, 1], "rotation": identity}] for y in (0, .5, 1)]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "preview.blend"
            save_preview(path, rig, [{"name": "actor"}], [samples], [0, .5, 1], [{"x": 2}])
            bpy.ops.wm.open_mainfile(filepath=str(path))
            actor = bpy.data.objects["actor"]
            points = [key for layer in actor.animation_data.action.layers
                      for strip in layer.strips for bag in strip.channelbags
                      for curve in bag.fcurves for key in curve.keyframe_points]
            self.assertTrue(points)
            self.assertEqual({key.interpolation for key in points}, {"LINEAR"})
            bpy.context.scene.frame_set(8, subframe=.5)  # quarter-second, between authored keys
            world = actor.matrix_world @ actor.pose.bones["COM"].matrix
            for value, expected in zip(world.translation, (2, .25, 1)):
                self.assertAlmostEqual(value, expected, places=5)
