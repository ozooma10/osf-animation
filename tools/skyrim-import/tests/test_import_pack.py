"""CLI orchestration checks using synthetic files; Blender is never launched."""
import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import import_pack


class ImportPackTests(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).resolve().parents[3] / "build/skyrim-import/test-cli"
        test_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=test_root)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.assertTrue(self.root.resolve().is_relative_to(test_root.resolve()))
        self.output = self.root / "converted"
        self.manifest = self.root / "plan.json"

    def fixture(self):
        """Only file presence is needed: validation must precede decoder work."""
        for name in ("source.hkx", "clip.hkx", "target.rig", "blender.exe"):
            (self.root / name).write_bytes(b"synthetic placeholder")
        addon = self.root / "addon/hkx"
        addon.mkdir(parents=True)
        (addon / "anim_skyrim.py").write_text("raise AssertionError('Never import this fixture')\n")
        plan = {
            "schema": 1, "id": "example.motion", "name": "Example motion", "pack": "Example pack",
            "roles": [{"name": "actor1", "gender": "female", "skeleton": str(self.root / "source.hkx")}],
            "stages": [{"clips": [str(self.root / "clip.hkx")], "loops": 0}],
        }
        return plan

    def arguments(self, plan):
        self.manifest.write_text(json.dumps(plan), encoding="utf-8")
        return import_pack.parser().parse_args([
            "--manifest", str(self.manifest), "--output", str(self.output),
            "--target-rig", str(self.root / "target.rig"),
            "--pynifly", str(self.root / "addon"), "--blender", str(self.root / "blender.exe"),
        ])

    def test_duplicate_role_names_fail_before_process_or_staging(self):
        plan = self.fixture()
        plan["roles"].append(copy.deepcopy(plan["roles"][0]))
        plan["stages"][0]["clips"] *= 2
        with patch.object(import_pack.subprocess, "run") as process:
            with self.assertRaisesRegex(ValueError, "Role names must be unique"):
                import_pack.run(self.arguments(plan))
        process.assert_not_called()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".converted.partial-*")))

    def test_unsafe_scene_ids_and_nonfinite_plan_values_fail_before_process(self):
        valid = self.fixture()
        invalid = []
        for identifier in ("..", "../escape", r"nested\escape", "C:escape", "a/b"):
            plan = copy.deepcopy(valid)
            plan["id"] = identifier
            invalid.append(plan)
        for field, value in (("timer", float("nan")), ("timer", float("inf")),
                             ("loops", float("-inf")), ("loops", 1.5)):
            plan = copy.deepcopy(valid)
            plan["stages"][0][field] = value
            invalid.append(plan)
        for value in (float("nan"), float("inf"), True):
            plan = copy.deepcopy(valid)
            plan["stages"][0]["offsets"] = [{"x": value}]
            invalid.append(plan)
        with patch.object(import_pack.subprocess, "run") as process:
            for index, plan in enumerate(invalid):
                with self.subTest(case=index), self.assertRaises(ValueError):
                    import_pack.run(self.arguments(plan))
        process.assert_not_called()
        self.assertFalse(self.output.exists())
        self.assertFalse(list(self.root.glob(".converted.partial-*")))

    def test_list_mixed_pack_needs_only_metadata_and_keeps_unsupported_rows(self):
        metadata = self.root / "mixed.json"
        metadata.write_text(json.dumps({"name": "Mixed pack", "animations": [
            {"id": "Human", "name": "Human scene", "actors": [
                {"type": "Female", "stages": [{"id": "missing_animation_event"}]}]},
            {"id": "Creature", "name": "Creature scene", "actors": [
                {"type": "Creature", "stages": [{"id": "missing_creature_event"}]}]},
            "malformed entry",
        ]}), encoding="utf-8")
        args = import_pack.parser().parse_args(["--slal-json", str(metadata), "--list"])
        stdout = io.StringIO()
        with patch.object(import_pack.subprocess, "run") as process, contextlib.redirect_stdout(stdout):
            self.assertEqual(import_pack.run(args), 0)
        process.assert_not_called()
        text = stdout.getvalue()
        self.assertIn("Human: Human scene (1 roles, 1 stages) [candidate]", text)
        self.assertIn("Creature: Creature scene (1 roles, 1 stages) [unsupported]", text)
        self.assertIn("[invalid] Animation entry is not an object", text)
        self.assertEqual(list(self.root.iterdir()), [metadata])

    def test_existing_output_is_refused_and_remains_untouched(self):
        plan = self.fixture()
        self.output.mkdir()
        sentinel = self.output / "user-file.txt"
        sentinel.write_bytes(b"existing user output")
        args = self.arguments(plan)
        with patch.object(import_pack.subprocess, "run") as process:
            with self.assertRaisesRegex(ValueError, "Output already exists"):
                import_pack.run(args)
        process.assert_not_called()
        self.assertEqual(sentinel.read_bytes(), b"existing user output")
        self.assertEqual(list(self.output.iterdir()), [sentinel])
        self.assertFalse(list(self.root.glob(".converted.partial-*")))

    def test_failed_blender_retains_diagnostic_log_without_publishing_output(self):
        args = self.arguments(self.fixture())
        source_paths = [self.manifest, self.root / "source.hkx", self.root / "clip.hkx"]
        original_bytes = {path: path.read_bytes() for path in source_paths}
        stdout, stderr = io.StringIO(), io.StringIO()

        def fail(command, *, stdout, stderr, env, timeout):
            stdout.write("Synthetic Blender decoder failure\n")
            self.assertIn("--background", command)
            self.assertIn("--disable-autoexec", command)
            self.assertEqual(env["PYTHONDONTWRITEBYTECODE"], "1")
            return subprocess.CompletedProcess(command, 7)

        with patch.object(import_pack.subprocess, "run", side_effect=fail) as process:
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                with self.assertRaisesRegex(RuntimeError, "Blender exited with 7"):
                    import_pack.run(args)
        process.assert_called_once()
        self.assertFalse(self.output.exists())
        staging, = self.root.glob(".converted.partial-*")
        self.assertEqual((staging / "blender.log").read_text(), "Synthetic Blender decoder failure\n")
        job = json.loads((staging / "job.json").read_text())
        self.assertEqual(Path(job["output"]), staging)
        self.assertIn(str(staging), stderr.getvalue())
        self.assertTrue(all(path.read_bytes() == previous for path, previous in original_bytes.items()))


if __name__ == "__main__":
    unittest.main()
