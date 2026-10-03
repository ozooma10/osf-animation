#pragma once

#include "Props/PropTypes.h"

#include "RE/N/NiSmartPointer.h"

#include <cstdint>
#include <string>

namespace RE
{
	class Actor;
	class NiAVObject;
}

namespace OSF::Props
{
	// Render-only scene prop. The clone and actor root stay strongly referenced until Destroy.
	struct Instance
	{
		::RE::NiPointer<::RE::NiAVObject> object;
		::RE::NiPointer<::RE::NiAVObject> actorRoot;
		std::uint32_t                     sourceForm = 0;

		[[nodiscard]] bool Empty() const noexcept { return !object; }
	};

	class PropService
	{
	public:
		static PropService& GetSingleton();

		// Game thread. On success the returned instance owns the prop.
		[[nodiscard]] Instance CreateAttached(
			::RE::Actor* a_actor, const Source& a_source,
			const Attachment& a_attachment, std::string* a_error = nullptr);

		// Game thread. May hand the same visual to a different role.
		[[nodiscard]] bool Attach(
			Instance& a_instance, ::RE::Actor* a_actor,
			const Attachment& a_attachment,
			std::string* a_error = nullptr);

		// Game thread. Idempotent for an empty instance.
		[[nodiscard]] bool Destroy(
			Instance& a_instance, std::string* a_error = nullptr);
	};
}
