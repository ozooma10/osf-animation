#include "API/EngineClient.h"
#include "API/UIBridgeWorld.h"

#include "API/BrowserSession.h"
#include "API/UIKeywordLabel.h"

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
		// Process-lifetime client owned by the browser session.
		OSFUI::API::Client* g_ui = nullptr;

		struct Picked
		{
			RE::TESObjectREFR* ref;
			RE::TESFormID      formID;  // must still resolve to `ref` at use
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
			EngineClient::CatalogData::GetSingleton().ForEachDef([&](const Catalog::Scene& a_def) {
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
			g_ui->EmitEvent(a_view, a_type, text.c_str());
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
			return nullptr;  // unloaded, reused, or deleted since the pick
		}
		return p.ref;
	}

	std::string ScanLabel(RE::TESObjectREFR* a_ref, RE::BGSKeyword* a_matchedKw)
	{
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
		         ? EngineClient::ActorGender(static_cast<RE::Actor*>(a_ref))
		         : std::string{};
	}

	RE::TESObjectREFR* CrosshairRef()
	{
		// The engine nulls this slot while any menu is up; capture before opening.
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
			RE::NiPointer<RE::NiNode> root;  // keeps the camera graph alive
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

				// Engine projection tracks live FOV/aspect; a fixed-FOV projection drifts.
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
			for (std::uint16_t i = 0; node->children && i < node->childrenSize; ++i) {
				if (auto* camera = FindCameraInNode(node->children[i], a_depth + 1)) {
					return camera;
				}
			}
			return nullptr;
		}

		RE::NiCamera* ActiveWorldCamera()
		{
			// ID 936470 = StorageTable::Camera host pointer; inline NiCamera at +0x80 is the world
			// render camera. PlayerCamera::cameraRoot only reaches the gameplay/viewmodel camera.
			static const REL::Relocation<std::uintptr_t> storageGlobal{ REL::ID(936470) };
			static const REL::Relocation<std::uintptr_t> cameraVtable{ RE::NiCamera::VTABLE[0] };

			const auto storage = *reinterpret_cast<const std::uintptr_t*>(storageGlobal.address());
			if (storage != 0) {
				auto* candidate = reinterpret_cast<RE::NiCamera*>(storage + 0x80);
				if (*reinterpret_cast<const std::uintptr_t*>(candidate) == cameraVtable.address()) {
					return candidate;
				}
			}

			// Fallback if the storage layout moves; may be the viewmodel lens.
			auto* playerCamera = RE::PlayerCamera::GetSingleton();
			const auto root = playerCamera ? playerCamera->cameraRoot : nullptr;
			return root ? FindCameraInNode(root.get()) : nullptr;
		}

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

		// Health checks: (1) while the scene orbit drives the camera, prefer the camera at OSF's own
		// orbit pose; (2) worldToCam is rebuilt asynchronously, so fail the query when it disagrees
		// with the world transform rather than place markers that lie.
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
			if (EngineClient::Get().ReadOrbitPose(orbitPos, orbitFwd)) {
				const auto poseErrorSq = [&](RE::NiCamera* a_camera) {
					const float dx = a_camera->world.translate.x - orbitPos[0];
					const float dy = a_camera->world.translate.y - orbitPos[1];
					const float dz = a_camera->world.translate.z - orbitPos[2];
					return dx * dx + dy * dy + dz * dz;
				};
				constexpr float kPoseToleranceSq = 1.5f * 1.5f;  // covers the orbit glide's frame lag
				RE::NiCamera*   alt = FindCameraInNode(root.get());
				const float     mainErr = out.camera ? poseErrorSq(out.camera) : std::numeric_limits<float>::max();
				const float     altErr = (alt && alt != out.camera) ? poseErrorSq(alt) : std::numeric_limits<float>::max();
				if (mainErr > kPoseToleranceSq) {
					// Relative test: the absolute tolerance assumes units, 4x closer holds in any.
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

			// Forward must project near center (0.12 tolerates one frame of skew). The right-offset
			// probe catches a degenerate matrix, whose (0,0) sentinel normalizes to exactly center.
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

			// worldBound is a cull sphere skewed by weapons, animation, and OSF's cull pin; use the head bone.
			static const RE::BSFixedString headName{ "C_Head" };
			if (RE::NiAVObject* head = root->GetObjectByName(headName)) {
				// Render-node transforms are meters (unlike REFR positions); 0.12 clears the head.
				a_point = head->world.translate + RE::NiPoint3{ 0.0f, 0.0f, 0.12f };
				return std::isfinite(a_point.x) && std::isfinite(a_point.y) && std::isfinite(a_point.z);
			}

			// Creature rigs may lack C_Head; clamp the cull radius (meters).
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
			// Clamped so huge workbenches and invisible idle markers stay readable.
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

		// ProcessLists::highActorHandles (+0x60) only: low-list actors are partially loaded and their
		// vfuncs __fastfail uncatchably.
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

			// Mixed REFR/Actor entries: require the Actor primary vtable (ID 451614).
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

		// The AI demotes rendered actors (crowds) out of the high list, so union with the cell walk.
		// The data3D gate keeps downstream vfunc calls safe (fully loaded actors only).
		void EnumerateLoadedActors(const RE::NiPoint3& a_origin, float a_radius, std::vector<RE::Actor*>& a_out)
		{
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

		// Minimum on-screen size (1080p px) bounds pick range without assuming world units.
		// 24 px keeps seated/crouched actors pickable at ordinary exterior distances.
		constexpr float kMinActorSizePx = 24.0f;
		constexpr float kMinFurnitureSizePx = 10.0f;

		// Game units (~70/m). Must reach at least as far as the size gate admits (~100 m).
		constexpr float kPickRangeUnits = 8192.0f;

		// Size at which hit-ellipse floors apply in full; smaller targets get shrunken floors.
		constexpr float kFullFloorActorSizePx = 130.0f;
		constexpr float kFullFloorFurnitureSizePx = 45.0f;

		float PickFloorScale(float a_sizePx, float a_fullSizePx)
		{
			return a_fullSizePx > 0.0f ? std::clamp(a_sizePx / a_fullSizePx, 0.3f, 1.0f) : 1.0f;
		}

		// The view renders these as hover markers and resolves clicks against them.
		struct PickScreenBound
		{
			RE::NiPoint3 screen;  // normalized; z = view depth, relative comparisons only
			float        radiusX{ 0.0f };
			float        radiusY{ 0.0f };
			float        sizePx{ 0.0f };  // unfloored extent at 1080p
		};

		// Uses C_Head/C_Hips bones: worldBound and the data3D root drift from the visible body on
		// OSF-animated actors. Feet = head mirrored across the hips.
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

			// Axial span vanishes when viewed down the body; also measure a shoulder-scale width.
			const RE::NiPoint3 axis{ headWorld.x - baseWorld.x, headWorld.y - baseWorld.y, headWorld.z - baseWorld.z };
			const float        bodyLen = std::sqrt(axis.Dot(axis));
			const RE::NiPoint3 mid{ (headWorld.x + baseWorld.x) * 0.5f, (headWorld.y + baseWorld.y) * 0.5f, (headWorld.z + baseWorld.z) * 0.5f };
			float              widthPx = 0.0f;
			RE::NiPoint3       sideScreen;
			if (bodyLen > 0.0f && a_projection.Project(mid + a_projection.right * (bodyLen * 0.18f), sideScreen)) {
				widthPx = 2.0f * std::hypot((sideScreen.x - a_out.screen.x) * a_width,
				                            (sideScreen.y - a_out.screen.y) * a_height);
			}
			const float heightScale = a_height / 1080.0f;
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
			// Doubled for actors to match the capsule path's full-body span.
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
				reply["species"] = a_ref->IsActor() ? EngineClient::ActorSpecies(static_cast<RE::Actor*>(a_ref)) : std::string{};
				reply["sex"] = RefSexTag(a_ref);
				if (auto* player = RE::PlayerCharacter::GetSingleton()) {
					reply["distance"] = std::sqrt(player->GetPosition().GetSquaredDistance(a_ref->GetPosition())) / 70.0f;
				}
			}
			SendJson(a_view, "osf.animation.pick", reply);
		}

		// The view sends the token of the marker it hit; this only re-validates it.
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

		// Polled ~10 Hz while a pick is armed; the only projection pass in the pick flow.
		void OnProjectPickables(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const json        j = ParsePayload(a_payload);
			const std::string slot = StrOr(j, "slot") == "furniture" ? "furniture" : "actor";
			const float       width = std::clamp(NumOr(j, "width", 0.0f), 320.0f, 10000.0f);
			const float       height = std::clamp(NumOr(j, "height", 0.0f), 200.0f, 10000.0f);
			const auto        projection = CurrentViewProjection(width, height);
			auto*             player = RE::PlayerCharacter::GetSingleton();

			const bool actorSlot = slot == "actor";

			// Eligibility is decided here; OnPickScreen only re-checks for changes since the poll.
			struct Candidate
			{
				RE::TESObjectREFR* ref;
				PickScreenBound    bound;
			};
			std::vector<Candidate> candidates;
			std::size_t enumerated = 0;
			std::size_t ineligible = 0;
			std::size_t no3D = 0;
			std::size_t behindCam = 0;     // healthy drop
			std::size_t projectFail = 0;   // in front with 3D yet failed: suspicious
			std::size_t droppedSmall = 0;
			std::size_t offScreen = 0;
			float       smallMaxPx = 0.0f;  // tuning signal for kMin*SizePx
			const auto consider = [&](RE::TESObjectREFR* a_ref) {
				PickScreenBound bound;
				if (!ComputePickScreenBound(*projection, a_ref, actorSlot, width, height, bound)) {
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
					++droppedSmall;
					smallMaxPx = std::max(smallMaxPx, bound.sizePx);
					return;
				}
				if (bound.screen.x < -0.1f || bound.screen.x > 1.1f ||
					bound.screen.y < -0.1f || bound.screen.y > 1.1f) {
					++offScreen;
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

			// Rank by view depth, not player distance: the orbit camera can roam away from the player.
			std::sort(candidates.begin(), candidates.end(), [](const Candidate& a_lhs, const Candidate& a_rhs) {
				return a_lhs.bound.screen.z < a_rhs.bound.screen.z;
			});
			const std::size_t cap = actorSlot ? 48 : 32;
			json              items = json::array();
			for (const Candidate& candidate : candidates) {
				if (items.size() >= cap) {
					break;
				}
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

		// GetPosition() here is the cached data.location, not the engine call.
		void OnScanNearby(const char*, const char* a_payload, const char* a_srcView, void*) noexcept
		{
			const auto  scanStart = std::chrono::steady_clock::now();
			const json  j = ParsePayload(a_payload);
			std::string kind = "actor";
			std::string sceneId;
			float       radius = 4096.0f;  // ~58 m
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
				std::int32_t       sceneCount = -1;      // furniture only; -1 = n/a
				std::int32_t       customCount = -1;     // furniture only: non-library subset
				RE::BGSKeyword*    matchedKw = nullptr;  // furniture only: labels unnamed markers
			};
			std::vector<Hit> hits;
			// Serialize after collection to keep heavy work out of engine locks.
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
				// Inverted index: one HasKeyword per unique keyword instead of per (def x keyword).
				std::vector<bool>                                               defCustom;  // def index -> non-library
				std::unordered_map<RE::BGSKeyword*, std::vector<std::uint32_t>> kwDefs;
				std::unordered_map<RE::TESFormID, std::vector<std::uint32_t>>   baseDefs;
				const auto                                                      addDef = [&](const Catalog::Scene& d) {
					if (!d.RequiresAnchor() || !d.clipsAvailable) {
						return;
					}
					const auto idx = static_cast<std::uint32_t>(defCustom.size());
					defCustom.push_back(!d.library);
					for (const auto kwId : d.anchorKeywords) {
						if (auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(kwId)) {
							kwDefs[kw].push_back(idx);
						}
					}
					for (const auto b : d.anchorBaseForms) {
						baseDefs[b].push_back(idx);
					}
				};
				auto& reg = EngineClient::CatalogData::GetSingleton();
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
				// Anchor keywords live on base records, so memoize per base: per-ref probing hitched.
				struct BaseMatch
				{
					std::int32_t    accepts = 0;
					std::int32_t    customAccepts = 0;
					RE::BGSKeyword* kw = nullptr;
				};
				std::unordered_map<RE::TESFormID, BaseMatch> baseCache;
				std::vector<std::uint32_t>                   matched;
				tes->ForEachReferenceInRange(originA, radius, [&](const RE::NiPointer<RE::TESObjectREFR>& a_ref) {
					RE::TESObjectREFR* ref = a_ref.get();
					if (ref && !ref->IsPlayerRef() && !ref->IsDeleted()) {
						const auto base = ref->GetBaseObject();
						if (!base) {
							return RE::BSContainer::ForEachResult::kContinue;
						}
						auto cit = baseCache.find(base->GetFormID());
						if (cit == baseCache.end()) {
							// Count every accepting def; the view shows "unlocks N scenes".
							matched.clear();
							BaseMatch m;
							if (const auto it = baseDefs.find(base->GetFormID()); it != baseDefs.end()) {
								matched.insert(matched.end(), it->second.begin(), it->second.end());
							}
							for (const auto& [kw, idxs] : kwDefs) {
								if (ref->HasKeyword(kw)) {
									if (!m.kw) {
										m.kw = kw;
									}
									matched.insert(matched.end(), idxs.begin(), idxs.end());
								}
							}
							std::sort(matched.begin(), matched.end());
							matched.erase(std::unique(matched.begin(), matched.end()), matched.end());
							m.accepts = static_cast<std::int32_t>(matched.size());
							for (const auto i : matched) {
								if (defCustom[i]) {
									m.customAccepts++;
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

			// Separate caps: dense unnamed AI markers would crowd real furniture off a shared list.
			constexpr std::size_t kMaxPerGroup = 40;
			std::size_t           namedCount = 0, markerCount = 0;
			for (const auto& h : hits) {
				const char* nm = h.ref->GetDisplayFullName();
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
					{ "species", h.ref->IsActor() ? EngineClient::ActorSpecies(static_cast<RE::Actor*>(h.ref)) : std::string{} },
					{ "sex", RefSexTag(h.ref) },
				};
				if (h.sceneCount >= 0) {
					item["sceneCount"] = h.sceneCount;
					item["customCount"] = h.customCount;
				}
				reply["items"].push_back(std::move(item));
			}
			const auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - scanStart).count();
			REX::DEBUG("[UI] osf.animation.scanNearby kind={} radius={} -> {} hit(s) in {} ms", kind, radius, hits.size(), scanMs);
			SendJson(a_srcView, "osf.animation.scanResults", reply);
		}

	}

	void RegisterCommands()
	{
		g_ui = &BrowserSession::Views();
		BrowserSession::RegisterSend("osf.animation.pickScreen", &OnPickScreen);
		BrowserSession::RegisterSend("osf.animation.projectPickables", &OnProjectPickables);
		BrowserSession::RegisterSend("osf.animation.projectActors", &OnProjectActors);
		BrowserSession::RegisterSend("osf.animation.scanNearby", &OnScanNearby);
	}
}
