"""Read generated SLAL JSON and FNIS lists; never execute pack source scripts.

Input schema follows orxx/SLAnimLoader's SLAnimGenerate.pyw gen_json_dict
methods. Source coordinate offsets remain Skyrim units for the retarget worker.
This prototype supports human clips without attached animation objects.
"""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path, PureWindowsPath
import shlex


class SlalError(ValueError):
    """The selected metadata cannot be imported without guessing."""


def _number(value: object, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise SlalError(f"{label} must be a finite number")
    result = float(value)
    if not math.isfinite(result):
        raise SlalError(f"{label} must be a finite number")
    return result


def _text(value: object, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise SlalError(f"{label} must be a non-empty string")
    return value


def _inside(path: Path, root: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_relative_to(root):
        raise SlalError(f"Pack resource escapes its root: {path}")
    return resolved


def _event_maps(root: Path) -> tuple[dict, dict]:
    """Index filenames and event declarations without loading animation bytes."""
    clips = {}
    for candidate in root.rglob("*"):
        if not candidate.is_file() or candidate.suffix.lower() != ".hkx":
            continue
        path = _inside(candidate, root)
        key = path.relative_to(root).as_posix().casefold()
        if key in clips and clips[key] != path:
            raise SlalError(f"Case-colliding clip paths: {clips[key]} and {path}")
        clips[key] = path

    events = {}
    lists = sorted((p for p in root.rglob("*.txt")
                    if p.name.lower().startswith("fnis_")
                    and p.name.lower().endswith("_list.txt")), key=str)
    for path in lists:
        path = _inside(path, root)
        for line_number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
            stripped = line.strip()
            if not stripped or stripped.startswith(("'", "#")):
                continue
            # FNIS comments use apostrophes, so shlex's quote handling must not
            # interpret the entire remainder as an unterminated quoted string.
            if stripped.split(None, 1)[0] not in {"s", "+", "b"}:
                continue
            try:
                tokens = shlex.split(stripped, posix=False)
            except ValueError as exc:
                raise SlalError(f"Malformed FNIS declaration {path}:{line_number}") from exc
            options = []
            cursor = 1
            while cursor < len(tokens) and tokens[cursor].startswith("-"):
                options.append(tokens[cursor])
                cursor += 1
            if len(tokens) < cursor + 2:
                raise SlalError(f"Incomplete FNIS declaration {path}:{line_number}")
            event = tokens[cursor]
            filename = tokens[cursor + 1].strip('"')
            relative = PureWindowsPath(filename)
            if relative.is_absolute() or relative.drive or ".." in relative.parts:
                raise SlalError(f"Unsafe FNIS clip path {path}:{line_number}: {filename}")
            target = _inside(path.parent.joinpath(*relative.parts), root)
            if target.suffix.lower() != ".hkx":
                raise SlalError(f"FNIS event {event} does not reference an HKX")
            key = target.relative_to(root).as_posix().casefold()
            attached_object = any(o == "-o" or o.startswith("-o,") for o in options)
            tail = tokens[cursor + 2:]
            if tail and not tail[0].startswith("'"):
                attached_object = True
            declaration = {"key": key, "list": path, "line": line_number,
                           "attached_object": attached_object}
            if event in events:
                previous = events[event]
                if (previous["key"], previous["attached_object"]) != (key, attached_object):
                    raise SlalError(f"Conflicting FNIS event {event}: {previous['list']} and {path}")
            else:
                events[event] = declaration
    return clips, events


def _input_record(path: Path) -> dict:
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def _read_document(json_path: str | Path) -> tuple[Path, dict]:
    path = Path(json_path).resolve()
    if path.suffix.lower() != ".json":
        raise SlalError("Use generated SLAnims/json/*.json; source scripts are not executed")
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise SlalError(f"Cannot read generated SLAL JSON: {path}: {exc}") from exc
    if not isinstance(data, dict) or not isinstance(data.get("animations"), list):
        raise SlalError("Expected SLAL object with an animations array")
    return path, data


def list_scene_summaries(json_path: str | Path) -> list[dict]:
    """List metadata candidates without resolving files or rejecting mixed packs.

    Each row has id/name, integer roles, integer-or-null stages, status and reason.
    ``candidate`` means human-role metadata only: load_scenes still checks FNIS
    props, event aliases, clip presence, numeric values and source compatibility.
    Invalid individual entries remain visible alongside valid candidates.
    """
    _, data = _read_document(json_path)
    identifiers = {}
    for entry in data["animations"]:
        identifier = entry.get("id") if isinstance(entry, dict) else None
        if isinstance(identifier, str):
            identifiers[identifier] = identifiers.get(identifier, 0) + 1
    result = []
    for entry in data["animations"]:
        summary = {"id": None, "name": None, "roles": 0, "stages": None,
                   "status": "invalid", "reason": "Animation entry is not an object"}
        if not isinstance(entry, dict):
            result.append(summary)
            continue
        identifier = entry.get("id")
        summary["id"] = identifier if isinstance(identifier, str) else None
        summary["name"] = entry.get("name", summary["id"])
        actors = entry.get("actors")
        if isinstance(actors, list):
            summary["roles"] = len(actors)
            counts = [len(a["stages"]) if isinstance(a, dict) and isinstance(a.get("stages"), list)
                      else None for a in actors]
            if counts and None not in counts and len(set(counts)) == 1:
                summary["stages"] = counts[0]
        if not isinstance(identifier, str) or not identifier.strip():
            summary["reason"] = "Missing or invalid animation id"
        elif identifiers[identifier] > 1:
            summary["reason"] = "Duplicate animation id"
        elif not isinstance(summary["name"], str) or not summary["name"].strip():
            summary["reason"] = "Missing or invalid animation name"
        elif entry.get("error"):
            summary["reason"] = f"SLAL generator error: {entry['error']}"
        elif not isinstance(actors, list) or not actors:
            summary["reason"] = "Missing actor definitions"
        elif any(not isinstance(a, dict) for a in actors):
            summary["reason"] = "Malformed actor definition"
        elif len(actors) > 5 or entry.get("creature_race") or any(
                a.get("type") not in ("Male", "Female") or a.get("race") for a in actors):
            summary.update(status="unsupported", reason="Prototype supports 1-5 human roles")
        elif not summary["stages"]:
            summary["reason"] = "Missing or unequal actor stage counts"
        elif any(not isinstance(stage, dict) or not isinstance(stage.get("id"), str)
                 or not stage["id"].strip() for actor in actors for stage in actor["stages"]):
            summary["reason"] = "Missing or malformed actor stage event"
        else:
            summary.update(status="candidate", reason="Human metadata; FNIS and clip validation pending")
        result.append(summary)
    return result


def load_scenes(json_path: str | Path, pack_root: str | Path, *, scene_id: str | None = None) -> list[dict]:
    """Return import plans for generated SLAL JSON.

    ``pack_root`` is the extracted Data root (or a mod root containing Data).
    Optional ``scene_id`` allows selecting a human scene from a mixed pack;
    unsupported selected scenes raise SlalError rather than silently disappearing.
    The CLI adds source skeleton paths after this metadata-only operation.
    """
    path, data = _read_document(json_path)
    root = Path(pack_root).resolve()
    if not root.is_dir():
        raise SlalError(f"Pack root does not exist: {root}")
    if not (root / "meshes").exists() and (root / "Data" / "meshes").is_dir():
        root = (root / "Data").resolve()
    pack = _text(data.get("name", path.stem), "pack name")
    ids = set()
    selected = []
    for entry in data["animations"]:
        if scene_id is not None and (not isinstance(entry, dict) or entry.get("id") != scene_id):
            continue
        if not isinstance(entry, dict):
            raise SlalError("Each SLAL animation must be an object")
        identifier = _text(entry.get("id"), "animation id")
        if identifier in ids:
            raise SlalError(f"Duplicate animation id: {identifier}")
        ids.add(identifier)
        if scene_id is None or identifier == scene_id:
            selected.append(entry)
    if scene_id is not None and not selected:
        raise SlalError(f"SLAL scene not found: {scene_id}")
    clips, events = _event_maps(root)
    plans = []
    for entry in selected:
        identifier = entry["id"]
        if entry.get("error"):
            raise SlalError(f"SLAL generator reported an error for {identifier}: {entry['error']}")
        actors = entry.get("actors")
        if not isinstance(actors, list) or not actors:
            raise SlalError(f"{identifier}: actors must be a non-empty array")
        if len(actors) > 5 or entry.get("creature_race"):
            raise SlalError(f"{identifier}: prototype supports 1-5 human roles")
        roles = []
        stage_count = None
        for number, actor in enumerate(actors, 1):
            if not isinstance(actor, dict) or actor.get("type") not in ("Male", "Female") or actor.get("race"):
                raise SlalError(f"{identifier}: actor {number} is not a supported human role")
            stages = actor.get("stages")
            if not isinstance(stages, list) or not stages:
                raise SlalError(f"{identifier}: actor {number} has no stages")
            if stage_count is not None and len(stages) != stage_count:
                raise SlalError(f"{identifier}: actors have unequal stage counts")
            stage_count = len(stages)
            roles.append({"name": f"actor{number}", "gender": actor["type"].lower()})
        global_stages = entry.get("stages", [])
        if not isinstance(global_stages, list):
            raise SlalError(f"{identifier}: scene stages must be an array")
        timers = {}
        for settings in global_stages:
            if not isinstance(settings, dict):
                raise SlalError(f"{identifier}: stage settings must be objects")
            number = settings.get("number")
            if type(number) is not int or not 1 <= number <= stage_count or number in timers:
                raise SlalError(f"{identifier}: invalid or duplicate stage number {number}")
            timer = _number(settings.get("timer", 0), f"{identifier} stage {number} timer")
            if timer < 0:
                raise SlalError(f"{identifier}: stage timer must be non-negative")
            timers[number] = timer
        output_stages = []
        used_lists = set()
        used_clips = set()
        unmapped = set()
        for stage_index in range(stage_count):
            output = {"clips": [], "offsets": []}
            timer = timers.get(stage_index + 1, 0)
            output.update({"timer": timer} if timer > 0 else {"loops": 0})
            for actor_index, actor in enumerate(actors):
                stage = actor["stages"][stage_index]
                if not isinstance(stage, dict):
                    raise SlalError(f"{identifier}: actor stage must be an object")
                event = _text(stage.get("id"), "stage event id")
                declaration = events.get(event)
                if declaration is None:
                    raise SlalError(f"{identifier}: no FNIS mapping for event {event}")
                if declaration["attached_object"]:
                    raise SlalError(f"{identifier}: event {event} requires an unsupported animation object")
                clip = clips.get(declaration["key"])
                if clip is None:
                    raise SlalError(f"{identifier}: missing HKX for event {event}: {declaration['key']}")
                output["clips"].append(str(clip))
                output["offsets"].append({
                    target: _number(stage.get(source, 0), f"{event} {source}")
                    for target, source in (("x", "side"), ("y", "forward"), ("z", "up"), ("heading", "rotate"))
                })
                used_lists.add(declaration["list"])
                used_clips.add(clip)
                for key, value in stage.items():
                    if key not in {"id", "side", "forward", "up", "rotate"}:
                        unmapped.add(f"actor {actor_index + 1} stage {stage_index + 1}: {key}")
                for key in actor.keys() - {"type", "stages"}:
                    unmapped.add(f"actor {actor_index + 1}: {key}")
            output_stages.append(output)
        if entry.get("sound"):
            unmapped.add("scene: sound")
        for settings in global_stages:
            for key in settings.keys() - {"number", "timer"}:
                unmapped.add(f"stage {settings['number']}: {key}")
        for key in entry.keys() - {"id", "name", "tags", "sound", "actors", "stages", "error"}:
            unmapped.add(f"scene: {key}")
        for key in data.keys() - {"name", "animations"}:
            unmapped.add(f"pack: {key}")
        raw_tags = entry.get("tags", "")
        if isinstance(raw_tags, str):
            tags = [t.strip() for t in raw_tags.split(",") if t.strip()]
        elif isinstance(raw_tags, list) and all(isinstance(t, str) for t in raw_tags):
            tags = list(raw_tags)
        else:
            raise SlalError(f"{identifier}: tags must be a string or array of strings")
        plans.append({
            "schema": 1, "id": identifier, "name": _text(entry.get("name", identifier), "scene name"),
            "pack": pack, "tags": tags, "roles": roles, "stages": output_stages,
            "source": {"kind": "slal", "json": str(path), "pack_root": str(root),
                       "offset_units": "skyrim", "metadata": entry,
                       "pack_metadata": {key: value for key, value in data.items() if key != "animations"},
                       "warnings": ["Not converted: " + item for item in sorted(unmapped)],
                       "inputs": [_input_record(p) for p in sorted({path} | used_lists | used_clips, key=str)]},
        })
    return plans
