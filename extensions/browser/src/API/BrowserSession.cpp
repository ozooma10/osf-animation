#include "API/BrowserSession.h"
#include "API/EngineClient.h"
#include "API/BrowserSessionQueue.h"
#include "API/BrowserHealth.h"

#include <atomic>
#include <filesystem>
#include <list>
#include <string>
#include <string_view>

namespace OSF::API::BrowserSession
{
	namespace
	{
		OSFUI::API::Client g_views;
		BrowserSessionQueue g_queue([](std::function<void()> task) { EngineClient::Post(std::move(task)); });
		std::atomic<bool> g_available{};
		const char* g_reason = "OSF Animation Browser is not installed.";
		void (*g_shown)(){};
		void (*g_hidden)(){};
		OSFUI::API::RelativePointerFn g_pointer{};
		bool g_registrationFailed{};
		struct Handler { OSFUI::API::SendFn callback; bool requiresVisible; };
		std::list<Handler> g_handlers; // SDK callback contexts live until process exit.

		void Fail(const char* a_reason)
		{
			g_reason = a_reason;
			g_available = false;
			Health::Report("browser.integration", "browser.integration", Health::Severity::kError, a_reason);
			REX::ERROR("[UI] {}", a_reason);
		}

		void OnLifecycle(const char* a_view, OSFUI::API::ViewLifecyclePhase a_phase, void*) noexcept
		{
			if (!a_view || std::string_view(a_view) != kViewId) return;
			try {
				if (a_phase == OSFUI::API::ViewLifecyclePhase::kShown) g_queue.Visibility(true, g_shown);
				else if (a_phase == OSFUI::API::ViewLifecyclePhase::kHidden) g_queue.Visibility(false, g_hidden);
			} catch (...) { REX::ERROR("[UI] Could not queue browser lifecycle notification"); }
		}

		void OnSend(const char* a_name, const char* a_payload, const char* a_source, void* a_context) noexcept
		{
			if (!g_available || !a_source || std::string_view(a_source) != kViewId) return;
			try {
				const auto handler = *static_cast<Handler*>(a_context);
				g_queue.Post([handler, name = std::string(a_name), payload = std::string(a_payload ? a_payload : "{}"), source = std::string(a_source)] {
					handler.callback(name.c_str(), payload.c_str(), source.c_str(), nullptr);
				}, handler.requiresVisible);
			} catch (...) { REX::ERROR("[UI] Could not queue browser command"); }
		}

		void OnPointer(const char* a_view, OSFUI::API::RelativePointerPhase a_phase,
			float a_dx, float a_dy, float a_wheel, void*) noexcept
		{
			if (!g_available || !a_view || std::string_view(a_view) != kViewId) return;
			try {
				g_queue.Post([=] { g_pointer(kViewId, a_phase, a_dx, a_dy, a_wheel, nullptr); });
			} catch (...) { REX::ERROR("[UI] Could not queue browser pointer input"); }
		}
	}

	OSFUI::API::Client& Views() { return g_views; }
	bool Available() { return g_available; }
	const char* UnavailableReason() { return g_reason; }

	bool Connect(void (*a_shown)(), void (*a_hidden)())
	{
		g_shown = a_shown;
		g_hidden = a_hidden;
		const std::filesystem::path folder = "Data/SFSE/Plugins/OSF/UI/views/osf.animation/browser";
		std::error_code ec;
		if (!REX::W32::GetModuleHandleW(OSFUI::API::kModuleName)) {
			g_reason = "OSF UI is not installed; Animation Browser is inactive.";
			Health::Clear("browser.integration");
			REX::DEBUG("[UI] {}", g_reason);
			return false;
		}
		if (!g_views.Init()) {
			Fail("Installed OSF UI does not provide API 2.0. Install the released OSF UI 2.0.0 or newer and restart Starfield.");
			return false;
		}
		if (!std::filesystem::is_regular_file(folder / "index.html", ec) || ec ||
			!g_views.RegisterView(kViewId)) {
			Fail("Animation Browser assets are missing or invalid. Reinstall the matching OSF Animation Browser archive and restart Starfield.");
			return false;
		}
		if (!g_views.RegisterViewLifecycle(kViewId, OnLifecycle, nullptr)) {
			Fail("Animation Browser lifecycle registration failed. Remove duplicate Animation installations and restart Starfield.");
			return false;
		}
		return true;
	}

	bool RegisterSend(const char* a_name, OSFUI::API::SendFn a_handler, bool a_requiresVisible)
	{
		g_handlers.push_back({ a_handler, a_requiresVisible });
		if (g_views.RegisterSend(a_name, OnSend, &g_handlers.back())) return true;
		g_registrationFailed = true;
		REX::ERROR("[UI] Could not register browser endpoint '{}'", a_name);
		return false;
	}

	bool RegisterPointer(OSFUI::API::RelativePointerFn a_handler)
	{
		g_pointer = a_handler;
		return g_views.RegisterRelativePointer(kViewId, OnPointer, nullptr);
	}

	void FinishRegistration(bool a_engineSubscribed)
	{
		if (g_registrationFailed || !a_engineSubscribed) {
			Fail("Animation Browser command registration failed. Remove duplicate Animation installations and restart Starfield.");
			return;
		}
		g_available = true;
		Health::Clear("browser.integration");
	}

	void Post(std::function<void()> a_task, bool a_requiresVisible) { g_queue.Post(std::move(a_task), a_requiresVisible); }
	void Reset(std::function<void()> cleanup) { g_queue.Reset(std::move(cleanup)); }
}
