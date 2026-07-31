#pragma once

namespace OSF::Registry::RequirementRegistry
{
	// Load Data/OSF/**/*.requirements.json and report each consumer's highest
	// declared minimum before startup prompts are enabled.
	void LoadAll();
}
