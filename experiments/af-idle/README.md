# Historical engine-idle AF prototype

This directory preserves the `af-slop` work while consolidating development on
`dev`. It is a June 2026 prototype, excluded from the production build, deployment,
and release package. The current engine continues to use `AFImport` and the shared
playback/runtime lifecycle for AF clips.

## Provenance

- Branch tip: `5ff884b0a82c00713af32c01bd47a041915b0e4c` (`WIP AF SLOP`).
- Earlier prototype commit: `ab60eeaa5f9b2ddb69f4b9b393fc23ad2bc55bb8` (`Add af idle player`).
- Original base: `e9b2a8a68bcaeb7e005336965725597cf96f82b1`.
- `branch.patch` is the complete binary-capable diff from that base to the branch
  tip, including its original integration, packaging, instructions, and submodule
  reference. It is historical material, not a patch to apply to current `dev`.
- `manifest.json` records the original Git blob identity and SHA-256 for every
  extracted file. Files retain their original repository-relative layout here.

The preserved files include the native idle player, AFX stub writer, template ESM
and generator, sample AF/AFX clips, and the original Papyrus sources and bytecode.
The native integration changes are retained in `branch.patch`; they are not
applied to the current engine.

## Why this is not enabled in production

The prototype routes engine mutations through SFSE tasks and pulses pending work
from animation hooks. Current OSF requires the verified frame drain and its
world-replacement lifetime rules. It also introduces a separate template-ESM
dependency, temporarily changes the player's animation archetype, and scans loose
animation files to write missing AFX sidecars at plugin startup. Those behaviors
have not been ported or qualified against the current runtime.

Future work can use this source to develop a separate engine-idle playback path.
Enabling it requires explicit integration with current actor ownership, frame
dispatch, save/load cancellation, packaging, Papyrus bindings, and live-game
validation. The archived PEX files are historical binaries and must not replace
the current production scripts.
