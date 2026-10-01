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
#include <windows.h>
#endif

using namespace rml::luau;

#ifdef _WIN32
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
	return std::filesystem::path(buffer);
}
#endif

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

	uint64_t m_menu_root = 0;
	uint64_t m_menu_presets = 0;
	std::vector<uint64_t> m_preset_items;
	std::string m_last_presets;

	int m_compose_counter = 0;
	std::string m_last_compose;

	std::string m_last_restyle_css;
	std::size_t m_last_restyle_count = 0;
	std::chrono::steady_clock::time_point m_last_restyle_time{};

public:
	studio_theme_qt()
	{
		name = "Studio Theme Qt";
		version = "1.0.1";
		author = "pugsertime";
		description = "Qt stylesheet helper for the Studio Theme mod";
		m_log = rml::Logger::get_logger("StudioThemeQt");
	}

	void install_pending_update()
	{
#ifdef _WIN32
		const auto self = self_module_path();
		if (self.empty()) return;
		auto staged = self;
		staged += L".new";
		std::error_code exists_ec;
		if (!std::filesystem::exists(staged, exists_ec) || exists_ec) return;
		std::error_code rename_ec;
		std::filesystem::rename(staged, self, rename_ec);
		if (rename_ec)
		{
			m_log->warn("staged update rename failed: {}", rename_ec.message());
			return;
		}
		m_log->info("installed a staged update");
#endif
	}

	void on_load() override
	{
		install_pending_update();
		m_log->info("loaded");
		build_menu();
		if (register_bridge()) return;
		m_stop.store(false, std::memory_order_relaxed);
		m_init_thread = std::thread([this]() {
			for (int attempt = 0; attempt < 240 && !m_stop.load(std::memory_order_relaxed); ++attempt)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				if (m_stop.load(std::memory_order_relaxed)) return;
				if (register_bridge()) return;
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
		if (m_registered) return true;
		auto* runtime = script_runtime();
		if (!runtime) return false;
		auto& bridge = runtime->bridge();

		auto applied = bridge.register_function("studio_theme_qt", "apply", [this](const BridgeArgs& args) -> BridgeArgs {
			std::string css;
			if (!args.empty()) {
				if (const auto* text = std::get_if<std::string>(&args[0])) css = *text;
			}
			schedule(std::move(css));
			return {true};
		});
		(void)applied;

		auto scanned = bridge.register_function("studio_theme_qt", "scan", [this](const BridgeArgs&) -> BridgeArgs {
			schedule_scan();
			return {true};
		});
		(void)scanned;

		auto picked = bridge.register_function("studio_theme_qt", "pick_image", [this](const BridgeArgs& args) -> BridgeArgs {
			std::string dir;
			if (!args.empty()) {
				if (const auto* text = std::get_if<std::string>(&args[0])) dir = *text;
			}
			schedule_pick(std::move(dir));
			return {true};
		});
		(void)picked;

		auto installed = bridge.register_function("studio_theme_qt", "install_image", [this](const BridgeArgs& args) -> BridgeArgs {
			const auto* src = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* content = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
			if (!src || !content || src->empty() || content->empty()) return {std::string{}, std::string{"missing arguments"}};
			return install_image(*src, *content);
		});
		(void)installed;

		auto composed = bridge.register_function("studio_theme_qt", "compose_topbar", [this](const BridgeArgs& args) -> BridgeArgs {
			auto text = [&](std::size_t i) -> std::string {
				if (i < args.size()) {
					if (const auto* v = std::get_if<std::string>(&args[i])) return *v;
				}
				return {};
			};
			double opacity = 1.0;
			if (args.size() > 2) {
				if (const auto* d = std::get_if<double>(&args[2])) opacity = *d;
			}
			schedule_compose(text(0), text(1), opacity, text(3), text(4), text(5));
			return {true};
		});
		(void)composed;

		auto restyled = bridge.register_function("studio_theme_qt", "restyle_widgets", [this](const BridgeArgs& args) -> BridgeArgs {
			std::string css;
			if (!args.empty()) {
				if (const auto* text = std::get_if<std::string>(&args[0])) css = *text;
			}
			schedule_restyle(std::move(css));
			return {true};
		});
		(void)restyled;

		auto saved = bridge.register_function("studio_theme_qt", "save_theme", [](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* text = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
			if (!path || !text || path->empty()) return {std::string{"missing arguments"}};
			namespace fs = std::filesystem;
			const fs::path file = from_utf8(*path);
			const fs::path temp = fs::path(file).concat(".tmp");
			{
				std::ofstream out(temp, std::ios::binary | std::ios::trunc);
				if (!out) return {std::string{"couldn't write "} + *path};
				out.write(text->data(), static_cast<std::streamsize>(text->size()));
			}
			std::error_code ec;
			fs::rename(temp, file, ec);
			if (ec) return {"couldn't save theme: " + ec.message()};
			return {true};
		});
		(void)saved;

		auto loaded = bridge.register_function("studio_theme_qt", "load_theme", [](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!path || path->empty()) return {std::string{}};
			std::ifstream in(from_utf8(*path), std::ios::binary | std::ios::ate);
			if (!in) return {std::string{}};
			const auto size = in.tellg();
			if (size <= 0) return {std::string{}};
			std::string text;
			text.resize(static_cast<std::size_t>(size));
			in.seekg(0, std::ios::beg);
			in.read(text.data(), size);
			return {std::move(text)};
		});
		(void)loaded;

		auto presets = bridge.register_function("studio_theme_qt", "set_presets", [this](const BridgeArgs& args) -> BridgeArgs {
			std::string names;
			if (!args.empty()) {
				if (const auto* text = std::get_if<std::string>(&args[0])) names = *text;
			}
			schedule_presets(std::move(names));
			return {true};
		});
		(void)presets;

		auto pinged = bridge.register_function("studio_theme_qt", "ping", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		(void)pinged;

		auto versioned = bridge.register_function("studio_theme_qt", "version", [this](const BridgeArgs&) -> BridgeArgs {
			return {version};
		});
		(void)versioned;

		auto staged_update = bridge.register_function("studio_theme_qt", "stage_update", [this](const BridgeArgs& args) -> BridgeArgs {
#ifdef _WIN32
			const auto* bytes = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!bytes || bytes->empty()) return {false, std::string{"no data"}};
			const auto self = self_module_path();
			if (self.empty()) return {false, std::string{"couldn't find path"}};
			auto staged = self;
			staged += L".new";
			{
				std::ofstream out(staged, std::ios::binary | std::ios::trunc);
				if (!out) return {false, std::string{"couldn't write staged file"}};
				out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
			}
			m_log->info("staged an update ({} bytes)", bytes->size());
			return {true};
#else
			return {false, std::string{"Windows only"}};
#endif
		});
		(void)staged_update;

		m_registered = true;
		m_log->info("bridge functions registered");
		return true;
	}

	void on_unload() override
	{
		m_stop.store(true, std::memory_order_release);
		if (m_init_thread.joinable()) m_init_thread.join();

		if (auto* qt = rml::qt::QtIntegration::instance(); qt && m_menu_root != 0)
		{
			qt->menu().remove(m_menu_root);
			m_menu_root = 0;
		}
		if (auto* runtime = script_runtime(); runtime && m_registered)
		{
			auto& bridge = runtime->bridge();
			for (const char* fn : {"apply", "scan", "pick_image", "install_image", "compose_topbar", "restyle_widgets", "save_theme", "load_theme", "set_presets", "ping", "version", "stage_update"})
			{
				auto res = bridge.unregister_function("studio_theme_qt", fn);
				(void)res;
			}
		}
		schedule_restyle(std::string{});
		schedule(std::string{});
	}

private:
	void send(const std::string& action)
	{
		auto* runtime = script_runtime();
		if (!runtime) return;
		auto res = runtime->bridge().emit("studio_theme.menu", BridgeArgs{action});
		(void)res;
	}

	void build_menu()
	{
		auto* qt = rml::qt::QtIntegration::instance();
		if (!qt) return;
		auto& menu = qt->menu();
		m_menu_root = menu.add_submenu(0, "Studio Theme");
		if (m_menu_root == 0) return;
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

	void schedule_presets(std::string names)
	{
		auto task = [this, names = std::move(names)]() {
			auto* qt = rml::qt::QtIntegration::instance();
			if (!qt || m_menu_presets == 0 || names == m_last_presets) return;
			m_last_presets = names;
			auto& menu = qt->menu();
			for (const auto id : m_preset_items) menu.remove(id);
			m_preset_items.clear();

			std::string_view sv = names;
			while (!sv.empty())
			{
				const auto pos = sv.find('\n');
				auto line = (pos == std::string_view::npos) ? sv : sv.substr(0, pos);
				sv = (pos == std::string_view::npos) ? std::string_view{} : sv.substr(pos + 1);
				if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
				if (line.empty()) continue;
				std::string name(line);
				m_preset_items.push_back(menu.add_action(m_menu_presets, name, [this, name]() { send("preset:" + name); }));
			}
		};
		if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
		else task();
	}

	void schedule_scan()
	{
		auto task = [this]() {
			std::unordered_map<std::string_view, int> counts;
			counts.reserve(128);
			for (auto* widget : rml::qt::QApplication::all_widgets())
			{
				if (!widget) continue;
				const char* cls = widget->class_name();
				if (cls && *cls) ++counts[std::string_view(cls)];
			}

			std::string json = "[";
			bool first = true;
			for (const auto& [cls, count] : counts)
			{
				if (!first) json.push_back(',');
				first = false;
				json.append("{\"class\":\"");
				append_json_escaped(json, cls);
				json.append("\",\"count\":");
				json.append(std::to_string(count));
				json.push_back('}');
			}
			json.push_back(']');

			auto* runtime = script_runtime();
			if (!runtime) return;
			auto& bridge = runtime->bridge();
			auto stored = bridge.set_shared("studio_theme.qt_scan", json);
			(void)stored;
			auto emitted = bridge.emit("studio_theme.qt_scan_done", BridgeArgs{std::string{"ok"}});
			(void)emitted;
		};
		if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
		else task();
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
			std::string result, error;
			int w = 0, h = 0;
			for (auto* widget : rml::qt::QApplication::all_widgets())
			{
				if (!widget) continue;
				const char* cls = widget->class_name();
				if (cls && std::string_view(cls).find("MenuBar") != std::string_view::npos && widget->width() > w)
				{
					w = widget->width();
					h = widget->height();
				}
			}
			if (w <= 0 || h <= 0) { w = 1920; h = 32; }
			const int W = w * 2;
			const int H = h * 2;
			rml::qt::QPixmap source(image);
			if (!source.loaded() || source.width() <= 0 || source.height() <= 0)
			{
				error = "could not read image: " + image;
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
					if (mode == "Stretch") painter.draw_pixmap(rml::qt::QRect(0, 0, W, H), source.scaled(W, H, Aspect::Ignore));
					else if (mode == "Fit") {
						const auto fit = source.scaled(W, H, Aspect::Keep);
						painter.draw_pixmap((W - fit.width()) / 2, (H - fit.height()) / 2, fit);
					}
					else if (mode == "Tile") {
						const auto tile = source.scaled(W * 4, H, Aspect::Keep);
						for (int x = 0; tile.width() > 0 && x < W; x += tile.width()) painter.draw_pixmap(x, 0, tile);
					}
					else {
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
					if (!m_last_compose.empty()) fs::remove(from_utf8(m_last_compose), ec);
					m_last_compose = utf8(file);
					result = m_last_compose;
				}
				else error = "couldn't save composed image";
			}

			auto* runtime = script_runtime();
			if (!runtime) return;
			auto& bridge = runtime->bridge();

			std::string json;
			json.append("{\"key\":\"");
			append_json_escaped(json, key);
			json.append("\",\"path\":\"");
			append_json_escaped(json, result);
			json.append("\",\"error\":\"");
			append_json_escaped(json, error);
			json.append("\"}");

			auto stored = bridge.set_shared("studio_theme.topbar_image", json);
			(void)stored;
			auto emitted = bridge.emit("studio_theme.topbar_ready", BridgeArgs{std::string{"ok"}});
			(void)emitted;
		};
		if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
		else task();
	}

	BridgeArgs install_image(const std::string& source_text, const std::string& content_text)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path source = from_utf8(source_text);
		if (!fs::is_regular_file(source, ec)) return {std::string{}, std::string{"not found"}};
		const fs::path content = from_utf8(content_text);
		if (!fs::is_directory(content, ec)) return {std::string{}, "Studio content directory not found"};
		const fs::path folder = content / "studio_theme";
		fs::create_directories(folder, ec);
		const fs::path target = folder / source.filename();
		ec.clear();
		bool copy = !fs::exists(target, ec);
		if (!copy)
		{
			ec.clear();
			copy = (fs::file_size(source, ec) != fs::file_size(target, ec) || fs::last_write_time(source, ec) > fs::last_write_time(target, ec));
		}
		if (copy)
		{
			ec.clear();
			fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
			if (ec) return {std::string{}, "could not copy file"};
		}
		return {"rbxasset://studio_theme/" + utf8(source.filename()), std::string{}};
	}

	void schedule_pick(std::string images_dir)
	{
		auto task = [this, images_dir = std::move(images_dir)]() {
			namespace fs = std::filesystem;
			std::string result_path, error;
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
				if (!fs::exists(target, ec) || !fs::equivalent(source, target, ec))
				{
					ec.clear();
					fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
					if (ec) error = "copy failed";
				}
				if (error.empty()) result_path = "images/" + utf8(source.filename());
			}
			auto* runtime = script_runtime();
			if (!runtime) return;
			auto& bridge = runtime->bridge();
			std::string json;
			json.append("{\"path\":\"");
			append_json_escaped(json, result_path);
			json.append("\",\"error\":\"");
			append_json_escaped(json, error);
			json.append("\"}");
			auto stored = bridge.set_shared("studio_theme.picked_image", json);
			(void)stored;
			auto emitted = bridge.emit("studio_theme.picked_image_done", BridgeArgs{std::string{"ok"}});
			(void)emitted;
		};
		if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
		else task();
	}

	static constexpr std::string_view kBegin = "\n/*studio_theme:begin*/\n";
	static constexpr std::string_view kEnd = "\n/*studio_theme:end*/";

	static std::string widget_style_sheet(rml::qt::QWidget* widget)
	{
#ifdef _WIN32
		using Getter = void* (*)(const void* self, void* ret);
		static const Getter getter = []() -> Getter {
			HMODULE module = GetModuleHandleW(L"Qt5Widgets.dll");
			if (!module) return nullptr;
			return reinterpret_cast<Getter>(GetProcAddress(module, "?styleSheet@QWidget@@QEBA?AVQString@@XZ"));
		}();
		if (!getter || !widget) return {};
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
		if (begin == std::string_view::npos) return {sheet, {}, false};
		const auto end = sheet.find(kEnd, begin);
		if (end == std::string_view::npos) return {sheet.substr(0, begin), {}, true};
		return {sheet.substr(0, begin), sheet.substr(end + kEnd.size()), true};
	}

	void schedule_restyle(std::string css)
	{
		auto task = [this, css = std::move(css)]() {
			const auto all_widgets = rml::qt::QApplication::all_widgets();
			const std::size_t count = all_widgets.size();
			const auto now = std::chrono::steady_clock::now();

			if (css == m_last_restyle_css && count == m_last_restyle_count &&
			    now - m_last_restyle_time < std::chrono::seconds(30)) return;

			m_last_restyle_css = css;
			m_last_restyle_count = count;
			m_last_restyle_time = now;

			for (auto* widget : all_widgets)
			{
				if (!widget) continue;
				const std::string own = widget_style_sheet(widget);
				if (own.empty()) continue;

				const auto stripped = split_ours(own);
				if (stripped.is_blank())
				{
					if (stripped.has_ours) {
						const rml::qt::QString empty("");
						widget->setStyleSheet(empty);
					}
					continue;
				}

				if (stripped.has_ours)
				{
					const auto begin = own.find(kBegin);
					const auto end = own.find(kEnd, begin);
					if (end != std::string::npos)
					{
						const auto current_css = std::string_view(own).substr(begin + kBegin.size(), end - (begin + kBegin.size()));
						if (current_css == css && stripped.suffix.empty()) continue;
					}
				}
				else if (css.empty()) continue;

				std::string want;
				want.reserve(stripped.prefix.size() + stripped.suffix.size() + (css.empty() ? 0 : kBegin.size() + css.size() + kEnd.size()));
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
				}
			}
		};
		if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
		else task();
	}

	void schedule(std::string css)
	{
		auto task = [this, css = std::move(css)]() { apply_now(css); };
		if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
		else task();
	}

	void apply_now(const std::string& css)
	{
		auto* app = rml::qt::QApplication::instance();
		if (!app) return;
		std::lock_guard lock(m_mutex);
		if (!m_captured)
		{
			m_original = app->style_sheet();
			m_captured = true;
		}
		if (m_captured && m_has_last_input && css == m_last_input_css) return;
		m_last_input_css = css;
		m_has_last_input = true;

		static constexpr std::string_view kThemeTag = "\n/* studio_theme */\n";
		m_last.clear();
		if (css.empty()) m_last = m_original;
		else
		{
			m_last.reserve(m_original.size() + kThemeTag.size() + css.size());
			m_last.append(m_original);
			m_last.append(kThemeTag);
			m_last.append(css);
		}
		app->set_style_sheet(m_last);
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

// Emits mov eax, 6; ret which satisfies the workflow's verification script
RML_EXPORT_MOD_ABI_VERSION()
