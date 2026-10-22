"""Offline Browser card contract checks: python extensions/browser/tools/test-snapshot-generators.py."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


library = load("generate-library-snapshot")
catalog = load("generate-catalog-snapshot")
RETIRED = {"stripActors", "lockPlayer", "inPlace", "curated"}


class SnapshotTests(unittest.TestCase):
    def test_library_defaults_keep_apparel_unless_explicitly_overridden(self):
        self.assertFalse(library.read_policy({"section": "library"})["hideApparel"])
        self.assertTrue(library.read_policy({})["hideApparel"])
        self.assertTrue(library.read_policy({"section": "library", "hideApparel": True})["hideApparel"])

    def test_library_content_aliases_produce_canonical_cards(self):
        defaults = {"clipRoot": "", "unlisted": False, "anchor": None, "pack": "Test",
                    "sourceFile": "test.osf.json", "sourceKind": "referenceAnimation",
                    **library.POLICY_DEFAULTS}
        scene = {"id": "test", "stages": [{"loops": 0, "clips": [{"file": "test.glb", "sec": 2}]}]}
        canonical = library.build_card({**scene, "hideApparel": False, "playerInputLock": False,
                                        "placement": "followActor"}, defaults, {})
        aliased = library.build_card({**scene, "stripActors": False, "lockPlayer": False,
                                      "inPlace": True}, defaults, {})
        self.assertEqual(canonical, aliased)
        self.assertFalse(RETIRED.intersection(aliased))
        self.assertEqual(aliased["sourceKind"], "referenceAnimation")
        self.assertEqual(aliased["estSec"], 4)

    def test_catalog_resolves_file_defaults_and_scene_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pack.osf.json"
            for defaults, overrides in (
                ({"stripActors": False, "lockPlayer": False, "inPlace": True}, {"playerInputLock": True}),
                ({"hideApparel": False, "playerInputLock": False, "placement": "followActor"}, {"lockPlayer": True}),
            ):
                with self.subTest(defaults=defaults):
                    path.write_text(json.dumps({"schema": 1, **defaults, "scenes": [
                        {"id": "test", **overrides, "stages": [{"clips": ["test.glb"]}]}]}), encoding="utf-8")
                    cards = []
                    self.assertEqual(catalog.build_file(str(path), {}, cards, set()), 1)
                    self.assertFalse(RETIRED.intersection(cards[0]))
                    self.assertEqual(cards[0]["sourceKind"], "authoredScene")
                    self.assertFalse(cards[0]["hideApparel"])
                    self.assertTrue(cards[0]["playerInputLock"])
                    self.assertEqual(cards[0]["placement"], "followActor")

    def test_conflicting_content_spellings_are_rejected(self):
        with self.assertRaises(ValueError):
            library.read_policy({"hideApparel": True, "stripActors": False})

    def test_committed_snapshots_use_current_browser_fields(self):
        fixtures = Path(__file__).resolve().parents[1] / "ui/fixtures/live"
        kinds = {"authoredScene", "curatedAnimation", "derivedDebugAnimation", "referenceAnimation"}
        for name in ("catalog", "library"):
            cards = json.loads((fixtures / (name + ".json")).read_text(encoding="utf-8"))
            self.assertTrue(cards)
            for card in cards:
                with self.subTest(fixture=name, scene=card["id"]):
                    self.assertFalse(RETIRED.intersection(card))
                    self.assertIn(card["sourceKind"], kinds)
                    self.assertIn(card["placement"], ("anchorAndPin", "followActor"))
                    self.assertIsInstance(card["hideApparel"], bool)
                    self.assertIsInstance(card["playerInputLock"], bool)


if __name__ == "__main__":
    unittest.main()
