"""Decoded source screening, including failures late in a multi-actor scene."""
import copy
from pathlib import Path
from types import SimpleNamespace as Record
import tempfile
import unittest

from preflight import inspect_animation, inspect_plan


def animation():
    return Record(num_frames=3, duration=1., num_tracks=1, track_to_bone_indices=[0], bone_names=["hip"],
                  tracks=[Record(translations=[(0, 0, 0)] * 3, rotations=[(0, 0, 0, 1)] * 3,
                                 scales=[(1, 1, 1)] * 3)])


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.converter = Record(source=Record(bones=["hip"]), binding=lambda a: [0])

    def test_reports_late_non_unit_and_negative_scales_without_mutation(self):
        anim = animation()
        anim.tracks[0].scales[1:] = [(1.1, 1, 1), (-1, 1, 1)]
        before = copy.deepcopy(anim)
        report = inspect_animation(anim, self.converter)
        self.assertEqual(report["scales"]["range"], [-1, 1.1])
        self.assertEqual(report["scales"]["non_unit_samples"], 2)
        self.assertEqual(report["scales"]["non_positive_samples"], 1)
        self.assertEqual(report["scales"]["examples"][1]["sample"], 2)
        self.assertEqual(report["scales"]["examples"][0]["bone"], "hip")
        self.assertEqual(len(report["errors"]), 2)
        self.assertEqual(anim, before)

    def test_malformed_channels_are_rejected_even_if_scales_are_unit(self):
        for field, value in (("translations", (0, 0, float("nan"))),
                             ("rotations", (0, 0, 0, 0)), ("scales", (1, 1))):
            with self.subTest(field=field):
                anim = animation()
                getattr(anim.tracks[0], field)[-1] = value
                self.assertTrue(inspect_animation(anim, self.converter)["errors"])

    def test_binding_rejection_is_retained_with_scale_diagnostics(self):
        def reject(_):
            raise ValueError("Unsupported skeleton binding")
        self.converter.binding = reject
        anim = animation()
        anim.tracks[0].scales[0] = (2, 2, 2)
        result = inspect_animation(anim, self.converter)
        self.assertIsNone(result["binding_mode"])
        self.assertEqual(result["errors"][0], "Unsupported skeleton binding")
        self.assertEqual(result["scales"]["examples"][0]["bone"], "track 0")

    def test_all_clips_checked_and_duration_mismatch_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / f"clip{i}.hkx" for i in range(4)]
            for path in paths:
                path.write_bytes(b"synthetic")
            plan = {"id": "example", "stages": [{"clips": list(map(str, paths[:2]))},
                                                 {"clips": list(map(str, paths[2:]))}]}
            calls = []
            def decode(path):
                calls.append(path)
                if path == str(paths[0]):
                    raise ValueError("broken HKX")
                anim = animation()
                if path == str(paths[3]):
                    anim.duration = 2
                    anim.tracks[0].scales[-1] = (.9, 1, 1)
                return anim
            result = inspect_plan(plan, Record(load_skyrim_animation=decode), [self.converter] * 2)
            self.assertEqual(calls, list(map(str, paths)))
            self.assertEqual(result["status"], "unsupported")
            self.assertIn("broken HKX", result["clips"][0]["errors"][0])
            self.assertTrue(result["clips"][3]["errors"])
            self.assertIn("Stage 2: participant clip lengths differ", result["errors"][0])
            self.assertTrue(all(len(row["sha256"]) == 64 for row in result["clips"]))

    def test_unit_scale_clip_is_compatible(self):
        result = inspect_animation(animation(), self.converter)
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["scales"]["non_unit_samples"], 0)
        self.assertEqual(result["binding_mode"], "explicit")
