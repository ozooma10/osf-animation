export const MOCK_ACTORS = [
  { token: 601, name: "Sarah Morgan", formId: 0x2, distance: 2, isActor: true, species: "human" },
  { token: 602, name: "Andreja", formId: 0x3, distance: 5, isActor: true, species: "human" },
  { token: 603, name: "Sam Coe", formId: 0x4, distance: 9, isActor: true, species: "human" },
  { token: 605, name: "Terrormorph", formId: 0x6, distance: 7, isActor: true, species: "terrormorph" },
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
    genders: ["any"],
    requiresFurniture: false,
    estSec: 4 + index % 5,
    priority: 0,
    weight: 1,
    sourceFile: "Data/OSF/Emotes/immersion.osf.json",
  })),
  { id: "solo.calibration", title: "Solo Calibration", tags: ["test", "solo", "free"], actorCount: 1, genders: ["any"], requiresFurniture: false, priority: 1, weight: 6, sourceFile: "Data/OSF/Scenes/test.osf.json" },
  { id: "ge.chair.love", title: "GE Chair Love", tags: ["ge", "chair", "paired"], actorCount: 2, roles: [{ name: "bottom", gender: "female" }, { name: "top", gender: "male" }], requiresFurniture: true, anchors: ["Chair"], stages: [{ index: 0, name: "Missionary06", tags: ["paired"], clipCount: 2, loopSec: 18.7, openEnded: true, estSec: 37.3 }, { index: 1, name: "Cowgirl07", tags: ["paired"], clipCount: 2, loopSec: 20, openEnded: true, estSec: 40 }], estSec: 77.3, openEnded: true, priority: 2, weight: 40, pack: "Gergel Ebanex", sourceFile: "Data/OSF/GE/chair.osf.json" },
];

export const MOCK_LIBRARY = [
  { id: "vanilla/furniture/chair", title: "Vanilla · Furniture / Chair", tags: ["vanilla", "furniture"], actorCount: 1, genders: ["any"], requiresFurniture: true, anchors: ["Chair"], sourceFile: "vanilla-furniture.osf.json", stages: [{ index: 0, name: "Idle", tags: [], clipCount: 1, loopSec: 2.7, openEnded: true, estSec: 5.4 }, { index: 1, name: "EnterFromStand", tags: ["transition"], clipCount: 1, loopSec: 7.3, estSec: 14.7 }] },
  { id: "vanilla/photomode", title: "Vanilla · Photomode", tags: ["vanilla", "photomode"], actorCount: 1, genders: ["any"], requiresFurniture: false, sourceFile: "vanilla-photomode.osf.json", stages: [{ index: 0, name: "Hero Pose", tags: ["pose"], clipCount: 1, loopSec: 3, openEnded: true, estSec: 6 }, { index: 1, name: "Vehicle_HangTen", tags: [], clipCount: 1, loopSec: 0.3, openEnded: true, estSec: 0.6 }] },
  { id: "vanilla/creature/terrormorph", title: "Vanilla · Terrormorph", species: "terrormorph", tags: ["vanilla", "creature"], actorCount: 1, genders: ["any"], requiresFurniture: false, sourceFile: "vanilla-creature.osf.json", stages: [{ index: 0, name: "BleedOut_Idle", tags: ["idle"], clipCount: 1, loopSec: 8.3, openEnded: true, estSec: 16.6 }] },
];

