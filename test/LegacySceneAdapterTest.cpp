#include "API/LegacySceneAdapter14.h"
#include "Check.h"
namespace
{
	using namespace OSF::API;
	struct Current final : OSF::API::IOSFSceneAPI
	{
		OSF::API::OSFStartOptions seen;
		RE::Actor*                actor = reinterpret_cast<RE::Actor*>(0x1234);
		int                       handle{}, stage{};
		std::uint32_t             GetInterfaceVersion() override { return {}; }
		void                      GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override
		{
			a_major = 2;
			a_minor = 0;
			a_patch = 0;
		}
		bool         IsReady() override { return {}; }
		std::int32_t StartScene(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const OSFStartOptions& a_opts) override
		{
			seen = a_opts;
			return 42;
		}
		std::int32_t StartSceneRoles(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const char* const* a_roles, std::uint32_t a_roleCount, const OSFStartOptions& a_opts) override
		{
			seen = a_opts;
			return 42;
		}
		std::int32_t StartSceneFiles(RE::Actor* const* a_actors, const char* const* a_files, std::uint32_t a_count, const OSFStartOptions& a_opts) override
		{
			seen = a_opts;
			return 42;
		}
		bool StopScene(std::int32_t a_handle) override { return {}; }
		bool StopSceneForActor(RE::Actor* a_actor) override { return {}; }
		bool SetStage(std::int32_t a_handle, std::int32_t a_stage) override
		{
			handle = a_handle;
			stage = a_stage;
			return true;
		}
		bool          Advance(std::int32_t a_handle) override { return {}; }
		bool          Navigate(std::int32_t a_handle, const char* a_edgeId) override { return {}; }
		bool          Play(RE::Actor* a_actor, const char* a_file, const char* a_animId) override { return {}; }
		bool          Stop(RE::Actor* a_actor) override { return {}; }
		bool          SetSpeed(RE::Actor* a_actor, float a_speed) override { return {}; }
		float         GetSpeed(RE::Actor* a_actor) override { return {}; }
		bool          IsPlaying(RE::Actor* a_actor) override { return {}; }
		std::int32_t  GetSceneForActor(RE::Actor* a_actor) override { return {}; }
		std::int32_t  GetSceneStage(std::int32_t a_handle) override { return a_handle + 7; }
		std::uint32_t GetSceneParticipants(std::int32_t a_handle, RE::Actor** a_out, std::uint32_t a_cap) override
		{
			if (a_out && a_cap)
				a_out[0] = actor;
			return 1;
		}
		std::uint64_t RegisterSceneEventCallback(OSFSceneEventCallback a_callback, void* a_context, std::int32_t a_sceneFilter = 0, std::int32_t a_eventMask = SceneEventType::kAll) override { return {}; }
		bool          UnregisterSceneEventCallback(std::uint64_t a_token) override { return {}; }
		bool          FindSceneByQuery(RE::Actor* const* a_actors, std::uint32_t a_count, const OSFSceneQuery& a_query, OSFSceneMatch& a_out) override { return {}; }
		std::int32_t  StartSceneByQuery(RE::Actor* const* a_actors, std::uint32_t a_count, const OSFSceneQuery& a_query, const OSFStartOptions& a_opts) override
		{
			seen = a_opts;
			return 42;
		}
	};
}
void RunLegacySceneAdapterTests()
{
	using OSF::Test::Check;
	using namespace OSF;
	Current                   current;
	API::LegacySceneAdapter14 adapter(current);
	Compat14::IOSFSceneAPI*   old = &adapter;
	Check(old->GetInterfaceVersion() == 0x00010002 && API::LegacySceneRequestSupported(0x00010000) && API::LegacySceneRequestSupported(0x00010002) && !API::LegacySceneRequestSupported(0x00010003) && !API::LegacySceneRequestSupported(0x00020000),
		"published scene ABI range has a separate frozen table");
	Compat14::OSFStartOptions opts;
	opts.stripMode = 0;
	opts.lockPlayerMode = 1;
	opts.playerControlMode = 0;
	opts.inPlaceMode = 1;
	opts.startStage = 3;
	opts.speed = 2;
	opts.anchorRef = reinterpret_cast<RE::TESObjectREFR*>(0x5678);
	std::memset(opts.camera, 'x', sizeof(opts.camera));
	Check(old->StartScene(&current.actor, 1, "legacy", opts) == 42 && current.seen.hideApparelMode == 0 && current.seen.playerInputLockMode == 1 && current.seen.sceneControlsMode == 0 && current.seen.followActorMode == 1 && current.seen.startStage == 3 &&
			  current.seen.speed == 2 && current.seen.anchorRef == opts.anchorRef && std::memcmp(opts.camera, current.seen.camera, sizeof(opts.camera)) == 0,
		"old start options map every renamed field without losing anchors or bounded camera bytes");
	opts.size = 128;
	old->StartScene(nullptr, 0, "legacy", opts);
	Check(current.seen.size == sizeof(current.seen) && current.seen.followActorMode == -1, "published prefix sizes inherit absent appended placement rather than reading it");
	opts.size = 4;
	old->StartScene(nullptr, 0, "legacy", opts);
	Check(current.seen.size == 0, "truncated options reach current validation as invalid");
	Check(old->SetStage(9, 2) && current.handle == 9 && current.stage == 2 && old->GetSceneStage(9) == 16, "old navigation/query vtable slots forward correctly");
	RE::Actor* result = nullptr;
	Check(old->GetSceneParticipants(9, &result, 1) == 1 && result == current.actor, "last published vtable slot preserves the roster");
}
