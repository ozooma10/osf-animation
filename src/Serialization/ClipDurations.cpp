#include "Serialization/ClipDurations.h"
#include "Util/ClipPath.h"
#include "Util/StringUtil.h"

namespace OSF::Serialization::ClipDurations
{
	namespace
	{
		struct State
		{
			std::mutex                lock;
			Catalog::DecodedDurations entries;
			std::uint64_t             revision{};
		};
		State& GetState()
		{
			static auto* state = new State();
			return *state;
		}
	}
	std::string          KeyFor(std::string_view file, std::string_view animId) { return Util::ToLower(Util::ClipSpecDisplay(std::filesystem::path{ std::string{ file } })) + '|' + std::string{ animId }; }
	std::optional<float> Lookup(std::string_view file, std::string_view animId)
	{
		const auto       key = KeyFor(file, animId);
		auto&            state = GetState();
		std::scoped_lock lock{ state.lock };
		const auto       it = state.entries.find(key);
		return it == state.entries.end() ? std::nullopt : std::optional{ it->second.sec };
	}
	void Record(std::string_view file, std::string_view animId, float seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600)
			return;
		const auto       key = KeyFor(file, animId);
		auto&            state = GetState();
		std::scoped_lock lock{ state.lock };
		state.entries[key] = { seconds, ++state.revision };
	}
	Catalog::DecodedDurations Snapshot()
	{
		auto&            state = GetState();
		std::scoped_lock lock{ state.lock };
		return state.entries;
	}
}
