#pragma once
#include "API/OSFEngineAPI.h"
namespace OSF::API::PreparationAPI
{
	std::uint64_t Register(const ScenePreparer*);
	bool          Unregister(std::uint64_t);
	bool          Equipment(std::uint64_t, std::uint64_t, const ActorEquipmentRequest*);
	bool          Resource(std::uint64_t, std::uint64_t, const char*);
	bool          Failure(std::uint64_t, std::uint64_t, const char*);
    bool          Guard(std::uint64_t, std::uint64_t, const PreparationCommitGuard*);
	bool          Read(std::int32_t, TextCallback, void*);
}
