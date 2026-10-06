#include "API/EngineClient.h"
#include <algorithm>
#include <cctype>
#include <memory>

namespace OSF::API::EngineClient
{
	namespace { const OSFEngineAPI* g_api{}; }
	bool Connect(const OSFEngineAPI* api) {
		g_api=api && api->version == kOSFEngineAPIVersion && api->size >= sizeof(OSFEngineAPI) ? api : nullptr;
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
	std::string ActorSpecies(RE::Actor* actor) {
		nlohmann::json result=nlohmann::json::object(); Get().ReadActorIdentity(actor,ReadJson,&result); return result.is_object() ? result.value("species",std::string{}) : std::string{};
	}
	std::string ActorGender(RE::Actor* actor) {
		nlohmann::json result=nlohmann::json::object(); Get().ReadActorIdentity(actor,ReadJson,&result); return result.is_object() ? result.value("gender",std::string{}) : std::string{};
	}
	CatalogData& CatalogData::GetSingleton() { static CatalogData value; return value; }
	void CatalogData::Refresh()
	{
		const auto revision=m_revision.load(std::memory_order_acquire);
		if (revision == m_loadedRevision) return;
		nlohmann::json result;
		if (!Get().ReadCatalog(ReadJson,&result) || !result.is_object() || result.value("schema",0) != 1) return;
		m_snapshot=result.get<Catalog::Snapshot>(); m_loadedRevision=revision;
	}
	const Catalog::Scene* CatalogData::Find(std::string_view id)
	{
		Refresh();
		const auto equal=[](char a,char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
		const auto it=std::ranges::find_if(m_snapshot.scenes,[&](const auto& s) { return std::ranges::equal(s.id,id,equal); });
		return it == m_snapshot.scenes.end() ? nullptr : &*it;
	}
	void CatalogData::ForEachDef(const std::function<void(const Catalog::Scene&)>& fn) { Refresh(); for (const auto& scene:m_snapshot.scenes) fn(scene); }
	const std::vector<Catalog::File>& CatalogData::FileStats() { Refresh(); return m_snapshot.files; }
	const std::vector<std::string>& CatalogData::LoadErrors() { Refresh(); return m_snapshot.errors; }
	std::size_t CatalogData::Size() { Refresh(); return m_snapshot.scenes.size(); }
}
