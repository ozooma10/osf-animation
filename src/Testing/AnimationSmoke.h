#pragma once

// Development-only mailbox and live smoke runner. Neither this translation unit nor
// its frame calls are present unless xmake is configured with --animation_smoke=y.
namespace OSF::Testing::AnimationSmoke
{
	void Tick() noexcept;
	void OnWorldReset() noexcept;
}
