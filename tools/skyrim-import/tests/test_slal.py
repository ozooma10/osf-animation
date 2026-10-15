import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from slal import SlalError, list_scene_summaries, load_scenes


class SlalTests(unittest.TestCase):
    def setUp(self):
        test_root = Path(__file__).resolve().parents[3] / "build/skyrim-import/test-slal"
        test_root.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=test_root)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.assertTrue(self.root.resolve().is_relative_to(test_root.resolve()))
        self.anim_dir = self.root / "meshes/actors/character/animations/Example"
        self.anim_dir.mkdir(parents=True)
        self.fnis = self.anim_dir / "FNIS_Example_List.txt"
        lines = ["Version V1.0", "' Generated fixture"]
        actors = []
        for actor in (1, 2):
            stages = []
            for stage in (1, 2):
                event = f"EX_Hello_A{actor}_S{stage}"
                filename = f"actual_{actor}_{stage}.hkx"
                (self.anim_dir / filename).write_bytes(b"synthetic HKX placeholder")
                prefix = "s -a" if stage == 1 else "+"
                lines.append(f"{prefix} {event} {filename}")
                stages.append({"id": event, "forward": 10 * actor,
                               "side": -2, "up": 3, "rotate": 180 if actor == 2 else 0})
            actors.append({"type": "Female" if actor == 1 else "Male", "stages": stages})
        self.fnis.write_text("\n".join(lines), encoding="utf-8")
        self.data = {"name": "Example Pack", "animations": [{
            "id": "EX_Hello", "name": "Hello", "actors": actors,
            "tags": "Pair, Greeting", "stages": [{"number": 1, "timer": 1.8}]}]}
        self.json = self.root / "SLAnims/json/Example.json"
        self.json.parent.mkdir(parents=True)
        self.save()

    def save(self):
        self.json.write_text(json.dumps(self.data), encoding="utf-8")

    def load(self, **kwargs):
        return load_scenes(self.json, self.root, **kwargs)

    def test_generated_schema_event_aliases_roles_and_timing(self):
        plan, = self.load()
        self.assertEqual(plan["roles"], [{"name": "actor1", "gender": "female"},
                                         {"name": "actor2", "gender": "male"}])
        self.assertEqual(plan["stages"][0]["timer"], 1.8)
        self.assertEqual(plan["stages"][1]["loops"], 0)
        self.assertEqual(Path(plan["stages"][1]["clips"][1]).name, "actual_2_2.hkx")
        self.assertEqual(plan["stages"][0]["offsets"][1], {"x": -2, "y": 20, "z": 3, "heading": 180})
        self.assertEqual(plan["source"]["offset_units"], "skyrim")
        self.assertEqual(len(plan["source"]["inputs"]), 6)
        self.assertEqual(plan["tags"], ["Pair", "Greeting"])

    def test_no_source_script_execution(self):
        script = self.json.with_suffix(".py")
        script.write_text("raise RuntimeError('must never run')", encoding="utf-8")
        with self.assertRaisesRegex(SlalError, "source scripts are not executed"):
            load_scenes(script, self.root)

    def test_source_behavior_is_preserved_and_reported(self):
        entry = self.data["animations"][0]
        entry["sound"] = "NoSound"
        entry["actors"][0]["stages"][0]["open_mouth"] = True
        self.save()
        plan, = self.load()
        self.assertEqual(len(plan["source"]["warnings"]), 2)
        self.assertEqual(plan["source"]["metadata"], entry)

    def test_unequal_stages_rejected(self):
        self.data["animations"][0]["actors"][1]["stages"].pop()
        self.save()
        with self.assertRaisesRegex(SlalError, "unequal stage counts"):
            self.load()

    def test_missing_event_and_missing_file_rejected(self):
        original = self.fnis.read_text()
        self.fnis.write_text(original.replace("EX_Hello_A1_S1", "different_event"))
        with self.assertRaisesRegex(SlalError, "no FNIS mapping"):
            self.load()
        self.fnis.write_text(original)
        (self.anim_dir / "actual_1_1.hkx").unlink()
        with self.assertRaisesRegex(SlalError, "missing HKX"):
            self.load()

    def test_conflicting_events_rejected(self):
        self.fnis.write_text(self.fnis.read_text() + "\nb EX_Hello_A1_S1 actual_2_2.hkx")
        with self.assertRaisesRegex(SlalError, "Conflicting FNIS event"):
            self.load()

    def test_attached_props_rejected(self):
        self.fnis.write_text(self.fnis.read_text().replace(
            "s -a EX_Hello_A1_S1 actual_1_1.hkx", "s -o EX_Hello_A1_S1 actual_1_1.hkx Chair"))
        with self.assertRaisesRegex(SlalError, "unsupported animation object"):
            self.load()

    def test_clip_path_traversal_rejected(self):
        self.fnis.write_text(self.fnis.read_text().replace("actual_1_1.hkx", "..\\outside.hkx"))
        with self.assertRaisesRegex(SlalError, "Unsafe FNIS clip path"):
            self.load()

    def test_selected_human_scene_in_mixed_pack(self):
        creature = copy.deepcopy(self.data["animations"][0])
        creature["id"] = "Other"
        creature["actors"][0]["type"] = "CreatureFemale"
        self.data["animations"].append(creature)
        self.save()
        self.assertEqual(len(self.load(scene_id="EX_Hello")), 1)
        with self.assertRaisesRegex(SlalError, "not a supported human"):
            self.load()

    def test_listing_mixed_pack_needs_no_animation_files(self):
        creature = copy.deepcopy(self.data["animations"][0])
        creature["id"] = "Creature"
        creature["actors"][0]["type"] = "CreatureFemale"
        self.data["animations"].extend([creature, {"id": "Broken", "error": "missing source clip"}, None])
        self.save()
        self.fnis.unlink()
        for clip in self.anim_dir.glob("*.hkx"):
            clip.unlink()
        summaries = list_scene_summaries(self.json)
        self.assertEqual([s["status"] for s in summaries], ["candidate", "unsupported", "invalid", "invalid"])
        self.assertEqual(summaries[0]["roles"], 2)
        self.assertEqual(summaries[0]["stages"], 2)
        self.assertIn("validation pending", summaries[0]["reason"])
        self.assertIn("SLAL generator error", summaries[2]["reason"])

    def test_listing_marks_duplicate_ids_but_can_select_other_scene(self):
        duplicate = copy.deepcopy(self.data["animations"][0])
        duplicate["id"] = "Duplicate"
        self.data["animations"].extend([duplicate, copy.deepcopy(duplicate), None])
        self.save()
        summaries = list_scene_summaries(self.json)
        self.assertEqual([s["status"] for s in summaries], ["candidate", "invalid", "invalid", "invalid"])
        self.assertEqual(summaries[1]["reason"], "Duplicate animation id")
        self.assertEqual(len(self.load(scene_id="EX_Hello")), 1)
        with self.assertRaisesRegex(SlalError, "Duplicate animation id"):
            self.load(scene_id="Duplicate")

    def test_listing_does_not_execute_source_scripts(self):
        script = self.json.with_suffix(".py")
        script.write_text("raise RuntimeError('must never run')", encoding="utf-8")
        with self.assertRaisesRegex(SlalError, "source scripts are not executed"):
            list_scene_summaries(script)

    def test_listing_malformed_role_type_does_not_hide_valid_scene(self):
        malformed = copy.deepcopy(self.data["animations"][0])
        malformed["id"] = "MalformedRole"
        malformed["actors"][0]["type"] = {"unexpected": "object"}
        self.data["animations"].append(malformed)
        self.save()
        self.assertEqual([s["status"] for s in list_scene_summaries(self.json)], ["candidate", "unsupported"])
        self.assertEqual(len(self.load(scene_id="EX_Hello")), 1)

    def test_unmapped_global_stage_and_pack_behavior_reported(self):
        self.data["custom_default"] = {"lock_actor": True}
        self.data["animations"][0]["stages"][0].update(sound="NoSound", custom_callback="test")
        self.save()
        plan, = self.load()
        self.assertEqual(plan["source"]["warnings"], [
            "Not converted: pack: custom_default", "Not converted: stage 1: custom_callback",
            "Not converted: stage 1: sound"])
        self.assertEqual(plan["source"]["pack_metadata"]["custom_default"], {"lock_actor": True})

    def test_nonfinite_offset_and_invalid_timer_rejected(self):
        entry = self.data["animations"][0]
        entry["actors"][0]["stages"][0]["side"] = float("nan")
        self.save()
        with self.assertRaisesRegex(SlalError, "finite number"):
            self.load()
        entry["actors"][0]["stages"][0]["side"] = 0
        entry["stages"][0]["timer"] = -1
        self.save()
        with self.assertRaisesRegex(SlalError, "non-negative"):
            self.load()

    def test_zero_timer_retains_manual_loop(self):
        self.data["animations"][0]["stages"][0]["timer"] = 0
        self.save()
        plan, = self.load()
        self.assertEqual(plan["stages"][0]["loops"], 0)
        self.assertNotIn("timer", plan["stages"][0])


if __name__ == "__main__":
    unittest.main()
