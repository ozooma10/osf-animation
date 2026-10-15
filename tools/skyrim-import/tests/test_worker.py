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


class PreviewTests(unittest.TestCase):
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
