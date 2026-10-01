// Studio Theme Qt helper (1.1.0 + theme features)
//
// Tiny native RML mod that lets the Studio Theme Luau scripts restyle the
// parts of Studio that are native Qt (dock panel tabs + title bars, menu
// bar, menus, status bar, tooltips, native dialogs). Luau can't reach Qt,
// so the scripts build a Qt stylesheet and hand it over the bridge:
//
//     bridge.call("studio_theme_qt", "apply", cssText)   -- "" = restore
//
// Studio's own stylesheet is captured once and always kept underneath ours,
// and it's put back when the mod unloads.

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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

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

using namespace rml::luau;

#ifdef _WIN32
// Our own on-disk DLL path, found via the address of this very function
// rather than DllMain (portable to however RML loads mods, no extra hook
// needed). Used so a staged update can be swapped in next launch.
static std::filesystem::path self_module_path()
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&self_module_path),
			&module))
	{
		return {};
	}
	wchar_t buffer[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
	if (length == 0 || length == MAX_PATH)
	{
		return {};
	}
	return std::filesystem::path(buffer, buffer + length);
}

#endif

namespace
{
#ifndef STUDIO_THEME_VERSION
// Default for local builds. Release/CI builds can define STUDIO_THEME_VERSION
// (for example, from the Git tag) so the DLL version updates automatically.
#define STUDIO_THEME_VERSION "1.1.2"
#endif

constexpr const char* kHelperVersion = STUDIO_THEME_VERSION;
constexpr const char* kSheetMarker = "/* studio_theme */";

#ifdef _WIN32
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
            WinHttpCloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

bool http_get(const std::string& url, std::string& body, int& status,
              std::size_t max_bytes, std::string& error)
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

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts) ||
        !parts.lpszHostName)
    {
        error = "bad url: " + url;
        return false;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path;
    if (parts.lpszUrlPath &&
        parts.dwUrlPathLength + parts.dwExtraInfoLength > 0)
    {
        path.assign(parts.lpszUrlPath,
                    parts.dwUrlPathLength + parts.dwExtraInfoLength);
    }
    else
    {
        path = L"/";
    }

    HttpHandle session(WinHttpOpen(
        L"studio-theme-mod/1.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
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

    const DWORD flags =
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;

    HttpHandle request(WinHttpOpenRequest(
        connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags));

    if (!request)
    {
        error = std::format("couldn't open the request (error {})", GetLastError());
        return false;
    }

    static const wchar_t kHeaders[] =
        L"Accept: application/vnd.github+json, application/octet-stream, */*\r\n";

    if (!WinHttpSendRequest(
            request.h, kHeaders, static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
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
    if (WinHttpQueryHeaders(
            request.h,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
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
            break;

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
bool http_get(const std::string&, std::string&, int& status,
              std::size_t, std::string& error)
{
    status = 0;
    error = "downloads need Windows";
    return false;
}
#endif

std::string json_string_after(const std::string& text, const std::string& key,
                              std::size_t from, std::size_t* end_pos = nullptr)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t at = text.find(needle, from);
    if (at == std::string::npos)
        return {};

    at += needle.size();
    while (at < text.size() &&
           (text[at] == ' ' || text[at] == '\t' ||
            text[at] == '\r' || text[at] == '\n'))
        ++at;

    if (at >= text.size() || text[at] != ':')
        return {};
    ++at;

    while (at < text.size() &&
           (text[at] == ' ' || text[at] == '\t' ||
            text[at] == '\r' || text[at] == '\n'))
        ++at;

    if (at >= text.size() || text[at] != '"')
        return {};
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
        *end_pos = at;

    return out;
}

bool ends_with(const std::string& text, const std::string& tail)
{
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
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

// target <- staged. A running DLL can be renamed but not overwritten.
bool swap_in(const std::filesystem::path& target,
             const std::filesystem::path& staged,
             std::string& how)
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
            fs::rename(old_file, target, undo);

        how = "couldn't move the new file into place: " + message;
        return false;
    }

    ec.clear();
    fs::remove(old_file, ec);
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
	std::string m_last_input_css;
	bool m_has_last_input = false;
	bool m_captured = false;

	std::mutex m_register_mutex;
	bool m_registered = false;
	std::atomic<bool> m_stop{false};
	std::thread m_init_thread;
	std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
	std::atomic<int> m_jobs{0};

	uint64_t m_menu_root = 0;
	uint64_t m_menu_presets = 0;
	std::vector<uint64_t> m_preset_items;
	std::string m_last_presets;

	int m_compose_counter = 0;
	std::string m_last_compose;

	// restyle_widgets state cache
	std::string m_last_restyle_css;
	std::size_t m_last_restyle_count = 0;
	std::chrono::steady_clock::time_point m_last_restyle_time{};

public:
	studio_theme_qt()
	{
		name = "Studio Theme Qt";
		version = kHelperVersion;
		author = "pugsertime";
		description = "Qt stylesheet helper + updater for the Studio Theme mod";
		m_log = rml::Logger::get_logger("StudioThemeQt");
	}

	// Install a staged update at the beginning of the next launch.
	void install_pending_update()
	{
#ifdef _WIN32
		const auto self = self_module_path();
		if (self.empty())
		{
			return;
		}
		auto staged = self;
		staged += L".new";
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

	void on_load() override
	{
		install_pending_update();
		m_log->info("loaded {}", kHelperVersion);
		build_menu();
		start_watchdog();
		if (register_bridge())
		{
			return;
		}
		m_stop.store(false, std::memory_order_relaxed);
		m_init_thread = std::thread([this]() {
			for (int attempt = 0; attempt < 240 && !m_stop.load(std::memory_order_relaxed); ++attempt)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				if (m_stop.load(std::memory_order_relaxed))
				{
					return;
				}
				if (register_bridge())
				{
					return;
				}
			}
			if (!m_stop.load(std::memory_order_relaxed))
			{
				m_log->error("gave up waiting for the script runtime; the theme scripts can't reach this helper");
			}
		});
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

		auto restyled = bridge.register_function("studio_theme_qt", "restyle_widgets", [this](const BridgeArgs& args) -> BridgeArgs {
			std::string css;
			if (!args.empty())
			{
				if (const auto* text = std::get_if<std::string>(&args[0]))
				{
					css = *text;
				}
			}
			schedule_restyle(std::move(css));
			return {true};
		});
		if (!restyled)
		{
			m_log->error("couldn't register restyle_widgets: {}", restyled.error());
		}

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
				out.write(text->data(), static_cast<std::streamsize>(text->size()));
			}
			std::error_code ec;
			fs::rename(temp, file, ec);
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
			std::ifstream in(from_utf8(*path), std::ios::binary | std::ios::ate);
			if (!in)
			{
				return {std::string{}};
			}
			const auto size = in.tellg();
			if (size <= 0)
			{
				return {std::string{}};
			}
			std::string text;
			text.resize(static_cast<std::size_t>(size));
			in.seekg(0, std::ios::beg);
			in.read(text.data(), size);
			return {std::move(text)};
		});
		if (!loaded)
		{
			m_log->error("couldn't register load_theme: {}", loaded.error());
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


		// Writes raw bytes to "<this dll>.new" next to the real one — not
		// in place, since Windows won't let a loaded DLL be overwritten
		// while it's running. install_pending_update() swaps it in at the
		// start of the next on_load(), so this always needs a restart to
		// actually take effect; nothing here claims otherwise.
		auto staged_update = bridge.register_function("studio_theme_qt", "stage_update", [this](const BridgeArgs& args) -> BridgeArgs {
#ifdef _WIN32
			const auto* bytes = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!bytes || bytes->empty())
			{
				return {false, std::string{"no data"}};
			}
			const auto self = self_module_path();
			if (self.empty())
			{
				return {false, std::string{"couldn't find my own path"}};
			}
			auto staged = self;
			staged += L".new";
			{
				std::ofstream out(staged, std::ios::binary | std::ios::trunc);
				if (!out)
				{
					return {false, std::string{"couldn't write the staged file"}};
				}
				out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
			}
			m_log->info("staged an update ({} bytes) — restart Studio to install it", bytes->size());
			return {true};
#else
			return {false, std::string{"only implemented on Windows"}};
#endif
		});
		if (!staged_update)
		{
			m_log->error("couldn't register stage_update: {}", staged_update.error());
		}


		// ===== updater bridge =====
		auto versioned = bridge.register_function("studio_theme_qt", "version",
			[](const BridgeArgs&) -> BridgeArgs {
				return {std::string{kHelperVersion}};
			});
		if (!versioned)
		{
			m_log->error("couldn't register version: {}", versioned.error());
		}

		auto checked = bridge.register_function("studio_theme_qt", "check_update",
			[this](const BridgeArgs& args) -> BridgeArgs {
				auto text = [&](std::size_t i) -> std::string {
					if (i < args.size())
					{
						if (const auto* v = std::get_if<std::string>(&args[i]))
							return *v;
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

		auto downloaded = bridge.register_function("studio_theme_qt", "download_update",
			[this](const BridgeArgs& args) -> BridgeArgs {
				auto text = [&](std::size_t i) -> std::string {
					if (i < args.size())
					{
						if (const auto* v = std::get_if<std::string>(&args[i]))
							return *v;
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

		auto pending = bridge.register_function("studio_theme_qt", "install_pending",
			[this](const BridgeArgs& args) -> BridgeArgs {
				const auto* target =
					args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
				if (!target || target->empty())
					return {std::string{"error"}, std::string{"missing path"}};

				namespace fs = std::filesystem;
				const fs::path dll = from_utf8(*target);
				fs::path staged = dll;
				staged += ".new";

				std::error_code ec;
				if (!fs::exists(staged, ec))
					return {std::string{"none"}, std::string{}};

				std::string how;
				const bool ok = swap_in(dll, staged, how);
				m_log->info("pending update at {}: {}", *target, how);
				return {std::string{ok ? how : "error"},
				        ok ? std::string{} : how};
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
		m_stop.store(true, std::memory_order_release);
		if (m_init_thread.joinable())
		{
			m_init_thread.join();
		}

		if (auto* qt = rml::qt::QtIntegration::instance(); qt && m_menu_root != 0)
		{
			qt->menu().remove(m_menu_root);
			m_menu_root = 0;
		}
		if (auto* runtime = script_runtime(); runtime && m_registered)
		{
			auto& bridge = runtime->bridge();
			for (const char* fn : {"apply", "scan", "pick_image", "install_image", "compose_topbar",
								"restyle_widgets", "save_theme", "load_theme", "set_presets", "ping",
								"version", "check_update", "download_update", "install_pending", "stage_update"})
			{
				auto removed = bridge.unregister_function("studio_theme_qt", fn);
				if (!removed)
				{
					m_log->debug("unregister {}: {}", fn, removed.error());
				}
			}
		}
		m_alive->store(false);
		for (int waited = 0; m_jobs.load() > 0 && waited < 200; ++waited)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
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
	void spawn_job(std::function<void()> fn)
	{
		++m_jobs;
		std::thread([this, fn = std::move(fn)]() {
			fn();
			--m_jobs;
		}).detach();
	}

	void publish(const char* key, const char* event, std::string json)
	{
		auto alive = m_alive;
		auto task = [this, alive, key, event, json = std::move(json)]() {
			if (!alive->load())
				return;

			auto* runtime = script_runtime();
			if (!runtime)
				return;

			auto& bridge = runtime->bridge();
			auto stored = bridge.set_shared(key, json);
			if (!stored)
				m_log->warn("couldn't publish {}: {}", key, stored.error());

			auto emitted = bridge.emit(event, BridgeArgs{std::string{"ok"}});
			if (!emitted)
				m_log->warn("couldn't emit {}: {}", event, emitted.error());
		};

		if (auto* qt = rml::qt::QtIntegration::instance())
			qt->run_on_gui_thread(task);
		else
			task();
	}

	void start_check(std::string repo, std::string asset, std::string job)
	{
		spawn_job([this, repo = std::move(repo), asset = std::move(asset),
		           job = std::move(job)]() {
			std::string tag;
			std::string url;
			std::string error;
			const std::string base_url =
				"https://api.github.com/repos/" + repo + "/releases";
			bool reached = false;

			for (const std::string& feed :
			     {base_url + "/latest", base_url + "?per_page=10"})
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
					const std::string candidate =
						json_string_after(body, "browser_download_url", from, &end);

					if (candidate.empty())
						break;

					from = end;

					if (ends_with(candidate, "/" + asset))
					{
						url = candidate;
						const std::size_t tag_at = body.rfind("\"tag_name\"", end);
						if (tag_at != std::string::npos)
							tag = json_string_after(body, "tag_name", tag_at);
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
				error = "couldn't reach GitHub";

			publish("studio_theme.update_check", "studio_theme.update_check_done",
				std::format(
					"{{\"job\":\"{}\",\"tag\":\"{}\",\"url\":\"{}\",\"error\":\"{}\"}}",
					json_escape(job), json_escape(tag), json_escape(url),
					json_escape(error)));

			m_log->info("update check {}: tag '{}' {}", repo, tag, error);
		});
	}

	void start_download(std::string url, std::string target, std::string job)
	{
		spawn_job([this, url = std::move(url), target = std::move(target),
		           job = std::move(job)]() {
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
					break;

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

					out.write(body.data(),
					          static_cast<std::streamsize>(body.size()));
					out.close();

					if (!out)
					{
						error = "writing " + utf8(staged) +
						        " failed (disk full?)";
						break;
					}
				}

				ok = swap_in(dll, staged, how);
				if (!ok)
					error = how;
			}
			while (false);

			publish("studio_theme.update_download",
			        "studio_theme.update_download_done",
			        std::format(
			            "{{\"job\":\"{}\",\"ok\":{},\"how\":\"{}\",\"bytes\":{},\"error\":\"{}\"}}",
			            json_escape(job), ok ? "true" : "false",
			            json_escape(ok ? how : std::string{}),
			            size, json_escape(error)));

			m_log->info("update download: ok={} {} {} bytes {}",
			            ok, how, size, error);
		});
	}

	// Keep our application stylesheet on top when Studio switches its own theme.
	void start_watchdog()
	{
		std::thread([this, alive = m_alive]() {
			while (alive->load() && !m_stop.load(std::memory_order_relaxed))
			{
				for (int i = 0; i < 20 &&
				     alive->load() &&
				     !m_stop.load(std::memory_order_relaxed); ++i)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}

				if (!alive->load() ||
				    m_stop.load(std::memory_order_relaxed))
					return;

				auto task = [this, alive]() {
					if (!alive->load())
						return;

					auto* app = rml::qt::QApplication::instance();
					if (!app)
						return;

					std::lock_guard lock(m_mutex);
					if (!m_captured || m_last.empty())
						return;

					if (app->style_sheet().find(kSheetMarker) !=
					    std::string::npos)
						return;

					m_log->info(
						"Studio replaced its stylesheet; putting the theme back on top");
					app->set_style_sheet(m_last);
				};

				if (auto* qt = rml::qt::QtIntegration::instance())
					qt->run_on_gui_thread(task);
			}
		}).detach();
	}

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
		menu.add_separator(m_menu_root);
		menu.add_action(m_menu_root, "Plain Studio (remove all theme colors)", [this]() { send("plain"); });
	}

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

			std::string_view sv = names;
			while (!sv.empty())
			{
				const auto pos = sv.find('\n');
				auto line = (pos == std::string_view::npos) ? sv : sv.substr(0, pos);
				sv = (pos == std::string_view::npos) ? std::string_view{} : sv.substr(pos + 1);

				if (!line.empty() && line.back() == '\r')
				{
					line.remove_suffix(1);
				}
				if (line.empty())
				{
					continue;
				}
				std::string name(line);
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

	void schedule_scan()
	{
		auto task = [this]() {
			std::unordered_map<std::string_view, int> counts;
			counts.reserve(128);

			for (auto* widget : rml::qt::QApplication::all_widgets())
			{
				if (!widget)
				{
					continue;
				}
				const char* cls = widget->class_name();
				if (cls && *cls)
				{
					++counts[std::string_view(cls)];
				}
			}

			std::string json;
			json.reserve(counts.size() * 40 + 2);
			json.push_back('[');
			bool first = true;
			for (const auto& [cls, count] : counts)
			{
				if (!first)
				{
					json.push_back(',');
				}
				first = false;
				json.append("{\"class\":\"");
				append_json_escaped(json, cls);
				json.append("\",\"count\":");
				json.append(std::to_string(count));
				json.push_back('}');
			}
			json.push_back(']');

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

	static void append_json_escaped(std::string& out, std::string_view text)
	{
		for (const char ch : text)
		{
			switch (ch)
			{
			case '"':  out.append("\\\""); break;
			case '\\': out.append("\\\\"); break;
			case '\b': out.append("\\b");  break;
			case '\f': out.append("\\f");  break;
			case '\n': out.append("\\n");  break;
			case '\r': out.append("\\r");  break;
			case '\t': out.append("\\t");  break;
			default:   out.push_back(ch);   break;
			}
		}
	}

	static std::string json_escape(std::string_view text)
	{
		std::string out;
		out.reserve(text.size() + 8);
		append_json_escaped(out, text);
		return out;
	}

	static std::string utf8(const std::filesystem::path& path)
	{
		const auto u8 = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
	}

	static std::filesystem::path from_utf8(std::string_view text)
	{
		return std::filesystem::path(
		    std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
	}

	static rml::qt::QColor parse_hex(std::string_view hex) noexcept
	{
		auto hex_digit = [](char c) noexcept -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};

		if (hex.size() >= 7 && hex[0] == '#')
		{
			const int r1 = hex_digit(hex[1]), r2 = hex_digit(hex[2]);
			const int g1 = hex_digit(hex[3]), g2 = hex_digit(hex[4]);
			const int b1 = hex_digit(hex[5]), b2 = hex_digit(hex[6]);
			if ((r1 | r2 | g1 | g2 | b1 | b2) >= 0)
			{
				return rml::qt::QColor((r1 << 4) | r2, (g1 << 4) | g2, (b1 << 4) | b2);
			}
		}
		return rml::qt::QColor(0x20, 0x20, 0x20);
	}

	void schedule_compose(std::string image, std::string hex, double opacity, std::string mode, std::string out_dir, std::string key)
	{
		auto task = [this, image = std::move(image), hex = std::move(hex), opacity, mode = std::move(mode), out_dir = std::move(out_dir), key = std::move(key)]() {
			namespace fs = std::filesystem;
			std::string result;
			std::string error;

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
					error = "couldn't save the composed image to " + utf8(file);
				}
			}

			auto* runtime = script_runtime();
			if (!runtime)
			{
				return;
			}
			auto& bridge = runtime->bridge();

			std::string json;
			json.reserve(key.size() + result.size() + error.size() + 40);
			json.append("{\"key\":\"");
			append_json_escaped(json, key);
			json.append("\",\"path\":\"");
			append_json_escaped(json, result);
			json.append("\",\"error\":\"");
			append_json_escaped(json, error);
			json.append("\"}");

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
			copy = (source_size != target_size || source_time > target_time);
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

			std::string json;
			json.reserve(result_path.size() + error.size() + 40);
			json.append("{\"path\":\"");
			append_json_escaped(json, result_path);
			json.append("\",\"error\":\"");
			append_json_escaped(json, error);
			json.append("\"}");

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

	// ===== per-widget stylesheets =====
	static constexpr std::string_view kBegin = "\n/*studio_theme:begin*/\n";
	static constexpr std::string_view kEnd = "\n/*studio_theme:end*/";

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

	struct StrippedViews
	{
		std::string_view prefix;
		std::string_view suffix;
		bool has_ours = false;

		[[nodiscard]] bool is_blank() const noexcept
		{
			auto is_ws = [](char ch) noexcept {
				return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
			};
			return std::all_of(prefix.begin(), prefix.end(), is_ws) &&
			       std::all_of(suffix.begin(), suffix.end(), is_ws);
		}
	};

	static StrippedViews split_ours(std::string_view sheet) noexcept
	{
		const auto begin = sheet.find(kBegin);
		if (begin == std::string_view::npos)
		{
			return {sheet, {}, false};
		}
		const auto end = sheet.find(kEnd, begin);
		if (end == std::string_view::npos)
		{
			return {sheet.substr(0, begin), {}, true};
		}
		return {sheet.substr(0, begin), sheet.substr(end + kEnd.size()), true};
	}

	void schedule_restyle(std::string css)
	{
		auto task = [this, css = std::move(css)]() {
			const auto all_widgets = rml::qt::QApplication::all_widgets();
			const std::size_t count = all_widgets.size();
			const auto now = std::chrono::steady_clock::now();

			if (css == m_last_restyle_css && count == m_last_restyle_count &&
			    now - m_last_restyle_time < std::chrono::seconds(30))
			{
				return;
			}
			m_last_restyle_css = css;
			m_last_restyle_count = count;
			m_last_restyle_time = now;

			int changed = 0;
			int owned = 0;
			for (auto* widget : all_widgets)
			{
				if (!widget)
				{
					continue;
				}
				const std::string own = widget_style_sheet(widget);
				if (own.empty())
				{
					continue;
				}

				const auto stripped = split_ours(own);
				if (stripped.is_blank())
				{
					if (stripped.has_ours)
					{
						const rml::qt::QString empty("");
						widget->setStyleSheet(empty);
						++changed;
					}
					continue;
				}

				++owned;

				// Skip rebuilding and setStyleSheet if our CSS is already set
				if (stripped.has_ours)
				{
					const auto begin = own.find(kBegin);
					const auto end = own.find(kEnd, begin);
					if (end != std::string::npos)
					{
						const auto current_css = std::string_view(own).substr(begin + kBegin.size(), end - (begin + kBegin.size()));
						if (current_css == css && stripped.suffix.empty())
						{
							continue;
						}
					}
				}
				else if (css.empty())
				{
					continue;
				}

				std::string want;
				want.reserve(stripped.prefix.size() + stripped.suffix.size() +
				             (css.empty() ? 0 : kBegin.size() + css.size() + kEnd.size()));
				want.append(stripped.prefix);
				want.append(stripped.suffix);
				if (!css.empty())
				{
					want.append(kBegin);
					want.append(css);
					want.append(kEnd);
				}

				if (want != own)
				{
					const rml::qt::QString sheet(want);
					widget->setStyleSheet(sheet);
					++changed;
				}
			}
			if (changed > 0)
			{
				m_log->info("restyled {} widget(s) that have their own stylesheet ({} total)", changed, owned);
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
		auto task = [this, css = std::move(css)]() {
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
		auto* app = rml::qt::QApplication::instance();
		if (!app)
		{
			m_log->warn("QApplication not available yet");
			return;
		}

		std::lock_guard lock(m_mutex);
		if (!m_captured)
		{
			m_original = app->style_sheet();
			m_captured = true;
		}

		if (m_captured && m_has_last_input && css == m_last_input_css)
		{
			return;
		}
		m_last_input_css = css;
		m_has_last_input = true;

		static constexpr std::string_view kThemeTag = "\n/* studio_theme */\n";
		m_last.clear();
		if (css.empty())
		{
			m_last = m_original;
		}
		else
		{
			m_last.reserve(m_original.size() + kThemeTag.size() + css.size());
			m_last.append(m_original);
			m_last.append(kThemeTag);
			m_last.append(css);
		}

		app->set_style_sheet(m_last);
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
}

RML_EXPORT_MOD_ABI_VERSION()
