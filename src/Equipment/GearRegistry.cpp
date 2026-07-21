#include "Equipment/GearRegistry.h"

#include "Util/FormRef.h"
#include "Util/StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace OSF::Equipment::Gear
{
	namespace
	{
		using json = nlohmann::json;
		using Util::ToLower;

		// Form-ref resolution is deferred to first game-thread use (load order is fixed by then) and
		// cached as a FormID, never a pointer (Starfield refcounts forms — see the keyword-cache fix).
		enum class Res : std::uint8_t
		{
			kPending,
			kOk,
			kBad  // malformed / plugin not loaded / not an ARMO — warned once, then skipped
		};

		struct Entry
		{
			std::string   ref;     // "<Plugin>|0xLOCAL" as authored
			std::string   slot;    // lowercase
			std::string   label;   // optional display name (v1.5 UI; carried through now)
			std::string   source;  // filename it loaded from, for collision warnings
			Res           res = Res::kPending;
			RE::TESFormID formId = 0;
		};

		struct OverrideRow
		{
			std::string actor;  // "player" or a form ref of the actor REFERENCE
			std::string slot;   // lowercase
			std::string item;   // form ref of a registered item, or "none" to suppress the slot
		};

		std::mutex               g_lock;  // guards the two lists (LoadAll vs game-thread selection)
		std::vector<Entry>       g_items;
		std::vector<OverrideRow> g_overrides;
		std::atomic<bool>        g_autoEquip{ true };

		// <Documents>\My Games\Starfield\SFSE\OSF\scene-gear.json (same home as wheel-pins.json),
		// or empty when the SFSE log directory can't be resolved.
		std::filesystem::path UserFilePath()
		{
			try {
				if (auto dir = SFSE::log::log_directory()) {
					return dir->parent_path() / "OSF" / "scene-gear.json";
				}
			} catch (...) {}
			return {};
		}

		// Parse one gear document's "items" array into a_items. a_seen dedups by lowercased ref —
		// first-loaded wins (user lane loads first, Data files in sorted order), mirroring the scene
		// registry's collision policy. Malformed entries warn and are skipped individually.
		void ParseItems(const json& a_doc, const std::string& a_source,
			std::vector<Entry>& a_items, std::unordered_map<std::string, std::string>& a_seen)
		{
			const auto it = a_doc.find("items");
			if (it == a_doc.end() || !it->is_array()) {
				return;
			}
			for (const auto& v : *it) {
				if (!v.is_object()) {
					continue;
				}
				Entry entry;
				entry.ref = v.value("item", std::string{});
				entry.slot = ToLower(v.value("slot", std::string{}));
				entry.label = v.value("label", std::string{});
				entry.source = a_source;
				if (entry.ref.empty() || entry.slot.empty()) {
					REX::WARN("[Equip] gear '{}': entry missing required 'item' or 'slot' — skipped", a_source);
					continue;
				}
				const std::string key = ToLower(entry.ref);
				if (const auto seen = a_seen.find(key); seen != a_seen.end()) {
					REX::WARN("[Equip] gear '{}': item '{}' already registered by '{}' — first wins",
						a_source, entry.ref, seen->second);
					continue;
				}
				a_seen.emplace(key, a_source);
				a_items.push_back(std::move(entry));
			}
		}

		// Parse the user file's "overrides" array. Only the user lane carries overrides; an
		// "overrides" key in a Data-lane file is ignored (those files are read-only registrations).
		void ParseOverrides(const json& a_doc, const std::string& a_source, std::vector<OverrideRow>& a_overrides)
		{
			const auto it = a_doc.find("overrides");
			if (it == a_doc.end() || !it->is_array()) {
				return;
			}
			for (const auto& v : *it) {
				if (!v.is_object()) {
					continue;
				}
				OverrideRow row;
				row.actor = v.value("actor", std::string{});
				row.slot = ToLower(v.value("slot", std::string{}));
				row.item = v.value("item", std::string{});
				if (row.actor.empty() || row.slot.empty() || row.item.empty()) {
					REX::WARN("[Equip] gear '{}': override missing 'actor', 'slot', or 'item' — skipped", a_source);
					continue;
				}
				a_overrides.push_back(std::move(row));
			}
		}

		// Load one gear file into the accumulators. Returns false only when the file exists but
		// won't parse (the caller logs); a missing file is a silent true.
		bool LoadFile(const std::filesystem::path& a_file, bool a_userLane,
			std::vector<Entry>& a_items, std::vector<OverrideRow>& a_overrides,
			std::unordered_map<std::string, std::string>& a_seen)
		{
			std::ifstream in(a_file, std::ios::binary);
			if (!in) {
				return true;
			}
			const json doc = json::parse(in, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			if (!doc.is_object()) {
				return false;
			}
			const std::string source = a_file.filename().string();
			ParseItems(doc, source, a_items, a_seen);
			if (a_userLane) {
				ParseOverrides(doc, source, a_overrides);
			}
			return true;
		}

		// Resolve a_entry's form ref (game thread; caller holds g_lock). kBad warns once at first use
		// — a ref naming an uninstalled plugin is expected (optional gear mods) and never fatal.
		void Resolve(Entry& a_entry)
		{
			if (a_entry.res != Res::kPending) {
				return;
			}
			auto* object = Util::ResolveBoundObject(a_entry.ref);
			if (!object) {
				a_entry.res = Res::kBad;
				REX::WARN("[Equip] gear item '{}' ('{}') did not resolve to a loaded item — plugin missing or ref malformed; skipped",
					a_entry.ref, a_entry.source);
				return;
			}
			if (!object->IsArmor()) {
				a_entry.res = Res::kBad;
				REX::WARN("[Equip] gear item '{}' ('{}') resolved to form {:#010x} but it is not an ARMO — skipped",
					a_entry.ref, a_entry.source, object->GetFormID());
				return;
			}
			a_entry.res = Res::kOk;
			a_entry.formId = object->GetFormID();
		}

		// Does a_row's actor key name a_actor? "player" matches the player; anything else is a form
		// ref of the actor reference. Dynamic (FF) actors can only ever match "player"-less rows by
		// ref, which cannot be authored — i.e. they have no overrides, by design (RFC §4.2).
		bool OverrideMatchesActor(const OverrideRow& a_row, RE::Actor* a_actor)
		{
			if (a_row.actor == "player") {
				return a_actor == static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton());
			}
			const auto id = Util::ComposeFormID(a_row.actor);
			return id && *id == a_actor->formID;
		}
	}

	void LoadAll()
	{
		namespace fs = std::filesystem;
		std::vector<Entry>                           items;
		std::vector<OverrideRow>                     overrides;
		std::unordered_map<std::string, std::string> seen;  // lowercased ref -> source (first wins)

		// User lane first, so a user registration wins a ref collision with a Data-lane file.
		if (const auto userFile = UserFilePath(); !userFile.empty()) {
			if (!LoadFile(userFile, /*userLane*/ true, items, overrides, seen)) {
				REX::ERROR("[Equip] gear file '{}' won't parse — user registrations and overrides ignored", userFile.string());
			}
		}

		const fs::path  dir = fs::current_path() / "Data" / "OSF";
		std::error_code ec;
		if (fs::is_directory(dir, ec)) {
			std::vector<fs::path> files;
			for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file(ec) && ToLower(entry.path().filename().string()).ends_with(".osfgear.json")) {
					files.push_back(entry.path());
				}
			}
			std::sort(files.begin(), files.end(), [](const fs::path& a_lhs, const fs::path& a_rhs) {
				return ToLower(a_lhs.filename().string()) < ToLower(a_rhs.filename().string());
			});
			for (const auto& file : files) {
				if (!LoadFile(file, /*userLane*/ false, items, overrides, seen)) {
					REX::ERROR("[Equip] gear file '{}' won't parse — skipped", file.filename().string());
				}
			}
		}

		const auto itemCount = items.size();
		const auto overrideCount = overrides.size();
		{
			std::lock_guard l{ g_lock };
			g_items = std::move(items);
			g_overrides = std::move(overrides);
		}
		if (itemCount || overrideCount) {
			REX::INFO("[Equip] gear registry: {} item(s), {} override(s) loaded", itemCount, overrideCount);
		}
	}

	bool AutoEquip()
	{
		return g_autoEquip.load(std::memory_order_relaxed);
	}

	void SetAutoEquip(bool a_enabled)
	{
		g_autoEquip.store(a_enabled, std::memory_order_relaxed);
	}

	std::string SlotOfForm(RE::TESFormID a_formId)
	{
		if (!a_formId) {
			return {};
		}
		std::lock_guard l{ g_lock };
		for (auto& entry : g_items) {
			Resolve(entry);
			if (entry.res == Res::kOk && entry.formId == a_formId) {
				return entry.slot;
			}
		}
		return {};
	}

	std::vector<Pick> SelectForActor(RE::Actor* a_actor, const std::vector<std::string>& a_takenSlots)
	{
		std::vector<Pick> picks;
		if (!a_actor || !AutoEquip()) {
			return picks;
		}
		std::lock_guard l{ g_lock };
		if (g_items.empty()) {
			return picks;
		}

		// Resolve pending refs once, then index by FormID for the inventory pass.
		std::unordered_map<RE::TESFormID, std::size_t> byForm;
		for (std::size_t i = 0; i < g_items.size(); i++) {
			Resolve(g_items[i]);
			if (g_items[i].res == Res::kOk) {
				byForm.emplace(g_items[i].formId, i);
			}
		}
		if (byForm.empty()) {
			return picks;
		}

		// One inventory pass: which registered items does this actor carry, and are they worn?
		// (Matched by FormID; the object pointer stays valid because the item is in the inventory.)
		struct Candidate
		{
			std::size_t         entry;
			RE::TESBoundObject* object;
			bool                worn;
		};
		std::vector<Candidate> candidates;
		{
			const auto guard = a_actor->inventoryList.LockRead();
			const RE::BGSInventoryList* list = *guard;
			if (!list) {
				return picks;  // never-materialized inventory (same guard as Hide) — no candidates
			}
			for (const auto& item : list->data) {
				if (!item.object) {
					continue;
				}
				if (const auto it = byForm.find(item.object->GetFormID()); it != byForm.end()) {
					candidates.push_back({ it->second, item.object, item.IsEquipped() });
				}
			}
		}
		if (candidates.empty()) {
			return picks;
		}
		// Stable ref order within a slot, so rule 3 ("first candidate") is deterministic.
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a_lhs, const Candidate& a_rhs) {
			return ToLower(g_items[a_lhs.entry].ref) < ToLower(g_items[a_rhs.entry].ref);
		});

		// Walk slots in candidate order; decide each slot once.
		std::vector<std::string> decided;
		for (const auto& lead : candidates) {
			const std::string& slot = g_items[lead.entry].slot;
			if (std::find(decided.begin(), decided.end(), slot) != decided.end()) {
				continue;
			}
			decided.push_back(slot);
			if (std::find(a_takenSlots.begin(), a_takenSlots.end(), slot) != a_takenSlots.end()) {
				REX::DEBUG("[Equip] actor {:X}: gear slot '{}' taken by the scene's authored equip — skipped",
					a_actor->formID, slot);
				continue;
			}

			// Rule 1: per-actor override (first matching row wins — user-file order).
			const Candidate* chosen = nullptr;
			bool             suppressed = false;
			for (const auto& row : g_overrides) {
				if (row.slot != slot || !OverrideMatchesActor(row, a_actor)) {
					continue;
				}
				if (ToLower(row.item) == "none") {
					suppressed = true;
					REX::DEBUG("[Equip] actor {:X}: gear slot '{}' suppressed by override", a_actor->formID, slot);
				} else {
					for (const auto& c : candidates) {
						if (g_items[c.entry].slot == slot && ToLower(g_items[c.entry].ref) == ToLower(row.item)) {
							chosen = &c;
							break;
						}
					}
					if (!chosen) {
						REX::DEBUG("[Equip] actor {:X}: gear override '{}' for slot '{}' not in inventory — falling through",
							a_actor->formID, row.item, slot);
						continue;  // kept on file so it revives when the item returns (RFC §5)
					}
				}
				break;
			}
			if (suppressed) {
				continue;
			}
			// Rule 2: a worn candidate; rule 3: the first candidate (list is ref-sorted).
			if (!chosen) {
				for (const auto& c : candidates) {
					if (g_items[c.entry].slot == slot && c.worn) {
						chosen = &c;
						break;
					}
				}
			}
			if (!chosen) {
				chosen = &lead;
			}
			picks.push_back({ chosen->object, chosen->worn, slot, g_items[chosen->entry].ref });
		}
		return picks;
	}
}
