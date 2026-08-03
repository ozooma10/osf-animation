#include "Studio/StudioPreviewService.h"

#include "Animation/GraphManager.h"
#include "Camera/CameraService.h"
#include "Props/PropService.h"
#include "Studio/SuitProtocolPreviewAPI.h"
#include "Util/Gzip.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <random>
#include <stop_token>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace OSF::Studio
{
	namespace
	{
		using json = nlohmann::json;
		using State = Suit::HelmetState;

		constexpr std::uint32_t kProtocolVersion = 2;
		constexpr std::uintmax_t kMaxRequestBytes = 256 * 1024;
		constexpr std::uint32_t kLeaseMs = 30000;
		constexpr float kFramesPerSecond = 30.0F;
		constexpr std::array<std::string_view, 4> kTransitionIDs{
			"head-to-held", "held-to-stowed", "stowed-to-held", "held-to-head"
		};

		struct Transition
		{
			std::string id;
			std::string file;
			State from{};
			State to{};
			std::uint32_t eventFrame{};
			std::vector<std::uint8_t> bytes;
		};

		struct HelmetRequest
		{
			Suit::Setup setup;
			Props::Attachment held;
			std::string selectionKind;
			std::string selectionID;
			std::unordered_map<std::string, Transition> transitions;
		};

		struct HelmetPlayback
		{
			bool active{};
			std::string source;
			Suit::API const* suit{};
			Props::Attachment heldAttachment;
			Props::Instance heldProp;
			std::vector<Transition> sequence;
			std::size_t segment{};
			bool eventApplied{};
			double expiresAt{};
		};

		std::mutex g_lifecycleMutex;
		std::mutex g_replyMutex;
		bool g_initialized = false;  // lifecycle mutex
		bool g_started = false;
		bool g_tickInstalled = false;  // lifecycle mutex
		std::atomic_bool g_running{ false };
		std::atomic<std::uint64_t> g_generation{ 0 };
		std::filesystem::path g_directory;
		std::string g_session;
		bool g_rawPreviewActive = false;  // game thread only
		std::string g_rawPreviewSource;   // game thread only
		HelmetPlayback g_helmet;          // game thread only
		// Declared last so its destructor requests stop and joins while every object the monitor
		// can touch is still alive during process shutdown.
		std::jthread g_monitor;

		double NowSeconds()
		{
			return std::chrono::duration<double>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		std::filesystem::path LinkDirectory()
		{
			try {
				if (auto directory = SFSE::log::log_directory()) {
					return directory->parent_path() / "OSF" / "Studio Link";
				}
			} catch (...) {}
			return {};
		}

		std::string NewSessionToken()
		{
			const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
			try {
				std::random_device random;
				return std::format("{:016x}{:08x}{:08x}",
					static_cast<std::uint64_t>(now), random(), random());
			} catch (...) {
				return std::format("{:016x}{:016x}", static_cast<std::uint64_t>(now),
					reinterpret_cast<std::uintptr_t>(&g_started));
			}
		}

		bool WriteJson(const std::filesystem::path& a_path, const json& a_value)
		{
			try {
				std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
				if (out) {
					out << a_value.dump(2) << '\n';
					out.flush();
					return static_cast<bool>(out);
				}
			} catch (...) {}
			return false;
		}

		void Reply(std::string_view a_id, bool a_ok, std::string_view a_message)
		{
			// Replies come from both the monitor thread (parse failures) and SFSE game-thread tasks.
			// Serialize writers, and publish atomically — write a temp file, then rename over
			// response.json — so Studio never reads a truncated or interleaved document.
			const std::scoped_lock lock{ g_replyMutex };
			const auto target = g_directory / "response.json";
			const auto tmp = g_directory / "response.json.tmp";
			if (WriteJson(tmp, json{
					{ "version", kProtocolVersion }, { "session", g_session },
					{ "id", a_id }, { "ok", a_ok }, { "message", a_message }
				})) {
				std::error_code ec;
				std::filesystem::rename(tmp, target, ec);
				if (ec) {
					std::filesystem::remove(tmp, ec);
				}
			}
		}

		std::optional<std::vector<std::uint8_t>> ReadClip(
			const std::string& a_name, std::string& a_error)
		{
			const std::filesystem::path relative{ a_name };
			if (relative.empty() || relative.has_parent_path() || relative.has_root_name() ||
				relative.has_root_directory() || relative.extension() != ".af") {
				a_error = "The preview request contained an invalid clip filename";
				return std::nullopt;
			}
			const auto file = g_directory / relative;
			std::error_code ec;
			const auto size = std::filesystem::file_size(file, ec);
			if (ec || size == 0 || size > Util::kMaxClipBytes) {
				a_error = "The preview clip is missing, empty, or exceeds the 32 MiB limit";
				return std::nullopt;
			}
			if (!std::filesystem::is_regular_file(file, ec) || ec ||
				std::filesystem::is_symlink(file, ec) || ec) {
				a_error = "The preview clip must be a regular file inside Studio Link";
				return std::nullopt;
			}
			std::ifstream in(file, std::ios::binary);
			if (!in) {
				a_error = "The preview clip could not be opened";
				return std::nullopt;
			}
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
			if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
				a_error = "The preview clip could not be read completely";
				return std::nullopt;
			}
			return bytes;
		}

		bool ReadAttachment(const json& a_value, Props::Attachment& a_out, std::string& a_error)
		{
			if (!a_value.is_object() || !a_value.contains("bone") ||
				!a_value["bone"].is_string()) {
				a_error = "A helmet attachment is missing its bone";
				return false;
			}
			a_out.node = a_value["bone"].get<std::string>();
			if (a_out.node.empty() || a_out.node.size() >= 64) {
				a_error = "A helmet attachment bone is empty or exceeds 63 characters";
				return false;
			}
			auto readTriple = [&](std::string_view a_key, std::array<float, 3>& a_target) {
				const auto iter = a_value.find(a_key);
				if (iter == a_value.end() || !iter->is_array() || iter->size() != 3) return false;
				for (std::size_t i = 0; i < 3; ++i) {
					if (!(*iter)[i].is_number()) return false;
					a_target[i] = (*iter)[i].get<float>();
					if (!std::isfinite(a_target[i])) return false;
				}
				return true;
			};
			if (!readTriple("translation", a_out.position) ||
				!readTriple("rotationDegrees", a_out.rotation) ||
				!a_value.contains("scale") || !a_value["scale"].is_number()) {
				a_error = "A helmet attachment has invalid transform values";
				return false;
			}
			a_out.scale = a_value["scale"].get<float>();
			if (!std::isfinite(a_out.scale) || a_out.scale <= 0.0F || a_out.scale > 10.0F) {
				a_error = "A helmet attachment scale must be within (0, 10]";
				return false;
			}
			return true;
		}

		Suit::Attachment ToSuitAttachment(const Props::Attachment& a_source)
		{
			Suit::Attachment result;
			std::memcpy(result.node, a_source.node.data(), a_source.node.size());
			std::copy(a_source.position.begin(), a_source.position.end(), result.position);
			std::copy(a_source.rotation.begin(), a_source.rotation.end(), result.rotation);
			result.scale = a_source.scale;
			return result;
		}

		std::optional<State> StateFromID(std::string_view a_id)
		{
			if (a_id == "equipped") return State::kEquipped;
			if (a_id == "held") return State::kHeld;
			if (a_id == "stowed") return State::kStowed;
			return std::nullopt;
		}

		std::optional<HelmetRequest> ParseHelmetRequest(const json& a_request, std::string& a_error)
		{
			if (!a_request.contains("setup") || !a_request["setup"].is_object() ||
				!a_request.contains("selection") || !a_request["selection"].is_object() ||
				!a_request.contains("files") || !a_request["files"].is_object()) {
				a_error = "Helmet preview requires a setup, selection, and transient AF files";
				return std::nullopt;
			}
			const auto& setup = a_request["setup"];
			if (setup.value("schemaVersion", 0u) != 1 || !setup.contains("runtime") ||
				!setup["runtime"].is_object() || !setup.contains("transitions") ||
				!setup["transitions"].is_object()) {
				a_error = "OSF Animation supports HelmetSetupV1 only";
				return std::nullopt;
			}

			HelmetRequest result;
			Props::Attachment stowed;
			Props::Attachment handoff;
			const auto& runtime = setup["runtime"];
			if (!ReadAttachment(runtime.value("held", json{}), result.held, a_error) ||
				!ReadAttachment(runtime.value("stowed", json{}), stowed, a_error) ||
				!ReadAttachment(runtime.value("handoff", json{}), handoff, a_error)) {
				return std::nullopt;
			}
			result.setup.stowed = ToSuitAttachment(stowed);
			result.setup.handoff = ToSuitAttachment(handoff);
			result.setup.handoffHoldMs = (std::min)(runtime.value("holdMs", 0u), 10000u);
			result.setup.handoffSettleMs = (std::min)(runtime.value("settleMs", 0u), 10000u);
			result.setup.leaseMs = (std::clamp)(a_request.value("leaseMs", kLeaseMs), 1000u, kLeaseMs);

			const auto& files = a_request["files"];
			const auto& transitions = setup["transitions"];
			for (const auto id : kTransitionIDs) {
				const auto key = std::string{ id };
				if (!transitions.contains(key) || !transitions[key].is_object() ||
					!files.contains(key) || !files[key].is_string()) {
					a_error = "Helmet preview is missing transition " + key;
					return std::nullopt;
				}
				const auto& value = transitions[key];
				const auto from = StateFromID(value.value("from", std::string{}));
				const auto to = StateFromID(value.value("to", std::string{}));
				const auto frame = value.value("eventFrame", -1);
				if (!from || !to || frame < 0 || frame > 100000) {
					a_error = "Helmet transition " + key + " has invalid endpoints or event frame";
					return std::nullopt;
				}
				result.transitions.emplace(key, Transition{
					.id = key, .file = files[key].get<std::string>(), .from = *from, .to = *to,
					.eventFrame = static_cast<std::uint32_t>(frame)
				});
			}
			const auto& selection = a_request["selection"];
			result.selectionKind = selection.value("kind", std::string{});
			result.selectionID = selection.value("id", std::string{});
			if (result.selectionKind != "state" && result.selectionKind != "transition" &&
				result.selectionKind != "chain") {
				a_error = "Helmet preview selection kind is invalid";
				return std::nullopt;
			}

			std::vector<std::string> required;
			const auto require = [&](std::string_view a_id) {
				const std::string id{ a_id };
				if (std::ranges::find(required, id) == required.end()) {
					required.push_back(id);
				}
			};
			if (result.selectionKind == "state") {
				const auto state = StateFromID(result.selectionID);
				if (!state) {
					a_error = "Unknown helmet state " + result.selectionID;
					return std::nullopt;
				}
				require(*state == State::kStowed ? "held-to-stowed" : "head-to-held");
			} else if (result.selectionKind == "transition") {
				if (!result.transitions.contains(result.selectionID)) {
					a_error = "Unknown helmet transition " + result.selectionID;
					return std::nullopt;
				}
				require(result.selectionID);
			} else if (result.selectionID == "equip") {
				require("stowed-to-held");
				require("held-to-head");
			} else if (result.selectionID == "unequip") {
				require("head-to-held");
				require("held-to-stowed");
			} else {
				a_error = "Unknown helmet chain " + result.selectionID;
				return std::nullopt;
			}

			// Read only the one or two clips this request will actually play. The setup still
			// validates all transition metadata, but no longer allocates up to four 32 MiB buffers.
			for (const auto& id : required) {
				auto& transition = result.transitions.at(id);
				auto bytes = ReadClip(transition.file, a_error);
				if (!bytes) {
					return std::nullopt;
				}
				transition.bytes = std::move(*bytes);
			}
			return result;
		}

		bool SuitCall(bool a_result, const char* a_error, std::string& a_message)
		{
			if (a_result) return true;
			a_message = a_error[0] ? a_error : "Suit Protocol rejected the helmet preview";
			return false;
		}

		void DestroyHeld()
		{
			if (g_helmet.heldProp.Empty()) return;
			std::string ignored;
			(void)Props::PropService::GetSingleton().Destroy(g_helmet.heldProp, &ignored);
		}

		bool ApplyState(State a_state, RE::Actor* a_player, std::string& a_error)
		{
			char suitError[256]{};
			if (a_state == State::kHeld && g_helmet.heldProp.Empty()) {
				Props::Source source{
					.kind = Props::SourceKind::kEquippedArmor,
					.keywords = { "ArmorTypeSpacesuitHelmet", "ArmorTypeHelmet" }
				};
				g_helmet.heldProp = Props::PropService::GetSingleton().CreateAttached(
					a_player, source, g_helmet.heldAttachment, &a_error);
				if (g_helmet.heldProp.Empty()) return false;
			}
			if (!SuitCall(g_helmet.suit->ApplyState(a_state, suitError, sizeof(suitError)),
				suitError, a_error)) {
				return false;
			}
			if (a_state != State::kHeld) DestroyHeld();
			return true;
		}

		const Transition& RequireTransition(const HelmetRequest& a_request, std::string_view a_id)
		{
			return a_request.transitions.at(std::string{ a_id });
		}

		bool StartSegment(RE::Actor* a_player, std::string& a_error)
		{
			if (g_helmet.segment >= g_helmet.sequence.size()) return false;
			const auto& segment = g_helmet.sequence[g_helmet.segment];
			if (!ApplyState(segment.from, a_player, a_error)) return false;
			auto& manager = Animation::GraphManager::GetSingleton();
			// Commit the source key only once the play succeeded: StopHelmetPreview only stops the
			// graph when the CURRENT animation matches g_helmet.source, so committing before a failed
			// play would leave the previous (still-playing) segment unstoppable.
			const std::string source = "studio-helmet:" + segment.id;
			if (!manager.PlayAnimationBytes(a_player, segment.bytes, source, &a_error)) {
				return false;
			}
			g_helmet.source = source;
			manager.SetSpeed(a_player, 1.0F);
			manager.SetAnimationHoldAtEnd(a_player, true);
			const auto playback = manager.GetAnimationPlayback(a_player);
			if (!playback || static_cast<float>(segment.eventFrame) / kFramesPerSecond >
				playback->duration + 0.0001F) {
				a_error = "Ownership event frame exceeds transition " + segment.id + " duration";
				return false;
			}
			g_helmet.eventApplied = segment.eventFrame == 0;
			if (g_helmet.eventApplied && !ApplyState(segment.to, a_player, a_error)) return false;
			return true;
		}

		bool HoldState(const HelmetRequest& a_request, State a_state, RE::Actor* a_player, std::string& a_error)
		{
			const Transition* poseClip{};
			bool atEnd{};
			switch (a_state) {
			case State::kEquipped: poseClip = &RequireTransition(a_request, "head-to-held"); break;
			case State::kHeld: poseClip = &RequireTransition(a_request, "head-to-held"); atEnd = true; break;
			case State::kStowed: poseClip = &RequireTransition(a_request, "held-to-stowed"); atEnd = true; break;
			}
			if (!ApplyState(a_state, a_player, a_error)) return false;
			auto& manager = Animation::GraphManager::GetSingleton();
			// Same commit-on-success ordering as StartSegment.
			const std::string source = "studio-helmet:state";
			if (!manager.PlayAnimationBytes(a_player, poseClip->bytes, source, &a_error)) return false;
			g_helmet.source = source;
			manager.SetAnimationHoldAtEnd(a_player, true);
			const auto playback = manager.GetAnimationPlayback(a_player);
			if (!playback) return false;
			manager.SetAnimationTime(a_player, atEnd ? playback->duration : 0.0F);
			manager.SetSpeed(a_player, 0.0F);
			return true;
		}

		void StopHelmetPreview()
		{
			if (!g_helmet.active) return;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player) {
				auto& manager = Animation::GraphManager::GetSingleton();
				if (manager.GetCurrentAnimation(player) == g_helmet.source) manager.StopAnimation(player);
			}
			DestroyHeld();
			if (g_helmet.suit) g_helmet.suit->Stop();
			g_helmet = {};
		}

		void FailHelmetPreview(std::string_view a_reason)
		{
			REX::WARN("[Anim] Studio helmet preview stopped: {}", a_reason);
			StopHelmetPreview();
		}

		void TickHelmetPreview()
		{
			if (!g_helmet.active) return;
			if (NowSeconds() >= g_helmet.expiresAt) {
				return FailHelmetPreview("30-second lease expired");
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player) return FailHelmetPreview("player became unavailable");
			if (g_helmet.sequence.empty()) return;
			auto& manager = Animation::GraphManager::GetSingleton();
			if (manager.GetCurrentAnimation(player) != g_helmet.source) {
				return FailHelmetPreview("another OSF animation replaced it");
			}
			const auto playback = manager.GetAnimationPlayback(player);
			if (!playback) return FailHelmetPreview("clip playback disappeared");
			const auto& segment = g_helmet.sequence[g_helmet.segment];
			std::string error;
			if (!g_helmet.eventApplied && playback->time >=
				static_cast<float>(segment.eventFrame) / kFramesPerSecond) {
				if (!ApplyState(segment.to, player, error)) return FailHelmetPreview(error);
				g_helmet.eventApplied = true;
			}
			if (playback->time + 0.0001F < playback->duration) return;
			if (!g_helmet.eventApplied && !ApplyState(segment.to, player, error)) {
				return FailHelmetPreview(error);
			}
			if (++g_helmet.segment < g_helmet.sequence.size()) {
				if (!StartSegment(player, error)) FailHelmetPreview(error);
				return;
			}
			manager.SetSpeed(player, 0.0F);
			g_helmet.eventApplied = true;
		}

		class PreviewTickTask final : public SFSE::ITaskDelegate
		{
		public:
			// Permanent tasks already run on the game thread inside the same drain as the transient
			// queue. It cannot be unregistered, so install it once and make it inert while the
			// runtime-switchable service is disabled.
			void Run() override
			{
				if (g_running.load(std::memory_order_acquire)) TickHelmetPreview();
			}
			void Destroy() override {}
		};

		bool IsCurrentGeneration(std::uint64_t a_generation)
		{
			return g_running.load(std::memory_order_acquire) &&
			       a_generation == g_generation.load(std::memory_order_acquire);
		}

		void QueuePing(std::string id, std::uint64_t generation)
		{
			SFSE::GetTaskInterface()->AddTask([id = std::move(id), generation]() {
				if (IsCurrentGeneration(generation)) Reply(id, true, "OSF Animation is ready");
			});
		}

		void QueueStop(std::string id, std::uint64_t generation, bool a_helmet)
		{
			SFSE::GetTaskInterface()->AddTask([id = std::move(id), generation, a_helmet]() {
				if (!IsCurrentGeneration(generation)) return;
				if (a_helmet) StopHelmetPreview();
				else {
					auto* player = RE::PlayerCharacter::GetSingleton();
					auto& manager = Animation::GraphManager::GetSingleton();
					if (g_rawPreviewActive && player && manager.GetCurrentAnimation(player) == g_rawPreviewSource) manager.StopAnimation(player);
					g_rawPreviewActive = false;
					g_rawPreviewSource.clear();
				}
				Reply(id, true, a_helmet ? "Helmet preview stopped and restored" : "Studio preview stopped");
			});
		}

		void QueuePlay(std::string id, std::vector<std::uint8_t> bytes, std::uint64_t generation)
		{
			SFSE::GetTaskInterface()->AddTask([id = std::move(id), bytes = std::move(bytes), generation]() mutable {
				if (!IsCurrentGeneration(generation)) return;
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player) return Reply(id, false, "The player is not available in the loaded world");
				auto& manager = Animation::GraphManager::GetSingleton();
				if (g_rawPreviewActive && manager.GetCurrentAnimation(player) != g_rawPreviewSource) {
					g_rawPreviewActive = false;
					g_rawPreviewSource.clear();
				}
				// A just-stopped Studio graph keeps IsPlaying true through its fade-out ramp (and
				// indefinitely while the game is paused) — only a graph this service did NOT start
				// counts as foreign ownership. A live helmet preview is ours too: retire it (suit
				// lease, prop) before the raw clip takes the player.
				if (!g_rawPreviewActive && manager.IsPlaying(player) &&
					!manager.GetCurrentAnimation(player).starts_with("studio-")) {
					return Reply(id, false, "The player is already playing a non-Studio OSF animation");
				}
				StopHelmetPreview();
				const std::string source = "studio-preview:" + id;
				std::string error;
				if (!manager.PlayAnimationBytes(player, bytes, source, &error)) return Reply(id, false, error);
				manager.SetSpeed(player, 1.0F);
				Camera::CameraService::GetSingleton().KickToThirdPerson();
				g_rawPreviewActive = true;
				g_rawPreviewSource = source;
				Reply(id, true, "Playing in game at normal speed");
			});
		}

		void QueueHelmet(std::string id, HelmetRequest request, std::uint64_t generation)
		{
			SFSE::GetTaskInterface()->AddTask([id = std::move(id), request = std::move(request), generation]() mutable {
				if (!IsCurrentGeneration(generation)) return;
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player) return Reply(id, false, "The player is not available in the loaded world");
				auto& manager = Animation::GraphManager::GetSingleton();
				if (g_rawPreviewActive && manager.GetCurrentAnimation(player) != g_rawPreviewSource) {
					g_rawPreviewActive = false;
					g_rawPreviewSource.clear();
				}
				if (g_helmet.active && manager.GetCurrentAnimation(player) != g_helmet.source) {
					StopHelmetPreview();
				}
				// Same own-fade carve-out as QueuePlay: a fading "studio-*" graph is ours, not foreign.
				if ((!g_helmet.active && manager.IsPlaying(player) &&
						!manager.GetCurrentAnimation(player).starts_with("studio-")) ||
					g_rawPreviewActive) {
					return Reply(id, false, "The player is already playing a non-Studio OSF animation");
				}
				StopHelmetPreview();
				const auto* suit = Suit::Acquire();
				if (!suit) return Reply(id, false, "Suit Protocol 0.1.14+ with helmet-preview API v1 is missing");
				char suitError[256]{};
				if (!suit->Begin(&request.setup, suitError, sizeof(suitError))) {
					return Reply(id, false, suitError[0] ? suitError : "Suit Protocol rejected the preview lease");
				}
				g_helmet.active = true;
				g_helmet.suit = suit;
				g_helmet.heldAttachment = request.held;
				g_helmet.expiresAt = NowSeconds() + static_cast<double>(request.setup.leaseMs) / 1000.0;
				std::string error;
				bool started{};
				if (request.selectionKind == "state") {
					const auto state = StateFromID(request.selectionID);
					if (!state) error = "Unknown helmet state " + request.selectionID;
					else started = HoldState(request, *state, player, error);
				} else {
					if (request.selectionKind == "transition") {
						const auto found = request.transitions.find(request.selectionID);
						if (found != request.transitions.end()) g_helmet.sequence.push_back(std::move(found->second));
					} else if (request.selectionID == "equip") {
						g_helmet.sequence.push_back(std::move(request.transitions.at("stowed-to-held")));
						g_helmet.sequence.push_back(std::move(request.transitions.at("held-to-head")));
					} else if (request.selectionID == "unequip") {
						g_helmet.sequence.push_back(std::move(request.transitions.at("head-to-held")));
						g_helmet.sequence.push_back(std::move(request.transitions.at("held-to-stowed")));
					}
					if (g_helmet.sequence.empty()) error = "Unknown helmet transition or chain " + request.selectionID;
					else started = StartSegment(player, error);
				}
				if (!started) {
					StopHelmetPreview();
					return Reply(id, false, error.empty() ? "OSF Animation could not start the helmet preview" : error);
				}
				Camera::CameraService::GetSingleton().KickToThirdPerson();
				Reply(id, true, "Helmet preview sent at authored 1x speed (30-second lease)");
			});
		}

		void ProcessRequest(const json& a_request)
		{
			if (!g_running.load(std::memory_order_acquire)) return;
			// The caller already type-checked "id"; everything else is untrusted. A wrong-typed
			// field throws out of value()/at() — reply with the parse failure instead of letting
			// it escape to the monitor's catch-all, which would drop the request without a reply.
			const std::string id = a_request.value("id", std::string{});
			if (id.empty() || id.size() > 128) return;
			try {
				if (!a_request.is_object() || a_request.value("version", 0u) != kProtocolVersion ||
					a_request.value("session", std::string{}) != g_session) return;
				const std::string command = a_request.value("command", std::string{});
				const auto generation = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
				if (command == "ping") return QueuePing(id, generation);
				if (command == "stop") return QueueStop(id, generation, false);
				if (command == "helmet.stop") return QueueStop(id, generation, true);
				if (command == "play") {
					std::string error;
					auto bytes = ReadClip(a_request.value("clip", std::string{}), error);
					if (!bytes) return Reply(id, false, error);
					return QueuePlay(id, std::move(*bytes), generation);
				}
				if (command == "helmet.preview") {
					std::string error;
					auto request = ParseHelmetRequest(a_request, error);
					if (!request) return Reply(id, false, error);
					return QueueHelmet(id, std::move(*request), generation);
				}
				Reply(id, false, "Unknown Studio Link command");
			} catch (const json::exception& e) {
				Reply(id, false, std::string{ "Malformed Studio Link request: " } + e.what());
			}
		}

		bool HasSession(const std::filesystem::path& a_file, std::string_view a_session) noexcept
		{
			if (a_session.empty()) return true;
			try {
				std::error_code ec;
				const auto size = std::filesystem::file_size(a_file, ec);
				if (ec || size == 0 || size > kMaxRequestBytes) return false;
				std::ifstream in(a_file, std::ios::binary);
				const json value = json::parse(in, nullptr, false);
				const auto session = value.is_object() ? value.find("session") : value.end();
				return session != value.end() && session->is_string() &&
				       session->get_ref<const std::string&>() == a_session;
			} catch (...) {
				return false;
			}
		}

		void CleanupInbox(const std::filesystem::path& a_directory, std::string_view a_session) noexcept
		{
			if (a_directory.empty()) return;
			try {
				// session.json is the readiness sentinel, so retire it first. Preserve files if a
				// different process/session replaced them while this instance was shutting down.
				for (const auto* name : { "session.json", "request.json", "response.json", "response.json.tmp" }) {
					const auto file = a_directory / name;
					std::error_code ec;
					if (a_session.empty()) {
						// Startup owns these protocol filenames and must also clear a dangling symlink.
						std::filesystem::remove(file, ec);
					} else if (std::filesystem::is_regular_file(file, ec) && !ec && HasSession(file, a_session)) {
						std::filesystem::remove(file, ec);
					}
				}
			} catch (...) {}
		}

		void Monitor(std::stop_token a_stop, std::filesystem::path a_directory, std::string a_session)
		{
			// std::jthread also requests stop from its destructor. Close admission in that
			// process-shutdown path, where there is no game-thread settings callback to do it.
			std::stop_callback closeAdmission{ a_stop, [] {
				g_running.store(false, std::memory_order_release);
			} };
			std::mutex waitMutex;
			std::condition_variable_any wait;
			std::string lastId;
			while (!a_stop.stop_requested()) {
				try {
					const auto requestFile = a_directory / "request.json";
					std::error_code ec;
					const auto size = std::filesystem::file_size(requestFile, ec);
					if (!ec && size > 0 && size <= kMaxRequestBytes) {
						std::ifstream in(requestFile, std::ios::binary);
						const json request = json::parse(in, nullptr, false);
						// Type-guarded id read: value() throws on a present-but-non-string "id", and an
						// escape here would skip the lastId latch below — re-reading the same file into
						// the same throw at 10 Hz forever, with no reply ever written.
						const auto idIt = request.is_object() ? request.find("id") : request.end();
						const std::string id = idIt != request.end() && idIt->is_string() ? idIt->get<std::string>() : std::string{};
						if (!id.empty() && id != lastId) { lastId = id; ProcessRequest(request); }
					}
				} catch (...) {
					if (!a_stop.stop_requested()) {
						REX::WARN("[Anim] Studio Link request monitor recovered from a filesystem error");
					}
				}
				if (!a_stop.stop_requested()) {
					std::unique_lock waitLock{ waitMutex };
					(void)wait.wait_for(waitLock, a_stop, std::chrono::milliseconds(100), [] {
						return false;
					});
				}
			}
			CleanupInbox(a_directory, a_session);
		}

		void StopRawPreview()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto& manager = Animation::GraphManager::GetSingleton();
			if (g_rawPreviewActive && player && manager.GetCurrentAnimation(player) == g_rawPreviewSource) {
				manager.StopAnimation(player);
			}
			g_rawPreviewActive = false;
			g_rawPreviewSource.clear();
		}

		void StopPreviewServiceLocked()
		{
			if (!g_started) return;
			// Close admission before invalidating the queue. A request already being parsed may
			// still enqueue, but it cannot pass both the running and generation checks, including
			// after a later restart (which advances the generation again).
			g_running.store(false, std::memory_order_release);
			g_generation.fetch_add(1, std::memory_order_acq_rel);
			g_monitor.request_stop();
			if (g_monitor.joinable()) g_monitor.join();

			// Setting callbacks and permanent tasks run on the game thread. Keep every engine/RE
			// operation here; the monitor only performs file IO and queues work.
			StopHelmetPreview();
			StopRawPreview();
			g_started = false;
			g_directory.clear();
			g_session.clear();
			REX::DEBUG("[Anim] Studio Link disabled");
		}

		void StartPreviewServiceLocked()
		{
			if (g_started) return;
			g_directory = LinkDirectory();
			if (g_directory.empty()) {
				REX::WARN("[Anim] Studio Link unavailable: SFSE user-data directory could not be resolved");
				return;
			}
			std::error_code ec;
			std::filesystem::create_directories(g_directory, ec);
			if (ec) {
				REX::WARN("[Anim] Studio Link unavailable: '{}' could not be created ({})", g_directory.string(), ec.message());
				g_directory.clear();
				return;
			}
			// A stopped or crashed prior session must not look live, and an old request id must
			// not become the new monitor's duplicate-suppression latch.
			CleanupInbox(g_directory, {});
			g_session = NewSessionToken();
			const bool helmetAvailable = Suit::Acquire() && Props::PropService::GetSingleton().Available();
			json session{
				{ "version", kProtocolVersion }, { "session", g_session }, { "game", "Starfield" },
				{ "runtime", "OSF Animation" },
				{ "capabilities", helmetAvailable ? json::array({ "raw-clip.v1", "helmet-preview.v1" }) : json::array({ "raw-clip.v1" }) },
				{ "components", {
					{ "osf-animation", "1.5.0 (helmet-preview API v1)" },
					{ "suit-protocol", helmetAvailable ? "0.1.14+ (helmet-preview API v1)" : "missing/outdated (requires 0.1.14+)" }
				} }
			};
			if (!WriteJson(g_directory / "session.json", session)) {
				REX::WARN("[Anim] Studio Link unavailable: session file could not be written");
				CleanupInbox(g_directory, {});
				g_directory.clear();
				g_session.clear();
				return;
			}
			if (!g_tickInstalled) {
				static PreviewTickTask tickTask;
				SFSE::GetTaskInterface()->AddPermanentTask(&tickTask);
				g_tickInstalled = true;
			}
			g_generation.fetch_add(1, std::memory_order_acq_rel);
			g_running.store(true, std::memory_order_release);
			try {
				g_monitor = std::jthread(Monitor, g_directory, g_session);
			} catch (const std::exception& e) {
				g_running.store(false, std::memory_order_release);
				g_generation.fetch_add(1, std::memory_order_acq_rel);
				CleanupInbox(g_directory, g_session);
				g_directory.clear();
				g_session.clear();
				REX::ERROR("[Anim] Studio Link monitor could not start: {}", e.what());
				return;
			}
			g_started = true;
			REX::INFO("[Anim] Studio Link v{} ready at '{}' (helmet preview {})", kProtocolVersion,
				g_directory.string(), helmetAvailable ? "available" : "unavailable");
		}
	}

	void SetPreviewServiceEnabled(bool a_enabled) noexcept
	{
		try {
			std::lock_guard lifecycleLock{ g_lifecycleMutex };
			if (!g_initialized) {
				g_initialized = true;
				// Default-off startup also clears the readiness marker left by an interrupted
				// previous game session, even when OSF UI is absent.
				CleanupInbox(LinkDirectory(), {});
			}
			if (a_enabled) StartPreviewServiceLocked();
			else StopPreviewServiceLocked();
		} catch (const std::exception& e) {
			REX::ERROR("[Anim] Studio Link lifecycle change failed: {}", e.what());
		} catch (...) {
			REX::ERROR("[Anim] Studio Link lifecycle change failed");
		}
	}
}
