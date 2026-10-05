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
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
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
#define STUDIO_THEME_HELPER_BASE "1.0"
#endif
#ifndef STUDIO_THEME_HELPER_VERSION
#define STUDIO_THEME_HELPER_VERSION STUDIO_THEME_HELPER_BASE ".0"
#endif
constexpr const char* kHelperVersion = STUDIO_THEME_HELPER_VERSION;

// What this helper can do, by name. The scripts ask for these instead of comparing version numbers
// (version numbers get reset; a capability either exists or it doesn't).
//   compose_panel  - compose_panel() exists (cuts a picture for any panel, sized to that panel)
//   compose_event  - the result arrives in the event payload, so several can be in flight at once
//   compose_cache  - finished cuts are kept on disk and reused (no re-cutting after a restart)
//   compose_native - pictures are cut at 1x (native size), for unscaled background-image
//   compose_watch  - the helper re-cuts a picture by itself when its panel is resized
constexpr const char* kCapabilities = "compose_panel compose_event compose_cache compose_native compose_watch";

#ifdef RML_ABI_VERSION
constexpr int kBuiltAbi = RML_ABI_VERSION;
#else
constexpr int kBuiltAbi = 0;
#endif

constexpr std::string_view kSheetMarker = "/* studio_theme */";

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
	{
		DWORD length = 0;
		DWORD length_size = sizeof(length);
		if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
		                        WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX) &&
		    length > 0 && length <= max_bytes)
		{
			body.reserve(length);
		}
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
		const std::size_t old_size = body.size();
		body.resize(old_size + available);
		DWORD read = 0;
		if (!WinHttpReadData(request.h, body.data() + old_size, available, &read))
		{
			error = std::format("download read failed (error {})", GetLastError());
			return false;
		}
		body.resize(old_size + read);
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

bool contains_any(std::string_view text, std::initializer_list<std::string_view> needles)
{
	for (const auto needle : needles)
	{
		if (text.find(needle) != std::string_view::npos)
		{
			return true;
		}
	}
	return false;
}

// Input-like widgets: line edits, combo boxes, spin boxes and Studio's own
// filter/search boxes (whose exact class names aren't known in advance, e.g. the
// Properties filter). They often carry their own stylesheet, which Qt prefers over
// the application-wide one, so they are restyled directly with a small color-only
// sheet. Anything that looks like a button, menu, dropdown group or ribbon piece is
// NOT an input, and Properties' value editors (not filter/search boxes) stay plain.
bool looks_like_input(std::string_view cls)
{
	if (contains_any(cls, {"Ribbon", "Button", "Menu", "Dropdown", "Group", "Label", "Tab", "ScriptEditor", "CodeEdit"}))
	{
		return false;
	}
	const bool filterish = contains_any(cls, {"Filter", "Search"});
	if (!filterish && contains_any(cls, {"Propert", "Editor"}))
	{
		return false;
	}
	return contains_any(cls, {"LineEdit", "TextEdit", "ComboBox", "SpinBox", "FilterBox", "FilterEdit",
	                          "FilterCombo", "FilterBar", "SearchBox", "SearchEdit", "SearchBar", "SearchCombo",
	                          "CommandBarEdit", "CommandLine"});
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

// ---- compose helpers begin (pure functions; unit-tested on their own) ----

// Stable 64-bit FNV-1a, hex. Names a finished cut after everything that shapes it, so the same
// picture / color / strength / mode / size always lands on the same file and is reused.
std::string fnv1a_hex(std::string_view text)
{
	std::uint64_t h = 14695981039346656037ull;
	for (const unsigned char c : text)
	{
		h ^= c;
		h *= 1099511628211ull;
	}
	return std::format("{:016x}", h);
}

// "MenuBar|TopBar" -> {"MenuBar", "TopBar"} (empty pieces dropped)
std::vector<std::string_view> split_classes(std::string_view list)
{
	std::vector<std::string_view> out;
	std::size_t start = 0;
	while (start <= list.size())
	{
		std::size_t bar = list.find('|', start);
		if (bar == std::string_view::npos)
		{
			bar = list.size();
		}
		const auto piece = list.substr(start, bar - start);
		if (!piece.empty())
		{
			out.push_back(piece);
		}
		start = bar + 1;
	}
	return out;
}

// A file-name-safe group name from a class list ("RBX::Studio::X|Y" -> "RBX__Studio__X_Y")
std::string safe_group(std::string_view text)
{
	std::string out;
	for (const char ch : text)
	{
		const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
		out.push_back(ok ? ch : '_');
	}
	return out.empty() ? std::string("panel") : out;
}

// Of the cuts already on disk for a group, the file names to delete so that only `keep_count`
// (newest first) stay. `current` is never listed.
std::vector<std::string> cuts_to_delete(std::vector<std::pair<std::int64_t, std::string>> files, const std::string& current, std::size_t keep_count)
{
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<std::string> out;
	for (std::size_t i = keep_count; i < files.size(); ++i)
	{
		if (files[i].second != current)
		{
			out.push_back(files[i].second);
		}
	}
	return out;
}
// What a watched panel looked like when it was last cut, and what we saw on the previous check.
struct CutSize
{
	int w = -1;
	int h = -1;
	int pend_w = -1;
	int pend_h = -1;
};
enum class CutStep
{
	Nothing, // up to date (or the panel isn't on screen)
	Wait,    // the size just changed: wait one more check to see whether it settles
	Recut    // changed and held still for a whole check: cut it again now
};
// Called on every check with the size the panel has now. Waiting one check (~0.3 s) means dragging
// a splitter doesn't re-cut on every pixel, but the picture is fixed as soon as you let go.
CutStep next_cut_step(CutSize& s, int w, int h)
{
	if (w < 8 || h < 8)
	{
		s.pend_w = s.pend_h = -1;
		return CutStep::Nothing;
	}
	if (w == s.w && h == s.h)
	{
		s.pend_w = s.pend_h = -1;
		return CutStep::Nothing;
	}
	if (w != s.pend_w || h != s.pend_h)
	{
		s.pend_w = w;
		s.pend_h = h;
		return CutStep::Wait;
	}
	s.pend_w = s.pend_h = -1;
	s.w = w;
	s.h = h;
	return CutStep::Recut;
}
// ---- compose helpers end ----

} // namespace

struct RestyleSpec
{
	std::string css_block;
	std::string leaf_block;
	bool has_css = false;
	bool has_leaf = false;
	std::set<std::string, std::less<>> allow;
};

class studio_theme_qt final : public ModBase
{
	std::shared_ptr<spdlog::logger> m_log;
	std::mutex m_mutex;
	std::string m_original;
	std::string m_css;
	std::shared_ptr<const RestyleSpec> m_restyle;
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
	std::size_t m_last_presets_hash = 0;
	bool m_have_presets = false;

	// Every picture we cut, by panel, so a resize can re-cut it (GUI thread only).
	struct CutRequest
	{
		std::string image, hex, mode, out_dir, key, size_class, classes;
		double opacity = 1.0;
		bool panel = false;
		CutSize size;
	};
	std::map<std::string, CutRequest> m_cuts;
	std::atomic<bool> m_watch_cuts{false};

	static constexpr std::string_view kBegin = "\n/*studio_theme:begin*/\n";
	static constexpr std::string_view kEnd = "\n/*studio_theme:end*/";
	static constexpr std::string_view kDefaultAllow =
	    "OutputWidgetRibbon\nOutputFilterTextEdit\nOutputFindSearchBar\nOutputRibbonCombinedFilterDropdown\n"
	    "OutputRibbonContextFilterDropdown\nOutputRibbonFilterDropdownGroup\nOutputRibbonMessageTypeFilterDropdown\n"
	    "OutputRibbonMoreDropdown\nRBX::Studio::detail::Menu\nQMenu\nQtitan::RibbonMenu\nQtitan::Menu\n"
	    "RBX::Studio::LogOutMenu\nQStatusBar\nRBX::Studio::CommandBarButton\nRBX::Studio::CommandBarButtonContainer\n"
	    "RBX::Studio::CommandBarHistoryListWidget";

public:
	studio_theme_qt()
	{
		name = "Studio Theme Qt";
		version = kHelperVersion;
		author = "Studio Theme";
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

		// string argument `i` of a call, or empty
		auto text_arg = [](const BridgeArgs& args, std::size_t i) -> std::string {
			if (i < args.size())
			{
				if (const auto* v = std::get_if<std::string>(&args[i]))
				{
					return *v;
				}
			}
			return {};
		};

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

		auto r3b = bridge.register_function("studio_theme_qt", "capabilities", [](const BridgeArgs&) -> BridgeArgs {
			return {std::string{kCapabilities}};
		});
		(void)r3b;

		auto r4 = bridge.register_function("studio_theme_qt", "check_update", [this, text_arg](const BridgeArgs& args) -> BridgeArgs {
			start_check(text_arg(args, 0), text_arg(args, 1), text_arg(args, 2));
			return {true};
		});
		(void)r4;

		auto r5 = bridge.register_function("studio_theme_qt", "download_update", [this, text_arg](const BridgeArgs& args) -> BridgeArgs {
			start_download(text_arg(args, 0), text_arg(args, 1), text_arg(args, 2));
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
				std::ifstream in(from_utf8(*path), std::ios::binary | std::ios::ate);
				if (!in)
				{
					return {std::string{}};
				}
				const auto size = static_cast<std::streamoff>(in.tellg());
				if (size <= 0)
				{
					return {std::string{}};
				}
				std::string text(static_cast<std::size_t>(size), '\0');
				in.seekg(0);
				in.read(text.data(), size);
				text.resize(static_cast<std::size_t>(in.gcount()));
				return {std::move(text)};
			}
			catch (...)
			{
				return {std::string{}};
			}
		});
		(void)r8;

		// Qt styling functions
		auto r9 = bridge.register_function("studio_theme_qt", "apply", [this, text_arg](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				schedule(text_arg(args, 0));
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

		auto r11 = bridge.register_function("studio_theme_qt", "pick_image", [this, text_arg](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				schedule_pick(text_arg(args, 0));
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

		// compose_topbar / compose_panel(image, "#RRGGBB", opacity, mode, outDir, key [, classes])
		// Cuts the picture to the size of the widget(s) it is for, blended onto the color, and answers
		// (event payload + shared slot) with {key, path, error}. Same cutter; the only difference is
		// which event answers: compose_topbar -> topbar_ready, compose_panel -> panel_ready.
		auto make_composer = [this, text_arg](bool panel) {
			return [this, text_arg, panel](const BridgeArgs& args) -> BridgeArgs {
				try
				{
					double opacity = 1.0;
					if (args.size() > 2)
					{
						if (const auto* d = std::get_if<double>(&args[2]))
						{
							opacity = *d;
						}
					}
					schedule_compose(text_arg(args, 0), text_arg(args, 1), opacity, text_arg(args, 3), text_arg(args, 4),
					                 text_arg(args, 5), text_arg(args, 6), panel);
					return {true};
				}
				catch (...)
				{
					return {false};
				}
			};
		};
		auto r13 = bridge.register_function("studio_theme_qt", "compose_topbar", make_composer(false));
		(void)r13;
		auto r13b = bridge.register_function("studio_theme_qt", "compose_panel", make_composer(true));
		(void)r13b;

		auto r14 = bridge.register_function("studio_theme_qt", "set_presets", [this, text_arg](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				schedule_presets(text_arg(args, 0));
				return {true};
			}
			catch (...)
			{
				return {false};
			}
		});
		(void)r14;

		auto r15 = bridge.register_function("studio_theme_qt", "restyle_widgets", [this, text_arg](const BridgeArgs& args) -> BridgeArgs {
			try
			{
				schedule_restyle(text_arg(args, 0), text_arg(args, 1), text_arg(args, 2));
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
				                       "capabilities", "save_theme", "load_theme", "apply", "scan", "pick_image",
				                       "install_image", "compose_topbar", "compose_panel", "set_presets", "restyle_widgets"})
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

	// Runs `task` on Qt's GUI thread (or right here when Qt isn't up).
	void on_gui_thread(std::function<void()> task)
	{
		if (auto* qt = rml::qt::QtIntegration::instance())
		{
			qt->run_on_gui_thread(std::move(task));
		}
		else
		{
			task();
		}
	}

	// Hands a JSON result to the scripts: shared slot + event. `payload` goes out as the event
	// argument too, so a script can take the result from the event itself and several results can
	// be in flight at once (a shared slot only keeps the newest).
	void publish(const char* key, const char* event, std::string json, bool as_payload = false)
	{
		auto alive = m_alive;
		auto task = [this, alive, key, event, as_payload, json = std::move(json)]() {
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
				auto emitted = bridge.emit(event, BridgeArgs{std::string{as_payload ? json : std::string{"ok"}}});
				(void)emitted;
			}
			catch (...)
			{
			}
		};

		try
		{
			on_gui_thread(std::move(task));
		}
		catch (...)
		{
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
			int tick = 0;
			while (alive->load() && !m_stop)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (!alive->load() || m_stop)
				{
					return;
				}
				++tick;
				const bool resize_check = tick % 3 == 0 && m_watch_cuts.load();
				const bool reassert_check = tick % 20 == 0;
				if (!resize_check && !reassert_check)
				{
					continue;
				}
				auto task = [this, alive, resize_check, reassert_check]() {
					if (!alive->load())
					{
						return;
					}
					try
					{
						if (resize_check)
						{
							check_cut_sizes();
						}
						if (reassert_check)
						{
							reassert();
						}
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
		std::shared_ptr<const RestyleSpec> spec;
		{
			std::lock_guard lock(m_mutex);
			if (m_wanted && app->style_sheet().find(kSheetMarker) == std::string::npos)
			{
				m_log->info("Studio replaced its stylesheet; restoring theme");
				apply_locked(m_css);
			}
			spec = m_restyle;
		}
		// Periodically re-ensure menus and dynamic dropdowns keep their theme
		if (spec && spec->has_css)
		{
			restyle_widgets_now(std::move(spec));
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
		on_gui_thread([this, names = std::move(names)]() {
			try
			{
				auto* qt = rml::qt::QtIntegration::instance();
				if (!qt || m_menu_presets == 0)
				{
					return;
				}
				const std::size_t digest = std::hash<std::string>{}(names);
				if (m_have_presets && digest == m_last_presets_hash)
				{
					return;
				}
				m_have_presets = true;
				m_last_presets_hash = digest;
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
		});
	}

	void schedule_scan()
	{
		on_gui_thread([this]() {
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
		});
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

	// The biggest width and the biggest height among the widgets whose class contains any of `wanted`.
	static std::pair<int, int> measure_classes(const std::vector<std::string_view>& wanted)
	{
		int w = 0;
		int h = 0;
		for (auto* widget : rml::qt::QApplication::all_widgets())
		{
			if (!widget)
			{
				continue;
			}
			const char* cls = widget->class_name();
			if (!cls)
			{
				continue;
			}
			const std::string_view name(cls);
			for (const auto want : wanted)
			{
				if (name.find(want) != std::string_view::npos)
				{
					w = std::max(w, widget->width());
					h = std::max(h, widget->height());
					break;
				}
			}
		}
		return {std::min(w, 8192), std::min(h, 4096)};
	}

	// Runs every ~0.3 s while something is cut: a panel that was resized (or that wasn't on screen
	// yet when it was first cut) is cut again once its size holds still, with no help from the scripts.
	void check_cut_sizes()
	{
		std::vector<CutRequest> again;
		for (auto& entry : m_cuts)
		{
			CutRequest& cut = entry.second;
			const auto size = measure_classes(split_classes(cut.classes));
			if (next_cut_step(cut.size, size.first, size.second) == CutStep::Recut)
			{
				again.push_back(cut);
			}
		}
		for (auto& cut : again)
		{
			schedule_compose(cut.image, cut.hex, cut.opacity, cut.mode, cut.out_dir, cut.key, cut.size_class, cut.panel);
		}
	}

	// Removes the group's old cuts, keeping the newest few (and `current`).
	static void prune_cuts(const std::filesystem::path& dir, const std::string& group, const std::filesystem::path& current)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		std::vector<std::pair<std::int64_t, std::string>> files;
		const std::string prefix = group + "_";
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			const std::string name = utf8(it->path().filename());
			if (name.rfind(prefix, 0) == 0 && it->path().extension() == ".bmp")
			{
				std::error_code te;
				const auto when = it->last_write_time(te).time_since_epoch().count();
				files.emplace_back(static_cast<std::int64_t>(when), name);
			}
		}
		for (const auto& name : cuts_to_delete(std::move(files), utf8(current.filename()), 6))
		{
			std::error_code re;
			fs::remove(dir / from_utf8(name), re);
		}
	}

	// Cuts `image` to the widgets named in `size_class` ("A|B": the biggest width and the biggest
	// height among them, so one picture covers every one of them), blended onto `hex` at `opacity`.
	// Native size (1x): the scripts draw it unscaled, so it can never stretch. The result is a BMP
	// (no compression: far quicker to write than PNG) named after everything that shapes it, so an
	// identical request is answered from disk without cutting again - also after a restart.
	void schedule_compose(std::string image, std::string hex, double opacity, std::string mode, std::string out_dir,
	                      std::string key, std::string size_class, bool panel)
	{
		auto alive = m_alive;
		on_gui_thread([this, alive, image = std::move(image), hex = std::move(hex), opacity, mode = std::move(mode),
		               out_dir = std::move(out_dir), key = std::move(key), size_class = std::move(size_class), panel]() {
			if (!alive->load())
			{
				return;
			}
			namespace fs = std::filesystem;
			std::string result;
			std::string error;
			std::string cut_group;
			std::string cut_classes;
			int cut_w = 0;
			int cut_h = 0;
			try
			{
				const bool top_bar = size_class.empty();
				const std::string classes = top_bar ? std::string("MenuBar|TopBar") : size_class;
				const std::string group = top_bar ? std::string("topbar") : safe_group(size_class);
				const auto wanted = split_classes(classes);

				auto size = measure_classes(wanted);
				int w = size.first;
				int h = size.second;
				cut_group = group;
				cut_classes = classes;
				if (w < 8 || h < 8)
				{
					if (top_bar)
					{
						w = 1920;
						h = 36;
					}
					else
					{
						error = "panel isn't on screen yet: " + size_class;
					}
				}

				if (error.empty())
				{
					cut_w = w;
					cut_h = h;
					std::error_code ec;
					const fs::path source_path = from_utf8(image);
					const auto source_size = fs::file_size(source_path, ec);
					ec.clear();
					const auto source_time = fs::last_write_time(source_path, ec).time_since_epoch().count();
					ec.clear();
					const std::string signature =
					    std::format("{}|{}|{}|{}|{:.3f}|{}|{}x{}", image, source_size, source_time, mode, opacity, hex, w, h);

					const fs::path dir = from_utf8(out_dir);
					fs::create_directories(dir, ec);
					ec.clear();
					const fs::path file = dir / std::format("{}_{}.bmp", group, fnv1a_hex(signature));

					if (fs::exists(file, ec) && fs::file_size(file, ec) > 0)
					{
						// already cut: reuse it (and mark it fresh so pruning keeps it)
						result = utf8(file);
						ec.clear();
						fs::last_write_time(file, fs::file_time_type::clock::now(), ec);
					}
					else
					{
						std::optional<rml::qt::QPixmap> source_holder;
						source_holder.emplace(image);
						auto& source = *source_holder;
						if (!source.loaded() || source.width() <= 0 || source.height() <= 0)
						{
							error = "Qt couldn't read the image: " + image;
						}
						else
						{
							rml::qt::QPixmap canvas(w, h);
							canvas.fill(parse_hex(hex));
							{
								rml::qt::QPainter painter(canvas);
								painter.set_render_hint(rml::qt::QPainter::SmoothPixmapTransform);
								painter.set_opacity(std::clamp(opacity, 0.0, 1.0));
								using Aspect = rml::qt::QPixmap::AspectMode;
								if (mode == "Stretch")
								{
									painter.draw_pixmap(rml::qt::QRect(0, 0, w, h), source.scaled(w, h, Aspect::Ignore));
								}
								else if (mode == "Fit")
								{
									const auto fit = source.scaled(w, h, Aspect::Keep);
									painter.draw_pixmap((w - fit.width()) / 2, (h - fit.height()) / 2, fit);
								}
								else if (mode == "Tile")
								{
									const auto tile = source.scaled(w * 4, h, Aspect::Keep);
									for (int x = 0; tile.width() > 0 && x < w; x += tile.width())
									{
										painter.draw_pixmap(x, 0, tile);
									}
								}
								else
								{
									const auto cover = source.scaled(w, h, Aspect::KeepByExpanding);
									painter.draw_pixmap((w - cover.width()) / 2, (h - cover.height()) / 2, cover);
								}
							}
							source_holder.reset();
							if (canvas.save(utf8(file)))
							{
								result = utf8(file);
							}
							else
							{
								error = "couldn't save composed image to " + utf8(file);
							}
						}
					}
					if (!result.empty())
					{
						prune_cuts(dir, group, file);
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

			// Remember this request so a resize re-cuts it. A failed cut is remembered with size 0, so the
			// watcher cuts it as soon as the panel is on screen.
			if (!cut_group.empty())
			{
				CutRequest& cut = m_cuts[cut_group];
				cut.image = image;
				cut.hex = hex;
				cut.mode = mode;
				cut.out_dir = out_dir;
				cut.key = key;
				cut.size_class = size_class;
				cut.classes = cut_classes;
				cut.opacity = opacity;
				cut.panel = panel;
				cut.size = CutSize{};
				cut.size.w = result.empty() ? 0 : cut_w;
				cut.size.h = result.empty() ? 0 : cut_h;
				m_watch_cuts = true;
			}

			const std::string json = std::format("{{\"key\":\"{}\",\"path\":\"{}\",\"error\":\"{}\"}}",
			                                     json_escape(key), json_escape(result), json_escape(error));
			publish(panel ? "studio_theme.panel_image" : "studio_theme.topbar_image",
			        panel ? "studio_theme.panel_ready" : "studio_theme.topbar_ready", json, true);
		});
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
		on_gui_thread([this, images_dir = std::move(images_dir)]() {
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
		});
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

	static std::string strip_ours(std::string_view sheet)
	{
		const auto begin = sheet.find(kBegin);
		if (begin == std::string_view::npos)
		{
			return std::string(sheet);
		}
		const auto end = sheet.find(kEnd, begin);
		if (end == std::string_view::npos)
		{
			return std::string(sheet.substr(0, begin));
		}
		std::string out;
		out.reserve(sheet.size() - (end + kEnd.size() - begin));
		out.append(sheet.substr(0, begin));
		out.append(sheet.substr(end + kEnd.size()));
		return out;
	}

	static bool blank(const std::string& text)
	{
		return std::all_of(text.begin(), text.end(), [](char ch) {
			return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
		});
	}

	static std::string make_block(const std::string& css)
	{
		std::string block;
		block.reserve(kBegin.size() + css.size() + kEnd.size());
		block.append(kBegin);
		block.append(css);
		block.append(kEnd);
		return block;
	}

	// True when `own` is already <clean base> + block, so no rewrite is needed.
	static bool already_styled(const std::string& own, const std::string& block)
	{
		if (own.size() < block.size())
		{
			return false;
		}
		const std::size_t tail = own.size() - block.size();
		return own.compare(tail, std::string::npos, block) == 0 && own.find(kBegin) == tail;
	}

	void restyle_widgets_now(std::shared_ptr<const RestyleSpec> spec)
	{
		auto alive = m_alive;
		on_gui_thread([this, alive, spec = std::move(spec)]() {
			if (!alive->load())
			{
				return;
			}
			try
			{
				const auto& allow = spec->allow;
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

					const std::string_view clsView(cls);
					const bool isMenu = (clsView.find("Menu") != std::string_view::npos && clsView.find("MenuBar") == std::string_view::npos);
					const bool listed = allow.find(clsView) != allow.end();
					// Input boxes (Properties filter etc.) get the small color-only sheet: it's cheap
					// and reaches inputs whose class name isn't known. Anything on the allow list or
					// menu-like keeps the full sheet.
					const bool isInput = !isMenu && !listed && spec->has_leaf && looks_like_input(clsView);
					const bool allowed = spec->has_css && (listed || isMenu || isInput);
					const std::string& block = isInput ? spec->leaf_block : spec->css_block;

					const std::string own = widget_style_sheet(widget);

					if (!allowed)
					{
						if (!own.empty() && own.find(kBegin) != std::string::npos)
						{
							widget->setStyleSheet(rml::qt::QString(strip_ours(own)));
						}
						continue;
					}

					if (already_styled(own, block))
					{
						continue;
					}

					// Ensure menus and dropdowns are themed directly even if they had no prior stylesheet
					std::string want = own.empty() ? std::string() : strip_ours(own);
					want.append(block);
					if (want != own)
					{
						widget->setStyleSheet(rml::qt::QString(want));
					}
				}
			}
			catch (...)
			{
			}
		});
	}

	void schedule_restyle(std::string css, std::string allow_text, std::string leaf_css)
	{
		auto spec = std::make_shared<RestyleSpec>();
		spec->has_css = !css.empty();
		spec->has_leaf = !leaf_css.empty();
		if (spec->has_css)
		{
			spec->css_block = make_block(css);
		}
		if (spec->has_leaf)
		{
			spec->leaf_block = make_block(leaf_css);
		}
		{
			std::istringstream lines(allow_text.empty() ? std::string(kDefaultAllow) : std::move(allow_text));
			std::string line;
			while (std::getline(lines, line))
			{
				if (!line.empty() && line.back() == '\r')
				{
					line.pop_back();
				}
				if (!line.empty())
				{
					spec->allow.insert(line);
				}
			}
		}
		std::shared_ptr<const RestyleSpec> shared = std::move(spec);
		{
			std::lock_guard lock(m_mutex);
			m_restyle = shared;
		}
		restyle_widgets_now(std::move(shared));
	}

	void schedule(std::string css)
	{
		auto alive = m_alive;
		on_gui_thread([this, alive, css = std::move(css)]() {
			if (!alive->load())
			{
				return;
			}
			apply_now(css);
		});
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
		if (css.empty())
		{
			if (current != m_original)
			{
				app->set_style_sheet(m_original);
			}
			return;
		}
		std::string full;
		full.reserve(m_original.size() + kSheetMarker.size() + css.size() + 2);
		full.append(m_original).append("\n").append(kSheetMarker).append("\n").append(css);
		if (current == full)
		{
			return;
		}
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
