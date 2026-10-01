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
		version = "1.0.0";
		author = "Studio Theme";
		description = "Lightweight file bridge";
		m_log = rml::Logger::get_logger("StudioThemeQt");
	}

	void on_load() override
	{
		m_log->info("loaded (file bridge active)");
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

		auto write_fn = [this](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* bytes = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;
			if (!path || !bytes || path->empty()) return {false, std::string{"missing arguments"}};

			namespace fs = std::filesystem;
			std::error_code ec;
			fs::create_directories(fs::path(*path).parent_path(), ec);

			std::ofstream out(*path, std::ios::binary | std::ios::trunc);
			if (!out) return {false, std::string{"could not open file for writing"}};
			out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
			m_log->info("wrote {} bytes to {}", bytes->size(), *path);
			return {true};
		};

		auto r1 = bridge.register_function("rml_updater", "write_file", write_fn);
		(void)r1;

		auto r2 = bridge.register_function("studio_theme_qt", "ping", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		(void)r2;

		auto r3 = bridge.register_function("studio_theme_qt", "stage_update", [write_fn](const BridgeArgs& args) -> BridgeArgs {
			const auto* bytes = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!bytes || bytes->empty()) return {false, std::string{"no data"}};
#ifdef _WIN32
			auto self = self_module_path();
			if (self.empty()) return {false, std::string{"could not find self path"}};
			auto target = self.parent_path() / "studio_theme_qt.dll";
			std::string target_str = target.string();
			return write_fn(BridgeArgs{target_str, *bytes});
#else
			return {false, std::string{"Windows only"}};
#endif
		});
		(void)r3;

		return true;
	}

	void on_unload() override
	{
		if (auto* runtime = script_runtime()) {
			auto u1 = runtime->bridge().unregister_function("rml_updater", "write_file");
			(void)u1;
			auto u2 = runtime->bridge().unregister_function("studio_theme_qt", "ping");
			(void)u2;
			auto u3 = runtime->bridge().unregister_function("studio_theme_qt", "stage_update");
			(void)u3;
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

RML_EXPORT_MOD_ABI_VERSION()
