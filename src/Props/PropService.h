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
	// One render-only scene prop owned by a live OSF scene. The exact cloned
	// object and actor root stay strongly referenced until Destroy; normal
	// scene-end cleanup detaches only that owned pointer.
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

		[[nodiscard]] bool Available();

		// GAME THREAD. Resolve and clone a visual, then attach it to the named
		// node on a_actor. On success the returned instance owns the prop.
		[[nodiscard]] Instance CreateAttached(
			::RE::Actor* a_actor, const Source& a_source,
			const Attachment& a_attachment, std::string* a_error = nullptr);

		// GAME THREAD. Move an existing scene prop to another actor-relative
		// attachment. The actor root must still be the one captured at create.
		[[nodiscard]] bool Attach(
			Instance& a_instance, const Attachment& a_attachment,
			std::string* a_error = nullptr);

		// GAME THREAD. Detach and release exactly this scene-owned visual.
		// Idempotent for an empty instance.
		[[nodiscard]] bool Destroy(
			Instance& a_instance, std::string* a_error = nullptr);
	};
}
