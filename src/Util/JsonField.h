#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace OSF::Util
{
	// Typed field access that composes the registry's OWN diagnostic vocabulary.
	//
	// The content parsers reject malformed author JSON with a sentence naming the file, the field,
	// and what was expected — that text is a product feature, not debug output: it reaches
	// OSF Animation.log, OSFAdvanced.GetSceneLoadErrors(), and the System Health cards. Hand-writing
	// it meant every field cost a find/is_X/throw/get block, and the same "string or array" and
	// "reject unknown keys" rules existed several times over.
	//
	// Every accessor here composes exactly one shape:
	//
	//     <subject><sep><field> must be <expected>
	//
	// `field` defaults to "'<key>'" and `expected` to the accessor's own noun, so a site already on
	// that wording passes nothing. Sites that were never in that shape pass `whole`.
	//
	// This is a VALIDATION layer, not a parsing one. Where the JSON *type* is the schema's
	// discriminator (a clip is a string OR an object; a sound `at` is a scalar, array, or ladder
	// object), the branch stays hand-written and only its else-arm calls Reject(), so the rejection
	// still reads like every other one.

	// How one rejection reads. Leave a field empty to take the accessor's default.
	struct JsonText
	{
		std::string_view field;     // "" -> "'<key>'"
		std::string_view expected;  // "" -> the accessor's default noun
		std::string_view whole;     // non-empty -> replaces everything after the separator
	};

	// The prop parsers build "props template 'helmet' scale must be ..." — subject and text joined
	// by a SPACE. Everything else uses ": ".
	enum class JsonJoin : std::uint8_t
	{
		kColon,
		kSpace
	};

	// A json value plus the diagnostic prefix that names it. Non-owning over the json but OWNS its
	// subject, because call sites pass temporaries (`JsonView{ j, a_subject + " layer" }`).
	class JsonView
	{
	public:
		JsonView(const nlohmann::json& a_json, std::string a_subject, JsonJoin a_join = JsonJoin::kColon) :
			_json(a_json), _subject(std::move(a_subject)), _join(a_join)
		{}

		[[nodiscard]] const nlohmann::json& Json() const noexcept { return _json; }
		[[nodiscard]] const std::string&    Subject() const noexcept { return _subject; }

		// Presence, unchecked — for the discriminator sites that keep branching by hand.
		[[nodiscard]] const nlohmann::json* Find(std::string_view a_key) const
		{
			if (!_json.is_object()) {
				return nullptr;
			}
			const auto it = _json.find(a_key);
			return it == _json.end() ? nullptr : &*it;
		}

		[[nodiscard]] bool Has(std::string_view a_key) const { return Find(a_key) != nullptr; }

		// The view's own value must be an object.
		void RequireObject(JsonText a_text = {}) const
		{
			if (!_json.is_object()) {
				throw std::runtime_error(Compose({}, a_text, "an object"));
			}
		}

		// --- Optional reads. Assign a_out ONLY when the key is present; a present wrong-typed
		// value is a rejection. Returns whether a_out was written, for sites that branch on it.

		bool Read(std::string_view a_key, std::string& a_out, JsonText a_text = {}) const
		{
			const auto* value = Find(a_key);
			if (!value) {
				return false;
			}
			if (!value->is_string()) {
				throw std::runtime_error(Compose(a_key, a_text, "a string"));
			}
			a_out = value->get<std::string>();
			return true;
		}

		bool ReadNonEmpty(std::string_view a_key, std::string& a_out, JsonText a_text = {}) const
		{
			const auto* value = Find(a_key);
			if (!value) {
				return false;
			}
			if (!value->is_string() || value->get_ref<const std::string&>().empty()) {
				throw std::runtime_error(Compose(a_key, a_text, "a non-empty string"));
			}
			a_out = value->get<std::string>();
			return true;
		}

		bool Read(std::string_view a_key, bool& a_out, JsonText a_text = {}) const
		{
			const auto* value = Find(a_key);
			if (!value) {
				return false;
			}
			if (!value->is_boolean()) {
				throw std::runtime_error(Compose(a_key, a_text, "a boolean"));
			}
			a_out = value->get<bool>();
			return true;
		}

		// Integer plus an inclusive range, because the schema distinguishes the two failures:
		// "'weight' must be an integer" and "'weight' must be in [1, 1000000]". The range text is
		// composed from the bounds, so it cannot drift from the check.
		bool ReadInt(std::string_view a_key, std::int32_t& a_out, std::int64_t a_min, std::int64_t a_max,
			JsonText a_type = {}, JsonText a_range = {}) const
		{
			const auto* value = Find(a_key);
			if (!value) {
				return false;
			}
			if (!value->is_number_integer()) {
				throw std::runtime_error(Compose(a_key, a_type, "an integer"));
			}
			const auto raw = value->get<std::int64_t>();
			if (raw < a_min || raw > a_max) {
				const std::string range = "in [" + std::to_string(a_min) + ", " + std::to_string(a_max) + "]";
				throw std::runtime_error(Compose(a_key, a_range, range));
			}
			a_out = static_cast<std::int32_t>(raw);
			return true;
		}

		// --- Required reads.

		[[nodiscard]] std::string RequiredNonEmpty(std::string_view a_key, JsonText a_text = {}) const
		{
			const auto* value = Find(a_key);
			if (!value || !value->is_string() || value->get_ref<const std::string&>().empty()) {
				throw std::runtime_error(Compose(a_key, a_text, "a non-empty string"));
			}
			return value->get<std::string>();
		}

		// --- The shapes this schema repeats.

		// `<key>` is a single string OR an array of strings — filters.keyword, filters.race,
		// anchor.keyword, anchor.base, source.equippedArmor.keyword. Absent yields an empty vector,
		// so callers keep the "not authored" case as an empty loop rather than a branch.
		//
		// Pass the item text's `field` with the word "entries" already on it
		// (`{ .field = "filters.keyword entries" }`), which is how one composition rule covers both
		// the shape rejection and the element rejection.
		[[nodiscard]] std::vector<std::string> StringOrList(std::string_view a_key,
			JsonText a_shape = {}, JsonText a_item = {}, bool a_nonEmptyItems = false) const
		{
			std::vector<std::string> out;
			const auto* value = Find(a_key);
			if (!value) {
				return out;
			}
			if (value->is_string()) {
				AppendString(out, *value, a_key, a_item, a_nonEmptyItems);
			} else if (value->is_array()) {
				for (const auto& entry : *value) {
					AppendString(out, entry, a_key, a_item, a_nonEmptyItems);
				}
			} else {
				throw std::runtime_error(Compose(a_key, a_shape, "a string or array of strings"));
			}
			return out;
		}

		// `<key>` must be an ARRAY of strings — stage `tags`, sceneControls `disable`,
		// `preserveBones`. Deliberately NOT StringOrList: these keys have never accepted a bare
		// string, and quietly starting to would widen the schema.
		[[nodiscard]] std::vector<std::string> ReadList(std::string_view a_key,
			JsonText a_shape = {}, JsonText a_item = {}, bool a_nonEmptyItems = false) const
		{
			std::vector<std::string> out;
			const auto* value = Find(a_key);
			if (!value) {
				return out;
			}
			if (!value->is_array()) {
				throw std::runtime_error(Compose(a_key, a_shape, "an array of strings"));
			}
			for (const auto& entry : *value) {
				AppendString(out, entry, a_key, a_item, a_nonEmptyItems);
			}
			return out;
		}

		// Reject any key outside a_allowed. Used where the authored namespace is a deliberately
		// strict subset (prop templates, route layers), so a stray key that would silently do
		// nothing is an error instead.
		void RejectUnknownKeys(std::span<const char* const> a_allowed,
			std::string_view a_lead = "has unknown key", std::string_view a_trailer = {}) const
		{
			if (!_json.is_object()) {
				return;
			}
			for (const auto& [key, value] : _json.items()) {
				(void)value;
				const bool known = std::ranges::any_of(a_allowed,
					[&key](const char* a_candidate) { return key == a_candidate; });
				if (!known) {
					std::string text = _subject;
					text += Separator();
					text += a_lead;
					text += " '";
					text += key;
					text += '\'';
					text += a_trailer;
					throw std::runtime_error(text);
				}
			}
		}

		// --- Lenient. The right type wins; anything else leaves a_out alone and NEVER throws.
		// Deliberately a different name from Read: the file-header harvest (pack/section/folder
		// labels) must not turn a cosmetic mistake into a whole-file parse failure.
		bool TryRead(std::string_view a_key, std::string& a_out) const
		{
			const auto* value = Find(a_key);
			if (!value || !value->is_string()) {
				return false;
			}
			a_out = value->get<std::string>();
			return true;
		}

		// The rejection every accessor uses, so a hand-written discriminator branch throws the
		// identical sentence.
		[[noreturn]] void Reject(std::string_view a_key, JsonText a_text,
			std::string_view a_default) const
		{
			throw std::runtime_error(Compose(a_key, a_text, a_default));
		}

	private:
		void AppendString(std::vector<std::string>& a_out, const nlohmann::json& a_entry,
			std::string_view a_key, JsonText a_item, bool a_nonEmpty) const
		{
			if (!a_entry.is_string() || (a_nonEmpty && a_entry.get_ref<const std::string&>().empty())) {
				throw std::runtime_error(Compose(a_key, a_item, a_nonEmpty ? "non-empty strings" : "strings"));
			}
			a_out.push_back(a_entry.get<std::string>());
		}

		[[nodiscard]] std::string_view Separator() const noexcept
		{
			return _join == JsonJoin::kColon ? ": " : " ";
		}

		[[nodiscard]] std::string Compose(std::string_view a_key, JsonText a_text,
			std::string_view a_default) const
		{
			std::string out = _subject;
			out += Separator();
			if (!a_text.whole.empty()) {
				out += a_text.whole;
				return out;
			}
			if (a_text.field.empty()) {
				out += '\'';
				out += a_key;
				out += '\'';
			} else {
				out += a_text.field;
			}
			out += " must be ";
			out += a_text.expected.empty() ? a_default : a_text.expected;
			return out;
		}

		const nlohmann::json& _json;
		std::string           _subject;
		JsonJoin              _join;
	};
}
