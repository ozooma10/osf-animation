#include "API/EngineClient.h"
#include "API/BrowserClipLibrary.h"
#include "API/BrowserCatalogJson.h"
#include "Serialization/ClipDurationCache.h"
#include <algorithm>
#include <cctype>
#include <memory>

namespace OSF::API::EngineClient
{
	namespace { const OSFEngineAPI* g_api{}; }
	bool Connect(const OSFEngineAPI* api) {
		g_api=api && EngineTableSupported(kOSFEngineAPIVersion, api->version, api->size, sizeof(OSFEngineAPI)) ? api : nullptr;
		if (api && !g_api) REX::ERROR("[API] Engine services incompatible: requested {:#x}/{} bytes, received {:#x}/{} bytes",
			kOSFEngineAPIVersion, sizeof(OSFEngineAPI), api->version, api->size);
		return g_api != nullptr;
	}
	const OSFEngineAPI& Get() { return *g_api; }
	bool Post(std::function<void()> task)
	{
		if (!g_api) return false;
		auto context=std::make_unique<std::function<void()>>(std::move(task));
		const bool accepted=g_api->Post([](void* value, bool execute) noexcept {
			std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(value));
			if (execute) { try { (*owned)(); } catch (...) { REX::ERROR("[UI] Browser frame command failed"); } }
		},context.get());
		if (accepted) context.release();
		return accepted;
	}
	void ReadJson(const char* text, void* context) noexcept
	{
		try { *static_cast<nlohmann::json*>(context)=nlohmann::json::parse(text); }
		catch (...) { *static_cast<nlohmann::json*>(context)=nullptr; }
	}
	nlohmann::json ActiveScenes() { auto result=nlohmann::json::array(); Get().ReadActiveScenes(ReadJson,&result); return result; }
	nlohmann::json MatchAnchor(RE::TESObjectREFR* ref) { auto result=nlohmann::json::array(); Get().MatchAnchor(ref,ReadJson,&result); return result; }
	nlohmann::json MatchCast(const std::vector<RE::Actor*>& actors) {
		auto result=nlohmann::json::array(); Get().MatchCast(actors.data(),static_cast<std::uint32_t>(actors.size()),ReadJson,&result); return result;
	}
	ActorIdentityInfo ActorIdentity(RE::Actor* actor) {
		nlohmann::json result=nlohmann::json::object(); Get().ReadActorIdentity(actor,ReadJson,&result);
		if (!result.is_object()) return {};
		return { result.value("species",std::string{}), result.value("gender",std::string{}) };
	}
	std::string ActorSpecies(RE::Actor* actor) { return ActorIdentity(actor).species; }
	std::string ActorGender(RE::Actor* actor) { return ActorIdentity(actor).gender; }
	CatalogData& CatalogData::GetSingleton() { static auto* value = new CatalogData(); return *value; }
	void CatalogData::Initialize(std::function<void(bool)> onChanged, std::function<void()> onDurationsChanged)
	{
		if (m_refresh) return;
		m_onChanged = std::move(onChanged);
		m_onDurationsChanged = std::move(onDurationsChanged);
		m_refresh = std::make_unique<CatalogRefreshQueue>([] {
			std::unique_ptr<Catalog::Snapshot> snapshot;
			if (!Get().ReadCatalog(BrowserCatalogJson::Read, &snapshot) || !snapshot)
				return std::unique_ptr<Catalog::Snapshot>{};
			BrowserClipLibrary::Decorate(*snapshot);
			return snapshot;
		}, [this] { Post([this] { PublishPending(); }); });
	}
	void CatalogData::PublishPending()
	{
		// Publish only in frame commands, never a nested metadata getter: references
		// returned by Find/FileStats remain valid throughout the current command.
		if (m_refresh) if (auto completed = m_refresh->Publish(m_snapshot)) {
			if (completed->success) m_durationDirty.store(true, std::memory_order_release);
			if (completed->scan) Browser::ClipDurationCache::Scan(m_snapshot->clips, m_onDurationsChanged);
			Refresh();
			if (!completed->success) REX::ERROR("[UI] Could not refresh catalog metadata; retaining the previous catalog");
			if (m_onChanged) m_onChanged(completed->success);
		}
	}
	void CatalogData::Refresh()
	{
		if (!m_durationDirty.exchange(false, std::memory_order_acq_rel)) return;
		Browser::ClipDurationCache::RefreshDecoded();
		for (auto& scene : m_snapshot->nodes) for (auto& stage : scene.stages) {
			if (!stage.durationKey.empty()) {
				stage.loopSec = Browser::ClipDurationCache::Lookup(stage.durationKey).value_or(-1.0f);
			}
		}
	}
	const Catalog::Node* CatalogData::Find(std::string_view id)
	{
		Refresh();
		const auto equal=[](char a,char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
		const auto it=std::ranges::find_if(m_snapshot->nodes,[&](const auto& s) { return std::ranges::equal(s.id,id,equal); });
		return it == m_snapshot->nodes.end() ? nullptr : &*it;
	}
	void CatalogData::ForEachDef(const std::function<void(const Catalog::Node&)>& fn) { Refresh(); for (const auto& scene:m_snapshot->nodes) fn(scene); }
	const std::vector<Catalog::File>& CatalogData::FileStats() { Refresh(); return m_snapshot->files; }
	const std::vector<std::string>& CatalogData::LoadErrors() { Refresh(); return m_snapshot->errors; }
	std::size_t CatalogData::Size() { Refresh(); return m_snapshot->nodes.size(); }
}
