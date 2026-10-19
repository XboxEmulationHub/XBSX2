// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "CPUEventQueue.h"

#include "common/Threading.h"
#include "common/WindowInfo.h"
#include "pcsx2/Host.h"

#include <atomic>
#include <memory>

struct VMBootParameters;
struct Pcsx2Config;

class EmuThread
{
public:
	EmuThread();
	~EmuThread();

	static bool start();
	static void stop();

	__fi bool isRunning() const { return m_running.load(std::memory_order_acquire); }
	__fi bool isOnEmuThread() const { return s_current_thread == this; }
	bool isFullscreen() const;
	__fi bool isRunningFullscreenUI() const { return m_run_fullscreen_ui.load(std::memory_order_acquire); }

	void loadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock);
	void checkForSettingChanges(const Pcsx2Config& old_config);

	std::optional<WindowInfo> acquireRenderWindow(bool recreate_window);
	void releaseRenderWindow();
	void setFullscreen(bool fullscreen);
	void onResizeRenderWindowRequested(s32 width, s32 height);

	void startVM(VMBootParameters boot_params);
	void shutdownVM(bool save_state = true);
	/// Blocking calls may return without running the function when the thread stops.
	void runOnCPUThread(std::function<void()> function, bool block = false);
	void processEventQueue();
	void onDisplayWindowResized(u32 width, u32 height, float scale);
	void requestExit();
	void onVMStopped();
	void onApplicationStateChanged(bool active);
	bool startFullscreenUI(bool fullscreen);
	void stopFullscreenUI();

private:
	static constexpr u32 BACKGROUND_CONTROLLER_POLLING_INTERVAL = 100;
	static constexpr u32 FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL = 8;

	void run();
	void stopInThread();
	void destroyVM();
	void idleEventLoop();

	static thread_local EmuThread* s_current_thread;

	Threading::Thread m_thread;
	UWPHost::CPUEventQueue m_cpu_events;
	std::atomic_bool m_running{false};
	std::atomic_bool m_shutdown_flag{false};
	std::atomic_bool m_run_fullscreen_ui{false};
	std::atomic_bool m_reopen_fullscreen_ui{false};
	bool m_save_state_on_shutdown = false;
	bool m_pause_on_focus_loss = false;
	bool m_was_paused_by_focus_loss = false;
};

extern EmuThread* g_emu_thread;

namespace winrt::Windows::UI::Core
{
	struct CoreWindow;
}

namespace UWPHost
{
	static constexpr u32 UI_THREAD_POLLING_INTERVAL = 16;

	/// Initializes settings on the UI thread before starting the CPU thread.
	bool InitializeConfig();

	/// Default language for the platform.
	const char* GetDefaultLanguage();

	/// Returns the language codes of the available translation catalogs.
	std::vector<std::string> GetAvailableLanguageList();

	/// Loads translations and fonts before starting the CPU thread.
	bool InstallTranslator();

	/// Sets the render window. Call on the UI thread before starting the CPU thread.
	void SetWindow(const winrt::Windows::UI::Core::CoreWindow& window);

	/// Updates the render window size. Must be called on the UI thread.
	void UpdateWindowSize();
	float GetDisplayScale();

	/// Queues a function on the UI thread.
	void RunOnUIThread(std::function<void()> function);

	/// Processes UI events, waiting until work is available.
	void ProcessUIEvents();

	/// Processes dialogs and settings saves. Returns true while a dialog is pending.
	bool ProcessPendingEvents();

	/// Saves settings immediately. Must be called on the UI thread.
	void SaveSettings();

	/// Returns true while relative mouse input is enabled.
	bool IsRelativeMouseMode();

	/// Stops the CPU thread and releases the render window.
	void Shutdown();
} // namespace UWPHost
