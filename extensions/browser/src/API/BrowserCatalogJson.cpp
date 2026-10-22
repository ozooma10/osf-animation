#include "API/BrowserCatalogJson.h"
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace OSF::API::BrowserCatalogJson
{
	namespace
	{
		using json = nlohmann::json;
		enum class Kind { Scalar, String, Array, Object };
		struct Operations;
		struct Target { void* value{}; const Operations* ops{}; };
		struct Member { Target target; std::uint64_t bit{}; };
		struct Operations {
			Kind kind;
			bool (*scalar)(void*, const json&){};
			void (*string)(void*, std::string&){};
			Target (*append)(void*){};
			Member (*member)(void*, std::string_view){};
			std::uint64_t required{};
		};
		template <class T> Target Ref(T& value);
		template <class T> inline constexpr bool isVector = false;
		template <class T> inline constexpr bool isVector<std::vector<T>> = true;
		struct CountFields {
			unsigned count{};
			template <class T> void Field(const char*, const T&) { ++count; }
		};
		struct FindField {
			std::string_view key;
			Member found;
			unsigned index{};
			template <class T> void Field(std::string_view name, T& value) {
				const auto i = index++;
				if (key == name) found = {Ref(value), std::uint64_t{1} << i};
			}
		};
		template <class T> Operations MakeOperations()
		{
			if constexpr (std::is_arithmetic_v<T>) {
				return {Kind::Scalar, [](void* value, const json& scalar) {
					// Retain the released converters' numeric/bool conversion rules.
					try { *static_cast<T*>(value) = scalar.get<T>(); return true; }
					catch (const json::exception&) { return false; }
				}};
			} else if constexpr (std::is_same_v<T, std::string>) {
				return {Kind::String, nullptr, [](void* value, std::string& text) {
					*static_cast<std::string*>(value) = std::move(text);
				}};
			} else if constexpr (isVector<T>) {
				return {Kind::Array, nullptr, nullptr, [](void* value) {
					auto& values = *static_cast<T*>(value);
					return Ref(values.emplace_back());
				}};
			} else {
				T empty;
				CountFields fields;
				VisitFields(fields, empty);
				if (fields.count > 64) throw std::logic_error("catalog record exceeds field mask");
				return {Kind::Object, nullptr, nullptr, nullptr, [](void* value, std::string_view key) {
					FindField find{key};
					VisitFields(find, *static_cast<T*>(value));
					return find.found;
				}, fields.count == 64 ? ~std::uint64_t{} : (std::uint64_t{1} << fields.count) - 1};
			}
		}
		template <class T> Target Ref(T& value)
		{
			static const auto ops = MakeOperations<T>();
			return {&value, &ops};
		}

		// The library lexer handles JSON syntax, escapes, UTF-8 and number decoding.
		// Frames address owned fields/elements only. Parent arrays cannot grow until
		// their current element's nested frames have ended, so these pointers stay valid.
		class Reader : public nlohmann::json_sax<json>
		{
		public:
			explicit Reader(Catalog::Snapshot& snapshot) : m_root(Ref(snapshot)) { m_stack.reserve(8); }
			bool legacy{};
			bool null() override { return Scalar(nullptr); }
			bool boolean(bool v) override { return Scalar(v); }
			bool number_integer(number_integer_t v) override { return Scalar(v); }
			bool number_unsigned(number_unsigned_t v) override { return Scalar(v); }
			bool number_float(number_float_t v, const string_t&) override { return Scalar(v); }
			bool string(string_t& value) override {
				if (m_skip) return true;
				const auto target = Take();
				if (!target.ops) return true;
				if (target.ops->kind != Kind::String) return Fallback();
				target.ops->string(target.value, value);
				return true;
			}
			bool binary(binary_t&) override { return false; } // JSON text has no binary token.
			bool start_object(std::size_t) override { return Start(Kind::Object); }
			bool start_array(std::size_t) override { return Start(Kind::Array); }
			bool key(string_t& key) override {
				if (m_skip) return true;
				auto& frame = m_stack.back();
				const auto member = frame.target.ops->member(frame.target.value, key);
				// DOM conversion uses the last duplicate, even if the earlier value was
				// incomplete or of the wrong type. Preserve that rare case via the old path.
				if (frame.seen & member.bit) return Fallback();
				frame.seen |= member.bit;
				frame.pending = member.target;
				return true;
			}
			bool end_object() override {
				if (m_skip) { --m_skip; return true; }
				const auto& frame = m_stack.back();
				if (frame.seen != frame.target.ops->required) return Fallback();
				m_stack.pop_back();
				return true;
			}
			bool end_array() override {
				if (m_skip) --m_skip; else m_stack.pop_back();
				return true;
			}
			bool parse_error(std::size_t, const std::string&, const json::exception&) override { return false; }
		private:
			struct Frame { Target target; Target pending; std::uint64_t seen{}; };
			Target Take() {
				if (m_stack.empty()) return std::exchange(m_root, {});
				auto& frame = m_stack.back();
				if (frame.target.ops->kind == Kind::Array) return frame.target.ops->append(frame.target.value);
				return std::exchange(frame.pending, {});
			}
			bool Fallback() { legacy = true; return false; }
			bool Scalar(const json& value) {
				if (m_skip) return true;
				const auto target = Take();
				if (!target.ops) return true;
				return (target.ops->scalar && target.ops->scalar(target.value, value)) || Fallback();
			}
			bool Start(Kind kind) {
				if (m_skip) { ++m_skip; return true; }
				const auto target = Take();
				if (!target.ops) { m_skip = 1; return true; }
				if (target.ops->kind != kind) return Fallback();
				m_stack.push_back({target});
				return true;
			}
			Target m_root;
			std::vector<Frame> m_stack;
			std::size_t m_skip{};
		};
	}

	std::unique_ptr<Catalog::Snapshot> Parse(std::string_view text) noexcept
	{
		try {
			auto snapshot = std::make_unique<Catalog::Snapshot>();
			Reader reader(*snapshot);
			if (json::sax_parse(text, &reader))
				return snapshot->schema == 3 ? std::move(snapshot) : nullptr;
			if (!reader.legacy) return {};
			snapshot.reset(); // Release partial data before the exceptional compatibility path.
			const auto value = json::parse(text);
			if (!value.is_object() || value.value("schema", 0) != 3) return {};
			return std::make_unique<Catalog::Snapshot>(value.get<Catalog::Snapshot>());
		} catch (...) { return {}; }
	}

	void Read(const char* text, void* context) noexcept
	{
		*static_cast<std::unique_ptr<Catalog::Snapshot>*>(context) = text ? Parse(text) : nullptr;
	}
}
