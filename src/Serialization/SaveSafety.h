#pragma once

namespace OSF::Serialization::SaveSafety
{
	// Registers the SaveLoadEvent sink, which (a) drops graphs and scenes before a load replaces
	// the world, and (b) opens a save window around every save-writing op (autosave/quicksave/
	// manual/exit save) that strips the per-NPC kAnimationDriven bit so a mid-scene save can't
	// bake it in (see GraphManager::OnSaveBegin). Plus a TESLoadGameEvent backstop that re-binds
	// the natives onto the rebuilt VM, does a late StopAll, and recovers a camera the load left
	// parked in an OSF-imposed alt state (CameraService::OnPostLoad). Call this at kPostDataLoad.
	// If the SaveLoadEvent prologue guard fails, only the backstop runs (no save window).
	void RegisterLoadEventSinks();
}
