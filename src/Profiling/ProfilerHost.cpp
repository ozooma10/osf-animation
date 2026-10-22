// Development-only: compiled into the profiling override build only (xmake --profiling=y).

#include "Profiling/ProfilerHost.h"

#include "Profiling/Profiler.h"
#include "Util/NativeMainThreadQueue.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace OSF::Profiling::Host
{
	namespace
	{
		using json = nlohmann::ordered_json;

		constexpr auto        kPollInterval = std::chrono::seconds(1);
		constexpr std::size_t kMaxCommandBytes = 64 * 1024;
		constexpr int         kMaxParseFailures = 3;

		struct State
		{
			std::mutex            lock;  // everything below; the writer thread updates lastDump
			std::filesystem::path dir;
			std::string           gameVersion;
			std::string           dllVersion;
			std::string           label = "session";
			std::string           captureStartedAt;
			bool                  autoDump = true;
			std::uint64_t         sequence = 0;
			int                   parseFailures = 0;
			json                  lastCommand = nullptr;
			json                  lastDump = nullptr;
			std::string           pendingLabel;
			std::string           pendingReason;
		};

		// Leaked on purpose: a detached writer may still finish after static destruction begins.
		State& Get()
		{
			static auto* state = new State;
			return *state;
		}

		std::atomic<bool> g_writing{ false };
		std::atomic<bool> g_dumpQueued{ false };

		std::string UtcNow()
		{
			return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
		}

		std::optional<Mode> ParseMode(std::string_view a_text)
		{
			if (a_text == "off") return Mode::kOff;
			if (a_text == "stats") return Mode::kStats;
			if (a_text == "trace") return Mode::kTrace;
			return std::nullopt;
		}

		// Caller holds state.lock.
		void WriteStatusLocked(State& a_state)
		{
			if (a_state.dir.empty()) {
				return;
			}
			const json status{
				{ "sequence", ++a_state.sequence },
				{ "build", kProfilingBuildMarker },
				{ "dllVersion", a_state.dllVersion },
				{ "gameVersion", a_state.gameVersion },
				{ "mode", ModeName(CurrentMode()) },
				{ "label", a_state.label },
				{ "captureStartedAt", a_state.captureStartedAt },
				{ "autoDump", a_state.autoDump },
				{ "ringEvents", RingCapacity() },
				{ "busy", g_writing.load() },
				{ "dir", a_state.dir.string() },
				{ "lastCommand", a_state.lastCommand },
				{ "lastDump", a_state.lastDump },
				{ "updatedAt", UtcNow() },
			};
			const auto      target = a_state.dir / "status.json";
			const auto      temp = a_state.dir / "status.json.tmp";
			std::error_code error;
			{
				std::ofstream file(temp, std::ios::binary | std::ios::trunc);
				file << status.dump(1, '\t');
				if (!file) {
					return;
				}
			}
			std::filesystem::rename(temp, target, error);
		}

		// Frame thread. Copies the window, optionally resets it, and serializes on a detached thread.
		bool Dump(std::string a_label, std::string a_reason, bool a_resetAfter)
		{
			auto& state = Get();
			if (g_writing.exchange(true)) {
				REX::WARN("[Profile] dump '{}' skipped: the previous dump is still being written", a_label);
				return false;
			}
			const auto started = std::chrono::steady_clock::now();
			Snapshot   snapshot;
			try {
				snapshot = Capture();
			} catch (const std::exception& e) {
				g_writing.store(false);
				REX::ERROR("[Profile] capture failed: {}", e.what());
				return false;
			}
			const double captureMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			REX::INFO("[Profile] snapshot '{}': {} threads, {} events in {:.1f} ms", a_label, snapshot.threads.size(), snapshot.events.size(), captureMs);
			if (a_resetAfter) {
				Reset();
				std::lock_guard lock{ state.lock };
				state.captureStartedAt = UtcNow();
			}

			DumpMeta meta;
			meta.label = std::move(a_label);
			meta.reason = std::move(a_reason);
			std::filesystem::path dir;
			{
				std::lock_guard lock{ state.lock };
				meta.dllVersion = state.dllVersion;
				meta.gameVersion = state.gameVersion;
				dir = state.dir;
			}
			if (dir.empty()) {
				g_writing.store(false);
				REX::WARN("[Profile] no profiles directory; dump discarded");
				return false;
			}
			try {
				std::thread([snapshot = std::move(snapshot), meta = std::move(meta), dir = std::move(dir)]() mutable {
					// Deliberately not registered with the profiler: one short-lived writer per dump.
					DumpResult result;
					try {
						result = Write(snapshot, dir, meta);
					} catch (const std::exception& e) {
						result.error = e.what();
					}
					for (const auto& line : result.summary) {
						REX::INFO("[Profile] {}", line);
					}
					if (!result.error.empty()) {
						REX::ERROR("[Profile] dump '{}' failed: {}", meta.label, result.error);
					}
					auto&           state = Get();
					std::lock_guard lock{ state.lock };
					state.lastDump = json{
						{ "label", meta.label },
						{ "reason", meta.reason },
						{ "stats", result.stats.empty() ? json(nullptr) : json(result.stats.string()) },
						{ "trace", result.trace.empty() ? json(nullptr) : json(result.trace.string()) },
						{ "error", result.error.empty() ? json(nullptr) : json(result.error) },
						{ "at", UtcNow() },
					};
					g_writing.store(false);
					WriteStatusLocked(state);
				}).detach();
			} catch (const std::exception& e) {
				g_writing.store(false);
				REX::ERROR("[Profile] could not start the dump writer: {}", e.what());
				return false;
			}
			return true;
		}

		json Execute(const json& a_command)
		{
			auto&             state = Get();
			const std::string command = a_command.value("command", "");
			const std::string label = a_command.value("label", "");
			json              outcome{ { "command", command }, { "at", UtcNow() }, { "ok", true }, { "error", nullptr } };
			const auto        fail = [&](std::string a_error) {
                outcome["ok"] = false;
                outcome["error"] = std::move(a_error);
			};

			if (a_command.contains("autoDump") && a_command["autoDump"].is_boolean()) {
				std::lock_guard lock{ state.lock };
				state.autoDump = a_command["autoDump"].get<bool>();
			}
			if (a_command.contains("ringEvents") && a_command["ringEvents"].is_number_unsigned()) {
				SetRingCapacity(a_command["ringEvents"].get<std::uint32_t>());
			}

			if (command == "start") {
				const auto mode = ParseMode(a_command.value("mode", "stats"));
				if (!mode) {
					fail("mode must be off, stats or trace");
				} else {
					Reset();
					SetMode(*mode);
					std::lock_guard lock{ state.lock };
					state.label = label.empty() ? std::string(ModeName(*mode)) : label;
					state.captureStartedAt = UtcNow();
					REX::INFO("[Profile] capture started ({}) '{}', ring {} events/thread", ModeName(*mode), state.label, RingCapacity());
				}
			} else if (command == "stop") {
				SetMode(Mode::kOff);
				REX::INFO("[Profile] capture stopped; data is kept until the next start or reset");
			} else if (command == "dump") {
				std::string name;
				{
					std::lock_guard lock{ state.lock };
					name = label.empty() ? state.label : label;
				}
				if (!Dump(name, "command", a_command.value("reset", false))) {
					fail(g_writing.load() ? "busy: a dump is still being written" : "dump failed; see the SFSE log");
				}
			} else if (command == "mark") {
				Mark(label.empty() ? std::string("mark") : label);
			} else if (command == "reset") {
				Reset();
				std::lock_guard lock{ state.lock };
				state.captureStartedAt = UtcNow();
			} else if (command != "status") {
				fail("unknown command '" + command + "'");
			}
			if (!outcome["ok"].get<bool>()) {
				REX::WARN("[Profile] command '{}' rejected: {}", command, outcome["error"].get<std::string>());
			}
			return outcome;
		}

		void QueueAutoDump(std::string a_label, std::string a_reason, bool a_override)
		{
			auto& state = Get();
			{
				std::lock_guard lock{ state.lock };
				if (!state.autoDump || CurrentMode() == Mode::kOff) {
					return;
				}
				// One dump per frame: a world reset ends every scene, and its label wins.
				if (a_override || state.pendingLabel.empty()) {
					state.pendingLabel = std::move(a_label);
					state.pendingReason = std::move(a_reason);
				}
			}
			if (g_dumpQueued.exchange(true)) {
				return;
			}
			Util::NativeMainThreadQueue::Post([] {
				auto&       state = Get();
				std::string label;
				std::string reason;
				{
					std::lock_guard lock{ state.lock };
					label = std::move(state.pendingLabel);
					reason = std::move(state.pendingReason);
					state.pendingLabel.clear();
					state.pendingReason.clear();
				}
				g_dumpQueued.store(false);
				if (!label.empty()) {
					Dump(std::move(label), std::move(reason), true);
				}
			});
		}
	}

	void Initialize(std::string_view a_gameVersion)
	{
		auto& state = Get();
		{
			std::lock_guard lock{ state.lock };
			state.gameVersion = std::string(a_gameVersion);
			state.dllVersion = SFSE::GetPluginVersion().string();
			if (const auto logs = SFSE::log::log_directory()) {
				state.dir = logs->parent_path() / "OSF" / "profiles";
				std::error_code error;
				std::filesystem::create_directories(state.dir, error);
				// A command left behind by an earlier session must not act on this one.
				if (std::filesystem::remove(state.dir / "command.json", error)) {
					REX::WARN("[Profile] removed a stale command.json from an earlier session");
				}
			}
		}
		REX::WARN("[Profile] {}: development-only profiler build of OSF Animation. Never ship or package this DLL.", kProfilingBuildMarker);
		Reset();
		SetMode(Mode::kStats);
		{
			std::lock_guard lock{ state.lock };
			state.captureStartedAt = UtcNow();
			REX::INFO("[Profile] capture started (stats) at load; commands go to {}", (state.dir / "command.json").string());
			WriteStatusLocked(state);
		}
	}

	void Poll() noexcept
	{
		try {
			static auto last = std::chrono::steady_clock::time_point{};
			const auto  now = std::chrono::steady_clock::now();
			if (now - last < kPollInterval) {
				return;
			}
			last = now;

			auto&                 state = Get();
			std::filesystem::path path;
			{
				std::lock_guard lock{ state.lock };
				if (state.dir.empty()) {
					return;
				}
				path = state.dir / "command.json";
			}
			std::error_code error;
			const auto      size = std::filesystem::file_size(path, error);
			if (error) {
				return;  // no command waiting
			}
			std::string text;
			if (size <= kMaxCommandBytes) {
				std::ifstream file(path, std::ios::binary);
				text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			}
			const json command = json::parse(text, nullptr, false);
			if (command.is_discarded() || !command.is_object()) {
				// Possibly still being written; give up after a few polls.
				std::lock_guard lock{ state.lock };
				if (++state.parseFailures >= kMaxParseFailures) {
					state.parseFailures = 0;
					std::filesystem::rename(path, state.dir / "command.rejected.json", error);
					state.lastCommand = json{ { "command", nullptr }, { "at", UtcNow() }, { "ok", false }, { "error", "command.json is not a JSON object" } };
					REX::WARN("[Profile] command.json is not a JSON object; moved to command.rejected.json");
					WriteStatusLocked(state);
				}
				return;
			}
			std::filesystem::remove(path, error);
			{
				std::lock_guard lock{ state.lock };
				state.parseFailures = 0;
			}
			const json outcome = Execute(command);
			std::lock_guard lock{ state.lock };
			state.lastCommand = outcome;
			WriteStatusLocked(state);
		} catch (const std::exception& e) {
			REX::ERROR("[Profile] command poll failed: {}", e.what());
		} catch (...) {
			REX::ERROR("[Profile] command poll failed");
		}
	}

	void OnSceneEnd(std::string_view a_sceneId) noexcept
	{
		try {
			QueueAutoDump("scene-end-" + std::string(a_sceneId), "scene-end", false);
		} catch (...) {
		}
	}

	void OnWorldReset(std::string_view a_reason) noexcept
	{
		try {
			QueueAutoDump("world-reset-" + std::string(a_reason), "world-reset", true);
		} catch (...) {
		}
	}
}
