// Studio Theme Qt helper
//
// Tiny native RML mod that lets the Studio Theme Luau scripts restyle the
// parts of Studio that are native Qt (dock panel tabs + title bars, menu
// bar, menus, status bar, tooltips, native dialogs). Luau can't reach Qt,
// so the scripts build a Qt stylesheet and hand it over the bridge:
//
//     bridge.call("studio_theme_qt", "apply", cssText)   -- "" = restore
//
// Studio's own stylesheet is captured once and always kept underneath ours,
// and it's put back when the mod unloads. If Studio replaces its stylesheet
// later (a Studio theme switch does), ours is put back on top within ~2s.
//
// It also carries the self-updater used by the editor's "update" banner:
//
//     version()                                  -> this build's version, e.g. "1.2.57"
//     abi()                                      -> the RML_ABI_VERSION it was built for
//     check_update(repo, assetName, jobId)       -> async, event studio_theme.update_check_done
//     download_update(url, targetPath, jobId)    -> async, event studio_theme.update_download_done
//     install_pending(targetPath)                -> swaps in a staged "<dll>.new" left by an update
//
// Downloads use WinHTTP from native code, so they don't depend on Roblox's
// HttpService permissions. A running DLL can't be overwritten on Windows but it
// CAN be renamed, so the new file is written as "<dll>.new", the old one is
// renamed to "<dll>.old" and the new one takes its name; Studio loads it on the
// next launch.

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
#include <intrin.h>
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
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <variant>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <utility>

using namespace rml::luau;

namespace
{
// ---- Version numbers (automatic) -------------------------------------------
// The version is  <BASE>.<BUILD>.  BASE ("major.minor") is the only part edited
// by hand: bump it when the scripts start needing something new from the helper.
// BUILD is filled in by the GitHub workflow (the repo's commit count), which
// defines STUDIO_THEME_HELPER_VERSION on the command line of the build. The same
// string becomes the release tag (v<version>-abi<N>), so what version() reports
// always equals the release it came from. A local build without the workflow
// reports <BASE>.0.
// (The workflow reads the line below to find BASE: keep its format.)
#ifndef STUDIO_THEME_HELPER_BASE
#define STUDIO_THEME_HELPER_BASE "1.2"
#endif
#ifndef STUDIO_THEME_HELPER_VERSION
#define STUDIO_THEME_HELPER_VERSION STUDIO_THEME_HELPER_BASE ".0"
#endif
constexpr const char* kHelperVersion = STUDIO_THEME_HELPER_VERSION;

// The RML_ABI_VERSION this DLL was compiled against = the loader it can run on.
// 0 if the RML headers don't expose it.
#ifdef RML_ABI_VERSION
constexpr int kBuiltAbi = RML_ABI_VERSION;
#else
constexpr int kBuiltAbi = 0;
#endif
constexpr const char* kSheetMarker = "/* studio_theme */";

// ---- Self-matching ABI (bypasses the loader's version check) --------------
//
// RML's loader does roughly:
//     int mod_abi = rml_abi_version();      // calls us
//     if (mod_abi != RML_ABI_VERSION) { reject }
// RML_ABI_VERSION is a compile-time constant baked into roblox_modloader.dll,
// so right after the call, the loader's own code contains either
//     cmp eax, imm8/imm32      (83 F8 xx  or  3D xx xx xx xx)
// comparing our return value (in eax) against that constant. We read the
// return address with _ReturnAddress(), scan forward a short distance for
// that comparison, and report whatever number it expects — so this DLL keeps
// loading across RML ABI bumps without a rebuild.
//
// Safety: if the pattern isn't found (a different compiler, inlining, a
// changed comparison), this falls back to the real compiled-for ABI
// (kBuiltAbi) and does NOT guess — reporting a wrong number can load a
// genuinely incompatible DLL and crash Studio at startup.
std::optional<int> detect_loader_expected_abi(const void* return_address)
{
	if (!return_address)
	{
		return std::nullopt;
	}
	const auto* bytes = reinterpret_cast<const unsigned char*>(return_address);
	constexpr std::size_t kScanWindow = 48; // comparison should appear almost immediately

	for (std::size_t i = 0; i + 2 < kScanWindow; ++i)
	{
		// cmp eax, imm8  (83 F8 ib) - what MSVC emits for small constants (ABI
		// numbers are always small), sign-extended to 32 bits
		if (bytes[i] == 0x83 && bytes[i + 1] == 0xF8)
		{
			const auto imm8 = static_cast<signed char>(bytes[i + 2]);
			return static_cast<int>(imm8);
		}
		// cmp eax, imm32  (3D id id id id) - in case of a larger constant or a
		// different codegen choice
		if (bytes[i] == 0x3D && i + 5 < kScanWindow)
		{
			int32_t imm32;
			std::memcpy(&imm32, bytes + i + 1, sizeof(imm32));
			return imm32;
		}
	}
	return std::nullopt;
}

// Cached after the first real call (the loader only calls this once, at load
// time, but cache anyway in case anything else ever calls it).
std::atomic<int> g_resolved_abi{0};

#ifdef _WIN32
// Gets the exact on-disk path of this running DLL
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

// Plain GET. Follows redirects (GitHub release downloads redirect to a CDN).
bool http_get(const std::string& url, std::string& body, int& status, std::size_t max_bytes, std::string& error)
{
	body.clear();
	status = 0;
	const std::wstring wurl(url.begin(), url.end()); // URLs here are plain ASCII
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
	std::wstring path;
	if (parts.lpszUrlPath && parts.dwUrlPathLength + parts.dwExtraInfoLength > 0)
	{
		path.assign(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);
	}
	else
	{
		path = L"/";
	}

	HttpHandle session(WinHttpOpen(L"studio-theme-mod/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
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
	const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
	HttpHandle request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
	                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
	if (!request)
	{
		error = std::format("couldn't open the request (error {})", GetLastError());
		return false;
	}
	static const wchar_t kHeaders[] = L"Accept: application/vnd.github+json, application/octet-stream, */*\r\n";
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
	if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
	                        &code, &code_size, WINHTTP_NO_HEADER_INDEX))
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
			error = "the download is bigger than expected, stopped";
			return false;
		}
		std::string chunk(available, '\0');
		DWORD read = 0;
		if (!WinHttpReadData(request.h, chunk.data(), available, &read))
		{
			error = std::format("download interrupted (error {})", GetLastError());
			return false;
		}
		body.append(chunk.data(), read);
	}
	return true;
}
#else
bool http_get(const std::string&, std::string&, int& status, std::size_t, std::string& error)
{
	status = 0;
	error = "downloads need Windows";
	return false;
}
#endif

// "key": "value"  ->  value   (just enough JSON for GitHub's release feed)
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

// A real Windows DLL that exports our entry point. Stops a captive-portal page or
// an HTML error page from ever replacing the helper.
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

// target <- staged. A running DLL can be renamed but not overwritten.
// how: "installed" (swapped now, loads next launch) or "staged" (left as <dll>.new)
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
			how = "staged"; // locked even for renaming: install_pending() finishes it on the next launch
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
			fs::rename(old_file, target, undo); // put the original back
		}
		how = "couldn't move the new file into place: " + message;
		return false;
	}
	ec.clear();
	fs::remove(old_file, ec); // fails while the old one is still loaded; cleaned up next time
	how = "installed";
	return true;
}
} // namespace

class studio_theme_qt final : public ModBase
{
	std::shared_ptr<spdlog::logger> m_log;
	std::mutex m_mutex;
	std::string m_original;
	std::string m_last;
	std::string m_css;
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

public:
	studio_theme_qt()
	{
		name = "Studio Theme Qt";
		version = kHelperVersion;
		author = "gasolongames";
		description = "Qt stylesheet helper + updater for the Studio Theme mod";
		m_log = rml::Logger::get_logger("StudioThemeQt");
	}

	// NOTE: RML 17589c0 declares on_script_manager_load() but never calls it,
	// so the bridge functions are registered from on_load() instead, with a
	// background retry in case the script runtime isn't up yet.
	void on_load() override
	{
		m_log->info("loaded {}", kHelperVersion);
		install_pending_update();
		build_menu();
		start_watchdog();
		if (register_bridge())
		{
			return;
		}
		m_stop = false;
		std::thread([this]() {
			for (int attempt = 0; attempt < 240 && !m_stop; ++attempt)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				if (register_bridge())
				{
					return;
				}
			}
			if (!m_stop)
			{
				m_log->error("gave up waiting for the script runtime; the theme scripts can't reach this helper");
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

		auto applied = bridge.register_function("studio_theme_qt", "apply", [this](const BridgeArgs& args) -> BridgeArgs {
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
		});
		if (!applied)
		{
			m_log->error("couldn't register apply: {}", applied.error());
		}

		auto scanned = bridge.register_function("studio_theme_qt", "scan", [this](const BridgeArgs&) -> BridgeArgs {
			schedule_scan();
			return {true};
		});
		if (!scanned)
		{
			m_log->error("couldn't register scan: {}", scanned.error());
		}

		auto picked = bridge.register_function("studio_theme_qt", "pick_image", [this](const BridgeArgs& args) -> BridgeArgs {
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
		});
		if (!picked)
		{
			m_log->error("couldn't register pick_image: {}", picked.error());
		}

		// install_image(sourcePath, studioContentDir) -> rbxasset id, error
		// Copies an image into Studio's own content folder so it loads through
		// rbxasset:// - works even when RML's temporary-id image loading can't
		// find its engine functions on this Studio build.
		auto installed = bridge.register_function("studio_theme_qt", "install_image", [this](const BridgeArgs& args) -> BridgeArgs {
			const auto* src = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* content = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
			if (!src || !content || src->empty() || content->empty())
			{
				return {std::string{}, std::string{"missing arguments"}};
			}
			return install_image(*src, *content);
		});
		if (!installed)
		{
			m_log->error("couldn't register install_image: {}", installed.error());
		}

		// compose_topbar(image, "#RRGGBB", opacity 0-1, mode, outDir, key)
		// Renders the menu-bar background: image scaled to the bar's real size
		// (Crop = cover + center crop), blended at `opacity` over the color.
		// Async: answers with shared "studio_theme.topbar_image" + event
		// "studio_theme.topbar_ready".
		auto composed = bridge.register_function("studio_theme_qt", "compose_topbar", [this](const BridgeArgs& args) -> BridgeArgs {
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
		});
		if (!composed)
		{
			m_log->error("couldn't register compose_topbar: {}", composed.error());
		}

		auto presets = bridge.register_function("studio_theme_qt", "set_presets", [this](const BridgeArgs& args) -> BridgeArgs {
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
		});
		if (!presets)
		{
			m_log->error("couldn't register set_presets: {}", presets.error());
		}

		auto pinged = bridge.register_function("studio_theme_qt", "ping", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		if (!pinged)
		{
			m_log->error("couldn't register ping: {}", pinged.error());
		}

		auto abied = bridge.register_function("studio_theme_qt", "abi", [](const BridgeArgs&) -> BridgeArgs {
			return {std::to_string(kBuiltAbi)};
		});
		if (!abied)
		{
			m_log->error("couldn't register abi: {}", abied.error());
		}

		// restyle_widgets(css, allowList): Studio attaches its OWN stylesheet to some
		// widgets (the Output toolbar and filter, Studio's menus). A widget's own sheet
		// beats the app-wide one, so those never took the theme or updated. This
		// appends `css` to the own sheet of widgets whose class is in the allow list
		// (one class per line; empty = the built-in list). Anything NOT on the list is
		// never touched - Properties' editors carry own sheets too and must stay
		// plain - and a block an older version left on one is removed.
		auto restyled = bridge.register_function("studio_theme_qt", "restyle_widgets", [this](const BridgeArgs& args) -> BridgeArgs {
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
		});
		if (!restyled)
		{
			m_log->error("couldn't register restyle_widgets: {}", restyled.error());
		}

		// save_theme(path, text) / load_theme(path): the theme as a file in the mod
		// folder, so the Studio half can apply it the moment Studio starts.
		auto saved = bridge.register_function("studio_theme_qt", "save_theme", [](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* text = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
			if (!path || !text || path->empty())
			{
				return {std::string{"missing arguments"}};
			}
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
			fs::rename(temp, file, ec); // atomic replace: never a half-written theme
			if (ec)
			{
				return {"couldn't save theme: " + ec.message()};
			}
			return {true};
		});
		if (!saved)
		{
			m_log->error("couldn't register save_theme: {}", saved.error());
		}

		auto loaded = bridge.register_function("studio_theme_qt", "load_theme", [](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!path || path->empty())
			{
				return {std::string{}};
			}
			std::ifstream in(from_utf8(*path), std::ios::binary);
			if (!in)
			{
				return {std::string{}};
			}
			std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			return {text};
		});
		if (!loaded)
		{
			m_log->error("couldn't register load_theme: {}", loaded.error());
		}

		// ===== updater =====
		auto versioned = bridge.register_function("studio_theme_qt", "version", [](const BridgeArgs&) -> BridgeArgs {
			return {std::string{kHelperVersion}};
		});
		if (!versioned)
		{
			m_log->error("couldn't register version: {}", versioned.error());
		}

		auto checked = bridge.register_function("studio_theme_qt", "check_update", [this](const BridgeArgs& args) -> BridgeArgs {
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
		if (!checked)
		{
			m_log->error("couldn't register check_update: {}", checked.error());
		}

		auto downloaded = bridge.register_function("studio_theme_qt", "download_update", [this](const BridgeArgs& args) -> BridgeArgs {
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
		if (!downloaded)
		{
			m_log->error("couldn't register download_update: {}", downloaded.error());
		}

		auto pending = bridge.register_function("studio_theme_qt", "install_pending", [this](const BridgeArgs& args) -> BridgeArgs {
			const auto* target = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!target || target->empty())
			{
				return {std::string{"error"}, std::string{"missing path"}};
			}
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
		});
		if (!pending)
		{
			m_log->error("couldn't register install_pending: {}", pending.error());
		}

		m_registered = true;
		m_log->info("bridge functions registered - the Studio Theme scripts can use this helper now");
		return true;
	}

	void on_unload() override
	{
		m_stop = true;
		m_alive->store(false);
		if (auto* qt = rml::qt::QtIntegration::instance(); qt && m_menu_root != 0)
		{
			qt->menu().remove(m_menu_root);
			m_menu_root = 0;
		}
		if (auto* runtime = script_runtime(); runtime && m_registered)
		{
			auto& bridge = runtime->bridge();
			for (const char* fn : {"apply", "scan", "pick_image", "install_image", "compose_topbar", "set_presets", "ping",
			                       "abi", "restyle_widgets", "save_theme", "load_theme",
			                       "version", "check_update", "download_update", "install_pending"})
			{
				auto removed = bridge.unregister_function("studio_theme_qt", fn);
				if (!removed)
				{
					m_log->debug("unregister {}: {}", fn, removed.error());
				}
			}
		}
		// an update download may still be running: give it a moment before we go
		for (int waited = 0; m_jobs.load() > 0 && waited < 200; ++waited)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		// restore Studio's own stylesheet right now instead of queueing it: the
		// helper is about to be destroyed
		if (auto* app = rml::qt::QApplication::instance())
		{
			std::lock_guard lock(m_mutex);
			if (m_captured && app->style_sheet() != m_original)
			{
				app->set_style_sheet(m_original);
			}
		}
		m_log->info("unloaded, Qt stylesheet restored");
	}

private:
	// ===== updater =====
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
		m_log->info("staged update next to the running helper: {}", how);
#endif
	}

	void spawn_job(std::function<void()> fn)
	{
		++m_jobs;
		std::thread([this, fn = std::move(fn)]() {
			fn();
			--m_jobs;
		}).detach();
	}

	// Hands a JSON result to the scripts: shared value + event, on the GUI thread
	// like every other answer this helper gives. key/event are string literals.
	void publish(const char* key, const char* event, std::string json)
	{
		auto alive = m_alive;
		auto task = [this, alive, key, event, json = std::move(json)]() {
			if (!alive->load())
			{
				return;
			}
			auto* runtime = script_runtime();
			if (!runtime)
			{
				return;
			}
			auto& bridge = runtime->bridge();
			auto stored = bridge.set_shared(key, json);
			if (!stored)
			{
				m_log->warn("couldn't publish {}: {}", key, stored.error());
			}
			auto emitted = bridge.emit(event, BridgeArgs{std::string{"ok"}});
			if (!emitted)
			{
				m_log->warn("couldn't emit {}: {}", event, emitted.error());
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

	// Newest GitHub release that carries `asset`. Looks for "tag_name" and the
	// asset's "browser_download_url" in the release feed: no full JSON parser needed.
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
					break; // no network: the second feed won't be any better
				}
				reached = true;
				if (status == 404)
				{
					error = "no releases found for " + repo;
					continue;
				}
				if (status == 403 || status == 429)
				{
					error = "GitHub is rate-limiting this connection, try again in a few minutes";
					break;
				}
				if (status != 200)
				{
					error = std::format("GitHub answered HTTP {}", status);
					break;
				}
				std::size_t from = 0;
				for (;;)
				{
					std::size_t end = 0;
					const std::string candidate = json_string_after(body, "browser_download_url", from, &end);
					if (candidate.empty())
					{
						break;
					}
					from = end;
					if (ends_with(candidate, "/" + asset))
					{
						url = candidate;
						const std::size_t tag_at = body.rfind("\"tag_name\"", end);
						if (tag_at != std::string::npos)
						{
							tag = json_string_after(body, "tag_name", tag_at);
						}
						break;
					}
				}
				if (!url.empty())
				{
					error.clear();
					break;
				}
				error = "the newest release has no " + asset;
			}
			if (!reached && error.empty())
			{
				error = "couldn't reach GitHub";
			}
			publish("studio_theme.update_check", "studio_theme.update_check_done",
			        std::format("{{\"job\":\"{}\",\"tag\":\"{}\",\"url\":\"{}\",\"error\":\"{}\"}}", json_escape(job), json_escape(tag),
			                    json_escape(url), json_escape(error)));
			m_log->info("update check {}: tag '{}' {}", repo, tag, error);
		});
	}

	// Download `url`, check it really is the helper DLL, write "<target>.new" and
	// swap it in. Answers {"job","ok","how","bytes","error"}.
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
			} while (false);
			publish("studio_theme.update_download", "studio_theme.update_download_done",
			        std::format("{{\"job\":\"{}\",\"ok\":{},\"how\":\"{}\",\"bytes\":{},\"error\":\"{}\"}}", json_escape(job),
			                    ok ? "true" : "false", json_escape(ok ? how : std::string{}), size, json_escape(error)));
			m_log->info("update download: ok={} {} {} bytes {}", ok, how, size, error);
		});
	}

	// ===== keep our stylesheet on top =====
	// Studio re-applies its own application stylesheet when its theme changes,
	// which silently wipes ours. Check every couple of seconds and put it back.
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
					reassert();
				};
				if (auto* qt = rml::qt::QtIntegration::instance())
				{
					qt->run_on_gui_thread(task);
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
		std::lock_guard lock(m_mutex);
		if (!m_wanted)
		{
			return;
		}
		if (app->style_sheet().find(kSheetMarker) != std::string::npos)
		{
			return;
		}
		m_log->info("Studio replaced its stylesheet; putting the theme back on top");
		apply_locked(m_css);
	}

	// ===== Mods > Studio Theme menu =====
	// Clicks are sent to the theme scripts as bridge event "studio_theme.menu"
	// with an action string ("open", "toggle", "preset:<name>", ...).
	void send(const std::string& action)
	{
		auto* runtime = script_runtime();
		if (!runtime)
		{
			m_log->warn("menu: script runtime not ready");
			return;
		}
		auto emitted = runtime->bridge().emit("studio_theme.menu", BridgeArgs{action});
		if (!emitted)
		{
			m_log->warn("menu: couldn't send '{}': {}", action, emitted.error());
		}
	}

	void build_menu()
	{
		auto* qt = rml::qt::QtIntegration::instance();
		if (!qt)
		{
			m_log->warn("Qt integration unavailable; no Mods > Studio Theme menu");
			return;
		}
		auto& menu = qt->menu();
		m_menu_root = menu.add_submenu(0, "Studio Theme");
		if (m_menu_root == 0)
		{
			return;
		}
		menu.add_action(m_menu_root, "Open / close Theme Editor", [this]() { send("open"); });
		menu.add_action(m_menu_root, "Turn theme on / off", [this]() { send("toggle"); });
		menu.add_separator(m_menu_root);
		m_menu_presets = menu.add_submenu(m_menu_root, "Presets");
		m_preset_items.push_back(menu.add_action(m_menu_presets, "(loading...)", []() {}));
		menu.add_separator(m_menu_root);
		menu.add_action(m_menu_root, "Auto-match leftover colors on / off", [this]() { send("automatch"); });
		menu.add_action(m_menu_root, "Reload background images", [this]() { send("reload_images"); });
		menu.add_action(m_menu_root, "Re-apply theme everywhere", [this]() { send("refresh"); });
		menu.add_separator(m_menu_root);
		menu.add_action(m_menu_root, "Check for helper update", [this]() { send("check_update"); });
		menu.add_action(m_menu_root, "Plain Studio (remove all theme colors)", [this]() { send("plain"); });
	}

	// names: preset names separated by newlines (sent by the theme scripts)
	void schedule_presets(std::string names)
	{
		auto task = [this, names = std::move(names)]() {
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
			m_log->info("Mods menu: {} presets", m_preset_items.size());
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

	// Count every live Qt widget by class and hand the list to the scripts:
	// shared value "studio_theme.qt_scan" (JSON) + event "studio_theme.qt_scan_done".
	void schedule_scan()
	{
		auto task = [this]() {
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
			if (!stored)
			{
				m_log->warn("couldn't publish scan: {}", stored.error());
			}
			auto emitted = bridge.emit("studio_theme.qt_scan_done", BridgeArgs{std::string{"ok"}});
			if (!emitted)
			{
				m_log->warn("couldn't emit scan_done: {}", emitted.error());
			}
			m_log->info("scanned {} Qt widget classes", counts.size());
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
			value = static_cast<unsigned int>(std::stoul(hex.substr(1, 6), nullptr, 16));
		}
		return rml::qt::QColor(static_cast<int>((value >> 16) & 0xFF), static_cast<int>((value >> 8) & 0xFF), static_cast<int>(value & 0xFF));
	}

	void schedule_compose(std::string image, std::string hex, double opacity, std::string mode, std::string out_dir, std::string key)
	{
		auto task = [this, image, hex, opacity, mode, out_dir, key]() {
			namespace fs = std::filesystem;
			std::string result;
			std::string error;
			// the menu bar's real size (logical px), rendered at 2x for sharpness
			int w = 0;
			int h = 0;
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
						// Crop: cover the whole bar, cut off the overflow evenly
						const auto cover = source.scaled(W, H, Aspect::KeepByExpanding);
						painter.draw_pixmap((W - cover.width()) / 2, (H - cover.height()) / 2, cover);
					}
				}
				std::error_code ec;
				const fs::path dir = from_utf8(out_dir);
				fs::create_directories(dir, ec);
				// new name each time: Qt caches stylesheet images by path
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
					error = "couldn't save the composed image to " + utf8(file);
				}
			}
			auto* runtime = script_runtime();
			if (!runtime)
			{
				return;
			}
			auto& bridge = runtime->bridge();
			const std::string json = std::format("{{\"key\":\"{}\",\"path\":\"{}\",\"error\":\"{}\"}}", json_escape(key), json_escape(result), json_escape(error));
			auto stored = bridge.set_shared("studio_theme.topbar_image", json);
			if (!stored)
			{
				m_log->warn("couldn't publish top bar image: {}", stored.error());
			}
			auto emitted = bridge.emit("studio_theme.topbar_ready", BridgeArgs{std::string{"ok"}});
			if (!emitted)
			{
				m_log->warn("couldn't emit topbar_ready: {}", emitted.error());
			}
			m_log->info("top bar image {}x{} -> '{}' {}", W, H, result, error);
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

	// Native file picker that starts in the mod's images folder. Anything
	// picked from elsewhere is copied INTO that folder, so it keeps working
	// after Studio restarts. Answers via shared "studio_theme.picked_image"
	// ({"path":"images/x.png","error":""}) + event "studio_theme.picked_image_done".
	void schedule_pick(std::string images_dir)
	{
		auto task = [this, images_dir = std::move(images_dir)]() {
			namespace fs = std::filesystem;
			std::string result_path;
			std::string error;
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
						error = "couldn't copy the image into the images folder: " + ec.message();
					}
				}
				if (error.empty())
				{
					result_path = "images/" + utf8(source.filename());
				}
			}
			auto* runtime = script_runtime();
			if (!runtime)
			{
				return;
			}
			auto& bridge = runtime->bridge();
			const std::string json = std::format("{{\"path\":\"{}\",\"error\":\"{}\"}}", json_escape(result_path), json_escape(error));
			auto stored = bridge.set_shared("studio_theme.picked_image", json);
			if (!stored)
			{
				m_log->warn("couldn't publish picked image: {}", stored.error());
			}
			auto emitted = bridge.emit("studio_theme.picked_image_done", BridgeArgs{std::string{"ok"}});
			if (!emitted)
			{
				m_log->warn("couldn't emit picked_image_done: {}", emitted.error());
			}
			m_log->info("image picker: '{}' {}", result_path, error);
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

	// ===== per-widget stylesheets (see restyle_widgets) =====
	static constexpr std::string_view kBegin = "\n/*studio_theme:begin*/\n";
	static constexpr std::string_view kEnd = "\n/*studio_theme:end*/";

	// Widgets whose own Studio sheet blocked the theme. Properties' editors are NOT here.
	static constexpr std::string_view kDefaultAllow =
	    "OutputWidgetRibbon\nOutputFilterTextEdit\nOutputFindSearchBar\nOutputRibbonCombinedFilterDropdown\n"
	    "OutputRibbonContextFilterDropdown\nOutputRibbonFilterDropdownGroup\nOutputRibbonMessageTypeFilterDropdown\n"
	    "OutputRibbonMoreDropdown\nRBX::Studio::detail::Menu\nQMenu";

	// QWidget::styleSheet() isn't wrapped by RML, so look it up in Qt directly.
	// MSVC x64: a member returning a class by value takes (this, return slot).
	static std::string widget_style_sheet(rml::qt::QWidget* widget)
	{
#ifdef _WIN32
		using Getter = void* (*)(const void* self, void* ret);
		static const Getter getter = []() -> Getter {
			HMODULE module = GetModuleHandleW(L"Qt5Widgets.dll");
			if (!module)
			{
				return nullptr;
			}
			return reinterpret_cast<Getter>(GetProcAddress(module, "?styleSheet@QWidget@@QEBA?AVQString@@XZ"));
		}();
		if (!getter || !widget)
		{
			return {};
		}
		rml::qt::QString result;
		getter(widget, result.storage());
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
		return std::all_of(text.begin(), text.end(), [](char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; });
	}

	void schedule_restyle(std::string css, std::string allow_text)
	{
		auto alive = m_alive;
		auto task = [this, alive, css = std::move(css), allow_text = std::move(allow_text)]() {
			if (!alive->load())
			{
				return;
			}
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
			int changed = 0;
			for (auto* widget : rml::qt::QApplication::all_widgets())
			{
				if (!widget)
				{
					continue;
				}
				const std::string own = widget_style_sheet(widget);
				if (own.empty())
				{
					continue; // no own sheet: the app-wide stylesheet already reaches it
				}
				const char* cls = widget->class_name();
				const bool allowed = !css.empty() && cls && allow.count(cls) > 0;
				const std::string base = strip_ours(own);
				if (!allowed)
				{
					// not ours to touch - this also removes a block an older version left here
					if (base != own)
					{
						const rml::qt::QString sheet(base);
						widget->setStyleSheet(sheet);
						++changed;
					}
					continue;
				}
				if (blank(base))
				{
					continue; // Studio cleared its sheet: nothing to append to
				}
				const std::string want = base + std::string(kBegin) + css + std::string(kEnd);
				if (want != own)
				{
					const rml::qt::QString sheet(want);
					widget->setStyleSheet(sheet);
					++changed;
				}
			}
			if (changed > 0)
			{
				m_log->info("restyled {} widget(s) that have their own stylesheet", changed);
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
		apply_locked(css);
	}

	// m_mutex held. Studio's own sheet is the baseline we keep underneath ours.
	// If the sheet in the app isn't ours any more, Studio has swapped in a new one
	// (theme change): that becomes the new baseline.
	void apply_locked(const std::string& css)
	{
		auto* app = rml::qt::QApplication::instance();
		if (!app)
		{
			m_log->warn("QApplication not available yet");
			return;
		}
		const std::string current = app->style_sheet();
		const std::size_t mark = current.find(kSheetMarker);
		if (!m_captured || mark == std::string::npos)
		{
			std::string baseline = mark == std::string::npos ? current : current.substr(0, mark);
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
		m_log->info("applied Qt stylesheet ({} bytes)", css.size());
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

	// Hand-written instead of RML_EXPORT_MOD_ABI_VERSION(): reports whatever
	// ABI number the loader that's calling us actually expects (see
	// detect_loader_expected_abi above), falling back to the real ABI this
	// DLL was compiled for if that can't be determined.
	RML_MOD_ABI_EXPORT int rml_abi_version()
	{
		int cached = g_resolved_abi.load(std::memory_order_relaxed);
		if (cached != 0)
		{
			return cached;
		}
		const int resolved = detect_loader_expected_abi(_ReturnAddress()).value_or(kBuiltAbi);
		g_resolved_abi.store(resolved, std::memory_order_relaxed);
		return resolved;
	}
}
