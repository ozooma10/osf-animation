ScriptName OSFCompat Native Hidden
; ============================================================================
; OSFCompat - non-public compatibility natives for the SAF -> OSF shim. 
; These are the escape hatches SAFScript/SAF need to reproduce NAF behaviour OSF deliberately doesn't expose:
;   - standalone player control / camera lock for the primitive (non-Scene) path
;   - the engine crosshair target (PlayerCharacter->commandTarget)
; ============================================================================

; Standalone player control lock: input-disable layer + persistent AI-driven flag.
; false releases.
Function SetPlayerControlLock(bool abLocked) Global Native

; Standalone camera lock: forces/holds third person (bounces on zoom-in). false restores.
Function SetPlayerCameraLock(bool abLocked) Global Native

; The raw engine crosshair reference (any ref kind), or None when the reticle is on nothing.
ObjectReference Function GetCrosshairRef() Global Native

; The crosshair reference cast to Actor, or None when it's on nothing / a non-actor ref.
Actor Function GetCrosshairActor() Global Native
