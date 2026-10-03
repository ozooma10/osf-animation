#pragma once

namespace OSF::Registry::RequirementRegistry
{
	// Reports each consumer's highest declared minimum; call before startup prompts are enabled.
	void LoadAll();
}
