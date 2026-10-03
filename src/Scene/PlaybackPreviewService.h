#pragma once

#include "Animation/GraphManager.h"
#include "Props/PropService.h"
#include "Registry/SceneRegistry.h"
#include "Scene/InspectionPropCursor.h"

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
			PreviewWorldAnchor anchor;  // alias of worldAnchor
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
		bool anchorImplicit = false;  // anchor sampled from actor[0], not caller-supplied
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

	// Scrub-only Layer-A playback with render-only props. Game-thread only; never touches
	// production SceneRuntime actions or ledgers.
	class PlaybackPreviewService
	{
	public:
		static PlaybackPreviewService& GetSingleton();

		std::optional<PreparedPreview> Prepare(const PreviewRequest& a_request, std::string& a_error) const;
		std::optional<PreparedRoutePreview> PrepareRoute(
			const RoutePreviewRequest& a_request, std::string& a_error) const;
		std::int32_t Start(PreparedPreview a_prepared, std::string& a_error);
		std::int32_t StartRoute(PreparedRoutePreview a_prepared, std::string& a_error);
		// Always pauses; a running clock would fight the scrubber.
		bool Seek(std::int32_t a_handle, float a_time);
		// 0 = paused (initial state), 1 = authored speed. A running preview loops until stopped.
		bool SetSpeed(std::int32_t a_handle, float a_speed);
		// Reconciles props of running previews; paused previews reconcile on Seek.
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
			InspectionPropCursor propCursor;
			float propDuration = -1.0f;
			// Pre-inspection anchor and transforms, inherited across stage switches; resampling the
			// already-placed cast would drift it by role 0's offset each switch.
			bool anchorImplicit = false;
			RE::NiPoint3 anchorPos{};
			float anchorHeading = 0.0f;
			std::vector<std::pair<RE::NiPoint3, float>> baseline;
			bool suspendsOverlay = false;
		};

		RE::Actor* RoleActor(const Preview& a_preview, std::string_view a_role) const;
		// a_durationSec: clip length, 0 = unknown.
		void ReconcileProps(Preview& a_preview, float a_fraction, bool a_atEnd, float a_durationSec,
			bool a_force = false);
		void ReconcileRouteProps(Preview& a_preview, float a_frame, bool a_atEnd, float a_durationFrames,
			bool a_force = false);
		void DestroyProps(Preview& a_preview);
		bool Retire(std::int32_t a_handle, bool a_stopGraph);
		std::int32_t MintHandle();

		std::unordered_map<std::int32_t, Preview> _previews;
		std::int32_t _nextHandle = -1;
	};

	using SceneInspectionService = PlaybackPreviewService;
}
