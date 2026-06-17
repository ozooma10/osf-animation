# Design proposal — scene-def matchmaking, role filters, weighted/boolean tag queries

**Status: IMPLEMENTED (2026-06-17), pending in-game verification of the RE-sensitive form-ref
resolver + keyword/race predicates.** All three phases landed in one pass: the matchmaking layer
(`src/Matchmaking/Matchmaker.*`), `roles[].filters` keyword/race + the `Plugin.esm|0xID` resolver
(`SceneRegistry`), `weight`, and the boolean `*Query` natives. This doc is now the design-of-record;
the contract text moved to SCENE_DESIGN.md (§1.2/§1.3/§1.6) and the RE detail to RE.md. The two
open sign-off items resolved as: native names = `Query` suffix; keyword = actorbase OR race (revisit
if SIF needs broader race-keyword folding). Covers the deferred items: per-role actor filters (P2/3),
weighted scene selection (P3/5), richer tag queries (P4/6).

## Context — why this is a "design pass", not a quick add

The proposal originally assumed `StartSceneByTags`/`FindScenes` matchmake `*.scene.json` scene defs
with priority tiers and role filters, as [SCENE_DESIGN.md](SCENE_DESIGN.md) §1.2/§1.3 describes. **They
don't.** Both natives matchmake the **PackRegistry** (`AnimationDef`), not the SceneRegistry:

- `StartSceneByTags` → `PackRegistry::PickByTags` ([PackRegistry.cpp:336](../src/Registry/PackRegistry.cpp)):
  filter packs by `actors.size()` + all-tags-AND, **full `std::shuffle`** (uniform, **no priority**),
  then brute-force gender-slot permutations; first fit wins.
- `FindScenes(int aiActorCount, string[] asTags)` → `PackRegistry::FindByTags`: same pack filter,
  returns ids `std::sort`-ed. **Takes an actor COUNT, not `Actor[]`.**
- Composed `*.scene.json` graphs (roles, `priority`, the reserved `filters`) are **not discoverable
  or matchmakable by tags at all** — only startable by explicit id (`StartScene`/`StartSceneRoles`).

So SCENE_DESIGN.md's ranking text (priority tiers, greedy role-filter assignment) is **aspirational —
only gender is implemented, and only on packs.** Items (3)/(5)/(6) are "implement the documented
scene-def matchmaking, then hang filters/weight/queries off it."

## Decisions locked (from the user)
- **Direction:** extend tag matchmaking to scene defs; filters/weight/queries live on scene defs.
- **Build order from the additive pass:** this is the deferred "design pass"; land after this spec is signed off.

## Grounding — what the existing pack path already does (informs the design)

Three of the review's main asks are **already how `PickByTags`/`FindByTags` behave**, so adopting them
for scene defs is *consistency with shipped code*, not new invention (and greedy would be a regression):

| Behavior | Existing pack path | Implication for scene defs |
|---|---|---|
| Role/slot fit | **Exhaustive** `std::next_permutation` over slot perms ([PackRegistry.cpp:360](../src/Registry/PackRegistry.cpp)) | Use complete matching, not greedy first-fit (§2.1) |
| Binding carried out | `SlottedPick{ id, order }` — the chosen slot→actor map | Candidate carries its resolved binding (§2.2) |
| `FindByTags` order | Deterministic `std::sort` | `FindScenes` stays deterministic (§3.1) |

Party sizes are ≤4-ish, so exhaustive matching is cheap (the pack matcher already does it).

---

## §1 Candidate model & ranking

One ranking model; scene defs and packs **compete in the same pool**. Scene defs are NOT implicitly
higher priority than packs.

- A **pack pseudo-candidate** has `priority = 0`, `weight = 1`.
- A **scene-def candidate** has `priority = <declared or 0>`, `weight = <declared or 1>`.
- "Scene-def-first" applies **only to id resolution** (the existing §1.2 rule: a bare id resolves
  scene registry then pack registry), **not** to ranking. A scene author who wants a composed scene to
  supersede raw pack selection sets `"priority": 1`+.

**Ranking for `StartSceneByTags*`:** (1) actor count matches; (2) tag query satisfied (§3); (3) a valid
role binding exists (§2); (4) group by highest `priority`; (5) **weight-proportional random within the
top priority tier**. The candidate set and tiering are deterministic; only step (5) is random.

### ID-collision rule (scene shadows pack)

The scene/pack id namespace is shared (SCENE_DESIGN §1.2) with `scene:`/`anim:` prefix overrides
already shipped. Rather than hard-reject duplicates (which would break the intentional shared
namespace), **a scene def shadows a same-id pack**:

- A pack pseudo-candidate whose id collides with a scene def id is **suppressed** from
  `FindScenes`/`StartSceneByTags*` (the scene def represents that id in matchmaking).
- The pack is still reachable explicitly via `anim:<id>` (`StartScene`).
- The collision is **logged loudly at load** (both sources named) — the existing behavior, now with a
  defined matchmaking consequence.

(Within a single registry, duplicate ids remain first-load-wins + logged, unchanged.)

## §2 Role binding & filters

### §2.1 Complete (exhaustive) matching, not greedy

Greedy "actors in array order each take the first unfilled role whose filters they satisfy" is
deterministic but **incomplete** — it can reject a satisfiable candidate:

```
roles: roleA(any), roleB(keyword=Creature)   actors: actor0(Creature), actor1(Human)
greedy: actor0→roleA, then actor1 can't fill roleB  ✗  (valid: actor0→roleB, actor1→roleA)
```

Use **deterministic complete matching** (mirrors the pack matcher, which already permutes
exhaustively): build actor→compatible-role edges; find a complete assignment; if none exists, reject
the candidate. Tie-break deterministically by **role-declaration order, then actor-array order** so the
same actors+scene always yield the same binding. Party sizes are tiny; brute-force permutation is fine.
This tightens SCENE_DESIGN §1.2's greedy wording — safe, since filter matchmaking is unimplemented and
has no shipped consumers.

### §2.2 Carry the binding on the candidate

Matchmaking resolves the binding; selection must **not** recompute it. The candidate carries it
(packs already do, via `SlottedPick.order`):

```cpp
struct MatchCandidate {
    enum class Source { kSceneDef, kPack } source;
    std::string id;
    int priority = 0;
    int weight = 1;
    std::vector<std::size_t> order;   // slot/role index -> caller actor-array index (as SlottedPick today)
};
```

`StartSceneByTags*` starts using the candidate's stored `order` + `source`, never a re-derived binding.

### §2.3 Filters are enforced on EVERY scene-def start path

`filters` = `gender` (existing, desugars to `filters.gender`), plus `keyword` and `race` (each a single
value or any-of array; multiple filter keys AND together). Enforcement is uniform:

| Start path | Binding | Filter behavior |
|---|---|---|
| `StartScene` / `StartSceneAt` | role-declaration order | validate each bound actor against its role's filters → reject (named) on violation |
| `StartSceneRoles` | explicit named binding | validate → reject (named) on violation |
| `StartSceneByTags` / `StartSceneByTagsQuery` | OSF complete matching (§2.1) | a role's filters are part of "compatible" — unfillable role ⇒ candidate rejected |

The explicit paths (`StartScene`/`StartSceneAt`) bind by declaration order then validate; they do **not**
run matching (matching is what the `*ByTags*` paths are for).

### §2.4 `gender` vs `filters.gender`

`gender` is legacy shorthand for `filters.gender`. Validation: both present **and differ → reject** the
scene at load (named); both present and equal → accept, normalize internally to `filters.gender`.

### §2.5 Keyword predicate semantics (public contract stays vague)

Public contract: *"`keyword` matches the actor using OSF's engine keyword predicate, equivalent to the
game's actor keyword condition as closely as the RE surface allows."* The concrete accessor
(`BGSKeywordForm::HasKeyword` vs actor-base vs race keywords vs a vfunc/offset helper) is **pinned in
RE.md after it's RE-verified** — don't overcommit the public contract before the accessor is proven
(same hedge used for the weapon-drawn bit). Whether race keywords count toward `keyword` must be defined
when the accessor lands.

## §3 Discovery & query surface

### §3.1 `FindScenes` is deterministic and filter-unaware

`FindScenes(int aiActorCount, string[] asTags)` keeps its signature: **actor-count + tag filtering
only**, returns ids ordered **priority descending, then id ascending**. `weight` is **ignored for
order** (it's sampling probability for `StartSceneByTags*`, not a relevance score). Because it has no
`Actor[]`, it **cannot evaluate keyword/race/gender filters** — it is a **discovery hint**, not a
guarantee that a later start will bind successfully (§3.3).

### §3.2 New actor-carrying query natives (no tag-prefix syntax)

Boolean tag queries + filter-aware matchmaking need `Actor[]`, so they arrive as **new natives**, not
`!`/`|` prefix conventions on tag strings (prefixes reserve characters in public tag names and create
escaping/debug problems):

```papyrus
string[] Function FindScenesForActorsQuery(Actor[] akActors, string[] akAllOf, string[] akAnyOf, string[] akNoneOf) Global Native
int      Function StartSceneByTagsQuery(Actor[] akActors, string[] akAllOf, string[] akAnyOf, string[] akNoneOf) Global Native
```

(`FindScenesForActorsQuery` is filter-aware — it has the actors — and returns ids deterministically per
§3.1. The legacy `FindScenes` stays for count-only discovery. `StartSceneByTags(akActors, akTags)`
stays as the all-of shorthand; `StartSceneByTagsQuery` is the boolean form.)

Query edge cases (defined, not implicit):
- empty `allOf` → no required tags; empty `anyOf` → ignored; empty `noneOf` → no exclusions.
- a tag in both `allOf` and `noneOf` → impossible query → no matches.
- duplicates de-duped before matching.
- case sensitivity == the existing tag matching (case-insensitive, `ToLower`).

### §3.3 Discovery → start gap (state it explicitly)

`FindScenes` returns ids only and is filter-unaware, so a `FindScenes → StartScene(actors, id)` flow can
**silently reject** (declaration-order binding fails a filter even though a valid binding exists).
Contract:

> `FindScenes`/`FindScenesForActorsQuery` report candidate ids; they do **not** promise a later
> `StartScene(actors, id)` reproduces a valid binding. For a filter-correct start, either let OSF bind
> (`StartSceneByTags*` — returns the handle, `GetSceneId` recovers the id) or bind manually
> (introspection getters + `StartSceneRoles`).

## §4 Weight

Optional scene-def `"weight"`: **integer in `[1, 1_000_000]`**; missing → `1`; non-int / `<= 0` /
out-of-range → **validation error** (named). Weighted-random selection sums weights with `uint64_t`
(overflow-safe) and samples **only within the top priority tier**. No `0`-means-"findable-not-startable"
class (keeps `FindScenes`/`StartSceneByTags` from diverging). Pack pseudo-candidates are always
`weight = 1` (no pack-schema change).

## §5 Form references (`keyword`/`race`)

OSF has no form-ref mechanism today; this is **new public surface** (SCENE_DESIGN §1.7). Refs are
`"Plugin.esm|0xLOCALID"` (Starfield strips runtime EditorIDs, so plugin + local FormID).

**Public schema doc (keep simple):**
```
"<plugin-filename>|0x<local-form-id>"   e.g. "Starfield.esm|0x00023A01"
- plugin filename matched case-insensitively by BASENAME (a path in the name → invalid)
- local form id only; runtime load-order bits (0xFF.. / 0xFE.. expanded) → invalid
```

**Implementation (RE.md, not the public doc):** CLSF exposes no `TESDataHandler::LookupForm`. Resolve
by finding the `TESFile*` in `TESDataHandler::compiledFileCollection` (full `files` 0..n / light
`smallFiles` 0xFE / medium `mediumFiles` 0xFD — tier layout RE-proven in osf-re, see
[TESDataHandler.h](../lib/commonlibsf/include/RE/T/TESDataHandler.h) notes), compose the runtime FormID
from the file's compile-index tier + the local id, then `TESForm::LookupByID` and confirm the form type
(`BGSKeyword` / `TESRace`). **Resolve once at scene load**, store the resolved `RE::BGSKeyword*` /
`RE::TESRace*` on the parsed role (not per matchmake).

Resolver validation (each → reject **that scene only**, with role + field in the message):
```
missing plugin           → scene 'x': role 'beast': filters.keyword[0] unresolved form 'Foo.esm|0x123' (no such plugin)
missing form             → ... unresolved form ... (form not found)
wrong form type          → ... filters.race[0] resolved to BGSKeyword, expected TESRace
path/garbage in plugin   → ... malformed form ref
runtime load-order bits  → ... form ref must use a local id, not a runtime id
```
Surfaced via `GetSceneValidationErrors` like other ref validation (§1.6).

**Content-neutrality:** keyword/race are engine concepts; OSF learns generic predicates, never SIF's
species taxonomy (race+keyword→"Terrormorph", variant maps) — that vocabulary stays in SIF.

## §6 Compatibility impact

Native signatures don't change, but this is **behavior-changing**:

> Extending `StartSceneByTags`/`FindScenes` from pack-only to pack+scene-def matchmaking changes
> observable behavior. The moment any matching scene def exists, it joins the candidate pool — existing
> pack-only callers may receive scene-def ids and see priority-based selection where they previously saw
> uniform pack selection.

Mitigations:
- pack candidates remain, as `priority = 0` / `weight = 1` pseudo-scenes;
- scene authors must set `priority > 0` to intentionally supersede packs;
- if a caller truly needs pack-only search, add `FindPacksByTags` later (additive).

(Main consumer is new — SIF. The SAF shim uses `StartSceneFiles`/`Sync`, not `StartSceneByTags`, so the
shim is unaffected; still, document the change rather than assume.)

## §7 Implementation shape

Don't bolt this into `PackRegistry`. Add a thin **matchmaking layer** (`src/Matchmaking/*` or
`SceneRuntime`-adjacent) that consumes both registries:

```cpp
struct TagQuery       { std::vector<std::string> allOf, anyOf, noneOf; };
struct RolePredicate  { SlotGender gender; std::vector<RE::BGSKeyword*> anyKeywords; std::vector<RE::TESRace*> anyRaces; };
// MatchCandidate as in §2.2
```

Flow (one path feeds `FindScenes*` and `StartSceneByTags*`, so they can't diverge):
```
1. normalize query (de-dupe, lower)            5. resolve role/slot binding (complete matching §2.1)
2. gather scene-def candidates                 6. FindScenes*: sort priority desc, id asc → ids
3. gather pack pseudo-candidates               7. StartSceneByTags*: top priority tier → weighted-random
4. suppress shadowed packs (§1) + count/tag    8. start from candidate.order + candidate.source (§2.2)
```

## §8 Contract / doc impact (when this lands)
- SCENE_DESIGN.md §1.2 (matchmaking spans scene defs; complete matching replaces greedy wording),
  §1.3 (`roles[].filters` keyword/race, scene `weight`, `gender` vs `filters.gender`), §1.6
  (filter/weight/form-ref validation), §1.7 (the form-ref scheme is new public surface;
  `*Query` natives).
- API.md (the `*Query` natives + the discovery→start gap, §3.3), OSF.psc.
- RE.md (form-ref resolver via `compiledFileCollection`; the keyword/race predicate accessors, once proven).
- PACK_SCHEMA.md only if pack slots ever gain filters — out of scope; filters stay a scene concept.
- All additive at the **signature** level (new natives, new optional fields); §6 covers the behavioral change.

## §9 Phasing
1. **Matchmaking layer + scene-def candidates** (the unlock): candidate model, shadow rule, complete
   matching, deterministic `FindScenes`, binding carried + started from. Items 5/6 are trivial on top.
2. **Role filters + form-ref resolver** (item 3): the `Plugin.esm|0x..` resolver, keyword/race
   predicates (the RE step for the accessor), load-time resolution/validation + `gender` conflict rule.
3. **Weight (5)** and **boolean `*Query` natives (6)**: small, on top of (1).

## §10 Tests to add to verification

```
Compatibility / IDs:
- pack-only install: StartSceneByTags stays ~uniform among fitting packs.
- scene def + pack same id: pack shadowed in matchmaking; anim:<id> still starts the pack.
- scene def priority 0 competes with packs; priority 1 always beats pack priority 0.

FindScenes / queries:
- FindScenes order deterministic across repeated calls; weight does NOT affect order.
- invalid/unresolved scene defs never appear.
- allOf == legacy all-AND; anyOf empty ignored; noneOf excludes; allOf∩noneOf → empty; dupes de-duped.

Role binding:
- restrictive+generic role case does NOT false-reject (complete matching).
- StartSceneByTags* starts with the binding computed during matchmaking (not a re-derive).
- StartSceneRoles / StartScene / StartSceneAt all reject actors violating keyword/race/gender filters.

Weight:
- missing → 1; non-int / 0 / negative / >1e6 → validation error; weighted choice only in top tier.

Form refs:
- full + light + medium plugin refs resolve; missing plugin / missing form / wrong type each reject
  only that scene with a role/field-specific message; path-in-name and runtime-id refs rejected.

gender:
- gender + filters.gender equal → normalized; differing → scene rejected.
```

## Open items for sign-off
- §2.5: confirm whether `keyword` includes race-derived keywords (define when the accessor is RE'd).
- §3.2: final native names (`FindScenesForActorsQuery` / `StartSceneByTagsQuery`) — bikeshed before they freeze.
