// Studio Theme Qt helper. C++20 / RML Qt5 wrappers.
// Full replacement for main.cpp; bridge event names and argument shapes are unchanged.
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
#include <RobloxModLoader/qt/qt_integration.hpp>
#include <RobloxModLoader/qt/qwidget.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace rml::luau;
class studio_theme_qt final : public ModBase
{
    std::shared_ptr<spdlog::logger> m_log;
    std::mutex m_register_mutex;
    bool m_registered = false;
    std::atomic<bool> m_stop{false};
    std::thread m_retry;
    std::mutex m_pending_mutex;
    std::string m_pending_css;
    bool m_apply_queued = false;
    std::string m_original;
    std::string m_last;
    bool m_captured = false;
    // An RML GUI callback may still be pending at unload. Never schedule a new
    // callback while unloading. See unload note below about host queue lifetime.
    uint64_t m_menu_root = 0;
    uint64_t m_menu_presets = 0;
    std::vector<uint64_t> m_preset_items;
    std::string m_last_presets;
    int m_compose_counter = 0;
    std::string m_last_compose;
    std::string m_last_widget_css;
public:
    studio_theme_qt()
    {
        name = "Studio Theme Qt";
        version = "1.0.1";
        author = "gasolongames";
        description = "Qt stylesheet helper for the Studio Theme mod";
        m_log = rml::Logger::get_logger("StudioThemeQt");
    }
    ~studio_theme_qt() override
    {
        m_stop.store(true);
        if (m_retry.joinable()) m_retry.join();
    }
    void on_load() override
    {
        m_stop.store(false);
        m_log->info("loaded");
        build_menu();
        if (register_bridge()) return;
        m_retry = std::thread([this] {
            for (int attempt = 0; attempt < 240 && !m_stop.load(); ++attempt)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (m_stop.load()) break;
                if (register_bridge()) return;
            }
            if (!m_stop.load()) m_log->error("gave up waiting for the script runtime");
        });
    }
    void on_script_manager_load() override { register_bridge(); }
    bool register_bridge()
    {
        std::lock_guard guard(m_register_mutex);
        if (m_registered) return true;
        if (m_stop.load()) return false;
        auto* runtime = script_runtime();
        if (!runtime) return false;
        auto& bridge = runtime->bridge();
        bool success = true;
        auto failed = [&](const char* name, const auto& result) {
            if (!result) { m_log->error("couldn't register {}: {}", name, result.error()); success = false; }
        };
        auto applied = bridge.register_function("studio_theme_qt", "apply", [this](const BridgeArgs& args) -> BridgeArgs {
            std::string css;
            if (!args.empty()) if (auto* v = std::get_if<std::string>(&args[0])) css = *v;
            schedule(std::move(css));
            return {true};
        });
        failed("apply", applied);
        auto scanned = bridge.register_function("studio_theme_qt", "scan", [this](const BridgeArgs&) -> BridgeArgs {
            schedule_scan(); return {true};
        });
        failed("scan", scanned);
        auto picked = bridge.register_function("studio_theme_qt", "pick_image", [this](const BridgeArgs& args) -> BridgeArgs {
            std::string dir;
            if (!args.empty()) if (auto* v = std::get_if<std::string>(&args[0])) dir = *v;
            schedule_pick(std::move(dir)); return {true};
        });
        failed("pick_image", picked);
        auto installed = bridge.register_function("studio_theme_qt", "install_image", [this](const BridgeArgs& args) -> BridgeArgs {
            auto* src = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
            auto* content = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
            if (!src || !content || src->empty() || content->empty()) return {std::string{}, std::string{"missing arguments"}};
            return install_image(*src, *content);
        });
        failed("install_image", installed);
        auto composed = bridge.register_function("studio_theme_qt", "compose_topbar", [this](const BridgeArgs& args) -> BridgeArgs {
            auto text = [&](std::size_t i) -> std::string {
                if (i < args.size()) if (auto* v = std::get_if<std::string>(&args[i])) return *v;
                return {};
            };
            double opacity = 1.0;
            if (args.size() > 2) if (auto* v = std::get_if<double>(&args[2])) opacity = *v;
            schedule_compose(text(0), text(1), opacity, text(3), text(4), text(5));
            return {true};
        });
        failed("compose_topbar", composed);
        auto restyled = bridge.register_function("studio_theme_qt", "restyle_widgets", [this](const BridgeArgs& args) -> BridgeArgs {
            std::string css;
            if (!args.empty()) if (auto* v = std::get_if<std::string>(&args[0])) css = *v;
            schedule_restyle(std::move(css)); return {true};
        });
        failed("restyle_widgets", restyled);
        auto saved = bridge.register_function("studio_theme_qt", "save_theme", [](const BridgeArgs& args) -> BridgeArgs {
            auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
            auto* text = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
            if (!path || !text || path->empty()) return {std::string{"missing arguments"}};
            namespace fs = std::filesystem;
            const fs::path file = from_utf8(*path);
            const fs::path temp = fs::path(file).concat(".tmp");
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out) return {std::string{"couldn't write "} + *path};
                out << *text;
                if (!out) return {std::string{"couldn't write "} + *path};
            }
            std::error_code ec;
            fs::rename(temp, file, ec);
#ifdef _WIN32
            // std::filesystem::rename is not an atomic replace when a file
            // already exists on some Windows runtimes. Preserve old file on failure.
            if (ec)
            {
                ec.clear();
                if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return {std::string{"couldn't replace theme"}};
            }
#endif
            if (ec) return {"couldn't save theme: " + ec.message()};
            return {true};
        });
        failed("save_theme", saved);
        auto loaded = bridge.register_function("studio_theme_qt", "load_theme", [](const BridgeArgs& args) -> BridgeArgs {
            auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
            if (!path || path->empty()) return {std::string{}};
            std::ifstream in(from_utf8(*path), std::ios::binary);
            if (!in) return {std::string{}};
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return {text};
        });
        failed("load_theme", loaded);
        auto presets = bridge.register_function("studio_theme_qt", "set_presets", [this](const BridgeArgs& args) -> BridgeArgs {
            std::string names;
            if (!args.empty()) if (auto* v = std::get_if<std::string>(&args[0])) names = *v;
            schedule_presets(std::move(names)); return {true};
        });
        failed("set_presets", presets);
        auto pinged = bridge.register_function("studio_theme_qt", "ping", [](const BridgeArgs&) -> BridgeArgs { return {true}; });
        failed("ping", pinged);
        // If a registration failed, return false for retry; the host may reject
        // duplicate names on retry. Log it rather than claiming success.
        if (!success) return false;
        m_registered = true;
        m_log->info("bridge functions registered");
        return true;
    }
    void on_unload() override
    {
        m_stop.store(true);
        if (m_retry.joinable()) m_retry.join();
        if (auto* runtime = script_runtime(); runtime && m_registered)
        {
            auto& bridge = runtime->bridge();
            for (const char* fn : {"apply", "scan", "pick_image", "install_image", "compose_topbar", "restyle_widgets", "save_theme", "load_theme", "set_presets", "ping"})
            {
                auto removed = bridge.unregister_function("studio_theme_qt", fn);
                if (!removed) m_log->debug("unregister {}: {}", fn, removed.error());
            }
            m_registered = false;
        }
        // Do not queue a lambda capturing this during unload. QtIntegration's
        // GUI queue has no documented drain/cancel API in the supplied headers.
        // In-place restore only when the unload callback is on the GUI thread.
        // Otherwise defer unload in the host until the GUI queue is drained.
        if (auto* qt = rml::qt::QtIntegration::instance(); qt)
        {
            if (m_menu_root != 0) qt->menu().remove(m_menu_root);
            m_menu_root = 0;
        }
        restyle_now("");
        apply_now("");
        m_log->info("unloaded, Qt stylesheet restored");
    }
private:
    void send(const std::string& action)
    {
        if (m_stop.load()) return;
        auto* runtime = script_runtime();
        if (!runtime) return;
        auto emitted = runtime->bridge().emit("studio_theme.menu", BridgeArgs{action});
        if (!emitted) m_log->warn("menu: couldn't send '{}': {}", action, emitted.error());
    }
    void build_menu()
    {
        auto* qt = rml::qt::QtIntegration::instance();
        if (!qt) { m_log->warn("Qt integration unavailable; no Mods menu"); return; }
        auto& menu = qt->menu();
        m_menu_root = menu.add_submenu(0, "Studio Theme");
        if (!m_menu_root) return;
        menu.add_action(m_menu_root, "Open / close Theme Editor", [this] { send("open"); });
        menu.add_action(m_menu_root, "Turn theme on / off", [this] { send("toggle"); });
        menu.add_separator(m_menu_root);
        m_menu_presets = menu.add_submenu(m_menu_root, "Presets");
        m_preset_items.push_back(menu.add_action(m_menu_presets, "(loading...)", [] {}));
        menu.add_separator(m_menu_root);
        menu.add_action(m_menu_root, "Auto-match leftover colors on / off", [this] { send("automatch"); });
        menu.add_action(m_menu_root, "Reload background images", [this] { send("reload_images"); });
        menu.add_action(m_menu_root, "Re-apply theme everywhere", [this] { send("refresh"); });
        menu.add_separator(m_menu_root);
        menu.add_action(m_menu_root, "Plain Studio (remove all theme colors)", [this] { send("plain"); });
    }
    void schedule_presets(std::string names)
    {
        auto task = [this, names = std::move(names)] {
            if (m_stop.load()) return;
            auto* qt = rml::qt::QtIntegration::instance();
            if (!qt || !m_menu_presets || names == m_last_presets) return;
            m_last_presets = names;
            auto& menu = qt->menu();
            for (auto id : m_preset_items) menu.remove(id);
            m_preset_items.clear();
            std::istringstream lines(names);
            std::string name;
            while (std::getline(lines, name))
            {
                if (!name.empty() && name.back() == '\r') name.pop_back();
                if (!name.empty()) m_preset_items.push_back(menu.add_action(m_menu_presets, name, [this, name] { send("preset:" + name); }));
            }
            m_log->info("Mods menu: {} presets", m_preset_items.size());
        };
        if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
    }
    void schedule_scan()
    {
        auto task = [this] {
            if (m_stop.load()) return;
            std::map<std::string, int> counts;
            for (auto* widget : rml::qt::QApplication::all_widgets())
            {
                if (!widget) continue;
                const char* cls = widget->class_name();
                if (cls && *cls) ++counts[cls];
            }
            std::string json = "[";
            bool first = true;
            for (const auto& [cls, count] : counts)
            {
                json += std::format("{}{{\"class\":\"{}\",\"count\":{}}}", first ? "" : ",", json_escape(cls), count);
                first = false;
            }
            json += "]";
            if (auto* runtime = script_runtime())
            {
                auto& bridge = runtime->bridge();
                auto stored = bridge.set_shared("studio_theme.qt_scan", json);
                if (!stored) m_log->warn("couldn't publish scan: {}", stored.error());
                auto emitted = bridge.emit("studio_theme.qt_scan_done", BridgeArgs{std::string{"ok"}});
                if (!emitted) m_log->warn("couldn't emit scan_done: {}", emitted.error());
            }
            m_log->info("scanned {} Qt widget classes", counts.size());
        };
        if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
    }
    static std::string json_escape(const std::string& text)
    {
        std::string out;
        for (unsigned char ch : text)
        {
            if (ch == '"' || ch == '\\') { out.push_back('\\'); out.push_back(char(ch)); }
            else if (ch == '\n') out += "\\n";
            else if (ch == '\r') out += "\\r";
            else if (ch == '\t') out += "\\t";
            else if (ch < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned int>(ch));
            else out.push_back(char(ch));
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
            try { value = static_cast<unsigned int>(std::stoul(hex.substr(1, 6), nullptr, 16)); }
            catch (...) {}
        }
        return rml::qt::QColor(int((value >> 16) & 255), int((value >> 8) & 255), int(value & 255));
    }
    void schedule_compose(std::string image, std::string hex, double opacity, std::string mode, std::string out_dir, std::string key)
    {
        auto task = [this, image = std::move(image), hex = std::move(hex), opacity, mode = std::move(mode), out_dir = std::move(out_dir), key = std::move(key)] {
            if (m_stop.load()) return;
            namespace fs = std::filesystem;
            std::string result, error;
            int w = 0, h = 0;
            for (auto* widget : rml::qt::QApplication::all_widgets())
            {
                if (!widget) continue;
                const char* cls = widget->class_name();
                if (cls && std::string_view(cls).find("MenuBar") != std::string_view::npos && widget->width() > w)
                { w = widget->width(); h = widget->height(); }
            }
            if (w <= 0 || h <= 0) { w = 1920; h = 32; }
            const int W = w * 2, H = h * 2;
            rml::qt::QPixmap source(image);
            if (!source.loaded() || source.width() <= 0 || source.height() <= 0)
                error = "Qt couldn't read the image: " + image;
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
                    else if (mode == "Fit")
                    {
                        const auto fit = source.scaled(W, H, Aspect::Keep);
                        painter.draw_pixmap((W - fit.width()) / 2, (H - fit.height()) / 2, fit);
                    }
                    else if (mode == "Tile")
                    {
                        const auto tile = source.scaled(W * 4, H, Aspect::Keep);
                        for (int x = 0; tile.width() > 0 && x < W; x += tile.width()) painter.draw_pixmap(x, 0, tile);
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
                    if (!m_last_compose.empty()) fs::remove(from_utf8(m_last_compose), ec);
                    m_last_compose = utf8(file);
                    result = m_last_compose;
                }
                else error = "couldn't save the composed image to " + utf8(file);
            }
            if (auto* runtime = script_runtime())
            {
                auto& bridge = runtime->bridge();
                auto json = std::format("{{\"key\":\"{}\",\"path\":\"{}\",\"error\":\"{}\"}}", json_escape(key), json_escape(result), json_escape(error));
                auto stored = bridge.set_shared("studio_theme.topbar_image", json);
                if (!stored) m_log->warn("couldn't publish top bar image: {}", stored.error());
                auto emitted = bridge.emit("studio_theme.topbar_ready", BridgeArgs{std::string{"ok"}});
                if (!emitted) m_log->warn("couldn't emit topbar_ready: {}", emitted.error());
            }
            m_log->info("top bar image {}x{} -> '{}' {}", W, H, result, error);
        };
        if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
    }
    BridgeArgs install_image(const std::string& source_text, const std::string& content_text)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path source = from_utf8(source_text);
        if (!fs::is_regular_file(source, ec)) return {std::string{}, std::string{"not found"}};
        const fs::path content = from_utf8(content_text);
        if (!fs::is_directory(content, ec)) return {std::string{}, "Studio content folder not found: " + content_text};
        const fs::path folder = content / "studio_theme";
        fs::create_directories(folder, ec);
        if (ec) return {std::string{}, "couldn't create image folder: " + ec.message()};
        const fs::path target = folder / source.filename();
        ec.clear();
        bool copy = !fs::exists(target, ec);
        if (!copy)
        {
            ec.clear(); const auto source_size = fs::file_size(source, ec);
            if (ec) return {std::string{}, ec.message()};
            const auto target_size = fs::file_size(target, ec);
            if (ec) return {std::string{}, ec.message()};
            const auto source_time = fs::last_write_time(source, ec);
            if (ec) return {std::string{}, ec.message()};
            const auto target_time = fs::last_write_time(target, ec);
            if (ec) return {std::string{}, ec.message()};
            copy = source_size != target_size || source_time > target_time;
        }
        if (copy)
        {
            ec.clear(); fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
            if (ec) return {std::string{}, "couldn't copy into Studio's content folder: " + ec.message()};
            m_log->info("installed image {} -> {}", source_text, utf8(target));
        }
        return {"rbxasset://studio_theme/" + utf8(source.filename()), std::string{}};
    }
    void schedule_pick(std::string images_dir)
    {
        auto task = [this, images_dir = std::move(images_dir)] {
            if (m_stop.load()) return;
            namespace fs = std::filesystem;
            std::string result_path, error;
            const std::string picked = rml::qt::QFileDialog::get_open_file_name(nullptr, "Choose a background image", images_dir, "Images (*.png *.jpg *.jpeg *.bmp *.tga)");
            if (!picked.empty())
            {
                std::error_code ec;
                const fs::path source = from_utf8(picked), folder = from_utf8(images_dir);
                fs::create_directories(folder, ec);
                const fs::path target = folder / source.filename();
                ec.clear();
                const bool same = fs::exists(target, ec) && fs::equivalent(source, target, ec);
                if (!same)
                {
                    ec.clear(); fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
                    if (ec) error = "couldn't copy the image into the images folder: " + ec.message();
                }
                if (error.empty()) result_path = "images/" + utf8(source.filename());
            }
            if (auto* runtime = script_runtime())
            {
                auto& bridge = runtime->bridge();
                const auto json = std::format("{{\"path\":\"{}\",\"error\":\"{}\"}}", json_escape(result_path), json_escape(error));
                auto stored = bridge.set_shared("studio_theme.picked_image", json);
                if (!stored) m_log->warn("couldn't publish picked image: {}", stored.error());
                auto emitted = bridge.emit("studio_theme.picked_image_done", BridgeArgs{std::string{"ok"}});
                if (!emitted) m_log->warn("couldn't emit picked_image_done: {}", emitted.error());
            }
            m_log->info("image picker: '{}' {}", result_path, error);
        };
        if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
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
        (void)widget; return {};
#endif
    }
    static std::string strip_ours(const std::string& sheet)
    {
        auto begin = sheet.find(kBegin);
        if (begin == std::string::npos) return sheet;
        auto end = sheet.find(kEnd, begin);
        if (end == std::string::npos) return sheet.substr(0, begin);
        return sheet.substr(0, begin) + sheet.substr(end + kEnd.size());
    }
    static bool blank(const std::string& text)
    {
        return std::all_of(text.begin(), text.end(), [](char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; });
    }
    void schedule_restyle(std::string css)
    {
        auto task = [this, css = std::move(css)] {
            if (!m_stop.load()) restyle_now(css);
        };
        if (auto* qt = rml::qt::QtIntegration::instance()) qt->run_on_gui_thread(task);
    }
    void restyle_now(const std::string& css)
    {
        int changed = 0, owned = 0;
        // Even with identical CSS, re-scan so widgets created after the previous
        // call still receive it. The Luau caller should debounce this bridge call.
        for (auto* widget : rml::qt::QApplication::all_widgets())
        {
            if (!widget) continue;
            const std::string own = widget_style_sheet(widget);
            if (own.empty()) continue;
            const std::string base = strip_ours(own);
            if (blank(base))
            {
                if (base != own) { widget->setStyleSheet(rml::qt::QString("")); ++changed; }
                continue;
            }
            ++owned;
            std::string want = base;
            if (!css.empty()) { want += kBegin; want += css; want += kEnd; }
            if (want != own) { widget->setStyleSheet(rml::qt::QString(want)); ++changed; }
        }
        m_last_widget_css = css;
        if (changed) m_log->info("restyled {} widget(s) that have their own stylesheet ({} total)", changed, owned);
    }
    void schedule(std::string css)
    {
        if (m_stop.load()) return;
        {
            std::lock_guard lock(m_pending_mutex);
            m_pending_css = std::move(css);
            if (m_apply_queued) return;
            m_apply_queued = true;
        }
        if (auto* qt = rml::qt::QtIntegration::instance())
        {
            qt->run_on_gui_thread([this] {
                std::string pending;
                {
                    std::lock_guard lock(m_pending_mutex);
                    pending = std::move(m_pending_css);
                    m_apply_queued = false;
                }
                if (!m_stop.load()) apply_now(pending);
            });
        }
        else
        {
            std::lock_guard lock(m_pending_mutex);
            m_apply_queued = false;
        }
    }
    void apply_now(const std::string& css)
    {
        auto* app = rml::qt::QApplication::instance();
        if (!app) { m_log->warn("QApplication not available yet"); return; }
        if (!m_captured) { m_original = app->style_sheet(); m_captured = true; }
        const std::string full = css.empty() ? m_original : m_original + "\n/* studio_theme */\n" + css;
        if (full == m_last) return;
        app->set_style_sheet(full);
        m_last = full;
        m_log->info("applied Qt stylesheet ({} bytes)", css.size());
    }
};
extern "C"
{
    RML_MOD_ABI_EXPORT ModBase* start_mod() { return new studio_theme_qt(); }
    RML_MOD_ABI_EXPORT void uninstall_mod(const ModBase* mod) { delete mod; }
}
RML_EXPORT_MOD_ABI_VERSION()
