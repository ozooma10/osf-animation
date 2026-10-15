"""Synthetic motion tests; execute with Blender --background --python this_file.

No game skeletons or animation assets are required. Normal Python discovery
skips this module when Blender's mathutils is unavailable.
"""

import copy
from pathlib import Path
import sys
from types import SimpleNamespace as Record
import unittest

try:
    from mathutils import Matrix, Quaternion
except ImportError as exc:
    raise unittest.SkipTest("Retargeting tests require Blender's Python runtime") from exc

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from retarget import HumanoidRetargeter, matrix, model_matrices
from target_rig import Bone, Rig


def rotation(axis, degrees):
    import math
    value = Quaternion(axis, math.radians(degrees))
    return (value.x, value.y, value.z, value.w)


IDENTITY = (0, 0, 0, 1)


def source_fixture(body_scale=1.0, root_rotation=IDENTITY):
    """A 100-units-per-meter source with differing pelvis/spine hierarchy."""
    definitions = [
        ("NPC Root [Root]", -1, (0, 0, 0)),
        ("NPC COM [COM ]", 0, (0, 0, 100)),
        ("NPC Pelvis [Pelv]", 1, (0, 0, -2)),
        ("NPC L Thigh [LThg]", 2, (-10, 0, -10)),
        ("NPC L Calf [LClf]", 3, (0, 0, -40)),
        ("NPC L Foot [Lft ]", 4, (0, 0, -40)),
        ("NPC R Thigh [RThg]", 2, (10, 0, -10)),
        ("NPC R Calf [RClf]", 6, (0, 0, -40)),
        ("NPC R Foot [Rft ]", 7, (0, 0, -40)),
        ("NPC Spine [Spn0]", 2, (0, 0, 10)),
        ("NPC Spine1 [Spn1]", 9, (0, 0, 15)),
        ("NPC Spine2 [Spn2]", 10, (0, 0, 15)),
        ("NPC Neck [Neck]", 11, (0, 0, 15)),
        ("NPC Head [Head]", 12, (0, 0, 15)),
        ("NPC L Hand [LHnd]", 11, (-40, 0, 0)),
        ("NPC R Hand [RHnd]", 11, (40, 0, 0)),
    ]
    return Record(
        bones=[name for name, _, _ in definitions],
        parents=[parent for _, parent, _ in definitions],
        reference_pose=[Record(translation=tuple(v * body_scale for v in tr),
                               rotation=root_rotation if index == 0 else IDENTITY,
                               scale=(1, 1, 1))
                        for index, (_, _, tr) in enumerate(definitions)],
    )


def target_fixture():
    definitions = [
        ("Root", -1, (0, 0, 0), IDENTITY),
        ("COM", 0, (0, 0, 1), IDENTITY),
        ("C_Hips", 1, (0, 0, -.02), IDENTITY),
        ("L_Thigh", 2, (-.1, 0, -.1), IDENTITY),
        ("L_Calf", 3, (0, 0, -.4), IDENTITY),
        ("L_Foot", 4, (0, 0, -.4), IDENTITY),
        ("R_Thigh", 2, (.1, 0, -.1), IDENTITY),
        ("R_Calf", 6, (0, 0, -.4), IDENTITY),
        ("R_Foot", 7, (0, 0, -.4), IDENTITY),
        ("C_Spine", 1, (0, 0, .08), IDENTITY),
        ("C_Spine1", 9, (0, 0, .15), IDENTITY),
        ("C_Spine2", 10, (0, 0, .15), IDENTITY),
        ("C_Chest", 11, (0, 0, .05), rotation((0, 1, 0), 15)),
        ("C_Neck", 12, (0, 0, .10), rotation((0, 1, 0), -15)),
        ("C_Neck1", 13, (0, 0, .05), IDENTITY),
        ("C_Head", 14, (0, 0, .10), IDENTITY),
        ("L_Wrist", 12, (-.4, 0, 0), IDENTITY),
        ("R_Wrist", 12, (.4, 0, 0), IDENTITY),
    ]
    return Rig(tuple(Bone(name, parent, rot, tr) for name, parent, tr, rot in definitions), len(definitions))


def animation_fixture(source):
    return Record(
        blend_hint=0, num_frames=2, duration=1.0, num_tracks=len(source.bones),
        track_to_bone_indices=list(range(len(source.bones))), bone_names=list(source.bones),
        tracks=[Record(translations=[pose.translation], rotations=[pose.rotation], scales=[pose.scale])
                for pose in source.reference_pose],
    )


class RetargetTests(unittest.TestCase):
    def assertMatrixClose(self, actual, expected, places=5):
        for row in range(4):
            for column in range(4):
                self.assertAlmostEqual(actual[row][column], expected[row][column], places=places)

    def converted(self, retargeter, animation):
        return retargeter.convert_frame(animation, retargeter.binding(animation), 1)

    def test_reference_pose_reproduces_every_target_local_bind_transform(self):
        source, rig = source_fixture(root_rotation=rotation((0, 0, 1), 90)), target_fixture()
        retargeter = HumanoidRetargeter(source, rig)
        for actual, bone in zip(self.converted(retargeter, animation_fixture(source)), rig.bones):
            with self.subTest(bone=bone.name):
                self.assertMatrixClose(matrix(actual["translation"], actual["rotation"], actual["scale"]),
                                       matrix(bone.translation, bone.rotation_xyzw))

    def test_extra_target_joints_do_not_double_source_rotations(self):
        source, rig = source_fixture(), target_fixture()
        retargeter = HumanoidRetargeter(source, rig)
        animation = animation_fixture(source)
        animation.tracks[source.bones.index("NPC Spine2 [Spn2]")].rotations = [rotation((0, 0, 1), 45)]
        animation.tracks[source.bones.index("NPC Neck [Neck]")].rotations = [rotation((1, 0, 0), 30)]
        output = self.converted(retargeter, animation)
        locals_ = [matrix(p["translation"], p["rotation"], p["scale"]) for p in output]
        models = model_matrices(locals_, [bone.parent for bone in rig.bones])
        indices = {bone.name: i for i, bone in enumerate(rig.bones)}
        # Chest and spine2 share one source; chest must retain only its bind offset.
        self.assertMatrixClose(locals_[indices["C_Chest"]], matrix((0, 0, .05), rotation((0, 1, 0), 15)))
        neck1 = locals_[indices["C_Neck1"]]
        self.assertMatrixClose(neck1, matrix((0, 0, .05), IDENTITY))
        expected = matrix((0, 0, 0), rotation((0, 0, 1), 45)) @ matrix((0, 0, 0), rotation((1, 0, 0), 30))
        actual = models[indices["C_Neck"]].copy()
        actual.translation = (0, 0, 0)
        self.assertMatrixClose(actual, expected)
        for pose, bone in zip(output, rig.bones):
            if bone.name != "COM":
                for actual_value, rest_value in zip(pose["translation"], bone.translation):
                    self.assertAlmostEqual(actual_value, rest_value, places=5)

    def test_com_displacement_converts_basis_and_units(self):
        source = source_fixture(root_rotation=rotation((0, 0, 1), 90))
        retargeter = HumanoidRetargeter(source, target_fixture())
        self.assertAlmostEqual(retargeter.unit_scale, .01, places=6)
        animation = animation_fixture(source)
        # Source's +X points along world +Y due to root rest rotation.
        animation.tracks[1].translations = [(20, 0, 100)]
        output = self.converted(retargeter, animation)
        for actual, expected in zip(output[1]["translation"], (.2, 0, 1)):
            self.assertAlmostEqual(actual, expected, places=5)
        self.assertEqual(output[0]["translation"], [0, 0, 0])

    def test_shared_scale_preserves_participant_spacing_across_body_sizes(self):
        source_a, source_b, rig = source_fixture(), source_fixture(body_scale=1.2), target_fixture()
        a = HumanoidRetargeter(source_a, rig)
        b = HumanoidRetargeter(source_b, rig, unit_scale=a.unit_scale)
        auto_b = HumanoidRetargeter(source_b, rig)
        self.assertAlmostEqual(auto_b.unit_scale, a.unit_scale / 1.2, places=6)
        for source, retargeter, offset in ((source_a, a, -50), (source_b, b, 50)):
            animation = animation_fixture(source)
            rest = source.reference_pose[1].translation
            animation.tracks[1].translations = [(rest[0] + offset, rest[1], rest[2])]
            out = self.converted(retargeter, animation)
            self.assertAlmostEqual(out[1]["translation"][0], offset * .01, places=5)

    def test_bind_posture_does_not_tilt_floor_offsets(self):
        source = source_fixture()
        source.reference_pose[source.bones.index("NPC Head [Head]")].translation = (0, 12, 15)
        retargeter = HumanoidRetargeter(source, target_fixture())
        from mathutils import Vector
        offset = retargeter.source_to_target @ Vector((0, -100, 0))
        self.assertAlmostEqual(offset.z, 0, places=6)
        self.assertAlmostEqual(offset.y, -100, places=5)

    def test_sexlab_positive_heading_faces_the_same_direction_in_osf(self):
        import math
        retargeter = HumanoidRetargeter(source_fixture(), target_fixture())
        out = retargeter.convert_offset({"y": 100, "z": 0, "heading": 90})
        self.assertAlmostEqual(out["y"], 1, places=5)
        self.assertAlmostEqual(out["z"], 0, places=5)
        self.assertEqual(out["heading"], -90)
        theta = math.radians(out["heading"])
        # OSF forward=(-sin(theta),cos(theta)); SexLab +90 faces +X.
        self.assertAlmostEqual(-math.sin(theta), 1, places=6)
        self.assertAlmostEqual(math.cos(theta), 0, places=6)

    def test_binding_requires_unique_complete_bounded_tracks(self):
        source = source_fixture()
        retargeter = HumanoidRetargeter(source, target_fixture())
        good = animation_fixture(source)
        by_name = copy.deepcopy(good)
        by_name.track_to_bone_indices = []
        self.assertEqual(retargeter.binding(by_name), list(range(len(source.bones))))
        mutations = (
            lambda a: (setattr(a, "track_to_bone_indices", []), setattr(a, "bone_names", [])),
            lambda a: a.track_to_bone_indices.__setitem__(1, 0),
            lambda a: a.track_to_bone_indices.__setitem__(0, len(source.bones)),
            lambda a: a.tracks.pop(),
            lambda a: setattr(a.tracks[0], "translations", []),
            lambda a: setattr(a, "blend_hint", 1),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                broken = copy.deepcopy(good)
                mutation(broken)
                with self.assertRaises(ValueError):
                    retargeter.binding(broken)
        duplicate_source = copy.deepcopy(source)
        duplicate_source.bones[1] = duplicate_source.bones[0].upper()
        with self.assertRaisesRegex(ValueError, "Duplicate source bone"):
            HumanoidRetargeter(duplicate_source, target_fixture())

    def test_authored_scale_compensation_is_rejected_instead_of_discarded(self):
        source = source_fixture()
        retargeter = HumanoidRetargeter(source, target_fixture())
        for scale in ((1.2, 1, 1), (-1, 1, 1), (0, 1, 1)):
            with self.subTest(scale=scale):
                animation = animation_fixture(source)
                animation.tracks[0].scales = [scale]
                with self.assertRaises(ValueError):
                    self.converted(retargeter, animation)


if __name__ == "__main__":
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(RetargetTests))
    if not result.wasSuccessful():
        raise RuntimeError("Retarget tests failed")
