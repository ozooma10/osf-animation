#pragma once
#include "OSFEngineAPI.h"
#include "OSFCatalog.h"
#include "API/CatalogRefreshQueue.h"
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
	struct ActorIdentityInfo
	{
		std::string species;
		std::string gender;
	};
	// One engine JSON round trip for both fields; prefer it when a caller needs species and sex.
	ActorIdentityInfo ActorIdentity(RE::Actor* actor);
	std::string ActorSpecies(RE::Actor* actor);
	std::string ActorGender(RE::Actor* actor);

	// Browser-owned copies of public metadata, refreshed only after catalog notifications.
	class CatalogData
	{
	public:
		static CatalogData& GetSingleton();
		// Initialize once on the frame owner before subscribing; callbacks run on the frame queue.
		void Initialize(std::function<void(bool)> onChanged, std::function<void()> onDurationsChanged = {});
		// Frame owner only. Also resumes a completion canceled by world replacement.
		void PublishPending();
		void Invalidate(bool scan = false) { if (m_refresh) m_refresh->Request(scan); }
		void InvalidateDurations() { m_durationDirty.store(true, std::memory_order_release); }
		const Catalog::Node* Find(std::string_view id);
		void ForEachDef(const std::function<void(const Catalog::Node&)>& fn);
		const std::vector<Catalog::File>& FileStats();
		const std::vector<std::string>& LoadErrors();
		std::size_t Size();
	private:
		void Refresh();
		std::unique_ptr<Catalog::Snapshot> m_snapshot = std::make_unique<Catalog::Snapshot>();
		std::atomic<bool> m_durationDirty{true};
		std::function<void(bool)> m_onChanged;
		std::function<void()> m_onDurationsChanged;
		std::unique_ptr<CatalogRefreshQueue> m_refresh;
	};
}
