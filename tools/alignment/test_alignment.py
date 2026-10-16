import copy
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import alignment as tool


def offset(x=0, y=0, z=0, heading=0):
    return dict(x=x, y=y, z=z, heading=heading)


def fixture():
    document = {"schema": 1, "id": "test.hug", "hideApparel": False,
                "roles": [{"name": "first", "offset": {"x": .1}}, {"name": "second"}],
                "stages": [{"loops": 0, "clips": ["a.glb", {"file": "b.glb", "mask": "upper"}]}]}
    scene = {"id": "test.hug", "node": "stage0", "linearStage": 0, "anchored": True,
             "target": {"handle": 1, "playback": 2, "revision": 3},
             "participants": [
                 {"role": "first", "file": "a.glb", "authored": offset(x=.1),
                  "offset": offset(y=.02), "effective": offset(x=.1, y=.02)},
                 {"role": "second", "file": "b.glb", "authored": offset(),
                  "offset": offset(heading=2), "effective": offset(heading=2)}]}
    return document, scene


class FakeClient(tool.Client):
    def __init__(self, directory, scene):
        super().__init__(directory)
        self.scene = copy.deepcopy(scene)
        self.commands = []

    def request(self, op, **fields):
        self.session = "process-1"
        self.commands.append((op, fields))
        return {"ok": True, "scenes": [copy.deepcopy(self.scene)]}


class SaveTests(unittest.TestCase):
    def test_read_retries_atomic_publication_sharing_violation(self):
        with patch.object(Path, "read_text", side_effect=[PermissionError("sharing"), '{"ok":true}']) as read:
            self.assertEqual(tool.read_json("response.json"), {"ok": True})
            self.assertEqual(read.call_count, 2)

    def test_stop_and_camera_use_exact_selected_target(self):
        unused, scene = fixture()
        with tempfile.TemporaryDirectory() as directory:
            client = FakeClient(directory, scene)
            client.attach("test.hug")
            client.camera(90, 10, 2, 1.2)
            self.assertEqual(client.commands[-1], ("camera", dict(target=scene["target"], view=[90, 10, 2, 1.2])))
            client.stop()
            self.assertEqual(client.commands[-1], ("stop", dict(target=scene["target"])))
            client.scene["target"]["revision"] += 1
            with self.assertRaisesRegex(ValueError, "stage changed"):
                client.stop()

    def test_effective_offsets_preserve_scene_fields(self):
        document, scene = fixture()
        result = tool.prepare_scene_save(document, scene)
        self.assertEqual(result["stages"][0]["clips"][0], {"file": "a.glb", "offset": offset(x=.1, y=.02)})
        self.assertEqual(result["stages"][0]["clips"][1]["mask"], "upper")
        self.assertEqual(result["roles"], document["roles"])
        self.assertFalse(result["hideApparel"])
        self.assertEqual(document["stages"][0]["clips"][0], "a.glb")

    def test_repeated_save_does_not_accumulate(self):
        document, scene = fixture()
        first = tool.prepare_scene_save(document, scene)
        second = tool.prepare_scene_save(first, scene, [a["effective"] for a in scene["participants"]])
        self.assertEqual(first, second)

    def test_multi_scene_templates_and_clip_root(self):
        document, scene = fixture()
        document["roles"] = ["template", "empty"]
        container = {"schema": 1, "clipRoot": "OSF/Test", "roles": {"template": {"offset": {"x": .1}}, "empty": {}},
                     "scenes": [{"id": "unrelated", "clip": "other.glb"}, document]}
        for actor in scene["participants"]:
            actor["file"] = "OSF/Test/" + actor["file"]
        result = tool.prepare_scene_save(container, scene)
        self.assertEqual(result["scenes"][0], container["scenes"][0])
        self.assertEqual(result["roles"], container["roles"])

    def test_wrong_source_or_old_offsets_rejected(self):
        document, scene = fixture()
        for mutation in (lambda d: d.update(id="wrong"),
                         lambda d: d["roles"][0]["offset"].update(x=.2),
                         lambda d: d["stages"][0]["clips"].__setitem__(0, "wrong.glb"),
                         lambda d: d["stages"][0]["clips"].pop()):
            changed = copy.deepcopy(document)
            mutation(changed)
            with self.assertRaises(ValueError):
                tool.prepare_scene_save(changed, scene)

    def test_clip_offset_overrides_role_offset(self):
        document, scene = fixture()
        document["stages"][0]["clips"][0] = {"file": "a.glb", "offset": {"y": .3}}
        scene["participants"][0]["authored"] = offset(y=.3)
        scene["participants"][0]["effective"] = offset(y=.32)
        result = tool.prepare_scene_save(document, scene)
        self.assertEqual(result["stages"][0]["clips"][0]["offset"], offset(y=.32))

    def test_direct_nodes_and_indirect_export_only(self):
        document, scene = fixture()
        document["nodes"] = [{"id": "hug", "clips": document.pop("stages")[0]["clips"]}]
        scene.update(node="hug", linearStage=-1)
        self.assertIn("offset", tool.prepare_scene_save(document, scene)["nodes"][0]["clips"][0])
        document["nodes"][0]["use"] = "another.scene"
        with self.assertRaises(ValueError):
            tool.prepare_scene_save(document, scene)

    def test_save_backup_and_refusal_of_external_edit(self):
        document, scene = fixture()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "hug.osf.json"
            tool.atomic_json(source, document)
            before = source.read_bytes()
            client = FakeClient(directory, scene)
            client.attach("test.hug", source)
            result = client.save()
            self.assertEqual(Path(result["backup"]).read_bytes(), before)
            self.assertFalse(Path(result["backup"]).name.endswith(".osf.json"))
            saved = source.read_bytes()
            client.save()
            self.assertEqual(source.read_bytes(), saved)
            source.write_text('{"external":true}')
            with self.assertRaisesRegex(ValueError, "changed since"):
                client.save()
            self.assertEqual(source.read_text(), '{"external":true}')

    def test_exact_target_required_after_restart(self):
        unused, scene = fixture()
        with tempfile.TemporaryDirectory() as directory:
            client = FakeClient(directory, scene)
            client.attach("test.hug")
            client.scene["target"]["revision"] += 1
            with self.assertRaisesRegex(ValueError, "stage changed"):
                client.adjust("first", {"y": .01}, True)
            self.assertTrue(all(op == "status" for op, unused in client.commands))

    def test_nudge_preserves_other_actor_and_axes(self):
        unused, scene = fixture()
        with tempfile.TemporaryDirectory() as directory:
            client = FakeClient(directory, scene)
            client.attach("test.hug")
            client.adjust("first", {"y": .01, "x": None}, True)
            op, args = client.commands[-1]
            self.assertEqual(op, "set")
            self.assertAlmostEqual(args["offsets"][0]["y"], .03)
            self.assertEqual(args["offsets"][1], scene["participants"][1]["offset"])
            client.adjust("first", {"y": .01}, False)
            self.assertEqual(client.commands[-1][1]["offsets"][0]["y"], .01)


class TransportTests(unittest.TestCase):
    def test_atomic_command_response_and_matching_sequence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tool.atomic_json(root / "ready.json", {"schema": 1, "enabled": True, "session": "current",
                                                   "updatedAtUtcMs": int(time.time() * 1000)})
            tool.atomic_json(root / "response.json", {"sequence": 1, "session": "old", "ok": False})
            seen = []

            def server():
                for unused in range(200):
                    if (root / "command.json").exists():
                        request = tool.read_json(root / "command.json")
                        seen.append(request)
                        tool.atomic_json(root / "response.json", {"sequence": request["sequence"],
                            "session": request["session"], "ok": True, "scenes": []})
                        return
                    time.sleep(.01)

            thread = threading.Thread(target=server)
            thread.start()
            try:
                with tool.client_lock(root):
                    response = tool.Client(root, timeout=2).request("status")
                self.assertTrue(response["ok"])
                self.assertEqual(seen[0]["op"], "status")
                self.assertGreater(seen[0]["expiresAtUtcMs"], time.time() * 1000)
                self.assertFalse(list(root.glob("*.tmp")))
            finally:
                thread.join()

    def test_inactive_bridge_rejected_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tool.atomic_json(root / "ready.json", {"enabled": False})
            with self.assertRaisesRegex(ValueError, "inactive"):
                tool.Client(root).request("status")
            self.assertFalse((root / "command.json").exists())


if __name__ == "__main__":
    unittest.main()
