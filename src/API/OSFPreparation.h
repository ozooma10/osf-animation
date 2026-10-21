#pragma once
#include <cstdint>

namespace OSF::API
{
	inline constexpr std::uint32_t kPreparationVersion = 1;
	enum class PreparationKind : std::uint32_t
	{
		Start,
		Handoff,
		Node,
		Stage
	};
	enum class PreparationPhase : std::uint32_t
	{
		Plan,
		Apply,
		Ready,
		Validate
	};
	enum class PreparationDecision : std::uint32_t
	{
		Approve,
		Pending,
		Veto
	};
	enum class PreparationOutcome : std::uint32_t
	{
		Committed,
		Vetoed,
		Failed,
		Cancelled,
		TimedOut,
		OwnerWithdrawn,
		WorldReset
	};
	struct PreparationContext
	{
		std::uint32_t        size{ sizeof(PreparationContext) }, version{ kPreparationVersion };
		std::uint64_t        attempt{}, provider{};
		std::int32_t         scene{};
		PreparationKind      kind{};
		const char*          fromScene{};
		const char*          fromNode{};
		const char*          toScene{};
		const char*          toNode{};
		std::int32_t         fromStage{ -1 }, toStage{ -1 };
		const std::uint32_t* actors{};  // exact destination role order, world-local form IDs
		std::uint32_t        actorCount{};
	};
	struct ScenePreparer
	{
		std::uint32_t size{ sizeof(ScenePreparer) }, version{ kPreparationVersion };
		std::int32_t  order{};            // ascending, registration order breaks ties
		std::uint32_t timeoutMs{ 5000 };  // 1..5000, bounds the entire attempt (minimum across providers)
		PreparationDecision (*prepare)(const PreparationContext*, PreparationPhase, void*){};
		void (*finish)(const PreparationContext*, PreparationOutcome, void*){};
		void* context{};
	};
}
