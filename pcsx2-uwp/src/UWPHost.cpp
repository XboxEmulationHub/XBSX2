// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/PrecompiledHeader.h"

#include "UWPHost.h"
#include "UWPUtils.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/GameList.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Sio.h"
#include "pcsx2/VMManager.h"

#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/Threading.h"
#include "common/WindowInfo.h"

#include <roapi.h>
#include <gamingdeviceinformation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Popups.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <chrono>

// clang-format off
namespace DataTransfer 	  = winrt::Windows::ApplicationModel::DataTransfer;
namespace DeviceInput 	  = winrt::Windows::Devices::Input;
namespace Display 		  = winrt::Windows::Graphics::Display;
namespace SystemThreading = winrt::Windows::System::Threading;
namespace CoreUI 		  = winrt::Windows::UI::Core;
namespace Popups 		  = winrt::Windows::UI::Popups;
namespace ViewManagement  = winrt::Windows::UI::ViewManagement;
// clang-format on

EmuThread* g_emu_thread = nullptr;
struct ShutdownRequest;
thread_local EmuThread* EmuThread::s_current_thread = nullptr;

//////////////////////////////////////////////////////////////////////////
// Local function declarations
//////////////////////////////////////////////////////////////////////////

namespace UWPHost
{
	static std::shared_ptr<EmuThread> GetEmuThread();
	static void RefreshGameList(bool invalidate_cache);
	static void CancelGameListRefresh();
	static void EnableGameListRefresh(bool enabled = true);
	static void PollGameListRefresh();
	static void SetFullscreen(bool fullscreen);
	static void ResizeDisplayWindow(s32 width, s32 height);
	static void RequestShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state, bool exit_application);
	static void ConfirmShutdown(const std::shared_ptr<ShutdownRequest>& request);
	static void FinishShutdown(const std::shared_ptr<ShutdownRequest>& request, bool confirmed, bool save_state);
	static void PollSettingsSave();
	static void QueueMessage(std::string_view title, std::string_view message, std::vector<std::string> buttons = {},
		u32 default_index = 0, u32 cancel_index = 0, std::function<void(s32)> callback = {});
	static void DismissMessageDialog(s32 selection);
	static void UpdateMouseMode();
	static void GetMaxOutputResolution(u32* width, u32* height);
	static std::optional<WindowInfo> GetRenderWindowInfo();
	static void InitializeClipboard();
	static void PollClipboard();
	static void StartUITimer();
	static bool ConfirmSettingsReset();
} // namespace UWPHost

//////////////////////////////////////////////////////////////////////////
// Local variable declarations
//////////////////////////////////////////////////////////////////////////

struct ShutdownRequest
{
	std::weak_ptr<EmuThread> thread;
	u64 generation = 0;
	bool was_running = false;
	bool allow_confirm = false;
	bool allow_save_state = false;
	bool default_save_state = false;
	bool exit_application = false;
};

struct MessageDialogRequest
{
	std::string title;
	std::string message;
	std::vector<std::string> buttons;
	u32 default_index = 0;
	u32 cancel_index = 0;
	std::function<void(s32)> callback;
	u64 vm_generation = 0;
};

// Keep the instance alive while concurrent host callbacks finish.
static std::atomic<std::shared_ptr<EmuThread>> s_emu_thread;
static std::atomic_bool s_cpu_thread_initialized = false;
static std::atomic_bool s_exit_requested = false;

class GameListProgressCallback final : public BaseProgressCallback
{
public:
	void Cancel() { m_cancel_requested.store(true, std::memory_order_release); }
	bool IsCancelled() const override { return m_cancel_requested.load(std::memory_order_acquire); }
	void SetTitle(const char* title) override {}
	void DisplayError(const char* message) override { Console.Error("%s", message); }
	void DisplayWarning(const char* message) override { Console.Warning("%s", message); }
	void DisplayInformation(const char* message) override { Console.WriteLn("%s", message); }
	void DisplayDebugMessage(const char* message) override { DevCon.WriteLn("%s", message); }
	void ModalError(const char* message) override { DisplayError(message); }
	bool ModalConfirmation(const char* message) override { return false; }
	void ModalInformation(const char* message) override { DisplayInformation(message); }

private:
	std::atomic_bool m_cancel_requested = false;
};

static std::mutex s_gamescan_mutex;
static Threading::Thread s_gamescanner_thread;
static std::unique_ptr<GameListProgressCallback> s_gamescan_progress;
static std::atomic_bool s_gamescan_active = false;
static bool s_gamescan_pending = true;
static bool s_gamescan_invalidate_cache = false;
static bool s_gamescan_enabled = true;

static std::atomic<u64> s_vm_generation = 0;
static std::shared_ptr<ShutdownRequest> s_shutdown_request;

static std::unique_ptr<INISettingsInterface> s_base_settings_interface;
static std::unique_ptr<INISettingsInterface> s_secrets_settings_interface;
static bool s_config_initialized = false;
static std::atomic_bool s_settings_save_requested = false;
static std::optional<std::chrono::steady_clock::time_point> s_settings_save_time;

static CoreUI::CoreWindow s_window{nullptr};
static CoreUI::CoreDispatcher s_dispatcher{nullptr};
static SystemThreading::ThreadPoolTimer s_ui_timer{nullptr};
static std::atomic_bool s_ui_wake_pending = false;
static std::mutex s_window_info_mutex;
static WindowInfo s_window_info;
static Display::DisplayInformation s_display_information{nullptr};
static winrt::event_token s_dpi_changed_token;
static bool s_dpi_changed_registered = false;
static ViewManagement::ApplicationView s_application_view{nullptr};
static CoreUI::CoreCursor s_default_cursor{nullptr};
static DeviceInput::MouseDevice s_mouse_device{nullptr};
static winrt::event_token s_mouse_moved_token;
static std::atomic_bool s_relative_mouse_mode = false;
static std::atomic_bool s_fullscreen = false;
static std::atomic_bool s_mouse_lock = false;
static std::atomic_bool s_hide_mouse_cursor = false;
static bool s_requested_relative_mouse_mode = false;
static bool s_requested_hide_cursor = false;
static bool s_cursor_hidden = false;
static std::atomic_bool s_window_active = true;
static std::atomic_bool s_window_closed = false;

static DataTransfer::IClipboardStatics s_clipboard{nullptr};
static winrt::event_token s_clipboard_changed_token;
static bool s_clipboard_changed_registered = false;
static std::atomic_bool s_clipboard_available = false;
static std::atomic_bool s_clipboard_read_requested = false;
static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> s_clipboard_read{nullptr};
static std::mutex s_clipboard_mutex;
static std::string s_clipboard_cache;

static std::mutex s_message_mutex;
static std::deque<MessageDialogRequest> s_messages;
static Popups::MessageDialog s_active_message{nullptr};
static winrt::Windows::Foundation::IAsyncOperation<Popups::IUICommand> s_message_dialog{nullptr};
static std::vector<Popups::IUICommand> s_message_commands;
static std::function<void(s32)> s_message_callback;
static u64 s_message_vm_generation = 0;


//////////////////////////////////////////////////////////////////////////
// CPU Thread
//////////////////////////////////////////////////////////////////////////

EmuThread::EmuThread() = default;

EmuThread::~EmuThread() = default;

bool EmuThread::start()
{
	if (s_window_closed.load(std::memory_order_acquire))
		return false;
	pxAssertRel(!s_emu_thread.load(std::memory_order_acquire), "Emu thread already started");
	const auto thread = std::make_shared<EmuThread>();
	s_emu_thread.store(thread, std::memory_order_release);
	g_emu_thread = thread.get();
	s_exit_requested.store(false, std::memory_order_release);
	s_cpu_thread_initialized.store(false, std::memory_order_release);
	UWPHost::EnableGameListRefresh(true);
	thread->m_thread.SetStackSize(static_cast<u32>(VMManager::EMU_THREAD_STACK_SIZE));
	thread->m_running.store(true, std::memory_order_release);
	if (thread->m_thread.Start([thread = thread.get()]() {
			s_current_thread = thread;
			thread->run();
			s_current_thread = nullptr;
			thread->m_running.store(false, std::memory_order_release);
		}))
		return true;

	thread->m_cpu_events.Close();
	thread->m_running.store(false, std::memory_order_release);
	g_emu_thread = nullptr;
	s_emu_thread.store(nullptr, std::memory_order_release);
	// Reuse Qt's translation for the startup error title.
	Host::ReportErrorAsync(TRANSLATE_SV("QtHost", "Startup Error"), "Failed to start CPU thread.");
	return false;
}

void EmuThread::stop()
{
	const std::shared_ptr<EmuThread> thread = UWPHost::GetEmuThread();
	if (!thread)
		return;
	pxAssertRel(!thread->isOnEmuThread(), "Cannot join the emu thread from itself");
	if (thread->isRunning())
		thread->requestExit();
	else
		thread->m_cpu_events.Close();
	while (thread->isRunning())
	{
		UWPHost::ProcessPendingEvents();
		if (thread->isRunning())
			UWPHost::ProcessUIEvents();
	}
	if (thread->m_thread.Joinable())
		thread->m_thread.Join();
	g_emu_thread = nullptr;
	s_emu_thread.store(nullptr, std::memory_order_release);
}

void EmuThread::stopInThread()
{
	m_shutdown_flag.store(true, std::memory_order_release);
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);
	m_cpu_events.Close();
}

bool EmuThread::startFullscreenUI(bool fullscreen)
{
	if (!isOnEmuThread())
		return m_cpu_events.Queue([this, fullscreen]() { startFullscreenUI(fullscreen); });
	m_reopen_fullscreen_ui.store(false, std::memory_order_release);
	if (m_shutdown_flag.load(std::memory_order_acquire))
		return false;
	if (MTGS::IsOpen())
		return true;

	setFullscreen(fullscreen);
	const bool initialized = ImGuiManager::InitializeFullscreenUI();
	m_run_fullscreen_ui.store(initialized, std::memory_order_release);
	if (initialized && MTGS::WaitForOpen())
		return true;

	m_run_fullscreen_ui.store(false, std::memory_order_release);
	// Reuse Qt's translation for the startup error title.
	Host::ReportErrorAsync(TRANSLATE_SV("QtHost", "Startup Error"), TRANSLATE_SV("VMManager", "Failed to initialize GS."));
	requestExit();
	return false;
}

void EmuThread::stopFullscreenUI()
{
	if (!isOnEmuThread())
	{
		runOnCPUThread([this]() { stopFullscreenUI(); });
		return;
	}
	if (!VMManager::HasValidVM() && MTGS::IsOpen())
		MTGS::WaitForClose();
	m_run_fullscreen_ui.store(false, std::memory_order_release);
}

void EmuThread::startVM(VMBootParameters boot_params)
{
	if (!isOnEmuThread())
	{
		runOnCPUThread([this, boot_params = std::move(boot_params)]() mutable { startVM(std::move(boot_params)); });
		return;
	}
	if (m_shutdown_flag.load(std::memory_order_acquire) || VMManager::GetState() != VMState::Shutdown)
		return;
	// Determine whether to start fullscreen or not.
	const bool fullscreen = boot_params.fullscreen.value_or(Host::GetBaseBoolSettingValue("UI", "StartFullscreen", false));
	if (!startFullscreenUI(fullscreen))
		return;

	setFullscreen(fullscreen);

	auto hardcore_disable_callback = [](std::string reason, VMBootRestartCallback restart_callback) {
		MTGS::RunOnGSThread([reason = std::move(reason), restart_callback = std::move(restart_callback)]() {
			ImGuiFullscreen::OpenConfirmMessageDialog(Achievements::GetHardcoreModeDisableTitle(),
				Achievements::GetHardcoreModeDisableText(reason.c_str()),
				[restart_callback](bool confirmed) {
					if (confirmed)
						Host::RunOnCPUThread(restart_callback);
				});
		});
	};
	auto done_callback = [](VMBootResult result, const Error& error) {
		if (result != VMBootResult::StartupSuccess)
		{
			// Reuse Qt's translation for the startup error title.
			Host::ReportErrorAsync(TRANSLATE_SV("QtHost", "Startup Error"), error.GetDescription());
			return;
		}
		if (Host::GetBoolSettingValue("UI", "StartPaused", false))
		{
			// When starting paused, redraw the window, so there's at least something there.
			MTGS::PresentCurrentFrame();
			Host::OnVMPaused();
		}
		else
		{
			// This will come back and call OnVMResumed().
			VMManager::SetState(VMState::Running);
		}
		InputManager::ReloadDevices();
	};
	VMManager::InitializeAsync(boot_params, std::move(hardcore_disable_callback), std::move(done_callback));
}

void EmuThread::shutdownVM(bool save_state /* = true */)
{
	if (!isOnEmuThread())
	{
		runOnCPUThread([this, save_state]() { shutdownVM(save_state); });
		return;
	}
	if (!VMManager::HasValidVM())
		return;
	m_save_state_on_shutdown = save_state;
	VMManager::SetState(VMState::Stopping);
}

void EmuThread::destroyVM()
{
	m_was_paused_by_focus_loss = false;
	VMManager::Shutdown(m_save_state_on_shutdown);
	m_save_state_on_shutdown = false;
}

void EmuThread::runOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (block && isOnEmuThread())
		function();
	else
		m_cpu_events.Queue(std::move(function), block);
}

void EmuThread::processEventQueue()
{
	while (m_cpu_events.ProcessNext())
	{
	}
}

void EmuThread::idleEventLoop()
{
	while (!m_shutdown_flag.load(std::memory_order_acquire))
	{
		processEventQueue();
		VMManager::IdlePollUpdate();

		const VMState state = VMManager::GetState();
		if (state != VMState::Shutdown && state != VMState::Paused)
			return;

		m_cpu_events.WaitForEvent(std::chrono::milliseconds(
			FullscreenUI::IsInitialized() ? FULLSCREEN_UI_CONTROLLER_POLLING_INTERVAL : BACKGROUND_CONTROLLER_POLLING_INTERVAL));
	}
}

void EmuThread::run()
{
	// UWP-specific initialization.
	const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(hr))
	{
		// Reuse Qt's translation for the startup error title.
		Host::ReportErrorAsync(TRANSLATE_SV("QtHost", "Startup Error"),
			fmt::format("Failed to initialize CPU thread WinRT apartment: {}", Error::CreateHResult(hr).GetDescription()));
		m_cpu_events.Close();
		return;
	}

	// Common host initialization (VM setup, etc).
	if (!VMManager::Internal::CPUThreadInitialize())
	{
		m_cpu_events.Close();
		VMManager::Internal::CPUThreadShutdown();
		RoUninitialize();
		return;
	}

	s_cpu_thread_initialized.store(true, std::memory_order_release);
	processEventQueue();
	if (!m_shutdown_flag.load(std::memory_order_acquire) && VMManager::GetState() == VMState::Shutdown)
	{
		startFullscreenUI(Host::GetBaseBoolSettingValue("UI", "StartFullscreen", false));
	}

	// Main CPU thread loop.
	while (!m_shutdown_flag.load(std::memory_order_acquire))
	{
		switch (VMManager::GetState())
		{
			case VMState::Initializing:
				pxFailRel("Shouldn't be in the starting state");
				break;

			case VMState::Shutdown:
				if (m_reopen_fullscreen_ui.load(std::memory_order_acquire))
					startFullscreenUI(isFullscreen());
				idleEventLoop();
				break;

			case VMState::Paused:
				idleEventLoop();
				break;

			case VMState::Running:
				processEventQueue();
				if (!m_shutdown_flag.load(std::memory_order_acquire) && VMManager::GetState() == VMState::Running)
					VMManager::Execute();
				break;

			case VMState::Resetting:
				VMManager::Reset();
				break;

			case VMState::Stopping:
				destroyVM();
				processEventQueue();
				if (!m_shutdown_flag.load(std::memory_order_acquire) && !VMManager::HasValidVM() && !MTGS::IsOpen())
					startFullscreenUI(isFullscreen());
				break;

			default:
				break;
		}
	}

	// Teardown in reverse order.
	s_cpu_thread_initialized.store(false, std::memory_order_release);
	m_cpu_events.Close();
	UWPHost::CancelGameListRefresh();
	if (VMManager::HasValidVM() || VMManager::GetState() == VMState::Stopping)
		destroyVM();
	stopFullscreenUI();
	VMManager::Internal::CPUThreadShutdown();
	RoUninitialize();
}

void EmuThread::requestExit()
{
	s_exit_requested.store(true, std::memory_order_release);
	if (isOnEmuThread())
		stopInThread();
	else
		runOnCPUThread([this]() { stopInThread(); });
}

void EmuThread::onVMStopped()
{
	m_reopen_fullscreen_ui.store(true, std::memory_order_release);
	UWPHost::EnableGameListRefresh(true);
}

void EmuThread::onDisplayWindowResized(u32 width, u32 height, float scale)
{
	if (!isOnEmuThread())
	{
		runOnCPUThread([this, width, height, scale]() { onDisplayWindowResized(width, height, scale); });
		return;
	}
	if (MTGS::IsOpen())
		MTGS::ResizeDisplayWindow(width, height, scale);
}

bool EmuThread::isFullscreen() const
{
	return s_fullscreen.load(std::memory_order_acquire);
}

std::optional<WindowInfo> EmuThread::acquireRenderWindow(bool recreate_window)
{
	return UWPHost::GetRenderWindowInfo();
}

void EmuThread::releaseRenderWindow()
{
	// CoreWindow belongs to the app and stays alive until frontend shutdown.
}

void EmuThread::setFullscreen(bool fullscreen)
{
	UWPHost::SetFullscreen(fullscreen);
}

void EmuThread::onResizeRenderWindowRequested(s32 width, s32 height)
{
	UWPHost::ResizeDisplayWindow(width, height);
}

void EmuThread::onApplicationStateChanged(bool active)
{
	if (!isOnEmuThread())
	{
		runOnCPUThread([this, active]() { onApplicationStateChanged(active); });
		return;
	}
	// Clear the state of all keyboard binds.
	// That way, if we had a key held down, and lost focus, the bind won't be stuck enabled because we never
	// got the key release message, because it happened in another window which "stole" the event.
	if (!active)
		InputManager::ClearBindStateFromSource(InputManager::MakeHostKeyboardKey(0));
	if (!active && VMManager::GetState() == VMState::Running && m_pause_on_focus_loss)
	{
		m_was_paused_by_focus_loss = true;
		VMManager::SetPaused(true);
	}
	else if (active && std::exchange(m_was_paused_by_focus_loss, false) && VMManager::GetState() == VMState::Paused)
	{
		VMManager::SetPaused(false);
	}
}

void EmuThread::loadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
	m_pause_on_focus_loss = si.GetBoolValue("UI", "PauseOnFocusLoss", false);
	s_mouse_lock.store(si.GetBoolValue("EmuCore", "EnableMouseLock", false), std::memory_order_release);
	s_hide_mouse_cursor.store(si.GetBoolValue("UI", "HideMouseCursor", false), std::memory_order_release);
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
	if (const auto thread = UWPHost::GetEmuThread())
		thread->loadSettings(si, lock);
}

void EmuThread::checkForSettingChanges(const Pcsx2Config& old_config)
{
	UWPHost::RunOnUIThread(&UWPHost::UpdateMouseMode);
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
	if (const auto thread = UWPHost::GetEmuThread())
		thread->checkForSettingChanges(old_config);
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	const auto thread = UWPHost::GetEmuThread();
	return thread ? thread->acquireRenderWindow(recreate_window) : std::nullopt;
}

void Host::ReleaseRenderWindow()
{
	if (const auto thread = UWPHost::GetEmuThread())
		thread->releaseRenderWindow();
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
	if (const auto thread = UWPHost::GetEmuThread())
		thread->onResizeRenderWindowRequested(width, height);
}

void Host::OnVMStarting()
{
	s_vm_generation.fetch_add(1, std::memory_order_acq_rel);
	UWPHost::RunOnUIThread(&UWPHost::UpdateMouseMode);
}

void Host::OnVMStarted()
{
	UWPHost::RunOnUIThread(&UWPHost::UpdateMouseMode);
}

void Host::OnVMDestroyed()
{
	s_shutdown_request.reset();
	s_vm_generation.fetch_add(1, std::memory_order_acq_rel);
	if (const auto thread = UWPHost::GetEmuThread())
		thread->onVMStopped();
	UWPHost::RunOnUIThread(&UWPHost::UpdateMouseMode);
}

void Host::OnVMPaused()
{
	UWPHost::RunOnUIThread(&UWPHost::UpdateMouseMode);
}

void Host::OnVMResumed()
{
	UWPHost::RunOnUIThread(&UWPHost::UpdateMouseMode);
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
	// OSD handles this.
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
	// OSD handles this.
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
	// OSD handles this.
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
	// OSD handles this.
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// FullscreenUI handles login. The core reports token errors.
}

void Host::OnAchievementsLoginSuccess(char const* display_name, u32 points, u32 sc_points, u32 unread_msg)
{
	// rcheevos handles this.
}

void Host::OnAchievementsRefreshed()
{
	// FullscreenUI handles this.
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnCoverDownloaderOpenRequested()
{
	// FullscreenUI handles this.
}

void Host::OnCreateMemoryCardOpenRequested()
{
	// FullscreenUI handles this.
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

void Host::PumpMessagesOnCPUThread()
{
	if (const auto thread = UWPHost::GetEmuThread())
		thread->processEventQueue();
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (const auto thread = UWPHost::GetEmuThread())
		thread->runOnCPUThread(std::move(function), block);
}

void Host::RunOnGSThread(std::function<void()> function)
{
	RunOnCPUThread([fn = std::move(function)]() { MTGS::RunOnGSThread(std::move(fn)); });
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	UWPHost::RefreshGameList(invalidate_cache);
}

void Host::CancelGameListRefresh()
{
	UWPHost::CancelGameListRefresh();
}

void Host::RequestExitApplication(bool allow_confirm)
{
	UWPHost::RequestShutdown(allow_confirm, true, EmuConfig.SaveStateOnShutdown, true);
}

void Host::RequestExitBigPicture()
{
	RequestExitApplication(true);
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	UWPHost::RequestShutdown(allow_confirm, allow_save_state, default_save_state, false);
}

bool Host::IsFullscreen()
{
	const auto thread = UWPHost::GetEmuThread();
	return thread && thread->isFullscreen();
}

void Host::SetFullscreen(bool enabled)
{
	if (const auto thread = UWPHost::GetEmuThread())
		thread->setFullscreen(enabled);
}

void Host::OnCaptureStarted(const std::string& filename)
{
	// The core displays capture notifications.
}

void Host::OnCaptureStopped()
{
}

bool UWPHost::InitializeConfig()
{
	s_config_initialized = false;
	const auto ReportError = [](std::string_view message) {
		// Reuse Qt's translation for the startup error title.
		Host::ReportErrorAsync(TRANSLATE_SV("QtHost", "Startup Error"), message);
		return false;
	};
	Error error;

	EmuFolders::SetAppRoot();

	if (!EmuFolders::SetResourcesDirectory())
		return ReportError("Failed to initialize the resource directory. See the log for details.");

	if (!EmuFolders::SetDataDirectory(&error))
	{
		return ReportError(fmt::format("Failed to create data directory '{}': {}", EmuFolders::DataRoot, error.GetDescription()));
	}

	// Write crash dumps to the data directory, since that'll be accessible for certain.
	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	// Load main settings ini
	std::string path(Path::Combine(EmuFolders::Settings, "PCSX2.ini"));
	const bool settings_exists = FileSystem::FileExists(path.c_str());
	Console.WriteLn("Loading config from %s.", path.c_str());
	s_base_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
	Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

	if (!settings_exists || !s_base_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
	{
		// If the config file doesn't exist, probably good to assume this is a new install so don't prompt to overwrite.
		if (settings_exists && !ConfirmSettingsReset())
			return false;

		VMManager::SetDefaultSettings(*s_base_settings_interface, true, true, true, true, true);
		s_base_settings_interface->SetIntValue("EmuCore/GS", "VsyncEnable", 1);

		auto lock = Host::GetSettingsLock();
		// Make sure we can actually save the config, and the user doesn't have some permission issue.
		if (!s_base_settings_interface->Save(&error))
		{
			return ReportError(fmt::format("Failed to save settings '{}': {}", s_base_settings_interface->GetFileName(), error.GetDescription()));
		}
	}

	// Layer secrets ini on top
	const std::string secrets_path(Path::Combine(EmuFolders::Settings, "secrets.ini"));
	const bool secrets_settings_exists = FileSystem::FileExists(secrets_path.c_str());
	Console.WriteLn("Loading secrets from %s.", secrets_path.c_str());
	s_secrets_settings_interface = std::make_unique<INISettingsInterface>(secrets_path);
	Host::Internal::SetSecretsSettingsLayer(s_secrets_settings_interface.get());
	if (!secrets_settings_exists || !s_secrets_settings_interface->Load())
	{
		if (!s_secrets_settings_interface->Save(&error))
		{
			return ReportError(fmt::format("Failed to save secrets '{}': {}", s_secrets_settings_interface->GetFileName(), error.GetDescription()));
		}
	}

	if (!UWPHost::InstallTranslator())
		return ReportError("Failed to initialize translations and fonts. See the log for details.");

	VMManager::Internal::LoadStartupSettings();
	s_config_initialized = true;
	return true;
}

bool UWPHost::ConfirmSettingsReset()
{
	if (!s_dispatcher || s_window_closed.load(std::memory_order_acquire))
		return false;
	bool done = false;
	bool confirmed = false;
	QueueMessage("XBSX2",
		"Settings failed to load, or are the incorrect version. Clicking Yes will reset all settings to defaults. Do you want to continue?",
		{TRANSLATE_STR("Achievements", "Yes"), TRANSLATE_STR("Achievements", "No")}, 1, 1, [&done, &confirmed](s32 choice) {
			confirmed = choice == 0;
			done = true;
		});
	while (!done)
	{
		ProcessPendingEvents();
		if (!done)
			ProcessUIEvents();
	}
	return confirmed;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	si.SetStringValue("UI", "Language", UWPHost::GetDefaultLanguage());
	si.SetBoolValue("UI", "ConfirmShutdown", true);
	si.SetBoolValue("UI", "PauseOnFocusLoss", false);
	si.SetBoolValue("UI", "StartPaused", false);
	si.SetBoolValue("UI", "StartFullscreen", false);
	si.SetBoolValue("UI", "HideMouseCursor", false);
}

void UWPHost::SaveSettings()
{
	pxAssertRel(!s_dispatcher || s_dispatcher.HasThreadAccess(), "Saving should happen on the UI thread.");
	if (!s_config_initialized)
		return;
	s_settings_save_requested.store(false, std::memory_order_release);
	s_settings_save_time.reset();
	auto lock = Host::GetSettingsLock();
	Error error;
	if (s_base_settings_interface && s_base_settings_interface->IsDirty() && !s_base_settings_interface->Save(&error))
		Console.ErrorFmt("Failed to save settings: {}", error.GetDescription());
}

void UWPHost::PollSettingsSave()
{
	const auto now = std::chrono::steady_clock::now();
	if (s_settings_save_requested.exchange(false, std::memory_order_acq_rel) && !s_settings_save_time)
		s_settings_save_time = now + std::chrono::seconds(1);
	if (s_settings_save_time && now >= s_settings_save_time.value())
		UWPHost::SaveSettings();
}

void Host::CommitBaseSettingChanges()
{
	s_settings_save_requested.store(true, std::memory_order_release);
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

void UWPHost::RunOnUIThread(std::function<void()> function)
{
	if (!s_dispatcher)
		return;
	const CoreUI::DispatchedHandler handler = [function = std::move(function)]() { function(); };
	winrt::Windows::Foundation::IAsyncAction action{nullptr};
	const HRESULT hr = UWP::GetABI(s_dispatcher)->RunAsync(static_cast<s32>(CoreUI::CoreDispatcherPriority::Normal), winrt::get_abi(handler), winrt::put_abi(action));
	if (FAILED(hr))
		Console.ErrorFmt("UWP: Failed to queue UI work: {}", Error::CreateHResult(hr).GetDescription());
}

void UWPHost::StartUITimer()
{
	const auto factory = winrt::try_get_activation_factory<SystemThreading::ThreadPoolTimer, SystemThreading::IThreadPoolTimerStatics>();
	if (!factory)
		return;
	const SystemThreading::TimerElapsedHandler handler = [dispatcher = s_dispatcher](const SystemThreading::ThreadPoolTimer&) {
		if (s_ui_wake_pending.exchange(true, std::memory_order_acq_rel))
			return;
		const CoreUI::DispatchedHandler wake = []() { s_ui_wake_pending.store(false, std::memory_order_release); };
		winrt::Windows::Foundation::IAsyncAction action{nullptr};
		if (FAILED(UWP::GetABI(dispatcher)->RunAsync(static_cast<s32>(CoreUI::CoreDispatcherPriority::Normal), winrt::get_abi(wake), winrt::put_abi(action))))
			s_ui_wake_pending.store(false, std::memory_order_release);
	};
	const auto period = std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::milliseconds(UI_THREAD_POLLING_INTERVAL));
	const HRESULT hr = UWP::GetABI(factory)->CreatePeriodicTimer(winrt::get_abi(handler), period.count(), winrt::put_abi(s_ui_timer));
	if (FAILED(hr))
		Console.ErrorFmt("UWP: Failed to create UI timer: {}", Error::CreateHResult(hr).GetDescription());
}

void UWPHost::ProcessUIEvents()
{
	if (s_dispatcher && s_ui_timer)
	{
		s_dispatcher.ProcessEvents(CoreUI::CoreProcessEventsOption::ProcessOneAndAllPending);
	}
	else
	{
		if (s_dispatcher)
			s_dispatcher.ProcessEvents(CoreUI::CoreProcessEventsOption::ProcessAllIfPresent);
		Threading::Sleep(UI_THREAD_POLLING_INTERVAL);
	}
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	RunOnCPUThread([folders, core, controllers, hotkeys, ui]() {
		{
			auto lock = Host::GetSettingsLock();
			VMManager::SetDefaultSettings(*s_base_settings_interface, folders, core, controllers, hotkeys, ui);
		}
		Host::CommitBaseSettingChanges();
		if (folders)
			VMManager::Internal::UpdateEmuFolders();
		VMManager::ApplySettings();
	});
	return true;
}

void UWPHost::QueueMessage(std::string_view title, std::string_view message, std::vector<std::string> buttons,
	u32 default_index, u32 cancel_index, std::function<void(s32)> callback)
{
	if (message.empty())
		return;
	std::lock_guard lock(s_message_mutex);
	s_messages.push_back({std::string(title), std::string(message), std::move(buttons), default_index, cancel_index,
		std::move(callback), s_vm_generation.load(std::memory_order_acquire)});
}

void UWPHost::DismissMessageDialog(s32 selection)
{
	auto callback = std::move(s_message_callback);
	s_message_dialog = nullptr;
	s_active_message = nullptr;
	s_message_commands.clear();
	if (callback)
		callback(selection);
}

void UWPHost::FinishShutdown(const std::shared_ptr<ShutdownRequest>& request, bool confirmed, bool save_state)
{
	if (s_shutdown_request != request)
		return;
	s_shutdown_request.reset();
	const auto thread = request->thread.lock();
	if (!thread || request->generation != s_vm_generation.load(std::memory_order_acquire) || !VMManager::HasValidVM())
		return;
	if (!confirmed)
	{
		if (request->was_running && VMManager::GetState() == VMState::Paused)
		{
			VMManager::SetPaused(false);
			thread->onApplicationStateChanged(s_window_active.load(std::memory_order_acquire));
		}
		return;
	}
	thread->shutdownVM(save_state);
	if (request->exit_application)
		thread->requestExit();
}

void UWPHost::ConfirmShutdown(const std::shared_ptr<ShutdownRequest>& request)
{
	if (s_shutdown_request != request)
		return;
	if (!request->allow_confirm || GSDumpReplayer::IsReplayingDump() || !Host::GetBoolSettingValue("UI", "ConfirmShutdown", true))
	{
		FinishShutdown(request, true, request->allow_save_state && request->default_save_state);
		return;
	}
	// Reuse Qt's existing shutdown dialog translations.
	std::vector<std::string> buttons;
	if (request->allow_save_state)
		buttons.emplace_back(TRANSLATE_STR("FullscreenUI", "Exit And Save State"));
	buttons.emplace_back(TRANSLATE_STR("FullscreenUI", "Exit Without Saving"));
	buttons.emplace_back(TRANSLATE_STR("QtHost", "Cancel"));
	const u32 cancel_index = static_cast<u32>(buttons.size() - 1);
	const u32 default_index = request->allow_save_state && !request->default_save_state ? 1 : 0;
	QueueMessage(TRANSLATE_STR("MainWindow", "Confirm Shutdown"), TRANSLATE_STR("MainWindow", "Are you sure you want to shut down the virtual machine?"), std::move(buttons),
		default_index, cancel_index, [request, cancel_index](s32 choice) {
			if (const auto thread = request->thread.lock())
			{
				thread->runOnCPUThread([request, cancel_index, choice]() {
					FinishShutdown(request, choice >= 0 && static_cast<u32>(choice) < cancel_index,
						request->allow_save_state && choice == 0);
				});
			}
		});
}

void UWPHost::RequestShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state, bool exit_application)
{
	const auto thread = GetEmuThread();
	if (!thread)
	{
		if (exit_application)
			s_exit_requested.store(true, std::memory_order_release);
		return;
	}
	thread->runOnCPUThread([weak_thread = std::weak_ptr<EmuThread>(thread), allow_confirm, allow_save_state, default_save_state, exit_application]() {
		const auto thread = weak_thread.lock();
		if (!thread)
			return;
		if (s_shutdown_request && allow_confirm)
		{
			s_shutdown_request->exit_application |= exit_application;
			return;
		}
		s_shutdown_request.reset();
		if (!VMManager::HasValidVM())
		{
			if (exit_application)
				thread->requestExit();
			return;
		}
		auto request = std::make_shared<ShutdownRequest>();
		request->thread = thread;
		request->generation = s_vm_generation.load(std::memory_order_acquire);
		request->was_running = VMManager::GetState() == VMState::Running;
		request->allow_confirm = allow_confirm;
		request->allow_save_state = allow_save_state && VMManager::GetDiscCRC() != 0 && !GSDumpReplayer::IsReplayingDump();
		request->default_save_state = default_save_state;
		request->exit_application = exit_application;
		s_shutdown_request = request;
		if (request->was_running)
			VMManager::SetPaused(true);
		if (allow_confirm && MemcardBusy::IsBusy() && !GSDumpReplayer::IsReplayingDump())
		{
			// Reuse Qt's translated warning and Yes/No labels.
			QueueMessage(TRANSLATE_STR("FullscreenUI", "WARNING: Memory Card Busy"), TRANSLATE_STR("FullscreenUI", "Your memory card is still saving data.\n\nWARNING: Shutting down now can IRREVERSIBLY CORRUPT YOUR MEMORY CARD.\n\nYou are strongly advised to select 'No' and let the save finish.\n\nDo you want to shutdown anyway and IRREVERSIBLY CORRUPT YOUR MEMORY CARD?"),
				{TRANSLATE_STR("Achievements", "Yes"), TRANSLATE_STR("Achievements", "No")}, 1, 1, [request](s32 choice) {
					if (const auto thread = request->thread.lock())
					{
						thread->runOnCPUThread([request, choice]() {
							if (choice == 0)
								ConfirmShutdown(request);
							else
								FinishShutdown(request, false, false);
						});
					}
				});
		}
		else
		{
			ConfirmShutdown(request);
		}
	});
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
	UWPHost::QueueMessage(title, message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
	UWPHost::QueueMessage(title, message);
}

void Host::OpenURL(const std::string_view url)
{
	UWPHost::RunOnUIThread([url = std::string(url)]() {
		winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(winrt::to_hstring(url)));
	});
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	if (!s_clipboard_available.load(std::memory_order_acquire))
		return false;
	UWPHost::RunOnUIThread([text = std::string(text)]() {
		const auto factory = winrt::try_get_activation_factory<DataTransfer::DataPackage>();
		winrt::Windows::Foundation::IInspectable object{nullptr};
		if (!factory || FAILED(UWP::GetABI(factory)->ActivateInstance(winrt::put_abi(object))))
			return;
		const auto package = object.try_as<DataTransfer::IDataPackage>();
		if (!package)
			return;
		HRESULT hr = UWP::GetABI(package)->SetText(winrt::get_abi(winrt::to_hstring(text)));
		if (SUCCEEDED(hr))
			hr = UWP::GetABI(s_clipboard)->SetContent(winrt::get_abi(package));
		if (FAILED(hr))
		{
			Console.ErrorFmt("UWP: Failed to copy text: {}", Error::CreateHResult(hr).GetDescription());
			return;
		}
		s_clipboard_read = nullptr;
		s_clipboard_read_requested.store(true, std::memory_order_release);
		std::lock_guard lock(s_clipboard_mutex);
		s_clipboard_cache = text;
	});
	return true;
}

std::string Host::GetTextFromClipboard()
{
	s_clipboard_read_requested.store(true, std::memory_order_release);
	std::lock_guard lock(s_clipboard_mutex);
	return s_clipboard_cache;
}

void Host::BeginTextInput()
{
	UWPHost::RunOnUIThread([]() { ViewManagement::Core::CoreInputView::GetForCurrentView().TryShowPrimaryView(); });
}

void Host::EndTextInput()
{
	UWPHost::RunOnUIThread([]() { ViewManagement::Core::CoreInputView::GetForCurrentView().TryHide(); });
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return UWPHost::GetRenderWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
	if (VMManager::HasValidVM() || FullscreenUI::HasActiveWindow())
	{
		// Reuse Qt's translations for controller notifications.
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD,
			fmt::format(TRANSLATE_FS("QtHost", "Controller {} connected."), identifier),
			Host::OSD_INFO_DURATION);
	}
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, const std::string_view identifier)
{
	if (VMManager::GetState() == VMState::Running &&
		Host::GetBoolSettingValue("UI", "PauseOnControllerDisconnection", false) &&
		InputManager::HasAnyBindingsForSource(key))
	{
		// Reuse Qt's translations for controller notifications.
		std::string message = fmt::format(TRANSLATE_FS("QtHost", "System paused because controller {} was disconnected."), identifier);
		Host::RunOnCPUThread([message]() { VMManager::SetPaused(true); });
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD, std::move(message),
			Host::OSD_WARNING_DURATION);
	}
	else if (VMManager::HasValidVM() || FullscreenUI::HasActiveWindow())
	{
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD,
			fmt::format(TRANSLATE_FS("QtHost", "Controller {} disconnected."), identifier),
			Host::OSD_INFO_DURATION);
	}
}

void UWPHost::UpdateMouseMode()
{
	if (!s_window)
		return;
	const bool running = VMManager::GetState() == VMState::Running;
	const bool allow_relative = running && s_window_active && !ImGuiManager::WantsMouseInput();
	const bool relative = s_mouse_device && allow_relative &&
	                      (s_requested_relative_mouse_mode || s_mouse_lock.load(std::memory_order_acquire));
	const bool hide_cursor = s_window_active && (relative ||
													(s_requested_hide_cursor && !s_requested_relative_mouse_mode) ||
													(running && s_fullscreen.load(std::memory_order_acquire) && s_hide_mouse_cursor.load(std::memory_order_acquire)));
	if (s_cursor_hidden != hide_cursor)
	{
		if (SUCCEEDED(UWP::GetABI(s_window)->put_PointerCursor(hide_cursor ? nullptr : winrt::get_abi(s_default_cursor))))
			s_cursor_hidden = hide_cursor;
	}
	s_relative_mouse_mode.store(relative && s_cursor_hidden, std::memory_order_release);
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
	UWPHost::RunOnUIThread([relative, hide_cursor]() {
		s_requested_relative_mouse_mode = relative;
		s_requested_hide_cursor = hide_cursor;
		UWPHost::UpdateMouseMode();
	});
}

void Host::SetMouseLock(bool state)
{
	RunOnCPUThread([state]() {
		SetBaseBoolSettingValue("EmuCore", "EnableMouseLock", state);
		CommitBaseSettingChanges();
		s_mouse_lock.store(state, std::memory_order_release);
	});
}

bool UWPHost::IsRelativeMouseMode()
{
	return s_relative_mouse_mode.load(std::memory_order_acquire);
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

//////////////////////////////////////////////////////////////////////////
// Hotkeys
//////////////////////////////////////////////////////////////////////////

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

//////////////////////////////////////////////////////////////////////////
// Interface Stuff
//////////////////////////////////////////////////////////////////////////

std::shared_ptr<EmuThread> UWPHost::GetEmuThread()
{
	return s_emu_thread.load(std::memory_order_acquire);
}

void UWPHost::RefreshGameList(bool invalidate_cache)
{
	std::lock_guard lock(s_gamescan_mutex);
	s_gamescan_pending = true;
	s_gamescan_invalidate_cache |= invalidate_cache;
}

void UWPHost::EnableGameListRefresh(bool enabled)
{
	std::lock_guard lock(s_gamescan_mutex);
	s_gamescan_enabled = enabled;
}

void UWPHost::PollGameListRefresh()
{
	std::lock_guard lock(s_gamescan_mutex);
	if (!s_gamescan_enabled || !s_cpu_thread_initialized.load(std::memory_order_acquire) || s_exit_requested.load(std::memory_order_acquire) || VMManager::GetState() != VMState::Shutdown || !s_gamescan_pending ||
		s_gamescan_active.load(std::memory_order_acquire))
		return;
	if (s_gamescanner_thread.Joinable())
		s_gamescanner_thread.Join();

	const bool invalidate_cache = std::exchange(s_gamescan_invalidate_cache, false);
	s_gamescan_pending = false;
	s_gamescan_progress = std::make_unique<GameListProgressCallback>();
	s_gamescan_active.store(true, std::memory_order_release);
	if (!s_gamescanner_thread.Start([invalidate_cache, progress = s_gamescan_progress.get()]() {
			GameList::Refresh(invalidate_cache, false, progress);
			s_gamescan_active.store(false, std::memory_order_release);
		}))
	{
		s_gamescan_active.store(false, std::memory_order_release);
		s_gamescan_progress.reset();
		Console.Error("UWP: Failed to start game list refresh thread.");
	}
}

void UWPHost::CancelGameListRefresh()
{
	std::lock_guard lock(s_gamescan_mutex);
	s_gamescan_enabled = false;
	// An interrupted scan can leave the list incomplete. Refresh it again after the VM stops.
	s_gamescan_pending |= s_gamescan_active.load(std::memory_order_acquire);
	if (s_gamescan_progress)
		s_gamescan_progress->Cancel();
	if (s_gamescanner_thread.Joinable())
		s_gamescanner_thread.Join();
	s_gamescan_progress.reset();
}

void UWPHost::ResizeDisplayWindow(s32 width, s32 height)
{
	if (width <= 0 || height <= 0)
		return;
	UWPHost::RunOnUIThread([width, height]() {
		const auto view = s_application_view.try_as<ViewManagement::IApplicationView3>();
		if (!view)
			return;
		const float scale = UWPHost::GetDisplayScale();
		bool resized = false;
		const HRESULT hr = UWP::GetABI(view)->TryResizeView(
			{static_cast<float>(width) / scale, static_cast<float>(height) / scale}, &resized);
		if (FAILED(hr))
			Console.ErrorFmt("UWP: Failed to resize window: {}", Error::CreateHResult(hr).GetDescription());
	});
}

void UWPHost::SetFullscreen(bool enabled)
{
	UWPHost::RunOnUIThread([enabled]() {
		const auto view = s_application_view.try_as<ViewManagement::IApplicationView3>();
		if (!view)
			return;
		bool entered = false;
		const HRESULT hr = enabled ? UWP::GetABI(view)->TryEnterFullScreenMode(&entered) : UWP::GetABI(view)->ExitFullScreenMode();
		if (FAILED(hr))
			Console.ErrorFmt("UWP: Failed to change fullscreen mode: {}", Error::CreateHResult(hr).GetDescription());
		UWPHost::UpdateWindowSize();
	});
}

void UWPHost::GetMaxOutputResolution(u32* width, u32* height)
{
	*width = 1920;
	*height = 1080;

	GAMING_DEVICE_MODEL_INFORMATION info = {};
	GetGamingDeviceModelInformation(&info);

	if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
	{
		switch (info.deviceId)
		{
			// The Xbox One S can output to 4K but I don't have one to test on so we'll limit it to 1080p like the original Xbox One.
			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
				*width = 1920;
				*height = 1080;
				break;

			// The Series S can output to 4K but if you try to make a 4K Swap Chain the application will crash,
			// so we limit it to 1440p.
			case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
				*width = 2560;
				*height = 1440;
				break;

			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
			case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
			case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
			default:
				*width = 3840;
				*height = 2160;
				break;
		}
	}
}

float UWPHost::GetDisplayScale()
{
	const auto info = Display::DisplayInformation::GetForCurrentView();
	const float scale = info ? static_cast<float>(info.RawPixelsPerViewPixel()) : 1.0f;
	return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

void UWPHost::SetWindow(const CoreUI::CoreWindow& window)
{
	s_window = window;
	s_dispatcher = window.Dispatcher();
	StartUITimer();
	s_display_information = Display::DisplayInformation::GetForCurrentView();
	if (s_display_information)
	{
		const winrt::Windows::Foundation::TypedEventHandler<Display::DisplayInformation, winrt::Windows::Foundation::IInspectable> handler =
			[](auto&&, auto&&) { UpdateWindowSize(); };
		s_dpi_changed_registered = SUCCEEDED(UWP::GetABI(s_display_information)->add_DpiChanged(winrt::get_abi(handler), &s_dpi_changed_token));
	}
	UWP::GetABI(window)->get_PointerCursor(winrt::put_abi(s_default_cursor));
	window.Activated([](auto&&, const CoreUI::WindowActivatedEventArgs& args) {
		s_window_active = args.WindowActivationState() != CoreUI::CoreWindowActivationState::Deactivated;
		if (s_window_active)
			s_clipboard_read_requested.store(true, std::memory_order_release);
		if (const auto thread = GetEmuThread())
			thread->onApplicationStateChanged(s_window_active);
		UpdateMouseMode();
	});
	window.Closed([](auto&&, auto&&) { s_window_closed.store(true, std::memory_order_release); });

	if (const auto factory = winrt::try_get_activation_factory<ViewManagement::ApplicationView, ViewManagement::IApplicationViewStatics2>())
		UWP::GetABI(factory)->GetForCurrentView(winrt::put_abi(s_application_view));

	if (const auto factory = winrt::try_get_activation_factory<DeviceInput::MouseDevice, DeviceInput::IMouseDeviceStatics>())
		UWP::GetABI(factory)->GetForCurrentView(winrt::put_abi(s_mouse_device));
	if (s_mouse_device)
	{
		const winrt::Windows::Foundation::TypedEventHandler<DeviceInput::MouseDevice, DeviceInput::MouseEventArgs> handler =
			[](const DeviceInput::MouseDevice&, const DeviceInput::MouseEventArgs& args) {
				if (!s_relative_mouse_mode.load(std::memory_order_acquire))
					return;
				const auto delta = args.MouseDelta();
				Host::RunOnCPUThread([delta]() {
					InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::X, static_cast<float>(delta.X));
					InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::Y, static_cast<float>(delta.Y));
				});
			};
		if (FAILED(UWP::GetABI(s_mouse_device)->add_MouseMoved(winrt::get_abi(handler), &s_mouse_moved_token)))
			s_mouse_device = nullptr;
	}

	InitializeClipboard();
	UpdateWindowSize();
}

void UWPHost::UpdateWindowSize()
{
	if (!s_window)
		return;
	if (s_application_view)
	{
		const auto view = s_application_view.try_as<ViewManagement::IApplicationView3>();
		bool fullscreen = false;
		if (view && SUCCEEDED(UWP::GetABI(view)->get_IsFullScreenMode(&fullscreen)))
			s_fullscreen.store(fullscreen, std::memory_order_release);
	}

	u32 max_width, max_height;
	GetMaxOutputResolution(&max_width, &max_height);

	WindowInfo wi;
	wi.type = WindowInfo::Type::WinRT;
	wi.surface_handle = winrt::get_abi(s_window);
	wi.surface_refresh_rate = 60.0f;
	wi.surface_scale = GetDisplayScale();

	// View WinRT APIs must go on the UI thread.
	if (winrt::try_get_activation_factory<Display::Core::HdmiDisplayInformation>())
	{
		const auto display = Display::Core::HdmiDisplayInformation::GetForCurrentView();
		if (display)
		{
			const auto mode = display.GetCurrentDisplayMode();
			if (mode)
			{
				wi.surface_width = mode.ResolutionWidthInRawPixels();
				wi.surface_height = mode.ResolutionHeightInRawPixels();
				wi.surface_refresh_rate = static_cast<float>(mode.RefreshRate());
			}
		}
	}
	if (wi.surface_width == 0 || wi.surface_height == 0)
	{
		const auto bounds = s_window.Bounds();
		wi.surface_width = static_cast<u32>(std::round(bounds.Width * wi.surface_scale));
		wi.surface_height = static_cast<u32>(std::round(bounds.Height * wi.surface_scale));
	}
	if (wi.surface_width == 0 || wi.surface_height == 0)
	{
		std::lock_guard lock(s_window_info_mutex);
		if (s_window_info.type != WindowInfo::Type::Surfaceless)
			return;
		wi.surface_width = max_width;
		wi.surface_height = max_height;
	}

	wi.surface_width = std::min(wi.surface_width, max_width);
	wi.surface_height = std::min(wi.surface_height, max_height);
	if (wi.surface_refresh_rate <= 0.0f)
		wi.surface_refresh_rate = 60.0f;

	{
		std::lock_guard lock(s_window_info_mutex);
		s_window_info = wi;
	}
	Console.WriteLnFmt("UWP: Using output resolution {}x{} at {} Hz", wi.surface_width, wi.surface_height, wi.surface_refresh_rate);

	if (const auto thread = GetEmuThread(); thread && thread->isRunning())
		thread->onDisplayWindowResized(wi.surface_width, wi.surface_height, wi.surface_scale);
}

std::optional<WindowInfo> UWPHost::GetRenderWindowInfo()
{
	std::lock_guard lock(s_window_info_mutex);
	if (s_window_info.type == WindowInfo::Type::Surfaceless)
		return std::nullopt;
	return s_window_info;
}

void UWPHost::InitializeClipboard()
{
	s_clipboard = winrt::try_get_activation_factory<DataTransfer::Clipboard, DataTransfer::IClipboardStatics>();
	if (s_clipboard)
	{
		const winrt::Windows::Foundation::EventHandler<winrt::Windows::Foundation::IInspectable> handler =
			[](auto&&, auto&&) { s_clipboard_read_requested.store(true, std::memory_order_release); };
		s_clipboard_changed_registered = SUCCEEDED(UWP::GetABI(s_clipboard)->add_ContentChanged(winrt::get_abi(handler), &s_clipboard_changed_token));
		s_clipboard_available.store(true, std::memory_order_release);
		s_clipboard_read_requested.store(true, std::memory_order_release);
	}
}

void UWPHost::PollClipboard()
{
	if (!s_clipboard)
		return;
	if (s_clipboard_read)
	{
		if (UWP::IsAsyncOperationPending(s_clipboard_read))
			return;
		winrt::hstring text;
		const HRESULT hr = UWP::GetABI(s_clipboard_read)->GetResults(winrt::put_abi(text));
		{
			std::lock_guard lock(s_clipboard_mutex);
			s_clipboard_cache = SUCCEEDED(hr) ? winrt::to_string(text) : std::string();
		}
		s_clipboard_read = nullptr;
	}
	if (!s_clipboard_read_requested.exchange(false, std::memory_order_acq_rel))
		return;

	DataTransfer::DataPackageView content{nullptr};
	bool contains_text = false;
	if (SUCCEEDED(UWP::GetABI(s_clipboard)->GetContent(winrt::put_abi(content))) && content &&
		SUCCEEDED(UWP::GetABI(content)->Contains(winrt::get_abi(winrt::hstring(L"Text")), &contains_text)) && contains_text)
	{
		if (FAILED(UWP::GetABI(content)->GetTextAsync(winrt::put_abi(s_clipboard_read))))
		{
			std::lock_guard lock(s_clipboard_mutex);
			s_clipboard_cache.clear();
		}
	}
	else
	{
		std::lock_guard lock(s_clipboard_mutex);
		s_clipboard_cache.clear();
	}
}

bool UWPHost::ProcessPendingEvents()
{
	PollGameListRefresh();
	PollSettingsSave();
	PollClipboard();
	UpdateMouseMode();
	if (s_window_closed.load(std::memory_order_acquire))
	{
		if (s_message_dialog)
		{
			const auto info = s_message_dialog.try_as<winrt::Windows::Foundation::IAsyncInfo>();
			if (info)
				UWP::GetABI(info)->Cancel();
		}
		DismissMessageDialog(-1);
		std::deque<MessageDialogRequest> pending;
		{
			std::lock_guard lock(s_message_mutex);
			pending.swap(s_messages);
		}
		for (auto& message : pending)
		{
			if (message.callback)
				message.callback(-1);
		}
		return false;
	}
	if (s_message_callback && s_message_dialog && (s_exit_requested.load(std::memory_order_acquire) || s_message_vm_generation != s_vm_generation.load(std::memory_order_acquire)))
	{
		const auto info = s_message_dialog.try_as<winrt::Windows::Foundation::IAsyncInfo>();
		if (info)
			UWP::GetABI(info)->Cancel();
		DismissMessageDialog(-1);
	}

	if (s_message_dialog)
	{
		if (UWP::IsAsyncOperationPending(s_message_dialog))
			return true;
		Popups::IUICommand command{nullptr};
		const HRESULT hr = UWP::GetABI(s_message_dialog)->GetResults(winrt::put_abi(command));
		s32 selection = -1;
		if (SUCCEEDED(hr))
		{
			for (size_t i = 0; i < s_message_commands.size(); i++)
			{
				if (command == s_message_commands[i])
				{
					selection = static_cast<s32>(i);
					break;
				}
			}
		}
		else
		{
			Console.ErrorFmt("UWP: Message dialog failed: {}", Error::CreateHResult(hr).GetDescription());
		}
		DismissMessageDialog(selection);
	}
	MessageDialogRequest message;
	{
		std::lock_guard lock(s_message_mutex);
		if (s_messages.empty())
			return false;
		bool visible = false;
		if (s_window && SUCCEEDED(UWP::GetABI(s_window)->get_Visible(&visible)) && !visible)
			return true;
		message = std::move(s_messages.front());
		s_messages.pop_front();
	}
	if (message.callback && (s_exit_requested.load(std::memory_order_acquire) ||
								message.vm_generation != s_vm_generation.load(std::memory_order_acquire)))
	{
		message.callback(-1);
		return true;
	}
	s_message_callback = std::move(message.callback);
	s_message_vm_generation = message.vm_generation;

	const auto factory = winrt::try_get_activation_factory<Popups::MessageDialog, Popups::IMessageDialogFactory>();
	HRESULT hr = E_NOTIMPL;
	if (factory)
	{
		hr = UWP::GetABI(factory)->CreateWithTitle(winrt::get_abi(winrt::to_hstring(message.message)),
			winrt::get_abi(winrt::to_hstring(message.title)), winrt::put_abi(s_active_message));
		if (SUCCEEDED(hr) && !message.buttons.empty())
		{
			const auto command_factory = winrt::try_get_activation_factory<Popups::UICommand, Popups::IUICommandFactory>();
			winrt::Windows::Foundation::Collections::IVector<Popups::IUICommand> commands{nullptr};
			hr = command_factory ? UWP::GetABI(s_active_message)->get_Commands(winrt::put_abi(commands)) : E_NOTIMPL;
			for (const std::string& label : message.buttons)
			{
				if (FAILED(hr))
					break;
				Popups::IUICommand command{nullptr};
				hr = UWP::GetABI(command_factory)->Create(winrt::get_abi(winrt::to_hstring(label)), winrt::put_abi(command));
				if (SUCCEEDED(hr))
					hr = UWP::GetABI(commands)->Append(winrt::get_abi(command));
				if (SUCCEEDED(hr))
					s_message_commands.push_back(std::move(command));
			}
			if (SUCCEEDED(hr))
				hr = UWP::GetABI(s_active_message)->put_DefaultCommandIndex(message.default_index);
			if (SUCCEEDED(hr))
				hr = UWP::GetABI(s_active_message)->put_CancelCommandIndex(message.cancel_index);
		}
		if (SUCCEEDED(hr))
			hr = UWP::GetABI(s_active_message)->ShowAsync(winrt::put_abi(s_message_dialog));
	}
	if (FAILED(hr))
	{
		Console.ErrorFmt("UWP: Failed to show message dialog: {}", Error::CreateHResult(hr).GetDescription());
		DismissMessageDialog(-1);
	}
	return true;
}

void UWPHost::Shutdown()
{
	EmuThread::stop();
	CancelGameListRefresh();
	s_shutdown_request.reset();
	// Ensure config is written. Prevents destruction order issues.
	SaveSettings();
	if (s_mouse_device)
		UWP::GetABI(s_mouse_device)->remove_MouseMoved(s_mouse_moved_token);
	s_mouse_device = nullptr;
	s_default_cursor = nullptr;
	s_application_view = nullptr;
	if (s_clipboard_changed_registered)
		UWP::GetABI(s_clipboard)->remove_ContentChanged(s_clipboard_changed_token);
	s_clipboard_changed_registered = false;
	s_clipboard_available.store(false, std::memory_order_release);
	s_clipboard_read = nullptr;
	s_clipboard = nullptr;
	if (s_dpi_changed_registered)
		UWP::GetABI(s_display_information)->remove_DpiChanged(s_dpi_changed_token);
	s_dpi_changed_registered = false;
	s_display_information = nullptr;
	if (s_ui_timer)
		UWP::GetABI(s_ui_timer)->Cancel();
	s_ui_timer = nullptr;
	std::lock_guard lock(s_window_info_mutex);
	s_window_info = {};
	s_dispatcher = nullptr;
	s_window = nullptr;
}
