#pragma once

#include <functional>

namespace OSF::Serialization::ClipDurationCatalog
{
	// Explicit catalog demand: load the cache, probe current scene clips, prune, then persist a snapshot.
	// IO runs on one background worker; changes are announced on the frame queue.
	// Records made after the save snapshot remain dirty until the next requested scan.
	void ScanSceneClipsAsync(std::function<void()> a_onChanged);
}
