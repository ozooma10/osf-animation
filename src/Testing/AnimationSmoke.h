#pragma once

// Dev-only smoke runner; compiled only with xmake --animation_smoke=y.
namespace OSF::Testing::AnimationSmoke
{
	void Tick() noexcept;
	void OnWorldReset() noexcept;
}
