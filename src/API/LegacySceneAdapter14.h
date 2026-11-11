#pragma once
#include "API/LegacySceneAPI14.h"
#include "API/OSFSceneAPI.h"
#include <cstddef>
#include <cstring>

namespace OSF::API
{
	// Exact published vtable, delegating to the single current runtime.
	inline constexpr bool LegacySceneRequestSupported(std::uint32_t version) { return version >= 0x00010000 && version <= Compat14::kOSFSceneAPIVersion; }
	static_assert(sizeof(Compat14::OSFStartOptions) == 136);
	static_assert(offsetof(Compat14::OSFStartOptions, camera) == 20);
	static_assert(offsetof(Compat14::OSFStartOptions, startStage) == 96);
	static_assert(offsetof(Compat14::OSFStartOptions, anchorRef) == 120);
	static_assert(offsetof(Compat14::OSFStartOptions, inPlaceMode) == 128);
	class LegacySceneAdapter14 final : public Compat14::IOSFSceneAPI
	{
	public:
		explicit LegacySceneAdapter14(OSF::API::IOSFSceneAPI& current) : _current(current) {}
		static OSFStartOptions Convert(const Compat14::OSFStartOptions& in)
		{
			OSFStartOptions out;
			static_assert(sizeof(out) == sizeof(in));
			if (in.size < offsetof(Compat14::OSFStartOptions, inPlaceMode)) {
				out.size = 0;
				return out;
			}
			out.size = sizeof(out);
			out.fadeMode = in.fadeMode;
			out.loopScale = in.loopScale;
			out.speed = in.speed;
			out.blendIn = in.blendIn;
			out.startStage = in.startStage;
			out.hasAnchor = in.hasAnchor;
			out.anchorX = in.anchorX;
			out.anchorY = in.anchorY;
			out.anchorZ = in.anchorZ;
			out.anchorHeadingRad = in.anchorHeadingRad;
			out.anchorRef = in.anchorRef;
			out.hideApparelMode = in.stripMode;
			out.playerInputLockMode = in.lockPlayerMode;
			out.sceneControlsMode = in.playerControlMode;
			if (in.size >= offsetof(Compat14::OSFStartOptions, inPlaceMode) + sizeof(in.inPlaceMode))
				out.followActorMode = in.inPlaceMode;
			std::memcpy(out.camera, in.camera, sizeof(out.camera));
			return out;
		}
		std::uint32_t GetInterfaceVersion() override { return 0x00010002; }
		void          GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override { _current.GetPluginVersion(a_major, a_minor, a_patch); }
		bool          IsReady() override { return _current.IsReady(); }
		std::int32_t  StartScene(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const Compat14::OSFStartOptions& a_opts) override { return _current.StartScene(a_actors, a_count, a_sceneId, Convert(a_opts)); }
		std::int32_t  StartSceneRoles(RE::Actor* const* a_actors, std::uint32_t a_count, const char* a_sceneId, const char* const* a_roles, std::uint32_t a_roleCount, const Compat14::OSFStartOptions& a_opts) override
		{
			return _current.StartSceneRoles(a_actors, a_count, a_sceneId, a_roles, a_roleCount, Convert(a_opts));
		}
		std::int32_t  StartSceneFiles(RE::Actor* const* a_actors, const char* const* a_files, std::uint32_t a_count, const Compat14::OSFStartOptions& a_opts) override { return _current.StartSceneFiles(a_actors, a_files, a_count, Convert(a_opts)); }
		bool          StopScene(std::int32_t a_handle) override { return _current.StopScene(a_handle); }
		bool          StopSceneForActor(RE::Actor* a_actor) override { return _current.StopSceneForActor(a_actor); }
		bool          SetStage(std::int32_t a_handle, std::int32_t a_stage) override { return _current.SetStage(a_handle, a_stage); }
		bool          Advance(std::int32_t a_handle) override { return _current.Advance(a_handle); }
		bool          Navigate(std::int32_t a_handle, const char* a_edgeId) override { return _current.Navigate(a_handle, a_edgeId); }
		bool          Play(RE::Actor* a_actor, const char* a_file, const char* a_animId) override { return _current.Play(a_actor, a_file, a_animId); }
		bool          Stop(RE::Actor* a_actor) override { return _current.Stop(a_actor); }
		bool          SetSpeed(RE::Actor* a_actor, float a_speed) override { return _current.SetSpeed(a_actor, a_speed); }
		float         GetSpeed(RE::Actor* a_actor) override { return _current.GetSpeed(a_actor); }
		bool          IsPlaying(RE::Actor* a_actor) override { return _current.IsPlaying(a_actor); }
		std::int32_t  GetSceneForActor(RE::Actor* a_actor) override { return _current.GetSceneForActor(a_actor); }
		std::int32_t  GetSceneStage(std::int32_t a_handle) override { return _current.GetSceneStage(a_handle); }
		std::uint32_t GetSceneParticipants(std::int32_t a_handle, RE::Actor** a_out, std::uint32_t a_cap) override { return _current.GetSceneParticipants(a_handle, a_out, a_cap); }

	private:
		OSF::API::IOSFSceneAPI& _current;
	};

}
