// Studio Theme Qt helper
//
// Native RML mod for styling Qt components and handling self-updates.
// Designed with isolated subsystems so the updater and bridge remain fully
// functional even if Qt integration or Studio styling encounters errors.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif
#endif

#include <RobloxModLoader/logger/logger.hpp>
#include <RobloxModLoader/luau/luau_bridge.hpp>
#include <RobloxModLoader/luau/script_runtime.hpp>
#include <RobloxModLoader/mod/mod_base.hpp>
#include <RobloxModLoader/qt/qapplication.hpp>
#include <RobloxModLoader/qt/qcolor.hpp>
#include <RobloxModLoader/qt/qfiledialog.hpp>
#include <RobloxModLoader/qt/qpainter.hpp>
#include <RobloxModLoader/qt/qpixmap.hpp>
#include <RobloxModLoader/qt/qrect.hpp>
#include <RobloxModLoader/qt/qstring.hpp>
#include <RobloxModLoader/qt/qobject.hpp>
#include <RobloxModLoader/qt/qt_integration.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>
#include <spdlog/spdlog.h>

#if __has_include(<RobloxModLoader/version.hpp>)
#include <RobloxModLoader/version.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace rml::luau;

namespace
{
#ifndef STUDIO_THEME_HELPER_BASE
#define STUDIO_THEME_HELPER_BASE "1.2"
#endif
#ifndef STUDIO_THEME_HELPER_VERSION
#define STUDIO_THEME_HELPER_VERSION STUDIO_THEME_HELPER_BASE ".0"
#endif
constexpr const char* kHelperVersion = STUDIO_THEME_HELPER_VERSION;

#ifdef RML_ABI_VERSION
constexpr int kBuiltAbi = RML_ABI_VERSION;
#else
constexpr int kBuiltAbi = 0;
#endif

constexpr const char* kSheetMarker = "/* studio_theme */";

#ifdef _WIN32
std::filesystem::path self_module_path()
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&self_module_path), &module))
	{
		return {};
	}
	wchar_t buffer[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
	if (length == 0 || length == MAX_PATH)
	{
		return {};
	}
	return std::filesystem::path(buffer);
}

struct HttpHandle
{
	HINTERNET h = nullptr;
	HttpHandle() = default;
	explicit HttpHandle(HINTERNET handle) : h(handle) {}
	HttpHandle(const HttpHandle&) = delete;
	HttpHandle& operator=(const HttpHandle&) = delete;
	~HttpHandle()
	{
		if (h)
		{
			WinHttpCloseHandle(h);
		}
	}
	explicit operator bool() const { return h != nullptr; }
};

bool http_get(const std::string& url, std::string& body, int& status, std::size_t max_bytes, std::string& error)
{
	body.clear();
	status = 0;
	const std::wstring wurl(url.begin(), url.end());
	URL_COMPONENTS parts{};
	parts.dwStructSize = sizeof(parts);
	parts.dwSchemeLength = static_cast<DWORD>(-1);
	parts.dwHostNameLength = static_cast<DWORD>(-1);
	parts.dwUrlPathLength = static_cast<DWORD>(-1);
	parts.dwExtraInfoLength = static_cast<DWORD>(-1);
	if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts) || !parts.lpszHostName)
	{
		error = "bad url: " + url;
		return false;
	}
	const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
	std::wstring path = (parts.lpszUrlPath && parts.dwUrlPathLength + parts.dwExtraInfoLength > 0)
	                        ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength)
	                        : L"/";

	HttpHandle session(WinHttpOpen(L"StudioThemeHelper/1.2 (Windows NT 10.0; Win64; x64)",
	                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
	                               WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session)
	{
		error = std::format("couldn't start WinHTTP (error {})", GetLastError());
		return false;
	}
	WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 120000);
	HttpHandle connect(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
	if (!connect)
	{
		error = std::format("couldn't connect (error {})", GetLastError());
		return false;
	}
	const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	HttpHandle request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
	                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
	if (!request)
	{
		error = std::format("couldn't open request (error {})", GetLastError());
		return false;
	}

	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

	static const wchar_t kHeaders[] =
	    L"User-Agent: StudioThemeHelper/1.2\r\n"
	    L"Accept: application/vnd.github+json, application/octet-stream, */*\r\n";

	if (!WinHttpSendRequest(request.h, kHeaders, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
	{
		error = std::format("couldn't reach {} (error {})", url, GetLastError());
		return false;
	}
	if (!WinHttpReceiveResponse(request.h, nullptr))
	{
		error = std::format("no response from {} (error {})", url, GetLastError());
		return false;
	}
	DWORD code = 0;
	DWORD code_size = sizeof(code);
	if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_size, WINHTTP_NO_HEADER_INDEX))
	{
		status = static_cast<int>(code);
	}
	for (;;)
	{
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request.h, &available))
		{
			error = std::format("download interrupted (error {})", GetLastError());
			return false;
		}
		if (available == 0)
		{
			break;
		}
		if (body.size() + available > max_bytes)
		{
			error = "download size exceeded safety limit";
			return false;
		}
		std::string chunk(available, '\0');
		DWORD read = 0;
		if (!WinHttpReadData(request.h, chunk.data(), available, &read))
		{
			error = std::format("download read failed (error {})", GetLastError());
			return false;
		}
		body.append(chunk.data(), read);
	}
	return true;
}

// Plain helper without local C++ objects to prevent C2712 __try object unwinding error
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
#else
bool http_get(const std::string&, std::string&, int& status, std::size_t, std::string& error)
{
	status = 0;
	error = "downloads require Windows";
	return false;
}
#endif

std::string json_string_after(const std::string& text, const std::string& key, std::size_t from, std::size_t* end_pos = nullptr)
{
	const std::string needle = "\"" + key + "\"";
	std::size_t at = text.find(needle, from);
	if (at == std::string::npos)
	{
		return {};
	}
	at += needle.size();
	while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
	{
		++at;
	}
	if (at >= text.size() || text[at] != ':')
	{
		return {};
	}
	++at;
	while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
	{
		++at;
	}
	if (at >= text.size() || text[at] != '"')
	{
		return {};
	}
	++at;
	std::string out;
	while (at < text.size() && text[at] != '"')
	{
		if (text[at] == '\\' && at + 1 < text.size())
		{
			++at;
			out.push_back(text[at] == 'n' ? '\n' : text[at]);
		}
		else
		{
			out.push_back(text[at]);
		}
		++at;
	}
	if (end_pos)
	{
		*end_pos = at;
	}
	return out;
}

bool ends_with(const std::string& text, const std::string& tail)
{
	return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

std::string json_escape(const std::string& text)
{
	std::string out;
	for (const char ch : text)
	{
		switch (ch)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(ch) < 0x20)
			{
				out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(ch)));
			}
			else
			{
				out.push_back(ch);
			}
		}
	}
	return out;
}

std::string utf8(const std::filesystem::path& path)
{
	const auto u8 = path.generic_u8string();
	return std::string(u8.begin(), u8.end());
}

std::filesystem::path from_utf8(const std::string& text)
{
	return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool looks_like_helper_dll(const std::string& bytes, std::string& why)
{
	if (bytes.size() < 32 * 1024)
	{
		why = std::format("only {} bytes, too small to be the helper", bytes.size());
		return false;
	}
	if (bytes[0] != 'M' || bytes[1] != 'Z')
	{
		why = "not a Windows DLL (no MZ header)";
		return false;
	}
	if (bytes.find("start_mod") == std::string::npos)
	{
		why = "not an RML mod (no start_mod export)";
		return false;
	}
	return true;
}

bool swap_in(const std::filesystem::path& target, const std::filesystem::path& staged, std::string& how)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path old_file = target;
	old_file += ".old";
	fs::remove(old_file, ec);
	ec.clear();
	if (fs::exists(target, ec))
	{
		ec.clear();
		fs::rename(target, old_file, ec);
		if (ec)
		{
			how = "staged";
			return true;
		}
	}
	ec.clear();
	fs::rename(staged, target, ec);
	if (ec)
	{
		const std::string message = ec.message();
		std::error_code undo;
		if (fs::exists(old_file, undo))
		{
			fs::rename(old_file, target, undo);
		}
		how = "couldn't move new file into place: " + message;
		return false;
	}
	ec.clear();
	fs::remove(old_file, ec);
	how = "installed";
	return true;
}

int parse_tag_abi(const std::string& tag)
{
	const std::string needle = "-abi";
	const auto pos = tag.rfind(needle);
	if (pos == std::string::npos)
	{
		return -1;
	}
	try
	{
		return std::stoi(tag.substr(pos + needle.size()));
	}
	catch (...)
	{
		return -1;
	}
}

} // namespace

class studio_theme_qt final : public ModBase
{
	std::shared_ptr<spdlog::logger> m_log;
	std::mutex m_mutex;
	std::string m_original;
	std::string m_last;
	std::string m_css;
	std::string m_restyle_css;
	std::string m_restyle_allow;
	bool m_wanted = false;
	bool m_captured = false;
	std::mutex m_register_mutex;
	bool m_registered = false;
	std::atomic<bool> m_stop{false};
	std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
	std::atomic<int> m_jobs{0};
	uint64_t m_menu_root = 0;
	uint64_t m_menu_presets = 0;
	std::vector<uint64_t> m_preset_items;
	std::string m_last_presets;
	int m_compose_counter = 0;
	std::string m_last_compose;

	static constexpr std::string_view kBegin = "\n/*studio_theme:begin*/\n";
	static constexpr std::string_view kEnd = "\n/*studio_theme:end*/";
	static constexpr std::string_view kDefaultAllow =
	    "OutputWidgetRibbon\nOutputFilterTextEdit\nOutputFindSearchBar\nOutputRibbonCombinedFilterDropdown\n"
	    "OutputRibbonContextFilterDropdown\nOutputRibbonFilterDropdownGroup\nOutputRibbonMessageTypeFilterDropdown\n"
	    "OutputRibbonMoreDropdown\nRBX::Studio::detail::Menu\nQMenu\nQtitan::RibbonMenu\nQtitan::Menu\n"
	    "RBX::Studio::LogOutMenu";

public:
	studio_theme_qt()
	{
		name = "Studio Theme Qt";
		version = kHelperVersion;
		author = "gasolongames";
		description = "Qt stylesheet helper + updater for the Studio Theme mod";
		m_log = rml::Logger::get_logger("StudioThemeQt");
	}

	void on_load() override
	{
		m_log->info("loaded {}", kHelperVersion);

		try
		{
			install_pending_update();
		}
		catch (const std::exception& e)
		{
			m_log->warn("install_pending_update failed: {}", e.what());
		}
		catch (...)
		{
		}

		// Register bridge first so updater functions are available immediately
		bool reg_ok = false;
		try
		{
			reg_ok = register_bridge();
		}
		catch (...)
		{
		}

		try
		{
			build_menu();
		}
		catch (...)
		{
		}

		try
		{
			start_watchdog();
		}
		catch (...)
		{
		}

		if (reg_ok)
		{
			return;
		}

		m_stop = false;
		std::thread([this]() {
			for (int attempt = 0; attempt < 240 && !m_stop; ++attempt)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				try
				{
					if (register_bridge())
					{
						return;
					}
				}
				catch (...)
				{
				}
			}
			if (!m_stop)
			{
				m_log->error("gave up waiting for script runtime");
			}
		}).detach();
	}

	void on_script_manager_load() override
	{
		register_bridge();
	}

	bool register_bridge()
	{
		std::lock_guard guard(m_register_mutex);
		if (m_registered)
		{
			return true;
		}
		auto* runtime = script_runtime();
		if (!runtime)
		{
			return false;
		}
		auto& bridge = runtime->bridge();

		// Core & updater functions (zero Qt dependencies)
		auto r1 = bridge.register_function("studio_theme_qt", "ping", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		(void)r1;

		auto r2 = bridge.register_function("studio_theme_qt", "version", [](const BridgeArgs&) -> BridgeArgs {
			return {std::string{kHelperVersion}};
		});
		(void)r2;

		auto r3 = bridge.register_function("studio_theme_qt", "abi", [](const BridgeArgs&) -> BridgeArgs {
			return {std::to_string(kBuiltAbi)};
		});
		(void)r3;

		auto r4 = bridge.register_function("studio_theme_qt", "check_update", [this](const BridgeArgs& args) -> BridgeArgs {
			auto text = [&](std::size_t i) -> std::string {
				if (i < args.size())
				{
					if (const auto* v = std::get_if<std::string>(&args[i]))
					{
						return *v;
					}
				}
				return {};
			};
			start_check(text(0), text(1), text(2));
			return {true};
		});
		(void)r4;

		auto r5 = bridge.register_function("studio_theme_qt", "download_update", [this](const BridgeArgs& args) -> BridgeArgs {
			auto text = [&](std::size_t i) -> std::string {
				if (i < args.size())
				{
					if (const auto* v = std::get_if<std::string>(&args[i]))
					{
						return *v;
					}
				}
				return {};
			};
			start_download(text(0), text(1), text(2));
			return {true};
		});
		(void)r5;

		auto r6 = bridge.register_function("studio_theme_qt", "install_pending", [this](const BridgeArgs& args) -> BridgeArgs {
			const auto* target = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!target || target->empty())
			{
				return {std::string{"error"}, std::string{"missing path"}};
			}
			try
			{
				namespace fs = std::filesystem;
				const fs::path dll = from_utf8(*target);
				fs::path staged = dll;
				staged += ".new";
				std::error_code ec;
				if (!fs::exists(staged, ec))
				{
					return {std::string{"none"}, std::string{}};
				}
				std::string how;
				const bool ok = swap_in(dll, staged, how);
				m_log->info("pending update at {}: {}", *target, how);
				return {std::string{ok ? how : "error"}, ok ? std::string{} : how};
			}
			catch (const std::exception& e)
			{
				return {std::string{"error"}, std::string{e.what()}};
			}
			catch (...)
			{
				return {std::string{"error"}, std::string{"unknown swap error"}};
			}
		});
		(void)r6;

		auto r7 = bridge.register_function("studio_theme_qt", "save_theme", [](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* text = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
			if (!path || !text || path->empty())
			{
				return {std::string{"missing arguments"}};
			}
			try
			{
				namespace fs = std::filesystem;
				const fs::path file = from_utf8(*path);
				const fs::path temp = fs::path(file).concat(".tmp");
				{
					std::ofstream out(temp, std::ios::binary | std::ios::trunc);
					if (!out)
					{
						return {std::string{"couldn't write "} + *path};
					}
					out << *text;
				}
				std::error_code ec;
				fs::rename(temp, file, ec);
				if (ec)
				{
					return {"couldn't save theme: " + ec.message()};
				}
				return {true};
			}
			catch (const std::exception& e)
			{
				return {std::string{e.what()}};
			}
			catch (...)
			{
				return {std::string{"save failed"}};
			}
		});
		(void)r7;

		auto r8 = bridge.register_function("studio_theme_qt", "load_theme", [](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!path || path->empty())
			{
				return {std::string{}};
			}
			try
			{
				std::ifstream in(from_utf8(*path), std::ios::binary);
				if (!in)
				{
					return {std::string{}};
				}
				std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				return {text};
			}
			catch (...)
			{
				return {std::string{}};
			}
		});
		(void)r8;

		// Qt styling functions
		auto r9 = bridge.register_function("studio_theme_qt", "apply", [this](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				std::string css;
				if (!args.empty())
				{
					if (const auto* text = std::get_if<std::string>(&args[0]))
					{
						css = *text;
					}
				}
				schedule(std::move(css));
				return {true};
			}
			catch (...)
			{
				return {false};
			}
		});
		(void)r9;

		auto r10 = bridge.register_function("studio_theme_qt", "scan", [this](const BridgeArgs&) -> BridgeArgs {
			try
			{
				schedule_scan();
				return {true};
			}
			catch (...)
			{
				return {false};
			}
		});
		(void)r10;

		auto r11 = bridge.register_function("studio_theme_qt", "pick_image", [this](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				std::string dir;
				if (!args.empty())
				{
					if (const auto* text = std::get_if<std::string>(&args[0]))
					{
						dir = *text;
					}
				}
				schedule_pick(std::move(dir));
				return {true};
			}
			catch (...)
			{
				return {false};
			}
		});
		(void)r11;

		auto r12 = bridge.register_function("studio_theme_qt", "install_image", [this](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				const auto* src = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
				const auto* content = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
				if (!src || !content || src->empty() || content->empty())
				{
					return {std::string{}, std::string{"missing arguments"}};
				}
				return install_image(*src, *content);
			}
			catch (...)
			{
				return {std::string{}, std::string{"install_image exception"}};
			}
		});
		(void)r12;

		auto r13 = bridge.register_function("studio_theme_qt", "compose_topbar", [this](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				auto text = [&](std::size_t i) -> std::string {
					if (i < args.size())
					{
						if (const auto* v = std::get_if<std::string>(&args[i]))
						{
							return *v;
						}
					}
					return {};
				};
				double opacity = 1.0;
				if (args.size() > 2)
				{
					if (const auto* d = std::get_if<double>(&args[2]))
					{
						opacity = *d;
					}
				}
				schedule_compose(text(0), text(1), opacity, text(3), text(4), text(5));
				return {true};
			}
			catch (...)
			{
				return {false};
			}
		});
		(void)r13;

		auto r14 = bridge.register_function("studio_theme_qt", "set_presets", [this](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				std::string names;
				if (!args.empty())
				{
					if (const auto* text = std::get_if<std::string>(&args[0]))
					{
						names = *text;
					}
				}
				schedule_presets(std::move(names));
				return {true};
			}
			catch (...)
			{
				return {false};
			}
		});
		(void)r14;

		auto r15 = bridge.register_function("studio_theme_qt", "restyle_widgets", [this](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				std::string css;
				std::string allow;
				if (args.size() > 0)
				{
					if (const auto* text = std::get_if<std::string>(&args[0]))
					{
						css = *text;
					}
				}
				if (args.size() > 1)
				{
					if (const auto* text = std::get_if<std::string>(&args[1]))
					{
						allow = *text;
					}
				}
				schedule_restyle(std::move(css), std::move(allow));
				return {true};
			}
			catch (...)
			{
				return {false};
			}
		});
		(void)r15;

		m_registered = true;
		m_log->info("bridge functions registered successfully");
		return true;
	}

	void on_unload() override
	{
		m_stop = true;
		m_alive->store(false);

		try
		{
			if (auto* qt = rml::qt::QtIntegration::instance(); qt && m_menu_root != 0)
			{
				qt->menu().remove(m_menu_root);
				m_menu_root = 0;
			}
		}
		catch (...)
		{
		}

		try
		{
			if (auto* runtime = script_runtime(); runtime && m_registered)
			{
				auto& bridge = runtime->bridge();
				for (const char* fn : {"version", "check_update", "download_update", "install_pending", "ping", "abi",
				                       "save_theme", "load_theme", "apply", "scan", "pick_image", "install_image",
				                       "compose_topbar", "set_presets", "restyle_widgets"})
				{
					try
					{
						auto res = bridge.unregister_function("studio_theme_qt", fn);
						(void)res;
					}
					catch (...)
					{
					}
				}
			}
		}
		catch (...)
		{
		}

		for (int waited = 0; m_jobs.load() > 0 && waited < 200; ++waited)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		try
		{
			if (auto* app = rml::qt::QApplication::instance())
			{
				std::lock_guard lock(m_mutex);
				if (m_captured && app->style_sheet() != m_original)
				{
					app->set_style_sheet(m_original);
				}
			}
		}
		catch (...)
		{
		}

		m_log->info("unloaded cleanly");
	}

private:
	void install_pending_update()
	{
#ifdef _WIN32
		const auto self = self_module_path();
		if (self.empty())
		{
			return;
		}
		auto staged = self;
		staged += ".new";
		std::error_code ec;
		if (!std::filesystem::exists(staged, ec))
		{
			return;
		}
		std::string how;
		swap_in(self, staged, how);
		m_log->info("staged update next to running helper: {}", how);
#endif
	}

	void spawn_job(std::function<void()> fn)
	{
		++m_jobs;
		std::thread([this, fn = std::move(fn)]() {
			try
			{
				fn();
			}
			catch (...)
			{
			}
			--m_jobs;
		}).detach();
	}

	void publish(const char* key, const char* event, std::string json)
	{
		auto alive = m_alive;
		auto task = [this, alive, key, event, json = std::move(json)]() {
			if (!alive->load())
			{
				return;
			}
			try
			{
				auto* runtime = script_runtime();
				if (!runtime)
				{
					return;
				}
				auto& bridge = runtime->bridge();
				auto stored = bridge.set_shared(key, json);
				(void)stored;
				auto emitted = bridge.emit(event, BridgeArgs{std::string{"ok"}});
				(void)emitted;
			}
			catch (...)
			{
			}
		};

		try
		{
			if (auto* qt = rml::qt::QtIntegration::instance())
			{
				qt->run_on_gui_thread(task);
			}
			else
			{
				task();
			}
		}
		catch (...)
		{
			task();
		}
	}

	void start_check(std::string repo, std::string asset, std::string job)
	{
		spawn_job([this, repo = std::move(repo), asset = std::move(asset), job = std::move(job)]() {
			std::string tag;
			std::string url;
			std::string error;
			const std::string base = "https://api.github.com/repos/" + repo + "/releases";
			bool reached = false;

			for (const std::string& feed : {base + "/latest", base + "?per_page=10"})
			{
				std::string body;
				int status = 0;
				std::string http_error;
				if (!http_get(feed, body, status, 4 * 1024 * 1024, http_error))
				{
					error = http_error;
					break;
				}
				reached = true;
				if (status == 404)
				{
					error = "no releases found for " + repo;
					continue;
				}
				if (status == 403 || status == 429)
				{
					error = "GitHub rate-limited request, try again in a few minutes";
					break;
				}
				if (status != 200)
				{
					error = std::format("GitHub returned HTTP {}", status);
					break;
				}

				std::size_t pos = 0;
				while ((pos = body.find("\"tag_name\"", pos)) != std::string::npos)
				{
					std::size_t end_tag = 0;
					std::string cand_tag = json_string_after(body, "tag_name", pos, &end_tag);
					if (cand_tag.empty())
					{
						pos += 10;
						continue;
					}

					const int release_abi = parse_tag_abi(cand_tag);

					// If built against an ABI, skip releases targeted at a different ABI
					if (kBuiltAbi > 0 && release_abi > 0 && release_abi != kBuiltAbi)
					{
						pos = end_tag;
						continue;
					}

					const std::size_t next_release_pos = body.find("\"tag_name\"", end_tag);
					const std::size_t search_limit = (next_release_pos == std::string::npos) ? body.size() : next_release_pos;

					std::string best_url;
					std::string fallback_url;

					std::size_t url_pos = end_tag;
					while (url_pos < search_limit)
					{
						std::size_t end_url = 0;
						std::string cand_url = json_string_after(body, "browser_download_url", url_pos, &end_url);
						if (cand_url.empty() || end_url >= search_limit)
						{
							break;
						}
						url_pos = end_url;

						if (!asset.empty() && ends_with(cand_url, "/" + asset))
						{
							best_url = cand_url;
							break;
						}
						if (kBuiltAbi > 0 && ends_with(cand_url, std::format("_abi{}.dll", kBuiltAbi)))
						{
							best_url = cand_url;
							break;
						}
						if (ends_with(cand_url, "/studio_theme_qt.dll"))
						{
							fallback_url = cand_url;
						}
					}

					if (!best_url.empty())
					{
						url = best_url;
						tag = cand_tag;
						break;
					}
					if (!fallback_url.empty())
					{
						url = fallback_url;
						tag = cand_tag;
						break;
					}

					pos = search_limit;
				}

				if (!url.empty())
				{
					error.clear();
					break;
				}
				error = "no compatible DLL found in newest releases";
			}

			if (!reached && error.empty())
			{
				error = "couldn't reach GitHub";
			}

			publish("studio_theme.update_check", "studio_theme.update_check_done",
			        std::format("{{\"job\":\"{}\",\"tag\":\"{}\",\"url\":\"{}\",\"error\":\"{}\"}}",
			                    json_escape(job), json_escape(tag), json_escape(url), json_escape(error)));
			m_log->info("update check {}: tag '{}' {}", repo, tag, error);
		});
	}

	void start_download(std::string url, std::string target, std::string job)
	{
		spawn_job([this, url = std::move(url), target = std::move(target), job = std::move(job)]() {
			bool ok = false;
			std::string how;
			std::string error;
			std::size_t size = 0;
			do
			{
				if (url.empty() || target.empty())
				{
					error = "missing url or target path";
					break;
				}
				std::string body;
				int status = 0;
				if (!http_get(url, body, status, 64u * 1024u * 1024u, error))
				{
					break;
				}
				if (status != 200)
				{
					error = std::format("download failed: HTTP {}", status);
					break;
				}
				if (!looks_like_helper_dll(body, error))
				{
					error = "refusing to install: " + error;
					break;
				}
				size = body.size();
				try
				{
					namespace fs = std::filesystem;
					const fs::path dll = from_utf8(target);
					std::error_code ec;
					fs::create_directories(dll.parent_path(), ec);
					fs::path staged = dll;
					staged += ".new";
					{
						std::ofstream out(staged, std::ios::binary | std::ios::trunc);
						if (!out)
						{
							error = "couldn't write " + utf8(staged);
							break;
						}
						out.write(body.data(), static_cast<std::streamsize>(body.size()));
						out.close();
						if (!out)
						{
							error = "writing " + utf8(staged) + " failed (disk full?)";
							break;
						}
					}
					ok = swap_in(dll, staged, how);
					if (!ok)
					{
						error = how;
					}
				}
				catch (const std::exception& e)
				{
					error = e.what();
					ok = false;
				}
				catch (...)
				{
					error = "unknown filesystem exception during download swap";
					ok = false;
				}
			} while (false);

			publish("studio_theme.update_download", "studio_theme.update_download_done",
			        std::format("{{\"job\":\"{}\",\"ok\":{},\"how\":\"{}\",\"bytes\":{},\"error\":\"{}\"}}",
			                    json_escape(job), ok ? "true" : "false", json_escape(ok ? how : std::string{}),
			                    size, json_escape(error)));
			m_log->info("update download: ok={} {} {} bytes {}", ok, how, size, error);
		});
	}

	void start_watchdog()
	{
		std::thread([this, alive = m_alive]() {
			while (alive->load() && !m_stop)
			{
				for (int i = 0; i < 20 && alive->load() && !m_stop; ++i)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
				if (!alive->load() || m_stop)
				{
					return;
				}
				auto task = [this, alive]() {
					if (!alive->load())
					{
						return;
					}
					try
					{
						reassert();
					}
					catch (...)
					{
					}
				};
				try
				{
					if (auto* qt = rml::qt::QtIntegration::instance())
					{
						qt->run_on_gui_thread(task);
					}
				}
				catch (...)
				{
				}
			}
		}).detach();
	}

	void reassert()
	{
		auto* app = rml::qt::QApplication::instance();
		if (!app)
		{
			return;
		}
		std::string r_css, r_allow;
		{
			std::lock_guard lock(m_mutex);
			if (m_wanted && app->style_sheet().find(kSheetMarker) == std::string::npos)
			{
				m_log->info("Studio replaced its stylesheet; restoring theme");
				apply_locked(m_css);
			}
			r_css = m_restyle_css;
			r_allow = m_restyle_allow;
		}
		// Periodically re-ensure menus and dynamic dropdowns keep their theme
		if (!r_css.empty())
		{
			restyle_widgets_now(r_css, r_allow);
		}
	}

	void send(const std::string& action)
	{
		auto* runtime = script_runtime();
		if (!runtime)
		{
			return;
		}
		auto res = runtime->bridge().emit("studio_theme.menu", BridgeArgs{action});
		(void)res;
	}

	void build_menu()
	{
		auto* qt = rml::qt::QtIntegration::instance();
		if (!qt)
		{
			return;
		}
		auto& menu = qt->menu();
		m_menu_root = menu.add_submenu(0, "Studio Theme");
		if (m_menu_root == 0)
		{
			return;
		}
		auto a1 = menu.add_action(m_menu_root, "Open / close Theme Editor", [this]() { send("open"); });
		(void)a1;
		auto a2 = menu.add_action(m_menu_root, "Turn theme on / off", [this]() { send("toggle"); });
		(void)a2;
		auto sep1 = menu.add_separator(m_menu_root);
		(void)sep1;
		m_menu_presets = menu.add_submenu(m_menu_root, "Presets");
		m_preset_items.push_back(menu.add_action(m_menu_presets, "(loading...)", []() {}));
		auto sep2 = menu.add_separator(m_menu_root);
		(void)sep2;
		auto a3 = menu.add_action(m_menu_root, "Auto-match leftover colors on / off", [this]() { send("automatch"); });
		(void)a3;
		auto a4 = menu.add_action(m_menu_root, "Reload background images", [this]() { send("reload_images"); });
		(void)a4;
		auto a5 = menu.add_action(m_menu_root, "Re-apply theme everywhere", [this]() { send("refresh"); });
		(void)a5;
		auto sep3 = menu.add_separator(m_menu_root);
		(void)sep3;
		auto a6 = menu.add_action(m_menu_root, "Check for helper update", [this]() { send("check_update"); });
		(void)a6;
		auto a7 = menu.add_action(m_menu_root, "Plain Studio (remove all theme colors)", [this]() { send("plain"); });
		(void)a7;
	}

	void schedule_presets(std::string names)
	{
		auto task = [this, names = std::move(names)]() {
			try
			{
				auto* qt = rml::qt::QtIntegration::instance();
				if (!qt || m_menu_presets == 0 || names == m_last_presets)
				{
					return;
				}
				m_last_presets = names;
				auto& menu = qt->menu();
				for (const auto id : m_preset_items)
				{
					menu.remove(id);
				}
				m_preset_items.clear();
				std::istringstream lines(names);
				std::string name;
				while (std::getline(lines, name))
				{
					if (!name.empty() && name.back() == '\r')
					{
						name.pop_back();
					}
					if (name.empty())
					{
						continue;
					}
					m_preset_items.push_back(menu.add_action(m_menu_presets, name, [this, name]() { send("preset:" + name); }));
				}
			}
			catch (...)
			{
			}
		};

		if (auto* qt = rml::qt::QtIntegration::instance())
		{
			qt->run_on_gui_thread(task);
		}
		else
		{
			task();
		}
	}

	void schedule_scan()
	{
		auto task = [this]() {
			try
			{
				std::map<std::string, int> counts;
				for (auto* widget : rml::qt::QApplication::all_widgets())
				{
					if (!widget)
					{
						continue;
					}
					const char* cls = widget->class_name();
					if (cls && *cls)
					{
						++counts[cls];
					}
				}
				std::string json = "[";
				bool first = true;
				for (const auto& [cls, count] : counts)
				{
					json += std::format("{}{{\"class\":\"{}\",\"count\":{}}}", first ? "" : ",", json_escape(cls), count);
					first = false;
				}
				json += "]";
				auto* runtime = script_runtime();
				if (!runtime)
				{
					return;
				}
				auto& bridge = runtime->bridge();
				auto stored = bridge.set_shared("studio_theme.qt_scan", json);
				(void)stored;
				auto emitted = bridge.emit("studio_theme.qt_scan_done", BridgeArgs{std::string{"ok"}});
				(void)emitted;
			}
			catch (...)
			{
			}
		};

		if (auto* qt = rml::qt::QtIntegration::instance())
		{
			qt->run_on_gui_thread(task);
		}
		else
		{
			task();
		}
	}

	static rml::qt::QColor parse_hex(const std::string& hex)
	{
		unsigned int value = 0x202020;
		if (hex.size() >= 7 && hex[0] == '#')
		{
			try
			{
				value = static_cast<unsigned int>(std::stoul(hex.substr(1, 6), nullptr, 16));
			}
			catch (...)
			{
			}
		}
		return rml::qt::QColor(static_cast<int>((value >> 16) & 0xFF),
		                       static_cast<int>((value >> 8) & 0xFF),
		                       static_cast<int>(value & 0xFF));
	}

	void schedule_compose(std::string image, std::string hex, double opacity, std::string mode, std::string out_dir, std::string key)
	{
		auto task = [this, image, hex, opacity, mode, out_dir, key]() {
			namespace fs = std::filesystem;
			std::string result;
			std::string error;
			try
			{
				int w = 0, h = 0;
				for (auto* widget : rml::qt::QApplication::all_widgets())
				{
					if (!widget)
					{
						continue;
					}
					const char* cls = widget->class_name();
					if (cls && std::string_view(cls).find("MenuBar") != std::string_view::npos && widget->width() > w)
					{
						w = widget->width();
						h = widget->height();
					}
				}
				if (w <= 0 || h <= 0)
				{
					w = 1920;
					h = 32;
				}
				const int W = w * 2;
				const int H = h * 2;
				rml::qt::QPixmap source(image);
				if (!source.loaded() || source.width() <= 0 || source.height() <= 0)
				{
					error = "Qt couldn't read the image: " + image;
				}
				else
				{
					rml::qt::QPixmap canvas(W, H);
					canvas.fill(parse_hex(hex));
					{
						rml::qt::QPainter painter(canvas);
						painter.set_render_hint(rml::qt::QPainter::SmoothPixmapTransform);
						painter.set_opacity(std::clamp(opacity, 0.0, 1.0));
						using Aspect = rml::qt::QPixmap::AspectMode;
						if (mode == "Stretch")
						{
							painter.draw_pixmap(rml::qt::QRect(0, 0, W, H), source.scaled(W, H, Aspect::Ignore));
						}
						else if (mode == "Fit")
						{
							const auto fit = source.scaled(W, H, Aspect::Keep);
							painter.draw_pixmap((W - fit.width()) / 2, (H - fit.height()) / 2, fit);
						}
						else if (mode == "Tile")
						{
							const auto tile = source.scaled(W * 4, H, Aspect::Keep);
							for (int x = 0; tile.width() > 0 && x < W; x += tile.width())
							{
								painter.draw_pixmap(x, 0, tile);
							}
						}
						else
						{
							const auto cover = source.scaled(W, H, Aspect::KeepByExpanding);
							painter.draw_pixmap((W - cover.width()) / 2, (H - cover.height()) / 2, cover);
						}
					}
					std::error_code ec;
					const fs::path dir = from_utf8(out_dir);
					fs::create_directories(dir, ec);
					const fs::path file = dir / std::format("topbar_{}.png", ++m_compose_counter);
					if (canvas.save(utf8(file)))
					{
						if (!m_last_compose.empty())
						{
							fs::remove(from_utf8(m_last_compose), ec);
						}
						m_last_compose = utf8(file);
						result = m_last_compose;
					}
					else
					{
						error = "couldn't save composed image to " + utf8(file);
					}
				}
			}
			catch (const std::exception& e)
			{
				error = e.what();
			}
			catch (...)
			{
				error = "unknown compose exception";
			}

			auto* runtime = script_runtime();
			if (!runtime)
			{
				return;
			}
			auto& bridge = runtime->bridge();
			const std::string json = std::format("{{\"key\":\"{}\",\"path\":\"{}\",\"error\":\"{}\"}}",
			                                     json_escape(key), json_escape(result), json_escape(error));
			auto stored = bridge.set_shared("studio_theme.topbar_image", json);
			(void)stored;
			auto emitted = bridge.emit("studio_theme.topbar_ready", BridgeArgs{std::string{"ok"}});
			(void)emitted;
		};

		if (auto* qt = rml::qt::QtIntegration::instance())
		{
			qt->run_on_gui_thread(task);
		}
		else
		{
			task();
		}
	}

	BridgeArgs install_image(const std::string& source_text, const std::string& content_text)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path source = from_utf8(source_text);
		if (!fs::is_regular_file(source, ec))
		{
			return {std::string{}, std::string{"not found"}};
		}
		const fs::path content = from_utf8(content_text);
		if (!fs::is_directory(content, ec))
		{
			return {std::string{}, "Studio content folder not found: " + content_text};
		}
		const fs::path folder = content / "studio_theme";
		fs::create_directories(folder, ec);
		const fs::path target = folder / source.filename();
		ec.clear();
		bool copy = !fs::exists(target, ec);
		if (!copy)
		{
			ec.clear();
			const auto source_size = fs::file_size(source, ec);
			const auto target_size = fs::file_size(target, ec);
			const auto source_time = fs::last_write_time(source, ec);
			const auto target_time = fs::last_write_time(target, ec);
			copy = source_size != target_size || source_time > target_time;
		}
		if (copy)
		{
			ec.clear();
			fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				return {std::string{}, "couldn't copy into Studio's content folder: " + ec.message()};
			}
			m_log->info("installed image {} -> {}", source_text, utf8(target));
		}
		return {"rbxasset://studio_theme/" + utf8(source.filename()), std::string{}};
	}

	void schedule_pick(std::string images_dir)
	{
		auto task = [this, images_dir = std::move(images_dir)]() {
			namespace fs = std::filesystem;
			std::string result_path;
			std::string error;
			try
			{
				const std::string picked = rml::qt::QFileDialog::get_open_file_name(
				    nullptr, "Choose a background image", images_dir, "Images (*.png *.jpg *.jpeg *.bmp *.tga)");
				if (!picked.empty())
				{
					std::error_code ec;
					const fs::path source = from_utf8(picked);
					const fs::path folder = from_utf8(images_dir);
					fs::create_directories(folder, ec);
					const fs::path target = folder / source.filename();
					ec.clear();
					const bool same = fs::exists(target, ec) && fs::equivalent(source, target, ec);
					if (!same)
					{
						ec.clear();
						fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
						if (ec)
						{
							error = "couldn't copy image into images folder: " + ec.message();
						}
					}
					if (error.empty())
					{
						result_path = "images/" + utf8(source.filename());
					}
				}
			}
			catch (const std::exception& e)
			{
				error = e.what();
			}
			catch (...)
			{
				error = "unknown file picker exception";
			}

			auto* runtime = script_runtime();
			if (!runtime)
			{
				return;
			}
			auto& bridge = runtime->bridge();
			const std::string json = std::format("{{\"path\":\"{}\",\"error\":\"{}\"}}",
			                                     json_escape(result_path), json_escape(error));
			auto stored = bridge.set_shared("studio_theme.picked_image", json);
			(void)stored;
			auto emitted = bridge.emit("studio_theme.picked_image_done", BridgeArgs{std::string{"ok"}});
			(void)emitted;
		};

		if (auto* qt = rml::qt::QtIntegration::instance())
		{
			qt->run_on_gui_thread(task);
		}
		else
		{
			task();
		}
	}

	static std::string widget_style_sheet(rml::qt::QWidget* widget)
	{
#ifdef _WIN32
		if (!widget)
		{
			return {};
		}
		using Getter = void* (*)(const void* self, void* ret);
		static const Getter getter = []() -> Getter {
			HMODULE module = GetModuleHandleW(L"Qt5Widgets.dll");
			if (!module)
			{
				return nullptr;
			}
			return reinterpret_cast<Getter>(GetProcAddress(module, "?styleSheet@QWidget@@QEBA?AVQString@@XZ"));
		}();
		if (!getter)
		{
			return {};
		}

		rml::qt::QString result;
		if (!safe_invoke_getter(reinterpret_cast<void*>(getter), widget, result.storage()))
		{
			return {};
		}
		return result.to_utf8();
#else
		(void)widget;
		return {};
#endif
	}

	static std::string strip_ours(const std::string& sheet)
	{
		const auto begin = sheet.find(kBegin);
		if (begin == std::string::npos)
		{
			return sheet;
		}
		const auto end = sheet.find(kEnd, begin);
		if (end == std::string::npos)
		{
			return sheet.substr(0, begin);
		}
		return sheet.substr(0, begin) + sheet.substr(end + kEnd.size());
	}

	static bool blank(const std::string& text)
	{
		return std::all_of(text.begin(), text.end(), [](char ch) {
			return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
		});
	}

	void restyle_widgets_now(const std::string& css, const std::string& allow_text)
	{
		auto alive = m_alive;
		auto task = [this, alive, css, allow_text]() {
			if (!alive->load())
			{
				return;
			}
			try
			{
				std::set<std::string> allow;
				{
					std::istringstream lines(allow_text.empty() ? std::string(kDefaultAllow) : allow_text);
					std::string line;
					while (std::getline(lines, line))
					{
						if (!line.empty() && line.back() == '\r')
						{
							line.pop_back();
						}
						if (!line.empty())
						{
							allow.insert(line);
						}
					}
				}
				for (auto* widget : rml::qt::QApplication::all_widgets())
				{
					if (!widget)
					{
						continue;
					}
					const char* cls = widget->class_name();
					if (!cls || !*cls)
					{
						continue;
					}

					std::string_view clsView(cls);
					const bool isMenu = (clsView.find("Menu") != std::string_view::npos && clsView.find("MenuBar") == std::string_view::npos);
					const bool allowed = !css.empty() && (allow.count(cls) > 0 || isMenu);

					const std::string own = widget_style_sheet(widget);

					if (!allowed)
					{
						if (!own.empty())
						{
							const std::string base = strip_ours(own);
							if (base != own)
							{
								widget->setStyleSheet(rml::qt::QString(base));
							}
						}
						continue;
					}

					// Ensure menus and dropdowns are themed directly even if they had no prior stylesheet
					const std::string base = own.empty() ? "" : strip_ours(own);
					const std::string want = base.empty() ? (std::string(kBegin) + css + std::string(kEnd))
					                                      : (base + std::string(kBegin) + css + std::string(kEnd));
					if (want != own)
					{
						widget->setStyleSheet(rml::qt::QString(want));
					}
				}
			}
			catch (...)
			{
			}
		};

		if (auto* qt = rml::qt::QtIntegration::instance())
		{
			qt->run_on_gui_thread(task);
		}
		else
		{
			task();
		}
	}

	void schedule_restyle(std::string css, std::string allow_text)
	{
		{
			std::lock_guard lock(m_mutex);
			m_restyle_css = css;
			m_restyle_allow = allow_text;
		}
		restyle_widgets_now(css, allow_text);
	}

	void schedule(std::string css)
	{
		auto alive = m_alive;
		auto task = [this, alive, css = std::move(css)]() {
			if (!alive->load())
			{
				return;
			}
			apply_now(css);
		};

		if (auto* qt = rml::qt::QtIntegration::instance())
		{
			qt->run_on_gui_thread(task);
		}
		else
		{
			task();
		}
	}

	void apply_now(const std::string& css)
	{
		std::lock_guard lock(m_mutex);
		try
		{
			apply_locked(css);
		}
		catch (...)
		{
		}
	}

	void apply_locked(const std::string& css)
	{
		auto* app = rml::qt::QApplication::instance();
		if (!app)
		{
			return;
		}
		const std::string current = app->style_sheet();
		const std::size_t mark = current.find(kSheetMarker);
		if (!m_captured || mark == std::string::npos)
		{
			std::string baseline = (mark == std::string::npos) ? current : current.substr(0, mark);
			if (mark != std::string::npos && !baseline.empty() && baseline.back() == '\n')
			{
				baseline.pop_back();
			}
			m_original = std::move(baseline);
			m_captured = true;
		}
		m_wanted = !css.empty();
		m_css = css;
		const std::string full = css.empty() ? m_original : m_original + "\n" + kSheetMarker + "\n" + css;
		if (current == full)
		{
			m_last = full;
			return;
		}
		m_last = full;
		app->set_style_sheet(full);
	}
};

extern "C"
{
	RML_MOD_ABI_EXPORT ModBase* start_mod()
	{
		return new studio_theme_qt();
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
