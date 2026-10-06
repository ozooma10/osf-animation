#pragma once
#include "OSFEngineAPI.h"
#include "OSFCatalog.h"
#include <atomic>
#include <functional>
#include <string_view>

namespace OSF::API::EngineClient
{
	bool Connect(const OSFEngineAPI* api = RequestEngineAPI());
	const OSFEngineAPI& Get();
	bool Post(std::function<void()> task);
	void ReadJson(const char* text, void* context) noexcept;
	nlohmann::json ActiveScenes();
	nlohmann::json MatchAnchor(RE::TESObjectREFR* ref);
	nlohmann::json MatchCast(const std::vector<RE::Actor*>& actors);
	std::string ActorSpecies(RE::Actor* actor);
	std::string ActorGender(RE::Actor* actor);

	// Browser-owned copies of public metadata, refreshed only after catalog notifications.
	class CatalogData
	{
	public:
		static CatalogData& GetSingleton();
		void Invalidate() { m_revision.fetch_add(1, std::memory_order_release); }
		const Catalog::Scene* Find(std::string_view id);
		void ForEachDef(const std::function<void(const Catalog::Scene&)>& fn);
		const std::vector<Catalog::File>& FileStats();
		const std::vector<std::string>& LoadErrors();
		std::size_t Size();
	private:
		void Refresh();
		std::atomic<std::uint64_t> m_revision{1};
		std::uint64_t m_loadedRevision{};
		Catalog::Snapshot m_snapshot;
	};
}
