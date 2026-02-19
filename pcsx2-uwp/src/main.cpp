#include "pcsx2/PrecompiledHeader.h"

#include "UWPKeyboard.h"

#include <gamingdeviceinformation.h>
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/CDVD/CDVDcommon.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/VMManager.h"

#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/GameList.h"

#ifdef ENABLE_ACHIEVEMENTS
#include "pcsx2/Achievements.h"
#endif

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Graphics::Display::Core;
using namespace Windows::UI::Core;

static winrt::Windows::UI::Core::CoreWindow* s_corewind = NULL;
static std::mutex m_event_mutex;
static std::condition_variable m_event_cv;
static std::deque<std::function<void()>> m_event_queue;
static std::thread::id s_cpu_thread_id;
static bool s_running = true;
static std::thread s_gamescanner_thread;
std::atomic<bool> b_gamescan_active = false;

static u32 s_xbox_output_width = 1920;
static u32 s_xbox_output_height = 1080;
static bool s_display_mode_query_failed = false;
static bool s_using_display_resolution = false;
static bool s_output_resolution_logged = false;

static void InitOutputResolution()
{
	u32 max_width = 1920;
	u32 max_height = 1080;

	GAMING_DEVICE_MODEL_INFORMATION info = {};
	GetGamingDeviceModelInformation(&info);

	if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
	{
		switch (info.deviceId)
		{
			// The Xbox One S can output to 4K but I don't have one to test on so we'll limit it to 1080p like the original Xbox One.
			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
				max_width = 1920;
				max_height = 1080;
				break;

			// The Series S can output to 4K but if you try to make a 4K Swap Chain the application will crash,
			// so we limit it to 1440p.
			case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
				max_width = 2560;
				max_height = 1440;
				break;

			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
			case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
			case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
			case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
			default:
				max_width = 3840;
				max_height = 2160;
				break;
		}
	}

	u32 display_width = 0;
	u32 display_height = 0;

	if (winrt::try_get_activation_factory<winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation>())
	{
		const HdmiDisplayInformation hdi = HdmiDisplayInformation::GetForCurrentView();
		if (hdi)
		{
			display_width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
			display_height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
		}
	}
	else
	{
		s_display_mode_query_failed = true;
	}

	if (display_width > 0 && display_height > 0)
	{
		s_using_display_resolution = true;
		s_xbox_output_width = std::min(display_width, max_width);
		s_xbox_output_height = std::min(display_height, max_height);
	}
	else
	{
		s_using_display_resolution = false;
		s_xbox_output_width = max_width;
		s_xbox_output_height = max_height;
	}
}

static void GetOutputSize(u32* width, u32* height)
{
	*width = s_xbox_output_width;
	*height = s_xbox_output_height;

	if (!s_output_resolution_logged)
	{
		s_output_resolution_logged = true;
		if (s_display_mode_query_failed)
		{
			Console.WriteLnFmt("UWP: Failed to get display mode for {}, using device maximum: {}x{}",
				GetConsoleModelString(), *width, *height);
		}

		if (s_using_display_resolution)
		{
			Console.WriteLnFmt("UWP: Using display resolution for {}: {}x{}",
				GetConsoleModelString(), *width, *height);
		}
		else
		{
			Console.WriteLnFmt("UWP: Using device maximum resolution for {}: {}x{}",
				GetConsoleModelString(), *width, *height);
		}
	}
}

namespace WinRTHost
{
	static bool InitializeConfig();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void QueueCPUEvent(std::function<void()> function);
	static void ProcessEventQueue();
	static void IdleEventLoop();
} // namespace WinRTHost

static std::unique_ptr<INISettingsInterface> s_settings_interface;
static std::unique_ptr<INISettingsInterface> s_secrets_settings_interface;
static std::atomic_bool s_reopen_fullscreen_ui = false;
static bool s_save_state_on_shutdown = false;

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

bool WinRTHost::InitializeConfig()
{
	Error error;

	EmuFolders::SetAppRoot();

	if (!EmuFolders::SetResourcesDirectory())
		return false;

	if (!EmuFolders::SetDataDirectory(&error))
	{
		Console.ErrorFmt("Failed to create data directory '{}': {}", EmuFolders::DataRoot, error.GetDescription());
		return false;
	}

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	ImGuiManager::SetFontPath(Path::Combine(EmuFolders::Resources, "fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf"));

	std::string path(Path::Combine(EmuFolders::Settings, "PCSX2.ini"));
	const bool settings_exists = FileSystem::FileExists(path.c_str());
	Console.WriteLn("Loading config from %s.", path.c_str());
	s_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
	Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());

	if (!settings_exists || !s_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
	{
		VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);

		// Enable vsync
		// s_settings_interface->SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
		s_settings_interface->SetIntValue("EmuCore/GS", "VsyncEnable", 1);

		auto lock = Host::GetSettingsLock();
		if (!s_settings_interface->Save(&error))
		{
			Console.ErrorFmt("Failed to save settings '{}': {}", s_settings_interface->GetFileName(), error.GetDescription());
			return false;
		}
	}

	const std::string secrets_path(Path::Combine(EmuFolders::Settings, "secrets.ini"));
	const bool secrets_settings_exists = FileSystem::FileExists(secrets_path.c_str());
	Console.WriteLn("Loading secrets from %s.", secrets_path.c_str());
	s_secrets_settings_interface = std::make_unique<INISettingsInterface>(secrets_path);
	Host::Internal::SetSecretsSettingsLayer(s_secrets_settings_interface.get());
	if (!secrets_settings_exists || !s_secrets_settings_interface->Load())
	{
		if (!s_secrets_settings_interface->Save(&error))
		{
			Console.ErrorFmt("Failed to save secrets '{}': {}", s_secrets_settings_interface->GetFileName(), error.GetDescription());
			return false;
		}
	}

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return WinRTHost::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
	s_reopen_fullscreen_ui.store(true, std::memory_order_release);
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
	m_event_cv.notify_one();
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	Host::RunOnCPUThread([reason]() {
		VMManager::SetPaused(true);
		FullscreenUI::SetAchievementsLoginReason(reason);
	});
}

void Host::OnAchievementsLoginSuccess(char const* display_name, u32 points, u32 sc_points, u32 unread_msg)
{
}

#ifdef ENABLE_ACHIEVEMENTS
void Host::OnAchievementsRefreshed()
{
}
#endif

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
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
	WinRTHost::ProcessEventQueue();
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (block && std::this_thread::get_id() == s_cpu_thread_id)
	{
		function();
		return;
	}

	if (!block)
	{
		WinRTHost::QueueCPUEvent(std::move(function));
		return;
	}

	std::condition_variable cv;
	std::mutex cv_mutex;
	bool done = false;
	WinRTHost::QueueCPUEvent([fn = std::move(function), &cv, &cv_mutex, &done]() {
		fn();
		{
			std::lock_guard<std::mutex> lk2(cv_mutex);
			done = true;
		}
		cv.notify_one();
	});
	std::unique_lock<std::mutex> lk(cv_mutex);
	cv.wait(lk, [&done]() { return done; });
}

void Host::RunOnGSThread(std::function<void()> function)
{
	RunOnCPUThread([fn = std::move(function)]() { MTGS::RunOnGSThread(std::move(fn)); });
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	if (b_gamescan_active)
		return;

	b_gamescan_active = true;
	s_gamescanner_thread = std::thread([invalidate_cache]() {
		GameList::Refresh(invalidate_cache, false);
		b_gamescan_active = false;
	});
	s_gamescanner_thread.detach();
}

void Host::CancelGameListRefresh()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
	s_running = false;
	m_event_cv.notify_one();
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	if (!VMManager::HasValidVM())
		return;

	s_save_state_on_shutdown = allow_save_state && default_save_state;
	VMManager::SetState(VMState::Stopping);
	m_event_cv.notify_one();
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

void Host::CommitBaseSettingChanges()
{
	auto lock = Host::GetSettingsLock();
	if (!s_settings_interface->Save())
		Console.Error("Failed to save settings.");
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	{
		auto lock = Host::GetSettingsLock();
		VMManager::SetDefaultSettings(*s_settings_interface.get(), folders, core, controllers, hotkeys, ui);
	}
	Host::CommitBaseSettingChanges();

	if (VMManager::HasValidVM())
		VMManager::ApplySettings();
	if (folders)
		VMManager::Internal::UpdateEmuFolders();

	return true;
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
	winrt::Windows::Foundation::Uri m_uri{winrt::to_hstring(url)};
	auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
	asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
								 winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
		return;
	});
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

void Host::BeginTextInput()
{
	winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView().TryShowPrimaryView();
}

void Host::EndTextInput()
{
	winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView().TryHide();
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return WinRTHost::GetPlatformWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
	if (VMManager::HasValidVM() || FullscreenUI::HasActiveWindow())
	{
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD,
			fmt::format("Controller {} connected.", identifier),
			Host::OSD_INFO_DURATION);
	}
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, const std::string_view identifier)
{
	if (VMManager::GetState() == VMState::Running &&
		Host::GetBoolSettingValue("UI", "PauseOnControllerDisconnection", false) &&
		InputManager::HasAnyBindingsForSource(key))
	{
		std::string message = fmt::format("System paused because controller {} was disconnected.", identifier);
		Host::RunOnCPUThread([message]() { VMManager::SetPaused(true); });
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD, std::move(message),
			Host::OSD_WARNING_DURATION);
	}
	else if (VMManager::HasValidVM() || FullscreenUI::HasActiveWindow())
	{
		Host::AddIconOSDMessage(fmt::format("controller_connected_{}", identifier), ICON_FA_GAMEPAD,
			fmt::format("Controller {} disconnected.", identifier),
			Host::OSD_INFO_DURATION);
	}
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

bool Host::LocaleCircleConfirm()
{
	using namespace winrt::Windows::Globalization;
	std::wstring currentLanguage = Language::CurrentInputMethodLanguageTag().c_str();
	return currentLanguage.rfind(L"ja", 0) == 0 ||
	       currentLanguage.rfind(L"zh", 0) == 0 ||
	       currentLanguage.rfind(L"ko", 0) == 0;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return UWPKeyboard::NameToCode(str);
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return UWPKeyboard::CodeToName(code);
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return UWPKeyboard::CodeToIcon(code);
}

std::optional<WindowInfo> WinRTHost::GetPlatformWindowInfo()
{
	WindowInfo wi;

	if (s_corewind)
	{
		u32 width = 0;
		u32 height = 0;
		GetOutputSize(&width, &height);

		wi.surface_width = width;
		wi.surface_height = height;
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::WinRT;
		wi.surface_handle = reinterpret_cast<void*>(winrt::get_abi(*s_corewind));

		if (winrt::try_get_activation_factory<winrt::Windows::Graphics::Display::Core::HdmiDisplayInformation>())
		{
			const HdmiDisplayInformation hdi = HdmiDisplayInformation::GetForCurrentView();
			if (hdi)
			{
				const HdmiDisplayMode mode = hdi.GetCurrentDisplayMode();
				wi.surface_refresh_rate = static_cast<float>(mode.RefreshRate());
			}
		}
		if (wi.surface_refresh_rate <= 0.0f)
			wi.surface_refresh_rate = 60.0f;
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}

	return wi;
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, 2, count_str.view());
	}

	return ret;
}

void WinRTHost::QueueCPUEvent(std::function<void()> function)
{
	{
		std::lock_guard<std::mutex> lk(m_event_mutex);
		m_event_queue.push_back(std::move(function));
	}
	m_event_cv.notify_one();
}

void WinRTHost::ProcessEventQueue()
{
	while (true)
	{
		std::function<void()> fn;
		{
			std::lock_guard<std::mutex> lk(m_event_mutex);
			if (m_event_queue.empty())
				break;
			fn = std::move(m_event_queue.front());
			m_event_queue.pop_front();
		}
		try
		{
			fn();
		}
		catch (const std::system_error& e)
		{
			Console.Error("UWP: CPU thread callback threw std::system_error: %s (%s)", e.what(), e.code().message().c_str());
		}
		catch (const std::exception& e)
		{
			Console.Error("UWP: CPU thread callback threw: %s", e.what());
		}
		catch (...)
		{
			Console.Error("UWP: CPU thread callback threw unknown exception");
		}
	}
}

void WinRTHost::IdleEventLoop()
{
	while (s_running)
	{
		ProcessEventQueue();
		VMManager::IdlePollUpdate();

		const VMState state = VMManager::GetState();
		if (state != VMState::Shutdown && state != VMState::Paused)
			return;

		std::unique_lock<std::mutex> lk(m_event_mutex);
		m_event_cv.wait_for(lk, std::chrono::milliseconds(8), []() {
			return !m_event_queue.empty() || !s_running;
		});
	}
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
	std::thread m_cpu_thread;
	winrt::hstring m_launchOnExit;
	std::string m_bootPath;

	IFrameworkView CreateView()
	{
		return *this;
	}

	void Initialize(CoreApplicationView const& v)
	{
		v.Activated({this, &App::OnActivate});

		namespace WGI = winrt::Windows::Gaming::Input;

		const char* error;
		if (!VMManager::PerformEarlyHardwareChecks(&error))
		{
			return;
		}


		if (!WinRTHost::InitializeConfig())
		{
			Console.Error("Failed to initialize config.");
			return;
		}


		if (winrt::try_get_activation_factory<WGI::RawGameController>())
		{
			WGI::RawGameController::RawGameControllerAdded(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					WinRTHost::QueueCPUEvent([]() { InputManager::ReloadDevices(); });
				});

			WGI::RawGameController::RawGameControllerRemoved(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					WinRTHost::QueueCPUEvent([]() { InputManager::ReloadDevices(); });
				});
		}
	}

	void OnActivate(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&,
		const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs& args)
	{
		std::stringstream filePath;

		if (args.Kind() == Windows::ApplicationModel::Activation::ActivationKind::Protocol)
		{
			auto protocolActivatedEventArgs{
				args.as<Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>()};
			auto query = protocolActivatedEventArgs.Uri().QueryParsed();

			for (uint32_t i = 0; i < query.Size(); i++)
			{
				auto arg = query.GetAt(i);

				// parse command line string
				if (arg.Name() == winrt::hstring(L"cmd"))
				{
					std::string argVal = winrt::to_string(arg.Value());

					// Strip the executable from the cmd argument
					if (argVal.rfind("xbsx2.exe", 0) == 0)
					{
						argVal = argVal.substr(10, argVal.length());
					}

					std::istringstream iss(argVal);
					std::string s;

					// Maintain slashes while reading the quotes
					while (iss >> std::quoted(s, '"', (char)0))
					{
						filePath << s;
					}
				}
				else if (arg.Name() == winrt::hstring(L"launchOnExit"))
				{
					// For if we want to return to a frontend
					m_launchOnExit = arg.Value();
				}
			}
		}

		std::string gamePath = filePath.str();
		if (!gamePath.empty() && gamePath != "")
		{
			WinRTHost::QueueCPUEvent([gamePath]() {
				VMBootParameters params{};
				params.filename = gamePath;
				params.source_type = CDVD_SourceType::Iso;

				if (VMManager::HasValidVM())
				{
					return;
				}

				if (VMManager::Initialize(std::move(params)) != VMBootResult::StartupSuccess)
				{
					return;
				}

				if (!Host::GetBoolSettingValue("UI", "StartPaused", false))
					VMManager::SetState(VMState::Running);
				else
					VMManager::SetPaused(true);

				MTGS::WaitForOpen();
				InputManager::ReloadDevices();
			});
		}
	}

	void Load(hstring const&)
	{
	}

	void Uninitialize()
	{
	}

	const void Run()
	{
		s_cpu_thread_id = std::this_thread::get_id();

		InitOutputResolution();

		CoreWindow window = CoreWindow::GetForCurrentThread();
		window.Activate();
		s_corewind = &window;

		auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
		navigation.BackRequested(
			[](const winrt::Windows::Foundation::IInspectable&,
				const winrt::Windows::UI::Core::BackRequestedEventArgs& args) { args.Handled(true); });

		VMManager::Internal::CPUThreadInitialize();

		WinRTHost::ProcessEventQueue();
		if (VMManager::GetState() != VMState::Running)
		{
			GameList::Refresh(false);
			ImGuiManager::InitializeFullscreenUI();

			MTGS::WaitForOpen();
		}

		window.Dispatcher().RunAsync(CoreDispatcherPriority::Normal, []() {
			Sleep(500);
			Host::RunOnCPUThread([]() { InputManager::ReloadDevices(); });
		});

		while (s_running)
		{
			switch (VMManager::GetState())
			{
				case VMState::Initializing:
					pxFailRel("Shouldn't be in the starting state state");
					break;

				case VMState::Shutdown:
					if (s_reopen_fullscreen_ui.exchange(false, std::memory_order_acq_rel) && !MTGS::IsOpen())
					{
						ImGuiManager::InitializeFullscreenUI();
						MTGS::WaitForOpen();
					}
					WinRTHost::IdleEventLoop();
					break;

				case VMState::Paused:
					WinRTHost::IdleEventLoop();
					break;

				case VMState::Running:
					WinRTHost::ProcessEventQueue();
					VMManager::Execute();
					break;

				case VMState::Resetting:
					VMManager::Reset();
					break;

				case VMState::Stopping:
					VMManager::Shutdown(s_save_state_on_shutdown);
					s_save_state_on_shutdown = false;
					WinRTHost::ProcessEventQueue();
					if (!MTGS::IsOpen())
					{
						ImGuiManager::InitializeFullscreenUI();
						MTGS::WaitForOpen();
					}
					break;

				default:
					break;
			}
		}

		if (!m_launchOnExit.empty())
		{
			winrt::Windows::Foundation::Uri m_uri{m_launchOnExit};
			auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
			asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
										 winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
				VMManager::Internal::CPUThreadShutdown();
				CoreApplication::Exit();
				return;
			});
		}
		else
		{
			VMManager::Internal::CPUThreadShutdown();
			CoreApplication::Exit();
		}
	}

	void SetWindow(CoreWindow const& window)
	{
		window.CharacterReceived({this, &App::OnKeyInput});
		window.KeyDown({this, &App::OnKeyDown});
		window.KeyUp({this, &App::OnKeyUp});
		window.PointerMoved({this, &App::OnPointerMoved});
		window.PointerPressed({this, &App::OnPointerPressed});
		window.PointerReleased({this, &App::OnPointerReleased});
		window.PointerWheelChanged({this, &App::OnPointerWheelChanged});
		window.SizeChanged({this, &App::OnSizeChanged});
	}

	void OnKeyInput(const IInspectable&, const winrt::Windows::UI::Core::CharacterReceivedEventArgs& args)
	{
		if (args.KeyCode() == L'\b')
			return;

		if (ImGuiManager::WantsTextInput())
		{
			MTGS::RunOnGSThread([c = args.KeyCode()]() {
				if (!ImGui::GetCurrentContext())
					return;

				ImGui::GetIO().AddInputCharacter(c);
			});
		}
	}

	void OnKeyDown(const IInspectable&, const winrt::Windows::UI::Core::KeyEventArgs& args)
	{
		if (args.KeyStatus().WasKeyDown)
			return;

		const u32 key = static_cast<u32>(args.VirtualKey());
		// This is needed or else it will cause new binds to break
		if (!UWPKeyboard::IsValidKey(key))
			return;

		Host::RunOnCPUThread([key]() {
			InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), 1.0f);
		});
	}

	void OnKeyUp(const IInspectable&, const winrt::Windows::UI::Core::KeyEventArgs& args)
	{
		const u32 key = static_cast<u32>(args.VirtualKey());
		// This is needed or else it will cause new binds to break
		if (!UWPKeyboard::IsValidKey(key))
			return;

		Host::RunOnCPUThread([key]() {
			InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), 0.0f);
		});
	}

	void OnPointerMoved(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		const auto pos = args.CurrentPoint().Position();
		const float scale = GetDisplayScale();
		const float x = pos.X * scale;
		const float y = pos.Y * scale;

		Host::RunOnCPUThread([x, y]() {
			InputManager::UpdatePointerAbsolutePosition(0, x, y);
		});
	}

	void OnPointerButton(const winrt::Windows::UI::Core::PointerEventArgs& args, bool pressed)
	{
		using winrt::Windows::UI::Input::PointerUpdateKind;
		const PointerUpdateKind kind = args.CurrentPoint().Properties().PointerUpdateKind();

		u32 button = 0;
		switch (kind)
		{
			case PointerUpdateKind::LeftButtonPressed:
			case PointerUpdateKind::LeftButtonReleased:
				button = 0;
				break;
			case PointerUpdateKind::RightButtonPressed:
			case PointerUpdateKind::RightButtonReleased:
				button = 1;
				break;
			case PointerUpdateKind::MiddleButtonPressed:
			case PointerUpdateKind::MiddleButtonReleased:
				button = 2;
				break;
			default:
				return;
		}

		Host::RunOnCPUThread([button, pressed]() {
			InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), pressed ? 1.0f : 0.0f);
		});
	}

	void OnPointerPressed(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		OnPointerButton(args, true);
	}

	void OnPointerReleased(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		OnPointerButton(args, false);
	}

	void OnPointerWheelChanged(const IInspectable&, const winrt::Windows::UI::Core::PointerEventArgs& args)
	{
		const float wheel_delta = static_cast<float>(args.CurrentPoint().Properties().MouseWheelDelta()) / 120.0f;
		if (wheel_delta == 0.0f)
			return;

		Host::RunOnCPUThread([wheel_delta]() {
			InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, wheel_delta);
		});
	}
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	SDL_SetMainReady();

	winrt::init_apartment();

	CoreApplication::Run(make<App>());

	winrt::uninit_apartment();
}