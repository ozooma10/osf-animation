#pragma once

#include "Animation/GraphManager.h"
#include "Props/PropService.h"
#include "Registry/SceneRegistry.h"

#include <optional>
#include <unordered_map>

namespace OSF::Scene
{
	struct InspectionAnchor
	{
		bool set = false;
		RE::NiPoint3 pos{};
		float heading = 0.0f;
	};

	struct InspectionRequest
	{
		Registry::SceneRef definition;
		std::vector<RE::Actor*> actors;
		std::vector<std::string> roleNames;
		std::int32_t startStage = 0;
		InspectionAnchor anchor;
	};

	struct PreparedInspection
	{
		Registry::SceneRef definition;
		std::string node;
		std::int32_t stage = -1;
		std::vector<RE::Actor*> participants;
		Animation::ScenePlan plan;
	};

	struct InspectionSnapshot
	{
		std::int32_t handle = 0;
		std::string sceneId;
		std::int32_t stage = -1;
		std::vector<RE::Actor*> participants;
		Animation::GraphManager::ScenePlayback playback;
	};

	// Owns scrub-only Layer-A scenes and the render-only prop state reconstructed for them.
	// Game-thread only; production SceneRuntime actions and ledgers are never rewound.
	class SceneInspectionService
	{
	public:
		static SceneInspectionService& GetSingleton();

		std::optional<PreparedInspection> Prepare(const InspectionRequest& a_request, std::string& a_error) const;
		std::int32_t Start(PreparedInspection a_prepared, std::string& a_error);
		// Scrubbing is frame-accurate transport, so a seek always pauses: dragging the timeline (or
		// stepping a frame) while the preview runs would otherwise fight the clock.
		bool Seek(std::int32_t a_handle, float a_time);
		// Preview transport: 0 = paused (the state a preview starts in), 1 = play at authored speed.
		// A running preview loops its clip — a browser preview has no timers, loop targets, or marks,
		// so it can only end when the view stops it.
		bool SetSpeed(std::int32_t a_handle, float a_speed);
		// Game-thread pump for RUNNING previews: reconciles their render-only props against the clock
		// the engine advanced since the last call. Paused previews reconcile on Seek instead, so this
		// is a no-op for them. Called from the view's playback poll (BuildActiveScenes).
		void Tick();
		bool Stop(std::int32_t a_handle);
		void StopForActor(RE::Actor* a_actor);
		void StopAll();
		bool Contains(std::int32_t a_handle) const;
		std::vector<InspectionSnapshot> List();

	private:
		struct PreviewProp
		{
			std::string id;
			Registry::ActionEntry action;
			Props::Instance instance;
		};

		struct Preview
		{
			std::int32_t handle = 0;
			std::string sceneId;
			std::string node;
			std::int32_t stage = -1;
			Registry::SceneRef definition;
			std::vector<RE::Actor*> participants;
			Animation::PlaybackId playbackId = 0;
			std::vector<PreviewProp> props;
		};

		RE::Actor* RoleActor(const Preview& a_preview, std::string_view a_role) const;
		// a_durationSec is the previewed clip's length (0 = not known yet); only `atFrame` actions need it.
		void ReconcileProps(Preview& a_preview, float a_fraction, bool a_atEnd, float a_durationSec = 0.0f);
		void DestroyProps(Preview& a_preview);
		bool Retire(std::int32_t a_handle, bool a_stopGraph);

		std::unordered_map<std::int32_t, Preview> _previews;
		std::int32_t _nextHandle = -1;
	};
}
