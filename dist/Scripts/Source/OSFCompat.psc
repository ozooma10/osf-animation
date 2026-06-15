ScriptName OSFCompat Native
; COMPATIBILITY-ONLY natives for the SAF->OSF shim's primitive (non-Scene) path:
; standalone player control/camera locks. NOT public API. The content-neutral core
; never auto-applies these; scene-integrated control/camera policy is OSF Intimacy.

; Standalone player control lock for the primitive (non-Scene) path.
Function SetPlayerControlLock(bool abLocked) Global Native

; Standalone camera lock for the primitive path: forces 3rd person, bounces back
; on zoom-in to 1st.
Function SetPlayerCameraLock(bool abLocked) Global Native

; DEBUG/RE-bisect: replaces the standalone player-lock input-disable bitmasks
; (live-reapplies to an active lock, so the bit layout can be bisected in one
; session). 0/0 = block nothing. Parked on OSFCompat (NOT the public OSF surface)
; so it is not covered by the 1.0 never-remove ABI guarantee.
Function SetSceneControlMask(int aiUserMask, int aiOtherMask) Global Native
