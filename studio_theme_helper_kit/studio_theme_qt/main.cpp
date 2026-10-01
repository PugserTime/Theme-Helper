#include <RobloxModLoader/logger/logger.hpp>
#include <RobloxModLoader/luau/luau_bridge.hpp>
#include <RobloxModLoader/luau/script_runtime.hpp>
#include <RobloxModLoader/mod/mod_base.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace rml::luau;

#ifdef _WIN32
// Gets the exact on-disk path of this running DLL
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

public:
	studio_theme_qt()
	{
		name = "Studio Theme Qt Updater";
		version = "1.0.1";
		author = "Studio Theme";
		description = "Native file updater and installer bridge";
		m_log = rml::Logger::get_logger("StudioThemeQt");
	}

	// Swaps any pending .new files into place at startup
	void install_pending_update()
	{
#ifdef _WIN32
		namespace fs = std::filesystem;
		const auto self = self_module_path();
		if (self.empty()) return;

		auto staged = self;
		staged += L".new";
		std::error_code ec;
		if (!fs::exists(staged, ec) || ec) return;

		auto old_bak = self;
		old_bak += L".old";
		fs::remove(old_bak, ec);
		ec.clear();

		// Windows allows renaming an active DLL, freeing up the original filename
		fs::rename(self, old_bak, ec);
		if (!ec)
		{
			fs::rename(staged, self, ec);
			if (!ec)
			{
				m_log->info("installed staged update — successfully updated DLL in place");
				fs::remove(old_bak, ec);
				return;
			}
			fs::rename(old_bak, self, ec); // Rollback on failure
		}
#endif
	}

	void on_load() override
	{
		install_pending_update();
		m_log->info("loaded (native file updater ready)");
	}

	void on_script_manager_load() override
	{
		register_bridge();
	}

	bool register_bridge()
	{
		auto* runtime = script_runtime();
		if (!runtime) return false;
		auto& bridge = runtime->bridge();

		// Universal file writer: handles folder creation and Windows in-use swapping
		auto write_fn = [this](const BridgeArgs& args) -> BridgeArgs {
			const auto* path_str = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* bytes = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
			if (!path_str || !bytes || path_str->empty())
			{
				return {false, std::string{"missing arguments"}};
			}

			namespace fs = std::filesystem;
			fs::path target(*path_str);
			std::error_code ec;
			fs::create_directories(target.parent_path(), ec);

			// 1. Try writing directly if the target is not locked
			{
				std::ofstream out(target, std::ios::binary | std::ios::trunc);
				if (out)
				{
					out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
					m_log->info("wrote {} bytes directly to {}", bytes->size(), *path_str);
					return {true};
				}
			}

			// 2. If locked (currently running DLL), write to .new first
			auto staged = target;
			staged += L".new";
			{
				std::ofstream out(staged, std::ios::binary | std::ios::trunc);
				if (!out)
				{
					return {false, std::string{"could not write to " + target.string()}};
				}
				out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
			}

			// 3. Perform Windows in-use rename swap: target -> target.old, then staged -> target
			auto old_bak = target;
			old_bak += L".old";
			fs::remove(old_bak, ec);
			ec.clear();
			fs::rename(target, old_bak, ec);
			if (!ec)
			{
				fs::rename(staged, target, ec);
				if (!ec)
				{
					m_log->info("swapped and updated {} in place ({} bytes)", *path_str, bytes->size());
					fs::remove(old_bak, ec);
					return {true};
				}
				fs::rename(old_bak, target, ec);
			}

			m_log->info("staged {} bytes as .new next to {}", bytes->size(), *path_str);
			return {true, std::string{"staged for next launch"}};
		};

		// 1. Direct write function for Luau
		auto r1 = bridge.register_function("rml_updater", "write_file", write_fn);
		(void)r1;

		// 2. Health check ping
		auto r2 = bridge.register_function("studio_theme_qt", "ping", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		(void)r2;

		// 3. Version report
		auto r3 = bridge.register_function("studio_theme_qt", "version", [this](const BridgeArgs&) -> BridgeArgs {
			return {version};
		});
		(void)r3;

		// 4. Stubs for studio.luau so it confirms connection without throwing errors
		auto r4 = bridge.register_function("studio_theme_qt", "apply", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		(void)r4;

		auto r5 = bridge.register_function("studio_theme_qt", "restyle_widgets", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		(void)r5;

		// 5. Stage update handler that puts the DLL straight into the native folder
		auto r6 = bridge.register_function("studio_theme_qt", "stage_update", [this, write_fn](const BridgeArgs& args) -> BridgeArgs {
			const auto* bytes = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!bytes || bytes->empty()) return {false, std::string{"no data"}};

#ifdef _WIN32
			auto self = self_module_path();
			if (self.empty()) return {false, std::string{"could not determine self path"}};

			// Writes the new DLL directly into the same native folder where this DLL lives
			auto target = self.parent_path() / "studio_theme_qt.dll";
			std::string target_str = target.string();
			return write_fn(BridgeArgs{target_str, *bytes});
#else
			return {false, std::string{"Windows only"}};
#endif
		});
		(void)r6;

		return true;
	}

	void on_unload() override
	{
		if (auto* runtime = script_runtime())
		{
			auto u1 = runtime->bridge().unregister_function("rml_updater", "write_file"); (void)u1;
			auto u2 = runtime->bridge().unregister_function("studio_theme_qt", "ping"); (void)u2;
			auto u3 = runtime->bridge().unregister_function("studio_theme_qt", "version"); (void)u3;
			auto u4 = runtime->bridge().unregister_function("studio_theme_qt", "apply"); (void)u4;
			auto u5 = runtime->bridge().unregister_function("studio_theme_qt", "restyle_widgets"); (void)u5;
			auto u6 = runtime->bridge().unregister_function("studio_theme_qt", "stage_update"); (void)u6;
		}
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

// Emits the exact machine code bytes expected by GitHub Actions (mov eax, 6; ret)
RML_EXPORT_MOD_ABI_VERSION()
