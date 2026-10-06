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
	// Rejections read "<subject><sep><field> must be <expected>"; the text is user-facing (log,
	// GetSceneLoadErrors, System Health). Type-discriminated branches stay hand-written and call Reject().
	struct JsonText
	{
		std::string_view field;     // "" -> "'<key>'"
		std::string_view expected;  // "" -> the accessor's default noun
		std::string_view whole;     // non-empty -> replaces everything after the separator
	};

	// kSpace is for the prop parsers ("props template 'helmet' scale must be ...").
	enum class JsonJoin : std::uint8_t
	{
		kColon,
		kSpace
	};

	// Non-owning over the json; owns the subject because call sites pass temporaries.
	class JsonView
	{
	public:
		JsonView(const nlohmann::json& a_json, std::string a_subject, JsonJoin a_join = JsonJoin::kColon) :
			_json(a_json), _subject(std::move(a_subject)), _join(a_join)
		{}
		[[nodiscard]] const nlohmann::json* Find(std::string_view a_key) const
		{
			if (!_json.is_object()) {
				return nullptr;
			}
			const auto it = _json.find(a_key);
			return it == _json.end() ? nullptr : &*it;
		}
		// Optional reads: a_out is written only when the key is present; wrong type throws.
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

		// Inclusive range; type and range failures are reported separately.
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

		// A string or an array of strings; absent yields empty. a_item.field should carry "entries".
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

		// Array only; not StringOrList so these keys keep rejecting a bare string.
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
