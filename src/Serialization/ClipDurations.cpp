#include "Serialization/ClipDurations.h"
#include "Serialization/ClipDurationState.h"
#include "Util/ClipPath.h"
#include "Util/StringUtil.h"

namespace OSF::Serialization::ClipDurations
{
	namespace Detail
	{
		State& GetState()
		{
			// Process lifetime: the optional catalog worker may still be running during CRT teardown.
			static auto* state = new State();
			return *state;
		}

		std::string KeyFor(std::string_view a_fileSpec, std::string_view a_animId)
		{
			return Util::ToLower(Util::ClipSpecDisplay(std::filesystem::path{ std::string{ a_fileSpec } })) +
			       '|' + std::string{ a_animId };
		}
	}

	std::optional<float> Lookup(std::string_view a_fileSpec, std::string_view a_animId)
	{
		const auto key = Detail::KeyFor(a_fileSpec, a_animId);
		auto& state = Detail::GetState();
		std::scoped_lock lock{ state.lock };
		const auto it = state.entries.find(key);
		return it == state.entries.end() ? std::nullopt : std::optional{ it->second.sec };
	}

	void Record(std::string_view a_fileSpec, std::string_view a_animId, float a_seconds)
	{
		if (!Detail::PlausibleSec(a_seconds)) return;
		const auto key = Detail::KeyFor(a_fileSpec, a_animId);
		auto& state = Detail::GetState();
		std::scoped_lock lock{ state.lock };
		auto& entry = state.entries[key];
		entry.decodedRevision = ++state.decodedRevision;
		if (entry.exact && std::abs(entry.sec - a_seconds) <= Detail::kExactConfirmTolerance) return;
		entry.sec = a_seconds;
		entry.exact = true;
		++state.revision;
	}
}
