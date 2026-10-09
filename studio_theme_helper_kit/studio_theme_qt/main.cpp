// Studio NVDA Native Accessibility Mod for Roblox Studio (RML)
// Captures native Qt shell widgets, menus, tooltips, and connects directly to NVDA.

#if defined(_MSC_VER)
#pragma warning(disable: 4834)
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <RobloxModLoader/logger/logger.hpp>
#include <RobloxModLoader/luau/luau_bridge.hpp>
#include <RobloxModLoader/luau/script_runtime.hpp>
#include <RobloxModLoader/mod/mod_base.hpp>
#include <RobloxModLoader/qt/qapplication.hpp>
#include <RobloxModLoader/qt/qobject.hpp>
#include <RobloxModLoader/qt/qt_integration.hpp>
#include <RobloxModLoader/qt/qstring.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rml::luau;

namespace
{

#ifdef _WIN32
std::filesystem::path self_directory()
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&self_directory), &module))
	{
		return {};
	}
	wchar_t buffer[MAX_PATH]{};
	DWORD len = GetModuleFileNameW(module, buffer, MAX_PATH);
	if (len == 0 || len == MAX_PATH) return {};
	return std::filesystem::path(buffer).parent_path();
}

static bool safe_invoke_getter(void* getter_fn, const void* self, void* ret_storage)
{
	__try
	{
		using Getter = void* (*)(const void*, void*);
		reinterpret_cast<Getter>(getter_fn)(self, ret_storage);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static bool safe_invoke_method(void* method_fn, const void* self, void* ret_storage, int arg)
{
	__try
	{
		using Method = void* (*)(const void*, void*, int);
		reinterpret_cast<Method>(method_fn)(self, ret_storage, arg);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}
#endif

// NVDA Controller Client Dynamic Wrapper
class NvdaController
{
	HMODULE m_dll = nullptr;
	using TestFn = error_status_t(__stdcall*)(void);
	using SpeakFn = error_status_t(__stdcall*)(const wchar_t*);
	using CancelFn = error_status_t(__stdcall*)(void);
	using BrailleFn = error_status_t(__stdcall*)(const wchar_t*);

	TestFn m_test = nullptr;
	SpeakFn m_speak = nullptr;
	CancelFn m_cancel = nullptr;
	BrailleFn m_braille = nullptr;

public:
	NvdaController() = default;

	bool init()
	{
		if (m_dll) return true;
		std::vector<std::filesystem::path> searchPaths = {
			self_directory() / L"nvdaControllerClient64.dll",
			self_directory() / L"nvdaControllerClient.dll",
			std::filesystem::current_path() / L"nvdaControllerClient64.dll",
			L"nvdaControllerClient64.dll",
			L"nvdaControllerClient.dll"
		};

		for (const auto& path : searchPaths)
		{
			m_dll = LoadLibraryW(path.c_str());
			if (m_dll) break;
		}

		if (!m_dll) return false;

		m_test = reinterpret_cast<TestFn>(GetProcAddress(m_dll, "nvdaController_testIfRunning"));
		m_speak = reinterpret_cast<SpeakFn>(GetProcAddress(m_dll, "nvdaController_speakText"));
		m_cancel = reinterpret_cast<CancelFn>(GetProcAddress(m_dll, "nvdaController_cancelSpeech"));
		m_braille = reinterpret_cast<BrailleFn>(GetProcAddress(m_dll, "nvdaController_brailleMessage"));

		return m_speak != nullptr;
	}

	~NvdaController()
	{
		if (m_dll) FreeLibrary(m_dll);
	}

	bool is_running() const
	{
		return (m_test && m_test() == 0);
	}

	void speak(const std::wstring& text, bool cancel_previous = true)
	{
		if (!m_speak || text.empty() || !is_running()) return;
		if (cancel_previous && m_cancel) m_cancel();
		m_speak(text.c_str());
	}

	void cancel()
	{
		if (m_cancel && is_running()) m_cancel();
	}

	void braille(const std::wstring& text)
	{
		if (m_braille && !text.empty() && is_running()) m_braille(text.c_str());
	}
};

// Qt widget text extraction helpers
class QtInspector
{
	void* m_fnFocusWidget = nullptr;
	void* m_fnWindowTitle = nullptr;
	void* m_fnToolTip = nullptr;
	void* m_fnButtonText = nullptr;
	void* m_fnLabelText = nullptr;
	void* m_fnLineEditText = nullptr;
	void* m_fnPlaceholderText = nullptr;
	void* m_fnParentWidget = nullptr;
	void* m_fnActiveAction = nullptr;
	void* m_fnActionText = nullptr;

public:
	QtInspector()
	{
		HMODULE hWidgets = GetModuleHandleW(L"Qt5Widgets.dll");
		if (!hWidgets) return;

		m_fnFocusWidget = GetProcAddress(hWidgets, "?focusWidget@QApplication@@SAPEAVQWidget@@XZ");
		m_fnWindowTitle = GetProcAddress(hWidgets, "?windowTitle@QWidget@@QEBA?AVQString@@XZ");
		m_fnToolTip = GetProcAddress(hWidgets, "?toolTip@QWidget@@QEBA?AVQString@@XZ");
		m_fnButtonText = GetProcAddress(hWidgets, "?text@QAbstractButton@@QEBA?AVQString@@XZ");
		m_fnLabelText = GetProcAddress(hWidgets, "?text@QLabel@@QEBA?AVQString@@XZ");
		m_fnLineEditText = GetProcAddress(hWidgets, "?text@QLineEdit@@QEBA?AVQString@@XZ");
		m_fnPlaceholderText = GetProcAddress(hWidgets, "?placeholderText@QLineEdit@@QEBA?AVQString@@XZ");
		m_fnParentWidget = GetProcAddress(hWidgets, "?parentWidget@QWidget@@QEBAPEAV1@XZ");
		m_fnActiveAction = GetProcAddress(hWidgets, "?activeAction@QMenu@@QEBAPEAVQAction@@XZ");
		m_fnActionText = GetProcAddress(hWidgets, "?text@QAction@@QEBA?AVQString@@XZ");
	}

	rml::qt::QWidget* get_focus_widget() const
	{
		if (!m_fnFocusWidget) return nullptr;
		using Fn = rml::qt::QWidget* (*)(void);
		return reinterpret_cast<Fn>(m_fnFocusWidget)();
	}

	rml::qt::QWidget* get_parent(rml::qt::QWidget* w) const
	{
		if (!m_fnParentWidget || !w) return nullptr;
		using Fn = rml::qt::QWidget* (*)(const void*);
		return reinterpret_cast<Fn>(m_fnParentWidget)(w);
	}

	std::string get_string_property(void* fn, const void* obj) const
	{
		if (!fn || !obj) return {};
		rml::qt::QString qstr;
		if (safe_invoke_getter(fn, obj, qstr.storage()))
		{
			return qstr.to_utf8();
		}
		return {};
	}

	std::string get_menu_action_text(rml::qt::QWidget* menuWidget) const
	{
		if (!menuWidget || !m_fnActiveAction || !m_fnActionText) return {};
		using FnAction = void* (*)(const void*);
		void* action = reinterpret_cast<FnAction>(m_fnActiveAction)(menuWidget);
		if (!action) return {};
		return get_string_property(m_fnActionText, action);
	}

	std::string describe_widget(rml::qt::QWidget* w) const
	{
		if (!w) return {};
		const char* rawClass = w->class_name();
		std::string_view cls = rawClass ? rawClass : "";

		std::string role = "";
		std::string label = "";

		// Menus
		if (cls.find("Menu") != std::string_view::npos && cls.find("MenuBar") == std::string_view::npos)
		{
			std::string actionText = get_menu_action_text(w);
			if (!actionText.empty()) return actionText + ", menu item";
			return "Menu";
		}

		// Text input / Search fields
		if (cls.find("LineEdit") != std::string_view::npos || cls.find("Search") != std::string_view::npos || cls.find("Filter") != std::string_view::npos)
		{
			role = "edit box";
			label = get_string_property(m_fnLineEditText, w);
			if (label.empty()) label = get_string_property(m_fnPlaceholderText, w);
		}
		// Buttons
		else if (cls.find("Button") != std::string_view::npos)
		{
			role = "button";
			label = get_string_property(m_fnButtonText, w);
		}
		// Labels
		else if (cls.find("Label") != std::string_view::npos)
		{
			role = "label";
			label = get_string_property(m_fnLabelText, w);
		}
		// Tabs
		else if (cls.find("TabBar") != std::string_view::npos)
		{
			role = "tab bar";
		}
		// Trees (Explorer / Properties)
		else if (cls.find("Tree") != std::string_view::npos)
		{
			role = "tree view";
		}
		// Output & text areas
		else if (cls.find("TextEdit") != std::string_view::npos || cls.find("Output") != std::string_view::npos)
		{
			role = "output log";
		}

		if (label.empty())
		{
			label = get_string_property(m_fnWindowTitle, w);
		}
		if (label.empty())
		{
			label = get_string_property(m_fnToolTip, w);
		}

		// Look up parent panel hierarchy for context
		std::string context = "";
		rml::qt::QWidget* parent = get_parent(w);
		int depth = 0;
		while (parent && depth < 5)
		{
			std::string title = get_string_property(m_fnWindowTitle, parent);
			if (!title.empty())
			{
				context = title;
				break;
			}
			parent = get_parent(parent);
			depth++;
		}

		std::string result = "";
		if (!label.empty()) result += label + " ";
		if (!role.empty()) result += role + " ";
		if (!context.empty() && context != label) result += "in " + context;

		return result;
	}
};

std::wstring to_wide(const std::string& utf8)
{
	if (utf8.empty()) return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (len <= 0) return {};
	std::wstring wstr(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wstr.data(), len);
	if (!wstr.empty() && wstr.back() == L'\0') wstr.pop_back();
	return wstr;
}

} // namespace

class studio_nvda final : public ModBase
{
	std::shared_ptr<spdlog::logger> m_log;
	NvdaController m_nvda;
	std::unique_ptr<QtInspector> m_qt;
	std::atomic<bool> m_stop{false};
	std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
	rml::qt::QWidget* m_lastFocusWidget = nullptr;
	std::string m_lastSpokenText = "";

public:
	studio_nvda()
	{
		name = "Studio NVDA Accessibility";
		version = "1.0.0";
		author = "Accessibility";
		description = "Direct NVDA screen reader integration for Roblox Studio native Qt and Luau UI";
		m_log = rml::Logger::get_logger("StudioNVDA");
	}

	void on_load() override
	{
		m_log->info("Studio NVDA mod loading...");
		if (m_nvda.init())
		{
			m_log->info("Connected directly to nvdaControllerClient. NVDA running: {}", m_nvda.is_running());
		}
		else
		{
			m_log->warn("Could not find nvdaControllerClient64.dll. Place it in mods/studio_nvda/native/");
		}

		m_qt = std::make_unique<QtInspector>();

		register_bridge();
		start_focus_watcher();
		start_keyboard_silence_hook();
	}

	void on_script_manager_load() override
	{
		register_bridge();
	}

	void on_unload() override
	{
		m_stop = true;
		m_alive->store(false);
		m_nvda.cancel();
		m_log->info("Studio NVDA unloaded.");
	}

private:
	void register_bridge()
	{
		auto* runtime = script_runtime();
		if (!runtime) return;
		auto& bridge = runtime->bridge();

		auto r1 = bridge.register_function("nvda", "speak", [this](const BridgeArgs& args) -> BridgeArgs {
			if (args.empty()) return {false};
			if (const auto* text = std::get_if<std::string>(&args[0]))
			{
				bool cancel = true;
				if (args.size() > 1)
				{
					if (const auto* b = std::get_if<bool>(&args[1])) cancel = *b;
				}
				m_nvda.speak(to_wide(*text), cancel);
				return {true};
			}
			return {false};
		});
		(void)r1;

		auto r2 = bridge.register_function("nvda", "cancel", [this](const BridgeArgs&) -> BridgeArgs {
			m_nvda.cancel();
			return {true};
		});
		(void)r2;

		auto r3 = bridge.register_function("nvda", "is_running", [this](const BridgeArgs&) -> BridgeArgs {
			return {m_nvda.is_running()};
		});
		(void)r3;

		auto r4 = bridge.register_function("nvda", "braille", [this](const BridgeArgs& args) -> BridgeArgs {
			if (!args.empty())
			{
				if (const auto* text = std::get_if<std::string>(&args[0]))
				{
					m_nvda.braille(to_wide(*text));
					return {true};
				}
			}
			return {false};
		});
		(void)r4;
	}

	void start_focus_watcher()
	{
		std::thread([this, alive = m_alive]() {
			while (alive->load() && !m_stop)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(80));
				if (!alive->load() || m_stop) return;

				auto task = [this, alive]() {
					if (!alive->load()) return;
					rml::qt::QWidget* current = m_qt->get_focus_widget();
					if (current && current != m_lastFocusWidget)
					{
						m_lastFocusWidget = current;
						std::string desc = m_qt->describe_widget(current);
						if (!desc.empty() && desc != m_lastSpokenText)
						{
							m_lastSpokenText = desc;
							m_nvda.speak(to_wide(desc), true);
						}
					}
				};

				try
				{
					if (auto* qt = rml::qt::QtIntegration::instance())
					{
						qt->run_on_gui_thread(task);
					}
				}
				catch (...) {}
			}
		}).detach();
	}

	void start_keyboard_silence_hook()
	{
		// Global screen reader standard: tapping Control silences NVDA instantly
		std::thread([this, alive = m_alive]() {
			bool wasPressed = false;
			while (alive->load() && !m_stop)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
				if (!alive->load() || m_stop) return;

				SHORT state = GetAsyncKeyState(VK_CONTROL);
				bool isPressed = (state & 0x8000) != 0;
				if (isPressed && !wasPressed)
				{
					m_nvda.cancel();
				}
				wasPressed = isPressed;
			}
		}).detach();
	}
};

extern "C"
{
	RML_MOD_ABI_EXPORT ModBase* start_mod()
	{
		return new studio_nvda();
	}

	RML_MOD_ABI_EXPORT void uninstall_mod(const ModBase* mod)
	{
		delete mod;
	}
}

#ifndef RML_EXPORT_MOD_ABI_VERSION
#ifdef RML_ABI_VERSION
#define RML_EXPORT_MOD_ABI_VERSION() \
	extern "C" RML_MOD_ABI_EXPORT int rml_mod_abi_version() { return RML_ABI_VERSION; }
#else
#define RML_EXPORT_MOD_ABI_VERSION()
#endif
#endif

RML_EXPORT_MOD_ABI_VERSION()
