"""Local client for OSF's opt-in live alignment interface (Python 3.10+)."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time
import uuid


def read_json(path):
    # ReplaceFileW can briefly deny opens while publishing a complete response.
    deadline = time.monotonic() + 2
    while True:
        try:
            return json.loads(Path(path).read_text(encoding="utf-8-sig"))
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.01)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        deadline = time.monotonic() + 2
        while True:
            try:
                os.replace(temporary, path)
                return
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.02)
    finally:
        temporary.unlink(missing_ok=True)


def default_directory():
    if os.name == "nt":
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders") as key:
            documents = Path(os.path.expandvars(winreg.QueryValueEx(key, "Personal")[0]))
    else:
        documents = Path.home() / "Documents"
    return documents / "My Games/Starfield/OSF/Alignment"


@contextmanager
def client_lock(directory):
    """Serialize whole read/modify/write operations between CLI instances."""
    directory = Path(directory)
    if not directory.is_dir():
        raise ValueError('Enable the bridge first: cgf "OSFAdvanced.SetAlignmentToolsEnabled" true')
    with (directory / "client.lock").open("a+b") as lock:
        if lock.tell() == 0:
            lock.write(b"\0")
            lock.flush()
        lock.seek(0)
        if os.name == "nt":
            import msvcrt
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise ValueError("Another alignment client is busy") from error
            try:
                yield
            finally:
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                yield
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)


class Client:
    def __init__(self, directory, timeout=5):
        self.directory = Path(directory)
        self.timeout = timeout
        self.session = None

    def request(self, op, **fields):
        ready = read_json(self.directory / "ready.json")
        if not ready.get("enabled") or abs(time.time() * 1000 - ready.get("updatedAtUtcMs", 0)) > 5000:
            raise ValueError('Alignment bridge is inactive; enable it in the running game')
        self.session = ready["session"]
        sequence = time.time_ns()
        response_file = self.directory / "response.json"
        if response_file.exists():
            sequence = max(sequence, read_json(response_file).get("sequence", 0) + 1)
        message = dict(schema=1, session=self.session, sequence=sequence, op=op,
                       expiresAtUtcMs=int((time.time() + self.timeout) * 1000), **fields)
        atomic_json(self.directory / "command.json", message)
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            try:
                response = read_json(response_file)
                if response.get("session") == self.session and response.get("sequence") == sequence:
                    if not response.get("ok"):
                        raise ValueError(response.get("error", "Alignment command rejected"))
                    return response
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(.05)
        raise TimeoutError("Game did not acknowledge the command; check that gameplay is running")

    @property
    def binding_file(self):
        return self.directory / "client-state.json"

    def current(self):
        binding = read_json(self.binding_file)
        response = self.request("status")
        if binding["session"] != self.session:
            raise ValueError("Game/bridge restarted; attach again")
        matches = [s for s in response["scenes"] if s["target"] == binding["target"]]
        if len(matches) != 1:
            raise ValueError("Selected scene or stage changed; attach again")
        return binding, matches[0]

    def attach(self, scene_id, scene_file=None):
        response = self.request("status")
        matches = [s for s in response["scenes"] if s["id"] == scene_id]
        if len(matches) != 1:
            raise ValueError(f"Expected one active scene {scene_id!r}; found {len(matches)}")
        scene = matches[0]
        if not scene["anchored"]:
            raise ValueError("Live alignment requires anchorAndPin placement")
        binding = {"session": self.session, "target": scene["target"], "id": scene_id}
        if scene_file:
            path = Path(scene_file).resolve(strict=True)
            if not path.name.lower().endswith(".osf.json"):
                raise ValueError("Scene source must be an .osf.json file")
            source_hash = digest(path)
            prepare_scene_save(read_json(path), scene)
            if digest(path) != source_hash:
                raise ValueError("Scene source changed while attaching")
            binding.update(scene_file=str(path), source_sha256=source_hash)
        atomic_json(self.binding_file, binding)
        return scene

    def adjust(self, role, components, relative=False):
        binding, scene = self.current()
        matches = [i for i, actor in enumerate(scene["participants"]) if actor["role"] == role]
        if len(matches) != 1:
            raise ValueError(f"Role {role!r} must identify exactly one participant")
        offsets = [deepcopy(actor["offset"]) for actor in scene["participants"]]
        offset = offsets[matches[0]]
        for component, value in components.items():
            if value is not None:
                if not math.isfinite(value):
                    raise ValueError("Offsets must be finite")
                offset[component] = offset[component] + value if relative else value
        return self.request("set", target=binding["target"], offsets=offsets)

    def speed(self, value):
        binding, unused = self.current()
        return self.request("set", target=binding["target"], speed=value)

    def reset(self):
        binding, unused = self.current()
        return self.request("reset", target=binding["target"])

    def stop(self):
        binding, unused = self.current()
        return self.request("stop", target=binding["target"])

    def camera(self, azimuth, elevation, radius, height):
        binding, unused = self.current()
        return self.request("camera", target=binding["target"], view=[azimuth, elevation, radius, height])

    def export(self, path):
        binding, scene = self.current()
        preset = {"schema": 1, "kind": "osf-alignment-preset", "scene": scene}
        atomic_json(Path(path), preset)
        return {"exported": str(Path(path).resolve()), "scene": scene["id"]}

    def save(self):
        binding, scene = self.current()
        if "scene_file" not in binding:
            raise ValueError("Attach with --scene-file to save into a source; export works for any scene")
        path = Path(binding["scene_file"])
        if digest(path) != binding["source_sha256"]:
            raise ValueError("Scene source changed since attach/save; no file was overwritten")
        document = prepare_scene_save(read_json(path), scene, binding.get("saved_effective"))
        # Backup only after validation, outside the *.osf.json discovery pattern.
        backup = path.with_name(path.name + ".alignment-backup-" + uuid.uuid4().hex + ".json")
        backup.write_bytes(path.read_bytes())
        if digest(backup) != binding["source_sha256"] or digest(path) != binding["source_sha256"]:
            raise ValueError("Scene source changed during save; no file was overwritten")
        atomic_json(path, document)
        binding["source_sha256"] = digest(path)
        binding["saved_effective"] = [actor["effective"] for actor in scene["participants"]]
        atomic_json(self.binding_file, binding)
        return {"saved": str(path), "backup": str(backup), "scene": scene["id"],
                "note": "Current live adjustment remains active. Reload content before the next scene start."}


def resource_key(value, root=""):
    value = value.replace("\\", "/")
    if value.lower().startswith("naf:"):
        value = value[4:]
    # Animation selectors do not change the clip's resource path.
    value = value.split(":", 1)[0]
    return (root.rstrip("/\\") + "/" + value if root else value).replace("\\", "/").casefold()


def role_offset(document, scene, index):
    roles = scene.get("roles", document.get("roles", []))
    if not isinstance(roles, list) or index >= len(roles):
        return {}
    role = roles[index]
    templates = document.get("roles", {})
    if isinstance(role, str) or isinstance(role, dict) and "id" in role:
        key = role if isinstance(role, str) else role["id"]
        if not isinstance(templates, dict) or key not in templates:
            raise ValueError("Unable to resolve role template for source validation")
        result = deepcopy(templates[key].get("offset", {}))
        if isinstance(role, dict) and "offset" in role:
            if role["offset"] is None:
                result = {}
            else:
                result.update(role["offset"])
        return result
    return role.get("offset", {}) if isinstance(role, dict) else {}


def prepare_scene_save(document, snapshot, previously_saved=None):
    """Apply effective placement to the current direct clip list, retaining all other content."""
    result = deepcopy(document)
    candidates = result.get("scenes", [result])
    matches = [scene for scene in candidates if scene.get("id") == snapshot["id"]]
    if len(matches) != 1:
        raise ValueError("Source must contain the exact active scene ID once")
    scene = matches[0]
    if "stages" in scene:
        index = snapshot["linearStage"]
        if not isinstance(index, int) or not 0 <= index < len(scene["stages"]):
            raise ValueError("Active linear stage does not match the source")
        container, field = scene["stages"][index], "clips"
    elif "clip" in scene:
        container, field = scene, "clip"
    else:
        matches = [node for node in scene.get("nodes", []) if node.get("id") == snapshot["node"]]
        if len(matches) != 1 or "clips" not in matches[0] or "use" in matches[0]:
            raise ValueError("Saving requires direct clips; export a preset for indirect/reused nodes")
        container, field = matches[0], "clips"
    value = container[field]
    clips = value if isinstance(value, list) else [value]
    actors = snapshot["participants"]
    if len(clips) != len(actors):
        raise ValueError("Source and active scene have different participant counts")
    root = scene.get("clipRoot", result.get("clipRoot", ""))
    output = []
    for index, (clip, actor) in enumerate(zip(clips, actors)):
        spec = {"file": clip} if isinstance(clip, str) else deepcopy(clip)
        if resource_key(spec["file"], root) != resource_key(actor["file"]):
            raise ValueError(f"Clip mismatch at role {actor['role']}; source does not match running scene")
        inherited = spec.get("offset", role_offset(result, scene, index)) or {}
        expected = previously_saved[index] if previously_saved else actor["authored"]
        if any(abs(float(inherited.get(key, 0) or 0) - expected[key]) > .0001 for key in ("x", "y", "z", "heading")):
            raise ValueError("Authored offsets do not match the loaded scene; reload/restart and attach again")
        effective = actor["effective"]
        if any(not math.isfinite(effective[key]) for key in ("x", "y", "z", "heading")):
            raise ValueError("Nonfinite effective placement")
        spec["offset"] = {key: round(effective[key], 7) for key in ("x", "y", "z", "heading")}
        output.append(spec)
    container[field] = output if isinstance(value, list) else output[0]
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=default_directory())
    parser.add_argument("--timeout", type=float, default=5)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status")
    commands.add_parser("reload")
    commands.add_parser("stop")
    start = commands.add_parser("start")
    start.add_argument("--scene", required=True)
    start.add_argument("--actors", nargs="+", required=True, type=lambda value: int(value, 0))
    start.add_argument("--anchor", required=True, type=lambda value: int(value, 0))
    camera = commands.add_parser("camera")
    camera.add_argument("--azimuth", required=True, type=float)
    camera.add_argument("--elevation", type=float, default=10)
    camera.add_argument("--radius", type=float, default=2.5)
    camera.add_argument("--height", type=float, default=1.1)
    attach = commands.add_parser("attach")
    attach.add_argument("--scene", required=True)
    attach.add_argument("--scene-file", type=Path)
    for name in ("set", "nudge"):
        adjust = commands.add_parser(name)
        adjust.add_argument("--role", required=True)
        for component in ("x", "y", "z", "heading"):
            adjust.add_argument("--" + component, type=float)
    speed = commands.add_parser("speed")
    speed.add_argument("value", type=float)
    commands.add_parser("pause")
    commands.add_parser("resume")
    commands.add_parser("reset")
    commands.add_parser("save")
    export = commands.add_parser("export")
    export.add_argument("path", type=Path)
    args = parser.parse_args(argv)
    try:
        if not 0 < args.timeout <= 30:
            raise ValueError("Timeout must be 0..30 seconds")
        client = Client(args.directory, args.timeout)
        with client_lock(args.directory):
            if args.command in ("status", "reload"):
                result = client.request(args.command)
            elif args.command == "start":
                result = client.request("start", scene=args.scene, actors=args.actors, anchor=args.anchor)
            elif args.command == "stop":
                result = client.stop()
            elif args.command == "camera":
                result = client.camera(args.azimuth, args.elevation, args.radius, args.height)
            elif args.command == "attach":
                result = client.attach(args.scene, args.scene_file)
            elif args.command in ("set", "nudge"):
                components = {key: getattr(args, key) for key in ("x", "y", "z", "heading")}
                if all(value is None for value in components.values()):
                    raise ValueError("Specify at least one position or heading component")
                result = client.adjust(args.role, components, args.command == "nudge")
            elif args.command in ("speed", "pause", "resume"):
                result = client.speed(args.value if args.command == "speed" else 0 if args.command == "pause" else 1)
            elif args.command == "reset":
                result = client.reset()
            elif args.command == "save":
                result = client.save()
            else:
                result = client.export(args.path)
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, KeyError, TimeoutError) as error:
        print(f"Alignment: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
