#pragma once
#include "API/OSFCatalog.h"
#include "Profiling/Profile.h"
#include <type_traits>

namespace OSF::API::CatalogJson
{
	namespace Detail
	{
		// One output buffer, no object/array DOM. Field names come from the same declaration
		// as the public JSON converters. The pinned nlohmann 3.11.3 serializer retains its
		// UTF-8 replacement and numeric formatting (including non-finite -> null) behavior.
		class Writer
		{
		public:
			Writer() : m_serializer(nlohmann::detail::output_adapter<char>(m_text), ' ',
				nlohmann::json::error_handler_t::replace) { m_text.reserve(4096); }

			template <std::size_t N, class T>
			void Field(const char (&name)[N], const T& value)
			{
				// Names are stringized C++ field identifiers, so they need no JSON escaping.
				m_text += '"';
				m_text.append(name, N - 1);
				m_text += "\":";
				Write(value);
				m_text += ',';
			}

			void Write(const std::string& value)
			{
				// Reuse one scalar's storage; no allocation per string or object member.
				m_string.get_ref<std::string&>() = value;
				m_serializer.dump(m_string, false, false, 0);
			}

			template <class T>
			void Write(const std::vector<T>& values)
			{
				m_text += '[';
				for (const auto& value : values) { Write(value); m_text += ','; }
				if (!values.empty()) m_text.pop_back();
				m_text += ']';
			}

			template <class T>
			void Write(const T& value)
			{
				if constexpr (std::is_arithmetic_v<T>) {
					m_serializer.dump(nlohmann::json(value), false, false, 0);
				} else {
					m_text += '{';
					VisitFields(*this, value);
					if (m_text.back() == ',') m_text.pop_back();
					m_text += '}';
				}
			}

			std::string Take() && { return std::move(m_text); }

		private:
			std::string m_text;
			nlohmann::json m_string = std::string{};
			nlohmann::detail::serializer<nlohmann::json> m_serializer;
		};
	}

	// Complete schema-3 UTF-8 document. Object member order follows the declarations;
	// array order and all values are identical to json(snapshot).dump(..., replace).
	inline std::string Serialize(const Catalog::Snapshot& snapshot)
	{
		OSF_PROFILE_SCOPE("API.CatalogSerialize");
		Detail::Writer writer;
		writer.Write(snapshot);
		return std::move(writer).Take();
	}
}
