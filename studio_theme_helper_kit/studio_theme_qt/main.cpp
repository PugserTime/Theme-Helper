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
#include <RobloxModLoader/qt/qobject.hpp>
#include <RobloxModLoader/qt/qt_integration.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
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
	int m_compose_counter = 0;
	std::string m_last_compose;

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
			for (const char* fn : {"apply", "scan", "pick_image", "install_image", "compose_topbar", "set_presets", "ping"})
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
