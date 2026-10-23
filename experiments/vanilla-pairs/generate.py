"""Generate the local two-actor vanilla audition pack (no copied game assets)."""
from __future__ import annotations

import json
from pathlib import Path

from placements import SOURCES, pair_placements

HERE = Path(__file__).resolve().parent
OUT = HERE / "Data/OSF/Auditions/VanillaPairs"
BASE = "meshes/actors/human/animations/"
PREFIX = "osf.audition.vanilla-pairs."
PACK = "OSF Vanilla Pairs - Audition"


def stage(name, paths, seconds, **timing):
    return {"name": name, "clips": [{"file": p, "sec": seconds} for p in paths], **timing}


def scene(identifier, name, roles, stages):
    family = identifier.split(".", 1)[0]
    assert roles == SOURCES[family]["roles"]
    return {
        "id": PREFIX + identifier,
        "name": name,
        # Take gender is a source label, not an admission restriction for auditioning.
        "roles": [{"name": role, "gender": "any", "offset": offset}
                  for role, offset in zip(roles, pair_placements(family))],
        "stages": stages,
    }


def envelope(folder, scenes):
    return {
        "schema": 1,
        "name": PACK + " - " + folder,
        "pack": PACK,
        "folder": folder,
        "tags": ["osf.audition.vanilla-pairs"],
        # Direct browser starts only: exclude temporary material from tag matchmaking.
        "unlisted": True,
        "hideApparel": False,
        "clearHeldItems": True,
        "playerInputLock": True,
        "placement": "anchorAndPin",
        "camera": "scene_orbit",
        "fade": False,
        "scenes": scenes,
    }


def make_packs():
    root = BASE + "furniture/scenes/mq204_hunterwalter/"
    def hw(clip):
        return [root + role + "/" + clip + ".af" for role in ["mq204_hunter", "mq204_walter"]]
    hold = stage("Hold - loops until Next or Stop", hw("sync_idle"), 203 / 30, loops=0)
    bash = stage("Bash, fall and recovery - once", hw("sync_bash"), 5)
    hunter = envelope("01 Hunter-Walter", [
        scene("hunter-walter.sequence", "Hunter-Walter | Hold > Next: bash", ["Hunter", "Walter"], [hold, bash]),
        scene("hunter-walter.hold", "Hunter-Walter | Hold loop", ["Hunter", "Walter"], [hold]),
        scene("hunter-walter.bash", "Hunter-Walter | Bash once (5s)", ["Hunter", "Walter"], [bash]),
    ])

    # Frame counts from the installed AF headers, matched reciprocally by role.
    reaction_frames = {
        ("male", "male"): [191, 201, 171, 291, 161],
        ("female", "male"): [151, 141, 121, 301],
        ("male", "female"): [191, 231, 301, 261, 111],
        ("female", "female"): [151, 101, 141, 381, 111],
    }
    survivors = []
    for victim in ["female", "male"]:
        root = BASE + "mq204a_030_deathscene/mq204a_030_" + victim + "victim/"
        for survivor in ["male", "female"]:
            s = root + "mq204a_030_survivor/" + ("female/" if survivor == "female" else "")
            v = root + "mq204a_030_victim/" + ("female/" if victim == "female" else "")
            def sv(clip):
                return [s + clip + ".af", v + clip + ".af"]
            idle = sv("sync_idle")
            stages = [
                stage("Approach and lower - once", sv("sync_start"), 269 / 30),
                stage("Support idle - Next for reaction 1", idle, 145 / 30, loops=0),
            ]
            sr = s + f"animarchetypemq204a_030_{survivor}reactsto{victim}/"
            vr = v + f"animarchetypemq204a_030_{victim}reactsto{survivor}/"
            for number, frames in enumerate(reaction_frames[(survivor, victim)], 1):
                paths = [p + f"reaction{number:02}.af" for p in [sr, vr]]
                stages.append(stage(f"Reaction {number} - once", paths, (frames - 1) / 30))
                stages.append(stage(f"Support idle after reaction {number} - Next or Stop", idle, 145 / 30, loops=0))
            survivors.append(scene(
                f"survivor-victim.{survivor}-{victim}",
                f"Survivor-Victim | {survivor.title()} / {victim.title()} takes",
                ["Survivor", "Victim"], stages))

    root = BASE + "furniture/scenes/dialoguecfthekey_establishingscene/"
    key_paths = [root + role + "/sync_idle.af" for role in ["cfthekey_marauder", "cfthekey_reaver"]]
    # hold is normalized against the last frame (1290), not the frame count (1291).
    # Inspect actual hand proximity, not AFX sound starts: shove reach peaks near
    # 611; the stable wrist catch is near 663. Foley 614/640 occurs off those poses.
    key = envelope("03 The Key", [
        scene("key.full", "The Key | Full confrontation once (43s)", ["Marauder", "Reaver"],
              [stage("Full scene - shove at 20.47s, struggle at 21.33s", key_paths, 43)]),
        scene("key.contacts", "The Key | Frozen contact checkpoints", ["Marauder", "Reaver"], [
            stage("Shove reach - frame 611 / 20.37s - Next", key_paths, 43, hold=611 / 1290),
            stage("Wrist catch - frame 663 / 22.10s - Next", key_paths, 43, hold=663 / 1290),
            stage("Body fall - frame 700 / 23.33s - Stop or Next", key_paths, 43, hold=700 / 1290),
        ]),
        scene("key.start-pose", "The Key | Frozen start for alignment", ["Marauder", "Reaver"],
              [stage("Source frame 0 - Stop when done", key_paths, 43, hold=0)]),
    ])
    return {
        "hunter-walter.osf.json": hunter,
        "survivor-victim.osf.json": envelope("02 Survivor-Victim", survivors),
        "the-key.osf.json": key,
    }


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    packs = make_packs()
    for name, pack in packs.items():
        (OUT / name).write_text(json.dumps(pack, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(packs)} content files / {sum(len(p['scenes']) for p in packs.values())} scenes to {OUT}")
