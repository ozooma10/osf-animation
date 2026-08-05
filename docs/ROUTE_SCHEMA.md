# Overlay route schema (v1)

Overlay routes are persistent, one-actor presentation controllers for masked animation that must
leave scene policy alone. They are loaded from the top-level `routes` array in any schema-1
`*.osf.json` file and are driven by native consumers through `OSFOverlayAPI.h`.

```json
{
  "schema": 1,
  "routes": [{
    "id": "suitprotocol.helmet",
    "stations": [
      { "id": "head" },
      { "id": "held", "layer": {
        "clip": "OSF/Suit/head_to_held.af", "holdAt": 1.0,
        "mask": "upperBody", "mode": "override"
      }},
      { "id": "stowed" }
    ],
    "transitions": [{
      "id": "head-to-held", "from": "head", "to": "held",
      "layer": { "clip": "OSF/Suit/head_to_held.af", "mask": "upperBody" },
      "commit": { "atFrame": 24, "marker": "suitprotocol.helmet.hide_head" },
      "markers": [{ "atFrame": 20, "id": "suitprotocol.helmet.moving" }],
      "sound": [{ "atFrame": 23, "spec": "$suit,helmet" }]
    }]
  }]
}
```

Stations without `layer` are zero-animation stations: OSF retains controller state but removes its
graph, leaving the actor fully engine-driven. An animated station needs `clip`, a named `mask`, and
optionally `mode` (`override` or `additive`), `weight`, and `holdAt` in `[0,1]`.

Transitions are directed. `from` and `to` must name stations; `layer.clip` and a named `layer.mask`
are mandatory. Requests use authored-order BFS by edge count and return `noPath` when the requested
pair is unreachable. `interrupt` is `finish` by default or `crossfade-before-commit`. Reverse travel
requires an ordinary authored reverse transition.

An override transition may add `contactPose`: an `atFrame`, 1-16 `bones`, and optional
`approachFrames`, `fullBeforeFrames`, `fullAfterFrames`, and `releaseFrames`. Those clip bones ease in
around contact and then return to the engine pose. The older live-IK `reach` experiment is not part
of the route schema.

Timing is always at 30 fps. A `commit` has one `atFrame` and external `marker`; the owner callback
must acknowledge it. Numeric commit, marker, prop, and sound times are checked against the decoded
clip duration before graph mutation, and a time at or after the clip end rejects playback.

Markers are instantaneous callbacks and sounds are one-shot playback requests; neither accepts a
`lifetime` field. Props alone declare `lifetime` as `transition`, `station`, `controller`, or
`external`. An attached prop uses `attachAtFrame`, `prop`, and either inline `source`/attachment
fields or a matching top-level `props` template. A destroy entry uses `destroyAtFrame`. OSF never
mutates external objects: an `external` prop needs no `source`, and the owner receives a typed
`kPropAttach` or `kPropDestroy` callback with its id in `event.prop` at the authored frame.

Routes reject scene policy (`stripActors`, `lockPlayer`, `camera`, `inPlace`, `fade`,
`playerControl`, `clearHeldItems`, `roles`) and future arbitration fields (`claims`, `conditions`,
`when`). Root/world motion stays engine-owned: route plans force `anchored=false` and an authored
mask can never include an implicit actor-wide fallback.

Phase 1 permits one live route per actor, even at a zero-animation station. A parked route therefore
blocks other owners until it is ended. Scenes always win: because Phase 1 has one stamper per actor,
OSF stops the overlay immediately before admitting the scene (there is no outgoing-overlay/incoming-
scene crossfade), destroys OSF-owned visual props, accepts pending station requests, and reconciles
from the acknowledged checkpoint when the scene ends. Routes and actor references are cleared on
world replacement and are never saved; owner registrations survive so consumers can rebuild from
their own semantic state.
