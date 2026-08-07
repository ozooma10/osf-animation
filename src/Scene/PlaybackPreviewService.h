#pragma once

#include "Animation/GraphManager.h"
#include "Props/PropService.h"
#include "Registry/SceneRegistry.h"

#include <optional>
#include <unordered_map>

namespace OSF::Scene
{
	struct PreviewWorldAnchor
	{
		bool set = false;
		RE::NiPoint3 pos{};
		float heading = 0.0f;
	};

	using InspectionAnchor = PreviewWorldAnchor;

	struct PreviewRequest
	{
		Registry::SceneRef definition;
		std::vector<RE::Actor*> actors;
		std::vector<std::string> roleNames;
		std::int32_t startStage = 0;
		union
		{
			PreviewWorldAnchor worldAnchor{};
			PreviewWorldAnchor anchor;  // compatibility field; aliases the same storage
		};
	};
	using InspectionRequest = PreviewRequest;

	struct PreparedPreview
	{
		Registry::SceneRef definition;
		std::string node;
		std::int32_t stage = -1;
		std::vector<RE::Actor*> participants;
		Animation::PlaybackPlan plan;
		bool anchorImplicit = false;  // plan anchor was sampled from (or inherited for) actor[0], not caller-supplied
	};
	using PreparedInspection = PreparedPreview;

	struct RoutePreviewRequest
	{
		Registry::RouteRef definition;
		std::string transition;
		RE::Actor* actor = nullptr;
	};
	using RouteInspectionRequest = RoutePreviewRequest;

	struct PreparedRoutePreview
	{
		Registry::RouteRef definition;
		std::string transition;
		RE::Actor* actor = nullptr;
		Animation::PlaybackPlan plan;
	};
	using PreparedRouteInspection = PreparedRoutePreview;

	struct PreviewSnapshot
	{
		std::int32_t handle = 0;
		std::string sceneId;
		std::int32_t stage = -1;
		std::vector<RE::Actor*> participants;
		Animation::GraphManager::SynchronizedPlaybackState playback;
		std::string inspectionKind = "scene";
		std::string routeId;
		std::string transitionId;
	};
	using InspectionSnapshot = PreviewSnapshot;

	// Owns scrub-only Layer-A playback sessions and the render-only prop state reconstructed for them.
	// Game-thread only; production SceneRuntime actions and ledgers are never rewound.
	class PlaybackPreviewService
	{
	public:
		static PlaybackPreviewService& GetSingleton();

		std::optional<PreparedPreview> Prepare(const PreviewRequest& a_request, std::string& a_error) const;
		std::optional<PreparedRoutePreview> PrepareRoute(
			const RoutePreviewRequest& a_request, std::string& a_error) const;
		std::int32_t Start(PreparedPreview a_prepared, std::string& a_error);
		std::int32_t StartRoute(PreparedRoutePreview a_prepared, std::string& a_error);
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
		std::vector<PreviewSnapshot> List();

	private:
		struct PreviewProp
		{
			std::string id;
			Registry::ActionEntry action;
			Props::Instance instance;
		};

		struct PreviewRouteProp
		{
			std::string id;
			Registry::RouteLifetime lifetime = Registry::RouteLifetime::kTransition;
			Props::Source source;
			Props::Attachment attachment;
			Props::Instance instance;
		};

		struct Preview
		{
			std::int32_t handle = 0;
			std::string sceneId;
			std::string node;
			std::int32_t stage = -1;
			Registry::SceneRef definition;
			Registry::RouteRef route;
			std::string transition;
			std::vector<RE::Actor*> participants;
			Animation::PlaybackId playbackId = 0;
			std::vector<PreviewProp> props;
			std::vector<PreviewRouteProp> routeProps;
			// Pre-inspection baseline, carried across stage switches: the anchor the first preview
			// resolved (only meaningful when anchorImplicit) and the cast's pre-inspection transforms.
			// A replacement preview for the same cast inherits both in Prepare — sampling live state
			// there would re-anchor on the already-placed actors (walking the cast by role 0's offset
			// per switch) and restore them to the outgoing stage's placement on final stop.
			bool anchorImplicit = false;
			RE::NiPoint3 anchorPos{};
			float anchorHeading = 0.0f;
			std::vector<std::pair<RE::NiPoint3, float>> baseline;
			bool suspendsOverlay = false;
		};

		RE::Actor* RoleActor(const Preview& a_preview, std::string_view a_role) const;
		// a_durationSec is the previewed clip's length (0 = not known yet); only `atFrame` actions need it.
		void ReconcileProps(Preview& a_preview, float a_fraction, bool a_atEnd, float a_durationSec = 0.0f);
		void ReconcileRouteProps(Preview& a_preview, float a_frame, bool a_atEnd, float a_durationFrames = 0.0f);
		void DestroyProps(Preview& a_preview);
		bool Retire(std::int32_t a_handle, bool a_stopGraph);
		std::int32_t MintHandle();

		std::unordered_map<std::int32_t, Preview> _previews;
		std::int32_t _nextHandle = -1;
	};

	using SceneInspectionService = PlaybackPreviewService;  // compatibility spelling
}
