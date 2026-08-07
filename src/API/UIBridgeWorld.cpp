#include "API/UIBridgeWorld.h"

#include "API/OSFUI_API.h"
#include "API/UIKeywordLabel.h"
#include "Camera/CameraService.h"
#include "Matchmaking/Matchmaker.h"
#include "Registry/SceneRegistry.h"
#include "Util/Profile.h"
#include "Util/Species.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OSF::API::UIBridgeWorld
{
	using json = nlohmann::json;

	namespace
	{
		// The client has process lifetime in UIBridge.cpp. It is set before any
		// world command is registered and only read by main-thread handlers.
		OSFUI::API::Client* g_ui = nullptr;

		struct Picked
		{
			RE::TESObjectREFR* ref;     // pointer resolved at pick time
			RE::TESFormID      formID;  // must still resolve to that same pointer at use
			bool               isActor;
		};

		std::unordered_map<std::int32_t, Picked>        g_tokens;
		std::unordered_map<RE::TESFormID, std::int32_t> g_formToken;
		std::int32_t                                    g_nextToken = 1;

		RE::BGSKeyword* AnchorLabelKeyword(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || a_ref->IsActor()) {
				return nullptr;
			}

			RE::BGSKeyword*                         match = nullptr;
			std::unordered_map<RE::TESFormID, bool> tested;
			Registry::SceneRegistry::GetSingleton().ForEachDef([&](const Registry::SceneDef& a_def) {
				if (match || !a_def.clipsAvailable) {
					return;
				}
				for (const auto kwId : a_def.anchorKeywords) {
					auto [it, inserted] = tested.emplace(kwId, false);
					if (!inserted) {
						if (it->second) {
							match = RE::TESForm::LookupByID<RE::BGSKeyword>(kwId);
						}
						continue;
					}
					auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(kwId);
					it->second = kw && a_ref->HasKeyword(kw) && !KeywordLabel(kw).empty();
					if (it->second) {
						match = kw;
						return;
					}
				}
			});
			return match;
		}

		void SendJson(const char* a_view, const char* a_type, const json& a_payload)
		{
			if (!g_ui || !*g_ui) {
				return;
			}
			const std::string text = a_payload.dump(-1, ' ', false, json::error_handler_t::replace);
			g_ui->SendToWeb(a_view, a_type, text.c_str());
		}

		json ParsePayload(const char* a_json)
		{
			constexpr std::size_t kMaxPayloadBytes = 1u << 20;
			if (!a_json) {
				return json::parse("", nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			}

			std::size_t length = 0;
			while (length <= kMaxPayloadBytes && a_json[length] != '\0') {
				++length;
			}
			if (length > kMaxPayloadBytes) {
				REX::WARN("[UI] refused an inbound payload larger than {} bytes", kMaxPayloadBytes);
				return json::parse("", nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
			}
			return json::parse(a_json, a_json + length, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
		}

		std::optional<std::int32_t> Int32Value(const json& a_value)
		{
			if (a_value.is_number_unsigned()) {
				const auto value = a_value.get<std::uint64_t>();
				if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
					return static_cast<std::int32_t>(value);
				}
				return std::nullopt;
			}
			if (a_value.is_number_integer()) {
				const auto value = a_value.get<std::int64_t>();
				if (value >= std::numeric_limits<std::int32_t>::min() &&
					value <= std::numeric_limits<std::int32_t>::max()) {
					return static_cast<std::int32_t>(value);
				}
			}
			return std::nullopt;
		}

		float NumOr(const json& a_obj, const char* a_key, float a_def)
		{
			if (!a_obj.is_object()) {
				return a_def;
			}
			const auto it = a_obj.find(a_key);
			if (it == a_obj.end() || !it->is_number()) {
				return a_def;
			}
			const double value = it->get<double>();
			return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max()
			         ? static_cast<float>(value)
			         : a_def;
		}

		std::int32_t IntOr(const json& a_obj, const char* a_key, std::int32_t a_def)
		{
			if (!a_obj.is_object()) {
				return a_def;
			}
			const auto it = a_obj.find(a_key);
			if (it == a_obj.end()) {
				return a_def;
			}
			return Int32Value(*it).value_or(a_def);
		}

		std::string StrOr(const json& a_obj, const char* a_key)
		{
			if (!a_obj.is_object()) {
				return {};
			}
			const auto it = a_obj.find(a_key);
			return it != a_obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
		}
	}

	std::int32_t AllocToken(RE::TESObjectREFR* a_ref)
	{
		// Re-scans reuse the form's browser-session token and refresh its pointer.
		const RE::TESFormID fid = a_ref->GetFormID();
		if (const auto it = g_formToken.find(fid); it != g_formToken.end()) {
			g_tokens[it->second] = Picked{ a_ref, fid, a_ref->IsActor() };
			return it->second;
		}
		const std::int32_t token = g_nextToken++;
		g_tokens[token] = Picked{ a_ref, fid, a_ref->IsActor() };
		g_formToken[fid] = token;
		return token;
	}

	RE::TESObjectREFR* ResolveToken(std::int32_t a_token)
	{
		if (a_token == -1) {
			return RE::PlayerCharacter::GetSingleton();
		}
		const auto it = g_tokens.find(a_token);
		if (it == g_tokens.end()) {
			return nullptr;
		}
		const Picked& p = it->second;
		RE::TESForm* form = RE::TESForm::LookupByID(p.formID);
		if (!form || form != static_cast<RE::TESForm*>(p.ref) || form->IsDeleted()) {
			// The form unloaded, was reused, or was deleted after the pick.
			return nullptr;
		}
		return p.ref;
	}

	std::string ScanLabel(RE::TESObjectREFR* a_ref, RE::BGSKeyword* a_matchedKw)
	{
		// Invisible markers fall back through anchor keyword, EditorID, and form ID.
		if (const char* nm = a_ref->GetDisplayFullName(); nm && nm[0]) {
			return nm;
		}
		if (!a_matchedKw) {
			a_matchedKw = AnchorLabelKeyword(a_ref);
		}
		if (std::string kwLabel = KeywordLabel(a_matchedKw); !kwLabel.empty()) {
			return kwLabel;
		}
		if (const auto base = a_ref->GetBaseObject()) {
			if (const char* edid = base->GetFormEditorID(); edid && edid[0]) {
				return edid;
			}
			return std::format("Furniture {:#010x}", base->GetFormID());
		}
		return std::format("Ref {:#010x}", a_ref->GetFormID());
	}

	std::string RefSexTag(RE::TESObjectREFR* a_ref)
	{
		return a_ref && a_ref->IsActor()
		         ? Matchmaking::ActorGenderTag(static_cast<RE::Actor*>(a_ref))
		         : std::string{};
	}

	RE::TESObjectREFR* CrosshairRef()
	{
		// The engine clears this slot while a menu is up, so callers capture it
		// immediately before opening the browser.
		auto*              player = RE::PlayerCharacter::GetSingleton();
		RE::TESObjectREFR* ref = player ? player->crosshairRef : nullptr;
		return (ref && (ref->Is(RE::FormType::kREFR) || ref->Is(RE::FormType::kACHR))) ? ref : nullptr;
	}

	void ClearSessionTokens()
	{
		g_tokens.clear();
		g_formToken.clear();
	}

	namespace
	{
		struct SafeViewProjection
		{
			// Keep the camera graph alive while a command projects all requested points.
			RE::NiPointer<RE::NiNode> root;
			RE::NiCamera*             camera{ nullptr };
			RE::NiPoint3              position;
			RE::NiPoint3              forward;
			RE::NiPoint3              right;
			RE::NiPoint3              up;

			bool Project(const RE::NiPoint3& a_world, RE::NiPoint3& a_screen) const
			{
				if (!camera) {
					return false;
				}
				const RE::NiPoint3 delta = a_world - position;
				const float depth = delta.Dot(forward);
				if (!std::isfinite(depth) || depth <= 0.01f) {
					return false;
				}

				// NiCamera owns the exact per-frame projection used for culling, including the
				// current FOV, aspect ratio and viewport. A hand-built fixed-FOV projection drifts
				// as soon as the game or scene camera changes its lens.
				a_screen = camera->WorldToScreenNormalized(a_world);
				a_screen.z = depth;
				return std::isfinite(a_screen.x) && std::isfinite(a_screen.y) &&
				       std::isfinite(a_screen.z);
			}

			float ProjectedRadius(const RE::NiPoint3& a_center, float a_radius,
				float a_width, float a_height) const
			{
				RE::NiPoint3 center;
				RE::NiPoint3 edgeX;
				RE::NiPoint3 edgeY;
				if (!Project(a_center, center) ||
					!Project(a_center + right * a_radius, edgeX) ||
					!Project(a_center + up * a_radius, edgeY)) {
					return 0.0f;
				}
				return std::max(std::abs(edgeX.x - center.x) * a_width,
					std::abs(edgeY.y - center.y) * a_height);
			}
		};

		RE::NiCamera* FindCameraInNode(RE::NiAVObject* a_object, std::uint32_t a_depth = 0)
		{
			if (!a_object || a_depth > 16) {
				return nullptr;
			}
			if (auto* camera = starfield_cast<RE::NiCamera*>(a_object)) {
				return camera;
			}
			auto* node = starfield_cast<RE::NiNode*>(a_object);
			if (!node) {
				return nullptr;
			}
			for (const auto& child : node->children) {
				if (auto* camera = FindCameraInNode(child.get(), a_depth + 1)) {
					return camera;
				}
			}
			return nullptr;
		}

		RE::NiCamera* ActiveWorldCamera()
		{
			// RUNTIME-PROVEN on 1.16.244: Address Library ID 936470 is the global
			// StorageTable::Camera host-memory pointer. Its inline NiCamera at +0x80 is
			// camera B, the main WORLD render camera. PlayerCamera::cameraRoot reaches
			// only camera A (the gameplay/viewmodel camera), which is wrong in third
			// person and scene orbit.
			static const REL::Relocation<std::uintptr_t> storageGlobal{ REL::ID(936470) };
			static const REL::Relocation<std::uintptr_t> cameraVtable{ RE::NiCamera::VTABLE[0] };

			const auto storage = *reinterpret_cast<const std::uintptr_t*>(storageGlobal.address());
			if (storage != 0) {
				auto* candidate = reinterpret_cast<RE::NiCamera*>(storage + 0x80);
				if (*reinterpret_cast<const std::uintptr_t*>(candidate) == cameraVtable.address()) {
					return candidate;
				}
			}

			// Defensive fallback for a future runtime whose renderer storage layout moves:
			// a camera nested under the mapped PlayerCamera root is still preferable to
			// dropping every indicator, although it may represent the viewmodel lens.
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const auto root = playerCamera ? playerCamera->cameraRoot : nullptr;
			return root ? FindCameraInNode(root.get()) : nullptr;
		}

		// Rate limiter for camera-anomaly logs: picking polls at ~10 Hz, so an unhealthy
		// camera would otherwise repeat the same line for as long as it stays unhealthy.
		bool ShouldLogCameraAnomaly()
		{
			static std::chrono::steady_clock::time_point s_last{};
			const auto now = std::chrono::steady_clock::now();
			if (now - s_last < std::chrono::seconds(2)) {
				return false;
			}
			s_last = now;
			return true;
		}

		// PlayerCamera::cameraRoot is a mapped, runtime-proven field already used by the
		// camera service. It supplies a lifetime pin and a fallback; ActiveWorldCamera()
		// resolves the separate main-world renderer camera used for exact projection.
		//
		// Two health checks guard the result, because the reported failure mode of world
		// picking was not "misses by a bit" but "markers and clicks land nowhere near the
		// visible world, until further notice":
		//   1. While OSF's scene orbit drives the camera (the browser is open — exactly when
		//      picking runs), the orbit pose is the one view pose OSF computes itself, so it
		//      can't go stale. A resolved camera sitting far from that pose is NOT the camera
		//      the world is rendered through — prefer whichever known camera is at the pose.
		//   2. worldToCam (what Project uses) is a CPU-side matrix rebuilt asynchronously from
		//      the camera's world transform (see NiCamera.h); a probe point straight down the
		//      camera's own forward must project to the viewport center. When matrix and
		//      transform disagree beyond one frame of skew, fail the whole query: no markers
		//      for that beat (plus a log saying why) beats markers that lie — and since a
		//      click resolves against the marker the user saw, a click never projects at all.
		std::optional<SafeViewProjection> CurrentViewProjection(float a_width, float a_height)
		{
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const auto root = playerCamera ? playerCamera->cameraRoot : nullptr;
			if (!root || !std::isfinite(a_width) || !std::isfinite(a_height) || a_width < 1.0f || a_height < 1.0f) {
				return std::nullopt;
			}

			SafeViewProjection out;
			out.root = root;
			out.camera = ActiveWorldCamera();

			float orbitPos[3];
			float orbitFwd[3];
			if (Camera::CameraService::GetSingleton().SceneOrbitPose(orbitPos, orbitFwd)) {
				const auto poseErrorSq = [&](RE::NiCamera* a_camera) {
					const float dx = a_camera->world.translate.x - orbitPos[0];
					const float dy = a_camera->world.translate.y - orbitPos[1];
					const float dz = a_camera->world.translate.z - orbitPos[2];
					return dx * dx + dy * dy + dz * dz;
				};
				constexpr float kPoseToleranceSq = 1.5f * 1.5f;  // generous: covers the orbit glide's frame lag
				RE::NiCamera*   alt = FindCameraInNode(root.get());
				const float     mainErr = out.camera ? poseErrorSq(out.camera) : std::numeric_limits<float>::max();
				const float     altErr = (alt && alt != out.camera) ? poseErrorSq(alt) : std::numeric_limits<float>::max();
				if (mainErr > kPoseToleranceSq) {
					// Swap only on clear RELATIVE dominance — the absolute tolerance is
					// authored in assumed units, but "4x closer to the pose OSF wrote"
					// holds in any unit.
					if (altErr * 4.0f < mainErr) {
						if (ShouldLogCameraAnomaly()) {
							REX::WARN("[UI] world-pick camera: storage camera sits {:.1f} from the live orbit pose — projecting through the cameraRoot camera instead ({:.1f})",
								std::sqrt(mainErr), std::sqrt(altErr));
						}
						out.camera = alt;
					} else if (ShouldLogCameraAnomaly()) {
						REX::WARN("[UI] world-pick camera sits {:.1f} from the live orbit pose — picking may not line up", std::sqrt(mainErr));
					}
				}
			}
			if (!out.camera) {
				return std::nullopt;
			}
			out.position = out.camera->world.translate;
			out.forward = {
				out.camera->world.rotate[0][0],
				out.camera->world.rotate[0][1],
				out.camera->world.rotate[0][2],
			};
			if (out.forward.Unitize() <= 0.001f) {
				return std::nullopt;
			}
			out.right = out.forward.Cross(RE::NiPoint3{ 0.0f, 0.0f, 1.0f });
			if (out.right.Unitize() <= 0.001f) {
				return std::nullopt;
			}
			out.up = out.right.Cross(out.forward);
			if (out.up.Unitize() <= 0.001f) {
				return std::nullopt;
			}

			// Health check 2 (see above): the camera's own forward axis must project to the
			// viewport center. 0.12 normalized is loose enough to pass one frame of update
			// skew during a violent orbit flick, and tight enough to catch a frozen matrix.
			// A second probe offset to the right must land measurably off the first — a
			// degenerate matrix collapses WorldToScreen to its (0,0) sentinel, which
			// normalizes to exactly (0.5, 0.5) and would sail through the center check.
			RE::NiPoint3 probe{ -1.0f, -1.0f, -1.0f };
			RE::NiPoint3 probeRight{ -1.0f, -1.0f, -1.0f };
			if (!out.Project(out.position + out.forward * 10.0f, probe) ||
				std::abs(probe.x - 0.5f) > 0.12f || std::abs(probe.y - 0.5f) > 0.12f ||
				!out.Project(out.position + out.forward * 10.0f + out.right, probeRight) ||
				std::abs(probeRight.x - probe.x) < 0.005f) {
				if (ShouldLogCameraAnomaly()) {
					REX::WARN("[UI] world-pick projection rejected: camera matrix disagrees with its own transform (forward probe {:.2f},{:.2f}; right offset {:.3f})",
						probe.x, probe.y, std::abs(probeRight.x - probe.x));
				}
				return std::nullopt;
			}
			return out;
		}

		bool RenderedBound(RE::TESObjectREFR* a_ref, bool a_actor, RE::NiPoint3& a_center, float& a_radius)
		{
			if (!a_ref || a_ref->IsDeleted()) {
				return false;
			}
			RE::NiPointer<RE::NiAVObject> node;
			{
				const auto loaded = a_ref->loadedData.LockRead();
				if (*loaded) {
					node = (*loaded)->data3D;
				}
			}
			if (!node) {
				return false;
			}
			a_center = node->worldBound.center;
			a_radius = node->worldBound.radius;
			if (!std::isfinite(a_radius) || a_radius < 0.01f || a_radius > 10000.0f ||
				!std::isfinite(a_center.x) || !std::isfinite(a_center.y) || !std::isfinite(a_center.z)) {
				a_center = node->world.translate;
				a_radius = a_actor ? 0.8f : 0.6f;
			}
			return true;
		}

		bool RenderedActorLabelPoint(RE::TESObjectREFR* a_ref, RE::NiPoint3& a_point)
		{
			if (!a_ref || !a_ref->IsActor() || a_ref->IsDeleted()) {
				return false;
			}
			RE::NiPointer<RE::NiAVObject> root;
			{
				const auto loaded = a_ref->loadedData.LockRead();
				if (*loaded) {
					root = (*loaded)->data3D;
				}
			}
			if (!root) {
				return false;
			}

			// The label belongs to the rendered head, not the actor's worldBound. worldBound is
			// a culling sphere and may be expanded or re-centered by weapons, animation and OSF's
			// compose-root cull pin, so center+radius is not a stable anatomical point.
			static const RE::BSFixedString headName{ "C_Head" };
			if (RE::NiAVObject* head = root->GetObjectByName(headName)) {
				// Render-node transforms are in meters (unlike TESObjectREFR logical
				// positions). Twelve centimetres clears the top of the rendered head.
				a_point = head->world.translate + RE::NiPoint3{ 0.0f, 0.0f, 0.12f };
				return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
			}

			// Creature rigs do not consistently expose C_Head. Keep their fallback close to the
			// rendered body by clamping the culling radius to plausible dimensions in METERS.
			const RE::NiPoint3 center = root->worldBound.center;
			const float radius = std::clamp(root->worldBound.radius, 0.45f, 1.35f);
			a_point = center + RE::NiPoint3{ 0.0f, 0.0f, radius };
			return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
		}

		bool RenderedFurnitureLabelPoint(RE::TESObjectREFR* a_ref, RE::NiPoint3& a_point)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!a_ref || a_ref->IsDeleted() || !base || !base->Is(RE::FormType::kFURN)) {
				return false;
			}

			RE::NiPoint3 center;
			float        radius = 0.0f;
			if (!RenderedBound(a_ref, false, center, radius)) {
				return false;
			}
			// Float the label above the rendered object. Clamp the culling radius so
			// oversized workbenches and tiny/invisible idle markers stay readable.
			a_point = center + RE::NiPoint3{ 0.0f, 0.0f, std::clamp(radius, 0.25f, 1.4f) };
			return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
		}

		void OnProjectActors(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json j = ParsePayload(a_payload);
			json items = json::array();
			if (!j.is_object() || !j.contains("tokens") || !j["tokens"].is_array()) {
				SendJson(a_srcView, "osf.animation.actorIndicators", json{ { "items", std::move(items) } });
				return;
			}
			const float width = std::clamp(NumOr(j, "width", 1280.0f), 320.0f, 10000.0f);
			const float height = std::clamp(NumOr(j, "height", 720.0f), 200.0f, 10000.0f);
			const auto projection = CurrentViewProjection(width, height);
			if (!projection) {
				SendJson(a_srcView, "osf.animation.actorIndicators", json{ { "items", std::move(items) } });
				return;
			}

			constexpr std::size_t kMaxIndicators = 16;
			for (const auto& value : j["tokens"]) {
				if (items.size() >= kMaxIndicators || !value.is_number_integer()) {
					break;
				}
				const std::int32_t token = Int32Value(value).value_or(0);
				RE::TESObjectREFR* ref = ResolveToken(token);
				if (!ref) {
					continue;
				}
				RE::NiPoint3 labelPoint;
				RE::NiPoint3 screen;
				const bool hasLabelPoint = ref->IsActor()
				                               ? RenderedActorLabelPoint(ref, labelPoint)
				                               : RenderedFurnitureLabelPoint(ref, labelPoint);
				const bool projected = hasLabelPoint &&
					projection->Project(labelPoint, screen);
				const bool visible = projected && screen.x >= 0.0f && screen.x <= 1.0f && screen.y >= 0.0f && screen.y <= 1.0f;
				items.push_back(json{
					{ "token", token },
					{ "x", projected ? screen.x : 0.0f },
					{ "y", projected ? screen.y : 0.0f },
					{ "visible", visible },
				});
			}
			SendJson(a_srcView, "osf.animation.actorIndicators", json{ { "items", std::move(items) } });
		}

		// ---- nearby-actor enumeration ----------------------------------------
		// ProcessLists::highActorHandles (CommonLibSF, +0x60): near-player, fully-3D actors that SPAN the loaded cell grid (interior + exterior neighbours).
		// Of the four process lists we ONLY touch high — lowActorHandles holds 600-1200 partially-loaded actors whose vfuncs __fastfail uncatchably.
		// EnumerateLoadedActors below fills the high tier's gaps from the cell grid instead.

		std::uintptr_t VtableAddr(REL::ID a_id) { return REL::Relocation<std::uintptr_t>{ a_id }.address(); }
		void EnumerateHighActors(std::vector<RE::Actor*>& a_out)
		{
			auto* pl = RE::ProcessLists::GetSingleton();
			if (!pl) {
				return;
			}

			auto&               handles = pl->highActorHandles;
			const std::uint32_t size = handles.size();
			if (size == 0 || size > 0x4000) {
				return;
			}

			// The list can hold mixed TESObjectREFR*/Actor*, so confirm each resolved object is a real Actor by its primary vtable before use.
			const std::uintptr_t actorVtbl = VtableAddr(REL::ID(451614));
			a_out.reserve(a_out.size() + size);
			for (std::uint32_t i = 0; i < size; ++i) {
				RE::BSPointerHandle<RE::Actor>& h = handles[i];
				if (!static_cast<bool>(h)) {
					continue;
				}
				const RE::NiPointer<RE::Actor> p = h.get();  // GetSmartPointer ID 35638; self-guards bad handles
				RE::Actor* const               a = p.get();
				if (!a || *reinterpret_cast<std::uintptr_t*>(a) != actorVtbl) {
					continue;
				}
				a_out.push_back(a);
			}
		}

		// The high list alone is NOT "every actor standing next to you": the AI
		// demotes loaded-and-rendered actors out of the high tier (city crowds,
		// ambient schedules), and those never appear in highActorHandles — the
		// in-game symptom was Scan Nearby / picking skipping random visible NPCs.
		// Union the high list with the loaded cell grid via
		// TES::ForEachReferenceInRange — the same walker the furniture scan uses
		// (in-game proven on .244) — filtered to exact-vtable Actors WITH rendered
		// 3D. The data3D gate keeps the low-bucket crash rule intact: downstream
		// code vfuncs these (GetDisplayFullName), which is only safe on fully
		// loaded actors, and a rendered scene graph is the raw-field proof of that.
		void EnumerateLoadedActors(const RE::NiPoint3& a_origin, float a_radius, std::vector<RE::Actor*>& a_out)
		{
			OSF_PROFILE_SCOPE_N("UI.Pick.EnumerateActors");

			EnumerateHighActors(a_out);
			auto* tes = RE::TES::GetSingleton();
			if (!tes || a_radius <= 0.0f) {
				return;
			}
			std::unordered_set<const RE::Actor*> seen(a_out.begin(), a_out.end());
			const std::uintptr_t                 actorVtbl = VtableAddr(REL::ID(451614));
			RE::NiPoint3A                        origin{};
			origin.x = a_origin.x;
			origin.y = a_origin.y;
			origin.z = a_origin.z;
			tes->ForEachReferenceInRange(origin, a_radius, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) {
				RE::TESObjectREFR* ref = a_ref.get();
				if (!ref || *reinterpret_cast<std::uintptr_t*>(ref) != actorVtbl) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				auto* actor = static_cast<RE::Actor*>(ref);
				if (seen.contains(actor)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				bool rendered = false;
				{
					const auto loaded = actor->loadedData.LockRead();
					rendered = *loaded && (*loaded)->data3D;
				}
				if (rendered) {
					seen.insert(actor);
					a_out.push_back(actor);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		// Minimum ON-SCREEN size of a pickable target, in 1080p-equivalent pixels
		// (PickScreenBound::sizePx). Picking is a sight interaction: a target that
		// renders smaller than this is too far to be deliberately aimed at, which
		// bounds the pick range WITHOUT assuming any world unit — this file's first
		// depth gate was authored in "meters" and turned out to filter everything
		// (in-game: nothing pickable), because nothing in this pipeline actually
		// knows the engine's unit scale; every working part of it is ratios of
		// projections. A standing human's span falls under 24 px around 90-100 m
		// out — but SEATED/CROUCHED bodies measure much shorter, and the original
		// 40 px gate dropped sitting actors at ordinary exterior distances
		// (in-game: exterior picking missed visible actors, '6 small' in the
		// stats). Accidental far picks stay unlikely at 24 px because the
		// acceptance floors shrink to a third at that size — distant targets
		// demand precise aim. SCAN remains the tool for anything beyond sight,
		// and there is still no mapped physics ray for true occlusion.
		constexpr float kMinActorSizePx = 24.0f;
		constexpr float kMinFurnitureSizePx = 10.0f;

		// Enumeration + click-revalidation range for actor picking, in game units
		// (~70/m). Must reach at least as far as the 24 px size gate admits (a
		// standing human passes it out to ~100 m): the old 4096-unit (~58 m)
		// revalidation could reject a click on a marker the picker itself offered.
		constexpr float kPickRangeUnits = 8192.0f;

		// The on-screen size at which the acceptance-ellipse floors apply in full;
		// below it they shrink proportionally (to a third at the pick minimum), so
		// small/far targets demand precise aim instead of being selectable
		// "through" nearer geometry via oversized invisible hit regions.
		constexpr float kFullFloorActorSizePx = 130.0f;
		constexpr float kFullFloorFurnitureSizePx = 45.0f;

		float PickFloorScale(float a_sizePx, float a_fullSizePx)
		{
			return a_fullSizePx > 0.0f ? std::clamp(a_sizePx / a_fullSizePx, 0.3f, 1.0f) : 1.0f;
		}

		// The screen-space acceptance ellipse of a pickable target: projected bound
		// center plus clamped pixel radii. The view renders these as hover markers
		// AND resolves the click against them (hottestPickTarget), so what the user
		// sees lit is by construction what a click selects.
		struct PickScreenBound
		{
			RE::NiPoint3 screen;  // normalized center; z = view depth (whatever unit the render camera uses — RELATIVE comparisons only)
			float        radiusX{ 0.0f };
			float        radiusY{ 0.0f };
			float        sizePx{ 0.0f };  // unfloored on-screen extent, normalized to a 1080p-tall viewport
		};

		// Actor hit regions come from the rendered skeleton, not worldBound: the
		// cull sphere is expanded and re-centered by weapons, animation, and OSF's
		// own compose-root cull pin, which made some actors unclickable — their
		// acceptance ellipse sat nowhere near the visible body. Both landmarks
		// must be RENDERED BONES (C_Head, C_Hips): bones are exactly where the
		// visible body is, while the data3D root's translate proved to drift away
		// from the body (runtime-observed: actors OSF had animated missed every
		// click with scores of 8-260 while furniture picks kept landing). Feet
		// are estimated by mirroring the head across the hips along the body
		// axis, spanning standing AND prone poses; the ellipse is axis-aligned,
		// so each radius covers whichever span component runs its way.
		bool ActorScreenCapsule(const SafeViewProjection& a_projection, RE::Actor* a_actor,
			float a_width, float a_height, PickScreenBound& a_out)
		{
			RE::NiPointer<RE::NiAVObject> root;
			{
				const auto loaded = a_actor->loadedData.LockRead();
				if (*loaded) {
					root = (*loaded)->data3D;
				}
			}
			if (!root) {
				return false;
			}
			static const RE::BSFixedString headName{ "C_Head" };
			static const RE::BSFixedString hipsName{ "C_Hips" };
			RE::NiAVObject* head = root->GetObjectByName(headName);
			RE::NiAVObject* hips = root->GetObjectByName(hipsName);
			RE::NiPoint3    headWorld;
			RE::NiPoint3    baseWorld;
			if (head && hips) {
				headWorld = head->world.translate + RE::NiPoint3{ 0.0f, 0.0f, 0.12f };
				baseWorld = hips->world.translate + (hips->world.translate - head->world.translate);
			} else if (RenderedActorLabelPoint(a_actor, headWorld)) {
				// Creature rigs without the humanoid bones: label point (clamped
				// worldBound top) against the root translate, as before.
				baseWorld = root->world.translate;
			} else {
				return false;
			}
			RE::NiPoint3 headScreen;
			RE::NiPoint3 baseScreen;
			if (!a_projection.Project(headWorld, headScreen) || !a_projection.Project(baseWorld, baseScreen)) {
				return false;
			}
			const float dxPx = std::abs(headScreen.x - baseScreen.x) * a_width;
			const float dyPx = std::abs(headScreen.y - baseScreen.y) * a_height;
			const float span = std::hypot(dxPx, dyPx);
			a_out.screen = {
				(headScreen.x + baseScreen.x) * 0.5f,
				(headScreen.y + baseScreen.y) * 0.5f,
				std::min(headScreen.z, baseScreen.z),
			};

			// On-screen size for the pick gate + floor scaling. The axial span alone
			// vanishes when the camera looks straight down the body (orbit overhead),
			// so measure width too: a shoulder-scale lateral offset, sized as a
			// fraction of the body axis itself so no world unit is assumed.
			const RE::NiPoint3 axis{ headWorld.x - baseWorld.x, headWorld.y - baseWorld.y, headWorld.z - baseWorld.z };
			const float        bodyLen = std::sqrt(axis.Dot(axis));
			const RE::NiPoint3 mid{ (headWorld.x + baseWorld.x) * 0.5f, (headWorld.y + baseWorld.y) * 0.5f, (headWorld.z + baseWorld.z) * 0.5f };
			float              widthPx = 0.0f;
			RE::NiPoint3       sideScreen;
			if (bodyLen > 0.0f && a_projection.Project(mid + a_projection.right * (bodyLen * 0.18f), sideScreen)) {
				widthPx = 2.0f * std::hypot((sideScreen.x - a_out.screen.x) * a_width,
				                            (sideScreen.y - a_out.screen.y) * a_height);
			}
			const float heightScale = a_height / 1080.0f;  // px thresholds are authored at 1080p
			a_out.sizePx = std::max(span, widthPx) / heightScale;

			const float minScale = PickFloorScale(a_out.sizePx, kFullFloorActorSizePx);
			a_out.radiusX = std::clamp(std::max(dxPx * 0.62f, span * 0.22f), 38.0f * minScale, 260.0f);
			a_out.radiusY = std::clamp(std::max(dyPx * 0.62f, span * 0.22f), 52.0f * minScale, 320.0f);
			return true;
		}

		bool ComputePickScreenBound(const SafeViewProjection& a_projection, RE::TESObjectREFR* a_ref, bool a_actor,
			float a_width, float a_height, PickScreenBound& a_out)
		{
			if (a_actor && ActorScreenCapsule(a_projection, static_cast<RE::Actor*>(a_ref), a_width, a_height, a_out)) {
				return true;
			}
			RE::NiPoint3 center;
			float radius = 0.0f;
			if (!RenderedBound(a_ref, a_actor, center, radius) || !a_projection.Project(center, a_out.screen)) {
				return false;
			}
			const float projectedRadius = a_projection.ProjectedRadius(center, radius, a_width, a_height);
			// The bound radius spans roughly half the object, so double it for actors to
			// stay comparable with the capsule path's full-body span measure.
			a_out.sizePx = projectedRadius * (a_actor ? 2.0f : 1.0f) / (a_height / 1080.0f);
			const float minScale = PickFloorScale(a_out.sizePx, a_actor ? kFullFloorActorSizePx : kFullFloorFurnitureSizePx);
			a_out.radiusX = std::clamp(projectedRadius * 1.15f, (a_actor ? 38.0f : 30.0f) * minScale, 220.0f);
			a_out.radiusY = std::clamp(projectedRadius * 1.15f, (a_actor ? 52.0f : 30.0f) * minScale, 260.0f);
			return true;
		}

		void SendScreenPick(const char* a_view, const std::string& a_slot, RE::TESObjectREFR* a_ref)
		{
			json reply{
				{ "slot", a_slot },
				{ "valid", a_ref != nullptr },
				{ "token", 0 },
				{ "name", "" },
				{ "formId", 0 },
			};
			if (a_ref) {
				const std::int32_t token = AllocToken(a_ref);
				reply["token"] = token;
				reply["name"] = ScanLabel(a_ref);
				reply["formId"] = a_ref->GetFormID();
				reply["species"] = a_ref->IsActor() ? Util::ActorSpecies(static_cast<RE::Actor*>(a_ref)) : std::string{};
				reply["sex"] = RefSexTag(a_ref);
				if (auto* player = RE::PlayerCharacter::GetSingleton()) {
					reply["distance"] = std::sqrt(player->GetPosition().GetSquaredDistance(a_ref->GetPosition())) / 70.0f;
				}
			}
			SendJson(a_view, "osf.animation.pick", reply);
		}

		// The view resolves a click against the SAME marker geometry it renders (the hot
		// marker from projectPickables) and sends that target's token, so the target the
		// user saw lit is by construction the one picked — no second projection pass at
		// click time that could disagree with the markers (stale camera, frame skew).
		// This side only re-validates that the token still names a live, eligible target.
		void OnPickScreen(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json         j = ParsePayload(a_payload);
			const std::string  slot = StrOr(j, "slot") == "furniture" ? "furniture" : "actor";
			const std::int32_t token = IntOr(j, "token", 0);
			auto*              player = RE::PlayerCharacter::GetSingleton();
			RE::TESObjectREFR* ref = token != 0 ? ResolveToken(token) : nullptr;
			if (ref && player) {
				bool eligible;
				if (slot == "actor") {
					auto* actor = ref->IsActor() ? static_cast<RE::Actor*>(ref) : nullptr;
					eligible = actor && !actor->IsPlayerRef() && !actor->IsDead();
				} else {
					const auto base = ref->GetBaseObject();
					eligible = !ref->IsDeleted() && base && base->Is(RE::FormType::kFURN);
				}
				if (!eligible || player->GetPosition().GetSquaredDistance(ref->GetPosition()) > kPickRangeUnits * kPickRangeUnits) {
					ref = nullptr;
				}
			} else {
				ref = nullptr;
			}
			REX::DEBUG("[UI] world PICK {} token={} -> {}", slot, token,
				ref ? std::format("'{}' ({:08X})", ScanLabel(ref), ref->GetFormID()) : "no longer a valid target");
			SendScreenPick(a_srcView, slot, ref);
		}

		// While a pick is armed the view polls this (~10 Hz) and marks pickable
		// targets — hover-only for actors, every candidate for furniture. Each item
		// carries the target's token, the marker anchor, and the acceptance ellipse
		// (ComputePickScreenBound). This is the ONLY projection pass in the pick
		// flow: the view renders these markers, resolves the click against them,
		// and sends back the hot marker's token (OnPickScreen just validates it).
		void OnProjectPickables(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			OSF_PROFILE_SCOPE_N("UI.ProjectPickables");

			const json        j = ParsePayload(a_payload);
			const std::string slot = StrOr(j, "slot") == "furniture" ? "furniture" : "actor";
			const float       width = std::clamp(NumOr(j, "width", 0.0f), 320.0f, 10000.0f);
			const float       height = std::clamp(NumOr(j, "height", 0.0f), 200.0f, 10000.0f);
			const auto        projection = CurrentViewProjection(width, height);
			auto*             player = RE::PlayerCharacter::GetSingleton();

			const bool actorSlot = slot == "actor";

			// Gather every candidate WITH its screen bound, then gate and rank on that
			// bound: on-screen and rendering at least kMin*SizePx (sight-ranged, unit-free).
			// Eligibility is decided HERE (a marker IS pickability); OnPickScreen re-checks
			// only to catch a target that died or left range between marker poll and click.
			struct Candidate
			{
				RE::TESObjectREFR* ref;
				PickScreenBound    bound;
			};
			std::vector<Candidate> candidates;
			// Per-stage drop counters, so the periodic stats line pinpoints WHICH gate
			// eats targets when "some actors won't mark" (in-game report: exteriors).
			std::size_t enumerated = 0;    // reached us from the engine at all
			std::size_t ineligible = 0;    // player / dead
			std::size_t no3D = 0;          // no rendered scene-graph node at all
			std::size_t behindCam = 0;     // healthy drop: not in front of the pick camera
			std::size_t projectFail = 0;   // has 3D, in front, but projection failed — the suspicious bucket
			std::size_t droppedSmall = 0;  // renders too small to aim at
			std::size_t offScreen = 0;
			float       smallMaxPx = 0.0f;  // largest size the small-gate rejected (tuning signal)
			const auto consider = [&](RE::TESObjectREFR* a_ref) {
				OSF_PROFILE_SCOPE_N("UI.Pick.ProjectCandidate");

				PickScreenBound bound;
				if (!ComputePickScreenBound(*projection, a_ref, actorSlot, width, height, bound)) {
					// Classify the failure for the stats line only: an exterior grid
					// legitimately holds many loaded actors behind the camera, and
					// those must not be mistaken for broken projections.
					RE::NiPointer<RE::NiAVObject> root;
					{
						const auto loaded = a_ref->loadedData.LockRead();
						if (*loaded) {
							root = (*loaded)->data3D;
						}
					}
					if (!root) {
						++no3D;
					} else if ((root->world.translate - projection->position).Dot(projection->forward) <= 0.01f) {
						++behindCam;
					} else {
						++projectFail;
					}
					return;
				}
				if (bound.sizePx < (actorSlot ? kMinActorSizePx : kMinFurnitureSizePx)) {
					++droppedSmall;  // beyond deliberate-aim range — SCAN covers those
					smallMaxPx = std::max(smallMaxPx, bound.sizePx);
					return;
				}
				if (bound.screen.x < -0.1f || bound.screen.x > 1.1f ||
					bound.screen.y < -0.1f || bound.screen.y > 1.1f) {
					++offScreen;  // comfortably off-screen — never hoverable
					return;
				}
				candidates.push_back({ a_ref, bound });
			};

			if (projection && player && actorSlot) {
				std::vector<RE::Actor*> actors;
				EnumerateLoadedActors(player->GetPosition(), kPickRangeUnits, actors);
				enumerated = actors.size();
				for (RE::Actor* actor : actors) {
					if (!actor || actor->IsPlayerRef() || actor->IsDead()) {
						++ineligible;
						continue;
					}
					consider(actor);
				}
			} else if (projection && player) {
				if (auto* tes = RE::TES::GetSingleton()) {
					RE::NiPoint3A origin{};
					origin.x = player->GetPosition().x;
					origin.y = player->GetPosition().y;
					origin.z = player->GetPosition().z;
					tes->ForEachReferenceInRange(origin, 4096.0f, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) {
						RE::TESObjectREFR* ref = a_ref.get();
						const auto base = ref ? ref->GetBaseObject() : nullptr;
						if (ref && !ref->IsPlayerRef() && !ref->IsDeleted() && base && base->Is(RE::FormType::kFURN)) {
							++enumerated;
							consider(ref);
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
			}
			OSF_PROFILE_PLOT("UI.Pick.Enumerated", static_cast<std::int64_t>(enumerated));
			OSF_PROFILE_PLOT("UI.Pick.Candidates", static_cast<std::int64_t>(candidates.size()));

			// Periodic snapshot of what each gate saw, at DEBUG (Settings > OSF
			// Animation > Advanced > Log level). Reading one line resolves where
			// unmarkable targets are lost:
			//   enumerated low while more actors are visible -> they escaped BOTH the
			//     HIGH-process list and the rendered-actor cell walk (beyond
			//     kPickRangeUnits, or no data3D — OSF must not touch the low list);
			//   small high -> the on-screen size gate ("small<Npx" is the largest
			//     body it rejected — the tuning signal for kMin*SizePx);
			//   behind -> healthy (loaded actors behind the pick camera);
			//   project-fail high -> VISIBLE actors failing projection = a real bug.
			{
				static std::chrono::steady_clock::time_point s_lastStats{};
				const auto now = std::chrono::steady_clock::now();
				if (now - s_lastStats >= std::chrono::seconds(5)) {
					s_lastStats = now;
					float nearDepth = std::numeric_limits<float>::max();
					float bigSize = 0.0f;
					for (const Candidate& candidate : candidates) {
						nearDepth = std::min(nearDepth, candidate.bound.screen.z);
						bigSize = std::max(bigSize, candidate.bound.sizePx);
					}
					REX::DEBUG("[UI] pickables {}: {} markers of {} enumerated (dropped: {} ineligible, {} no-3d, {} behind, {} project-fail, {} small<{:.0f}px, {} off-screen), nearest depth {:.2f}, largest {:.0f}px",
						slot, candidates.size(), enumerated, ineligible, no3D, behindCam, projectFail, droppedSmall, smallMaxPx, offScreen,
						candidates.empty() ? -1.0f : nearDepth, bigSize);
				}
			}

			// The caps keep crowded scenes bounded. Rank by VIEW DEPTH — the ordering
			// the user sees — so the cap always drops the deepest markers. (The old
			// player-distance sort starved visible actors of markers whenever the
			// orbit camera roamed away from the player. Depth is used RELATIVELY only;
			// its absolute unit is unconfirmed.)
			std::sort(candidates.begin(), candidates.end(), [](const Candidate& a_lhs, const Candidate& a_rhs) {
				return a_lhs.bound.screen.z < a_rhs.bound.screen.z;
			});
			const std::size_t cap = actorSlot ? 48 : 32;
			json              items = json::array();
			for (const Candidate& candidate : candidates) {
				if (items.size() >= cap) {
					break;
				}
				// Marker anchor: actors use the rendered head the name labels sit on;
				// furniture floats the marker just above its rendered bound. The bound
				// center is the fallback either way.
				RE::NiPoint3 anchorScreen = candidate.bound.screen;
				RE::NiPoint3 anchorWorld;
				RE::NiPoint3 center;
				float        radius = 0.0f;
				const bool   anchored = actorSlot
				      ? RenderedActorLabelPoint(candidate.ref, anchorWorld)
				      : (RenderedBound(candidate.ref, false, center, radius) &&
							(anchorWorld = center + RE::NiPoint3{ 0.0f, 0.0f, std::clamp(radius, 0.25f, 1.4f) }, true));
				if (anchored) {
					RE::NiPoint3 projected;
					if (projection->Project(anchorWorld, projected)) {
						anchorScreen = projected;
					}
				}
				items.push_back(json{
					{ "token", AllocToken(candidate.ref) },
					{ "x", anchorScreen.x },
					{ "y", anchorScreen.y },
					{ "cx", candidate.bound.screen.x },
					{ "cy", candidate.bound.screen.y },
					{ "rx", candidate.bound.radiusX },
					{ "ry", candidate.bound.radiusY },
					{ "depth", candidate.bound.screen.z },
				});
			}
			SendJson(a_srcView, "osf.animation.pickTargets", json{ { "slot", slot }, { "items", std::move(items) } });
		}

		// Nearby-furniture enumeration goes through RE::TES::ForEachReferenceInRange (CommonLibSF),
		// which spans the loaded interior cell or exterior grid — see OnScanNearby's furniture branch.

		// Distance math uses TESObjectREFR::GetPosition() (cached data.location), the same source the rest of OSF Animation uses for actor/anchor placement.
		void OnScanNearby(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const auto  scanStart = std::chrono::steady_clock::now();
			const json  j = ParsePayload(a_payload);
			std::string kind = "actor";
			std::string sceneId;
			float       radius = 4096.0f;  // ~58m; a room / nearby area
			if (j.is_object()) {
				if (const auto it = j.find("kind"); it != j.end() && it->is_string()) {
					kind = it->get<std::string>();
				}
				if (const auto it = j.find("sceneId"); it != j.end() && it->is_string()) {
					sceneId = it->get<std::string>();
				}
				if (const auto it = j.find("radius"); it != j.end() && it->is_number()) {
					radius = NumOr(j, "radius", radius);
				}
			}
			const bool wantActor = (kind != "furniture");

			json reply;
			reply["kind"] = kind;
			reply["items"] = json::array();

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				REX::DEBUG("[UI] osf.animation.scanNearby kind={} -> no player", kind);
				SendJson(a_srcView, "osf.animation.scanResults", reply);
				return;
			}

			const RE::NiPoint3 origin = player->GetPosition();
			const float        radiusSq = (radius > 0.0f) ? radius * radius : 4096.0f * 4096.0f;

			struct Hit
			{
				RE::TESObjectREFR* ref;
				float              distSq;
				std::int32_t       sceneCount = -1;      // furniture only: total anchor-bound scenes that accept it (-1 = n/a)
				std::int32_t       customCount = -1;     // furniture only: of those, how many are custom (non-library) scenes
				RE::BGSKeyword*    matchedKw = nullptr;  // furniture only: the anchor keyword that matched (labels unnamed markers)
			};
			std::vector<Hit> hits;
			// Collect candidate pointers + distance only; serialize (GetDisplayFullName / token minting) afterwards so the heavy work stays out of any engine lock.
			if (wantActor) {
				std::vector<RE::Actor*> actors;
				EnumerateLoadedActors(origin, (radius > 0.0f) ? radius : 4096.0f, actors);
				for (RE::Actor* actor : actors) {
					if (!actor || actor->IsPlayerRef() || actor->IsDeleted() || actor->IsDead()) {
						continue;
					}
					const float distSq = origin.GetSquaredDistance(actor->GetPosition());
					if (distSq <= radiusSq) {
						hits.push_back({ actor, distSq });
					}
				}
			} else if (auto* tes = RE::TES::GetSingleton()) {
				// Inverted anchor index, built fresh each scan: keyword -> accepting def indices and base form -> accepting def indices.
				// Matching a ref then costs one HasKeyword per UNIQUE keyword instead of per (def x keyword).
				std::vector<bool>                                               defCustom;  // def index -> custom (non-library) scene
				std::unordered_map<RE::BGSKeyword*, std::vector<std::uint32_t>> kwDefs;
				std::unordered_map<RE::TESFormID, std::vector<std::uint32_t>>   baseDefs;
				const auto                                                      addDef = [&](const Registry::SceneDef& d) {
					if (!d.RequiresAnchor() || !d.clipsAvailable) {
						return;
					}
					const auto idx = static_cast<std::uint32_t>(defCustom.size());
					defCustom.push_back(!d.library);
					// Resolve each keyword id fresh for this scan; the pointer only lives as a map key
					// for the duration of the sweep (HasKeyword needs the form, not the id).
					for (const auto kwId : d.anchorKeywords) {
						if (auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(kwId)) {
							kwDefs[kw].push_back(idx);
						}
					}
					for (const auto b : d.anchorBaseForms) {
						baseDefs[b].push_back(idx);
					}
				};
				auto& reg = Registry::SceneRegistry::GetSingleton();
				if (!sceneId.empty()) {
					if (const auto def = reg.Find(sceneId)) {
						addDef(*def);
					}
				}
				if (defCustom.empty()) {
					reg.ForEachDef(addDef);
				}

				RE::NiPoint3A originA{};
				originA.x = origin.x;
				originA.y = origin.y;
				originA.z = origin.z;
				// Anchor keywords live on BASE records (the ESM extractor reads them from FURN/ACTI
				// forms), so keyword probing is memoized PER UNIQUE BASE: a POI places the same chair
				// or marker base dozens of times, and the unique-keyword set is ~150 strong with the
				// vanilla packs — probing it per REF (refs x keywords engine calls) was still a hitch.
				struct BaseMatch
				{
					std::int32_t    accepts = 0;
					std::int32_t    customAccepts = 0;
					RE::BGSKeyword* kw = nullptr;  // a matching keyword (labels unnamed markers)
				};
				std::unordered_map<RE::TESFormID, BaseMatch> baseCache;
				std::vector<std::uint32_t>                   matched;  // scratch: accepting def indices
				// ForEachReferenceInRange spans the loaded interior cell or exterior grid and only
				// visits refs already within radius; we just filter to furniture our scenes anchor to.
				tes->ForEachReferenceInRange(originA, radius, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) {
					RE::TESObjectREFR* ref = a_ref.get();
					if (ref && !ref->IsPlayerRef() && !ref->IsDeleted()) {
						const auto base = ref->GetBaseObject();
						if (!base) {
							return RE::BSContainer::ForEachResult::kContinue;  // anchors match by base form / base-record keywords
						}
						auto cit = baseCache.find(base->GetFormID());
						if (cit == baseCache.end()) {
							// First ref of this base: count every accepting def (not just the first) —
							// the view shows "unlocks N scenes" next to each nearby anchor.
							matched.clear();
							BaseMatch m;
							if (const auto it = baseDefs.find(base->GetFormID()); it != baseDefs.end()) {
								matched.insert(matched.end(), it->second.begin(), it->second.end());
							}
							for (const auto& [kw, idxs] : kwDefs) {
								if (ref->HasKeyword(kw)) {
									if (!m.kw) {
										m.kw = kw;  // any matching keyword will do
									}
									matched.insert(matched.end(), idxs.begin(), idxs.end());
								}
							}
							// A def can match via its base form AND several keywords — count it once.
							std::sort(matched.begin(), matched.end());
							matched.erase(std::unique(matched.begin(), matched.end()), matched.end());
							m.accepts = static_cast<std::int32_t>(matched.size());
							for (const auto i : matched) {
								if (defCustom[i]) {
									m.customAccepts++;  // custom (authored) scene, vs a generated vanilla-library pack
								}
							}
							cit = baseCache.emplace(base->GetFormID(), m).first;
						}
						const BaseMatch& m = cit->second;
						if (m.accepts != 0) {
							hits.push_back({ ref, origin.GetSquaredDistance(ref->GetPosition()), m.accepts, m.customAccepts, m.kw });
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}

			std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.distSq < b.distSq; });

			// Cap named refs and unnamed AI markers SEPARATELY: markers are dense (every sandbox
			// cell has dozens) and a single shared cap would crowd real furniture off the list.
			constexpr std::size_t kMaxPerGroup = 40;
			std::size_t           namedCount = 0, markerCount = 0;
			for (const auto& h : hits) {
				const char* nm = h.ref->GetDisplayFullName();
				// Furniture with no display name = invisible AI/idle marker (or unnamed outpost
				// piece) — still a legitimate anchor, but the view lists it under its own group.
				const bool marker = !wantActor && !(nm && nm[0]);
				auto&      count = marker ? markerCount : namedCount;
				if (count >= kMaxPerGroup) {
					continue;
				}
				count++;
				const std::int32_t token = AllocToken(h.ref);
				json               item = {
					{ "token", token },
					{ "name", ScanLabel(h.ref, h.matchedKw) },
					{ "formId", h.ref->GetFormID() },
					{ "distance", std::sqrt(h.distSq) / 70.0f },  // game units -> ~meters
					{ "isActor", h.ref->IsActor() },
					{ "marker", marker },
					{ "species", h.ref->IsActor() ? Util::ActorSpecies(static_cast<RE::Actor*>(h.ref)) : std::string{} },
					{ "sex", RefSexTag(h.ref) },
				};
				if (h.sceneCount >= 0) {
					item["sceneCount"] = h.sceneCount;
					item["customCount"] = h.customCount;  // subset that is custom (authored), not vanilla library
				}
				reply["items"].push_back(std::move(item));
			}
			const auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - scanStart).count();
			REX::DEBUG("[UI] osf.animation.scanNearby kind={} radius={} -> {} hit(s) in {} ms", kind, radius, hits.size(), scanMs);
			SendJson(a_srcView, "osf.animation.scanResults", reply);
		}

	}

	void RegisterCommands(OSFUI::API::Client& a_ui)
	{
		g_ui = &a_ui;
		a_ui.RegisterCommand("osf.animation.pickScreen", &OnPickScreen, nullptr);
		a_ui.RegisterCommand("osf.animation.projectPickables", &OnProjectPickables, nullptr);
		a_ui.RegisterCommand("osf.animation.projectActors", &OnProjectActors, nullptr);
		a_ui.RegisterCommand("osf.animation.scanNearby", &OnScanNearby, nullptr);
	}
}
