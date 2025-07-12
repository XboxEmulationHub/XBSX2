#include "pcsx2/PrecompiledHeader.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>
#include <gamingdeviceinformation.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sstream>
#include <string>

#include "fmt/core.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/PAD/PAD.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/GameList.h"

#ifdef ENABLE_ACHIEVEMENTS
#include "pcsx2/Achievements.h"
#endif

#include "3rdparty/imgui/include/imgui.h"

using namespace winrt;

using namespace Windows;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Graphics::Display::Core;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Composition;

static winrt::Windows::UI::Core::CoreWindow* s_corewind = NULL;
static std::mutex m_event_mutex;
static std::deque<std::function<void()>> m_event_queue;
static bool s_running = true;
static std::thread s_gamescanner_thread;
std::atomic<bool> b_gamescan_active = false;


namespace WinRTHost
{
	static bool InitializeConfig();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void ProcessEventQueue();
} // namespace WinRTHost

static std::unique_ptr<INISettingsInterface> s_settings_interface;

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

bool WinRTHost::InitializeConfig()
{
	// Taken from gsrunner
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;

	ImGuiManager::SetFontPath(Path::Combine(EmuFolders::Resources, "fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf"));

	const std::string path(Path::Combine(EmuFolders::Settings, "PCSX2.ini"));
	Console.WriteLn("Loading config from %s.", path.c_str());
	s_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
	Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());

	if (!s_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
	{
		VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);

		// Enable vsync
		//s_settings_interface->SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
		s_settings_interface->SetIntValue("EmuCore/GS", "VsyncEnable", true);


		auto lock = Host::GetSettingsLock();
		if (!s_settings_interface->Save())
			Console.Error("Failed to save settings.");
	}

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::CommitBaseSettingChanges()
{
	auto lock = Host::GetSettingsLock();
	if (!s_settings_interface->Save())
		Console.Error("Failed to save settings.");
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	{
		auto lock = Host::GetSettingsLock();
		VMManager::SetDefaultSettings(*s_settings_interface.get(), folders, core, controllers, hotkeys, ui);
	}

	Host::CommitBaseSettingChanges();

	return true;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}


void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	Host::RunOnCPUThread([reason]() {
		VMManager::SetPaused(true);
		FullscreenUI::SetAchievementsLoginReason(reason);
		FullscreenUI::DrawAchievementsLoginWindow();
	});
} 

void Host::OnAchievementsLoginSuccess(char const* display_name, u32 points, u32 sc_points, u32 unread_msg)
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::SetMouseMode(bool relative, bool hide_cursor)
{
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
}

bool Host::ConfirmMessage(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
	{
		Console.Error(
			"ConfirmMessage: %.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
	}
	else if (!message.empty())
	{
		Console.Error("ConfirmMessage: %.*s", static_cast<int>(message.size()), message.data());
	}

	return true;
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
	Host::AddKeyedOSDMessage(fmt::format("{} connected.", identifier), fmt::format("{} connected.", identifier), 5.0f);
}

void Host::OnInputDeviceDisconnected(InputBindingKey key, const std::string_view identifier)
{
	Host::AddKeyedOSDMessage(fmt::format("{} connected.", identifier), fmt::format("{} disconnected.", identifier), 5.0f);
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
	VMManager::Internal::VSyncOnCPUThread;
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
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
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

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	std::unique_lock<std::mutex> lk(m_event_mutex);
	m_event_queue.push_back(function);
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	// Could queue up scans but this seems to help prevent crashes
	if (!b_gamescan_active)
	{
		s_gamescanner_thread = std::thread([invalidate_cache]() {
			b_gamescan_active = true;
			GameList::Refresh(invalidate_cache, false);
			b_gamescan_active = false;
		});
		s_gamescanner_thread.detach();
	}
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

bool Host::InNoGUIMode() // taken from gsrunner impl
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

void Host::RequestExitApplication(bool allow_confirm)
{
	s_running = false;
}

bool Host::LocaleCircleConfirm()
{
	using namespace winrt::Windows::Globalization;

	// Get the current input method language tag
	std::wstring currentLanguage = Language::CurrentInputMethodLanguageTag().c_str();

	// Check if the current language is Japanese, Chinese, or Korean
	bool isTargetLanguage = currentLanguage.rfind(L"ja", 0) == 0 ||
							currentLanguage.rfind(L"zh", 0) == 0 ||
							currentLanguage.rfind(L"ko", 0) == 0;

	return isTargetLanguage;
}

void Host::RequestExitBigPicture(void)
{
	// No escape bwahaha!
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::Shutdown(allow_save_state && default_save_state);
}

#ifdef ENABLE_ACHIEVEMENTS
void Host::OnAchievementsRefreshed()
{
}
#endif

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

std::optional<WindowInfo> WinRTHost::GetPlatformWindowInfo()
{
	WindowInfo wi;

	if (s_corewind)
	{
		u32 width = 1920, height = 1080;
		float scale = 1.0;
		GAMING_DEVICE_MODEL_INFORMATION info = {};
		GetGamingDeviceModelInformation(&info);
		if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
		{
			HdmiDisplayInformation hdi = HdmiDisplayInformation::GetForCurrentView();
			if (hdi)
			{
				width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
				height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
				// Our UI is based on 1080p, and we're adding a modifier to zoom in by 80%
				scale = ((float)width / 1920.0f) * 1.8f;
			}
		}

		wi.surface_width = width;
		wi.surface_height = height;
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::Win32;
		wi.surface_handle = reinterpret_cast<void*>(winrt::get_abi(*s_corewind));
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}

	return wi;
}

void Host::PumpMessagesOnCPUThread()
{
	WinRTHost::ProcessEventQueue();
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

// Taken from gsrunner impl
std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

// Taken from gsrunner impl
std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}
void WinRTHost::ProcessEventQueue()
{
	if (!m_event_queue.empty())
	{
		std::unique_lock lk(m_event_mutex);
		while (!m_event_queue.empty())
		{
			m_event_queue.front()();
			m_event_queue.pop_front();
		}
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


		try
		{
			WGI::RawGameController::RawGameControllerAdded(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					m_event_queue.push_back([]() {
						InputManager::ReloadDevices();
					});
				});

			WGI::RawGameController::RawGameControllerRemoved(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					m_event_queue.push_back([]() {
						InputManager::ReloadDevices();
					});
				});
		}
		catch (winrt::hresult_error)
		{
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
			std::unique_lock<std::mutex> lk(m_event_mutex);
			m_event_queue.push_back([gamePath]() {
				VMBootParameters params{};
				params.filename = gamePath;
				params.source_type = CDVD_SourceType::Iso;

				if (VMManager::HasValidVM())
				{
					return;
				}

				if (!VMManager::Initialize(std::move(params)))
				{
					return;
				}

				VMManager::SetState(VMState::Running);

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

	void Run()
	{
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
			InputManager::ReloadDevices();
		});

		while (s_running)
		{
			window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

			if (VMManager::HasValidVM())
			{
				switch (VMManager::GetState())
				{
					case VMState::Initializing:
						pxFailRel("Shouldn't be in the starting state state");
						break;

					case VMState::Paused:
						InputManager::PollSources();
						WinRTHost::ProcessEventQueue();
						break;

					case VMState::Running:
						VMManager::Execute();
						break;

					case VMState::Resetting:
						VMManager::Reset();
						break;

					case VMState::Stopping:
						WinRTHost::ProcessEventQueue();
						return;

					default:
						break;
				}
			}
			else
			{
				WinRTHost::ProcessEventQueue();
				InputManager::PollSources();
			}

			Sleep(1);
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
	}

	void OnKeyInput(const IInspectable&, const winrt::Windows::UI::Core::CharacterReceivedEventArgs& args)
	{
		if (ImGuiManager::WantsTextInput())
		{
			MTGS::RunOnGSThread([c = args.KeyCode()]() {
				if (!ImGui::GetCurrentContext())
					return;

				ImGui::GetIO().AddInputCharacter(c);
			});
		}
	}
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	SDL_SetMainReady();

	winrt::init_apartment();

	CoreApplication::Run(make<App>());

	winrt::uninit_apartment();
}