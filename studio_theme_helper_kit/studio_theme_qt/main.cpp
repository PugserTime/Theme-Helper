// Studio Theme Qt helper
//
// Tiny native RML mod that lets the Studio Theme Luau scripts restyle the
// parts of Studio that are native Qt (dock panel tabs + title bars, menu
// bar, menus, status bar, tooltips, native dialogs). Luau can't reach Qt,
// so the scripts build a Qt stylesheet and hand it over the bridge:
//
//     bridge.call("studio_theme_qt", "apply", cssText)   -- "" = restore
//     bridge.call("studio_theme_qt", "install_image", file, contentDir)
//                                       -- copy an image into Studio's content\studio_theme\
//
// Studio's own stylesheet is captured once and always kept underneath ours,
// and it's put back when the mod unloads.

#include <RobloxModLoader/logger/logger.hpp>
#include <RobloxModLoader/luau/luau_bridge.hpp>
#include <RobloxModLoader/luau/script_runtime.hpp>
#include <RobloxModLoader/mod/mod_base.hpp>
#include <RobloxModLoader/qt/qapplication.hpp>
#include <RobloxModLoader/qt/qfiledialog.hpp>
#include <RobloxModLoader/qt/qobject.hpp>
#include <RobloxModLoader/qt/qt_integration.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <variant>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <utility>

using namespace rml::luau;

class studio_theme_qt final : public ModBase
{
	std::shared_ptr<spdlog::logger> m_log;
	std::mutex m_mutex;
	std::string m_original;
	std::string m_last;
	bool m_captured = false;
	std::mutex m_register_mutex;
	bool m_registered = false;
	std::atomic<bool> m_stop{false};
	uint64_t m_menu_root = 0;
	uint64_t m_menu_presets = 0;
	std::vector<uint64_t> m_preset_items;
	std::string m_last_presets;

public:
	studio_theme_qt()
	{
		name = "Studio Theme Qt";
		version = "1.0.0";
		author = "gasolongames";
		description = "Qt stylesheet helper for the Studio Theme mod";
		m_log = rml::Logger::get_logger("StudioThemeQt");
	}

	// NOTE: RML 17589c0 declares on_script_manager_load() but never calls it,
	// so the bridge functions are registered from on_load() instead, with a
	// background retry in case the script runtime isn't up yet.
	void on_load() override
	{
		m_log->info("loaded");
		build_menu();
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

		// install_image(sourcePath, contentDir): copy an image into Studio's own
		// content\\studio_theme\\ folder so it loads as rbxasset://studio_theme/<name>
		// (the one route Studio always supports, even when RML's loader is broken).
		// Answers { "rbxasset://studio_theme/<name>" } or { "", "<error>" }.
		auto installed = bridge.register_function("studio_theme_qt", "install_image", [this](const BridgeArgs& args) -> BridgeArgs {
			namespace fs = std::filesystem;
			std::string source_text;
			std::string content_text;
			if (args.size() > 0)
			{
				if (const auto* text = std::get_if<std::string>(&args[0]))
				{
					source_text = *text;
				}
			}
			if (args.size() > 1)
			{
				if (const auto* text = std::get_if<std::string>(&args[1]))
				{
					content_text = *text;
				}
			}
			if (source_text.empty() || content_text.empty())
			{
				return BridgeArgs{std::string{}, std::string{"install_image needs a file path and Studio's content folder"}};
			}
			std::error_code ec;
			const fs::path source = from_utf8(source_text);
			if (!fs::exists(source, ec) || !fs::is_regular_file(source, ec))
			{
				return BridgeArgs{std::string{}, std::string{"file not found: "} + source_text};
			}
			const fs::path folder = from_utf8(content_text) / "studio_theme";
			ec.clear();
			fs::create_directories(folder, ec);
			if (ec)
			{
				m_log->warn("install_image: couldn't create {}: {}", utf8(folder), ec.message());
				return BridgeArgs{std::string{}, std::string{"couldn't create the studio_theme content folder: "} + ec.message()};
			}
			const fs::path target = folder / source.filename();
			ec.clear();
			const bool same = fs::exists(target, ec) && fs::equivalent(source, target, ec);
			if (!same)
			{
				ec.clear();
				fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
				if (ec)
				{
					m_log->warn("install_image: couldn't copy to {}: {}", utf8(target), ec.message());
					return BridgeArgs{std::string{}, std::string{"couldn't copy the image into Studio's content folder: "} + ec.message()};
				}
			}
			m_log->info("install_image: {} -> {}", source_text, utf8(target));
			return BridgeArgs{std::string{"rbxasset://studio_theme/"} + utf8(source.filename())};
		});
		if (!installed)
		{
			m_log->error("couldn't register install_image: {}", installed.error());
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
		m_registered = true;
		m_log->info("bridge functions registered - the Studio Theme scripts can use this helper now");
		return true;
	}

	void on_unload() override
	{
		m_stop = true;
		if (auto* qt = rml::qt::QtIntegration::instance(); qt && m_menu_root != 0)
		{
			qt->menu().remove(m_menu_root);
			m_menu_root = 0;
		}
		if (auto* runtime = script_runtime(); runtime && m_registered)
		{
			auto& bridge = runtime->bridge();
			for (const char* fn : {"apply", "scan", "pick_image", "install_image", "set_presets", "ping"})
			{
				auto removed = bridge.unregister_function("studio_theme_qt", fn);
				if (!removed)
				{
					m_log->debug("unregister {}: {}", fn, removed.error());
				}
			}
		}
		schedule(std::string{});
		m_log->info("unloaded, Qt stylesheet restored");
	}

private:
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
				std::string escaped;
				for (const char ch : cls)
				{
					if (ch == '"' || ch == '\\')
					{
						escaped.push_back('\\');
					}
					escaped.push_back(ch);
				}
				json += std::format("{}{{\"class\":\"{}\",\"count\":{}}}", first ? "" : ",", escaped, count);
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

	static std::string json_escape(const std::string& text)
	{
		std::string out;
		for (const char ch : text)
		{
			if (ch == '"' || ch == '\\')
			{
				out.push_back('\\');
			}
			out.push_back(ch);
		}
		return out;
	}

	static std::string utf8(const std::filesystem::path& path)
	{
		const auto u8 = path.generic_u8string();
		return std::string(u8.begin(), u8.end());
	}

	static std::filesystem::path from_utf8(const std::string& text)
	{
		return std::filesystem::path(std::u8string(text.begin(), text.end()));
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
		std::string full = css.empty() ? m_original : m_original + "\n/* studio_theme */\n" + css;
		if (full == m_last)
		{
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
}

RML_EXPORT_MOD_ABI_VERSION()
