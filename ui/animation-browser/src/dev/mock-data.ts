export const MOCK_ACTORS = [
  { token: 601, name: "Sarah Morgan", formId: 0x2, distance: 2, isActor: true, species: "human", sex: "female" },
  { token: 602, name: "Andreja", formId: 0x3, distance: 5, isActor: true, species: "human", sex: "female" },
  { token: 603, name: "Sam Coe", formId: 0x4, distance: 9, isActor: true, species: "human", sex: "male" },
  { token: 605, name: "Terrormorph", formId: 0x6, distance: 7, isActor: true, species: "terrormorph", sex: "" },
];

export const MOCK_ANCHORS = [
  { token: 501, name: "Industrial Chair", formId: 0x12a57a, distance: 3, isActor: false, sceneCount: 2, customCount: 1 },
  { token: 502, name: "Ak Bunk Bed", formId: 0x1234, distance: 6, isActor: false, sceneCount: 1, customCount: 1 },
  { token: 503, name: "Lean Wall", formId: 0x2234, distance: 4, isActor: false, sceneCount: 2, customCount: 1, marker: true },
  { token: 504, name: "Ground Sit", formId: 0x2235, distance: 8, isActor: false, sceneCount: 1, customCount: 0, marker: true },
];

export const MOCK_ANCHOR_MATCH: Record<number, string[]> = {
  501: ["ge.chair.love", "vanilla/furniture/chair"],
  502: ["ge.akbunk.sequence"],
  503: ["ge.chair.love", "vanilla/furniture/bench"],
  504: ["vanilla/furniture/bench"],
};

const EMOTES = ["Wave", "Cheer", "Clap", "Point", "Salute", "Shrug", "Facepalm", "Flex", "Dance", "Bow", "Thumbs Up", "Warm Hands", "Sit Ground", "Whistle"];

export const MOCK_CATALOG = [
  ...EMOTES.map((name, index) => ({
    id: `emote.${name.toLowerCase().replace(/\s+/g, "")}`,
    title: name,
    tags: [`player.emote.${name.toLowerCase().replace(/\s+/g, "")}`, "emote"],
    actorCount: 1,
    requiresFurniture: false,
    estSec: 4 + index % 5,
    priority: 0,
    weight: 1,
    sourceFile: "Data/OSF/Emotes/immersion.osf.json",
  })),
  { id: "solo.calibration", title: "Solo Calibration", tags: ["test", "solo", "free"], actorCount: 1, requiresFurniture: false, priority: 1, weight: 6, sourceFile: "Data/OSF/Scenes/test.osf.json" },
  { id: "ge.chair.love", title: "GE Chair Love", tags: ["ge", "chair", "paired"], actorCount: 2, roles: [{ name: "bottom", gender: "female" }, { name: "top", gender: "male" }], requiresFurniture: true, anchors: ["Chair"], stages: [{ index: 0, name: "Missionary06", tags: ["paired"], clipCount: 2, loopSec: 18.7, openEnded: true, estSec: 37.3, tracks: [
    { kind: "action", at: 0, trackPosition: "enter", label: "osf.prop.attach", detail: "helmet", role: "bottom" },
    { kind: "sound", at: 0.22, trackPosition: "fraction", label: "$scene,{gender},soft", role: "bottom", repeat: true },
    { kind: "cue", at: 0.42, trackPosition: "fraction", label: "helmet.stow", role: "bottom" },
    { kind: "sound", at: 0.72, trackPosition: "fraction", label: "$scene,{gender},loud", role: "bottom" },
    { kind: "camera", at: 0, trackPosition: "enter", label: "scene_orbit" },
    { kind: "action", at: 1, trackPosition: "end", label: "osf.prop.destroy", detail: "helmet", role: "bottom" },
  ] }, { index: 1, name: "Cowgirl07", tags: ["paired"], clipCount: 2, loopSec: 20, openEnded: true, estSec: 40 }], estSec: 77.3, openEnded: true, priority: 2, weight: 40, pack: "Gergel Ebanex", sourceFile: "Data/OSF/GE/chair.osf.json" },
];

// A representative import report: one clean pack, one partially-rejected pack, one file rejected
// whole, one compat pack whose clips are missing, one clean-but-empty file, plus the cross-file
// bucket — every state the panel has to render, so standalone exercises all of them.
export const MOCK_IMPORTS = {
  files: [
    { path: "Emotes/immersion.osf.json", file: "immersion.osf.json", pack: "OSF Emotes", schema: 1, bytes: 24_118, parseMs: 3.4, declaredScenes: 14, scenes: 14, nodes: 14, stages: 14, roles: 14, clips: 14, distinctClips: 14, cues: 6, actions: 2, clipEntries: 14, species: ["human"] },
    { path: "GE/chair.osf.json", file: "chair.osf.json", pack: "Gergel Ebanex", schema: 1, bytes: 181_902, parseMs: 22.8, declaredScenes: 43, scenes: 41, rejectedScenes: 2, anchored: 41, nodes: 96, stages: 96, roles: 82, clips: 192, distinctClips: 133, cues: 12, sounds: 34, cameras: 4, clipEntries: 133, species: ["human"], errors: 2, warnings: 1, problems: [{ severity: "error", code: "scene-invalid", message: "Role 'top': 'poseMode' must be 'override' or 'additive'.", hint: "Correct the role field in the named scene, then reload.", scene: "ge.chair.broken", role: "top" }, { severity: "error", code: "scene-invalid", message: "Duplicate explicit role name 'f'.", hint: "Give every explicit role a unique name.", scene: "ge.chair.dup", role: "f" }, { severity: "warn", code: "scene-warning", message: "Timer is longer than the referenced clip.", hint: "Shorten the timer or use a longer clip.", scene: "ge.chair.late", node: "n2" }], problemCount: 3 },
    { path: "Snu/snusnu.osf.json", file: "snusnu.osf.json", pack: "Snu Snu OSF", schema: 1, bytes: 39_400, parseMs: 6.1, declaredScenes: 6, scenes: 6, nodes: 6, stages: 6, roles: 12, clips: 12, distinctClips: 12, missingClips: 12, missingClipExamples: ["SnuSnu/k01_MP.glb", "SnuSnu/k01_FP.glb"], hidden: 6, species: ["human"], warnings: 1, problems: [{ severity: "warn", code: "missing-clips", message: "6 scenes are unavailable because 12 referenced clips are not installed.", hint: "Install the animation pack that owns these paths, or correct the clip names.", clip: "SnuSnu/k01_MP.glb" }], problemCount: 1 },
    { path: "Vanilla/vanilla-photomode.osf.json", file: "vanilla-photomode.osf.json", pack: "Vanilla · Photomode", library: true, schema: 1, bytes: 1_204_886, parseMs: 148.2, scenes: 312, unlisted: 312, nodes: 312, stages: 1_842, roles: 312, clips: 1_842, distinctClips: 1_811, species: ["human"] },
    { path: "Broken/oldpack.osf.json", file: "oldpack.osf.json", schema: 0, bytes: 3_140, parseMs: 0.6, declaredScenes: 1, rejectedScenes: 1, rejected: true, errors: 1, problems: [{ severity: "error", code: "file-invalid", message: "Schema 0 is unsupported; expected schema 1.", hint: "Update the file to schema 1, then reload." }], problemCount: 1 },
    { path: "Scenes/test.osf.json", file: "test.osf.json", schema: 1, bytes: 812, parseMs: 0.4, scenes: 1, nodes: 1, stages: 1, roles: 1, clips: 1, distinctClips: 1, clipEntries: 1, species: ["human"] },
	{ path: "Suit/routes.osf.json", file: "routes.osf.json", pack: "Suit Protocol", schema: 1, bytes: 4210, parseMs: 1.2, declaredRoutes: 3, routes: 2, rejectedRoutes: 1, errors: 1, problems: [{ severity: "error", code: "route-invalid", message: "Route transition mask is missing.", hint: "Add a named mask and reload." }], problemCount: 1 },
    { path: "Notes/placeholder.osf.json", file: "placeholder.osf.json", schema: 1, bytes: 96, parseMs: 0.2 },
    { path: "", file: "", errors: 1, problems: [{ severity: "error", code: "unknown-scene-reference", message: "Use target 'ge.chair.finish' does not name a loaded scene.", hint: "Correct the target id or restore the referenced scene.", scene: "ge.chair.love", node: "n3" }], problemCount: 1 },
  ],
	totals: { files: 8, rejectedFiles: 1, declaredScenes: 377, scenes: 374, rejectedScenes: 3, declaredRoutes: 3, routes: 2, rejectedRoutes: 1, registered: 374, clipEntries: 148, hidden: 6, missingClips: 12, errors: 5, warnings: 2, bytes: 1_458_564, parseMs: 182.9 },
};

export const MOCK_LIBRARY = [
  { id: "vanilla/furniture/chair", title: "Vanilla · Furniture / Chair", tags: ["vanilla", "furniture"], actorCount: 1, requiresFurniture: true, anchors: ["Chair"], sourceFile: "vanilla-furniture.osf.json", stages: [{ index: 0, name: "Idle", tags: [], clipCount: 1, loopSec: 2.7, openEnded: true, estSec: 5.4 }, { index: 1, name: "EnterFromStand", tags: ["transition"], clipCount: 1, loopSec: 7.3, estSec: 14.7 }] },
  { id: "vanilla/photomode", title: "Vanilla · Photomode", tags: ["vanilla", "photomode"], actorCount: 1, requiresFurniture: false, sourceFile: "vanilla-photomode.osf.json", stages: [{ index: 0, name: "Hero Pose", tags: ["pose"], clipCount: 1, loopSec: 3, openEnded: true, estSec: 6 }, { index: 1, name: "Vehicle_HangTen", tags: [], clipCount: 1, loopSec: 0.3, openEnded: true, estSec: 0.6 }] },
  { id: "vanilla/creature/terrormorph", title: "Vanilla · Terrormorph", species: "terrormorph", tags: ["vanilla", "creature"], actorCount: 1, requiresFurniture: false, sourceFile: "vanilla-creature.osf.json", stages: [{ index: 0, name: "BleedOut_Idle", tags: ["idle"], clipCount: 1, loopSec: 8.3, openEnded: true, estSec: 16.6 }] },
  // Registered and derived one-clip entries share the osf.scene-clip/ id namespace. Explicit
  // `sourceKind` is authoritative; other mock records omit it to keep legacy bridge fallback live.
  { id: "osf.scene-clip/a1b2c3d4e5f60001", title: "Hand Extended 01", tags: ["scene.clip"], sourceKind: "curatedAnimation", curated: true, unlisted: true, actorCount: 1, placement: "followActor", pack: "Moods of Andromas", folder: "Standing", sourceFile: "moods-of-andromas.osf.json", stages: [{ index: 0, name: "Hand Extended 01", tags: ["scene.clip"], clipCount: 1, loopSec: 2.4, openEnded: true, estSec: 4.8 }] },
  { id: "osf.scene-clip/a1b2c3d4e5f60002", title: "GE\\chair\\Missionary06_FP.glb", tags: ["scene.clip"], sourceKind: "derivedDebugAnimation", unlisted: true, actorCount: 1, placement: "followActor", pack: "Gergel Ebanex", sourceFile: "chair.osf.json", stages: [{ index: 0, name: "GE\\chair\\Missionary06_FP.glb", tags: ["scene.clip"], clipCount: 1, loopSec: 18.7, openEnded: true, estSec: 37.3 }] },
];

export const MOCK_ROUTES = [
  {
    id: "suitprotocol.route.helmet",
    sourceFile: "SuitProtocol/suitprotocol.routes.osf.json",
    stations: [
      { id: "equipped", layer: null },
      { id: "held", layer: { clip: "OSF/SuitProtocol/helmet_hold.af", animId: "", durationHint: 2.2, mask: "upperBody", mode: "override", weight: 1, holdAt: 1 } },
      { id: "stowed", layer: null },
    ],
    transitions: [
      {
        id: "head-to-held", from: "equipped", to: "held", interruption: "finish",
        layer: { clip: "OSF/SuitProtocol/helmet_head_to_held.af", animId: "", durationHint: 3.3, mask: "upperBody", mode: "override", weight: 1, holdAt: -1 },
        commit: { frame: 24, id: "suitprotocol.transition.commit" },
        markers: [{ frame: 16, id: "suitprotocol.helmet.moving" }],
        props: [{ frame: 18, id: "helmet", attach: true, lifetime: "station", node: "R_AnimObject1" }],
        sounds: [{ frame: 25, spec: "$helmet,unseal" }],
      },
      {
        id: "held-to-stowed", from: "held", to: "stowed", interruption: "crossfade-before-commit",
        layer: { clip: "OSF/SuitProtocol/helmet_held_to_stowed.af", animId: "", durationHint: 2.3, mask: "upperBody", mode: "override", weight: 1, holdAt: -1 },
        commit: { frame: 65, id: "suitprotocol.transition.commit" },
        markers: [], props: [{ frame: 64, id: "helmet", attach: false, lifetime: "station", node: "" }], sounds: [],
      },
      {
        id: "stowed-to-held", from: "stowed", to: "held", interruption: "finish",
        layer: { clip: "OSF/SuitProtocol/helmet_stowed_to_held.af", animId: "", durationHint: 2.3, mask: "upperBody", mode: "override", weight: 1, holdAt: -1 },
        commit: { frame: 4, id: "suitprotocol.transition.commit" }, markers: [],
        props: [{ frame: 4, id: "helmet", attach: true, lifetime: "station", node: "R_AnimObject1" }], sounds: [],
      },
      {
        id: "held-to-head", from: "held", to: "equipped", interruption: "finish",
        layer: { clip: "OSF/SuitProtocol/helmet_held_to_head.af", animId: "", durationHint: 3.3, mask: "upperBody", mode: "override", weight: 1, holdAt: -1 },
        commit: { frame: 75, id: "suitprotocol.transition.commit" }, markers: [],
        props: [{ frame: 74, id: "helmet", attach: false, lifetime: "station", node: "" }], sounds: [{ frame: 76, spec: "$helmet,seal" }],
      },
    ],
  },
  {
    id: "backpack.route.demo",
    sourceFile: "Examples/backpack-route.osf.json",
    stations: [{ id: "worn", layer: null }, { id: "held", layer: null }],
    transitions: [{ id: "remove", from: "worn", to: "held", interruption: "finish",
      layer: { clip: "OSF/Examples/backpack_remove.af", animId: "", durationHint: 1.8, mask: "upperBody", mode: "override", weight: 1, holdAt: -1 },
      commit: null, markers: [{ frame: 28, id: "backpack.removed" }], props: [], sounds: [] }],
  },
];

