# Deferred — parked for post-launch

Work that was started or designed but is **not part of the initial release**. Kept
here (out of `dist/`, so it can never leak into the packaged archive) rather than
deleted, because the design work is sound and these are the leading post-launch
candidates. Nothing in this folder is built, wired, or staged.

## CrewLife/

A Papyrus-only ambient-companion module (moments cast on crew). Never finished:

- Needs Creation Kit quest wiring that does not exist (`CrewLifeMain` expects
  sibling scripts attached by property on a start-game-enabled quest).
- References `OSF.GetConfig*` natives that were never added to the framework.
- Overlaps the **Ambient NPC Life Director** (see `../GameplayModuels_Plan.md`
  Phase 2), which is the better-architected native version of the same idea.

Revive path: fold into the Ambient Director effort rather than shipping as-is.

## WheelPins_Plan.md

Design for user-pinnable emote-wheel entries (persisted pin list + rounder,
count-adaptive wheel geometry). Pure QoL — the shipping tag-prefix wheel works
without it. Plan only; no code was written. Good early post-launch pickup.

## Also deferred (no artifacts to park — plan-only, in root)

- **Ambient NPC Life Director** — `../GameplayModuels_Plan.md` Phase 2. Never
  built (no `src/Director/`). The intended flagship post-launch feature.
- **Item-use animations** (eat/drink/med) — `../GameplayModuels_Plan.md` Phase 3.4.
  Never built; flagged highest-risk in its own plan.
