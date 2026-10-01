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
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif

using namespace rml::luau;

// Static function pointer safely converts to LPCWSTR for GetModuleHandleExW
static std::filesystem::path self_module_path()
{
#ifdef _WIN32
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
#else
	return {};
#endif
}

class rml_updater final : public ModBase
{
	std::shared_ptr<spdlog::logger> m_log;

public:
	rml_updater()
	{
		name = "RML File & Update Helper";
		version = "1.0.0";
		author = "Studio Theme";
		description = "Lightweight file bridge for Luau scripts";
		m_log = rml::Logger::get_logger("RmlUpdater");
	}

	void on_load() override
	{
		m_log->info("loaded (universal dynamic ABI spoofer active)");
	}

	void on_script_manager_load() override
	{
		register_bridge();
	}

	void register_bridge()
	{
		auto* runtime = script_runtime();
		if (!runtime) return;

		auto& bridge = runtime->bridge();

		auto write_fn = [this](const BridgeArgs& args) -> BridgeArgs {
			const auto* path = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			const auto* bytes = args.size() > 1 ? std::get_if<std::string>(&args[1]) : nullptr;

			if (!path || !bytes || path->empty())
				return {false, std::string{"missing arguments"}};

			namespace fs = std::filesystem;
			std::error_code ec;
			fs::create_directories(fs::path(*path).parent_path(), ec);

			std::ofstream out(*path, std::ios::binary | std::ios::trunc);
			if (!out)
				return {false, std::string{"could not open file for writing"}};

			out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
			m_log->info("wrote {} bytes to {}", bytes->size(), *path);
			return {true};
		};

		// [[nodiscard]] return values are handled to prevent compiler warnings
		auto reg1 = bridge.register_function("rml_updater", "write_file", write_fn);
		if (!reg1) { m_log->debug("reg write_file: {}", reg1.error()); }

		auto reg2 = bridge.register_function("rml_updater", "ping", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});
		if (!reg2) { m_log->debug("reg ping: {}", reg2.error()); }

		auto reg3 = bridge.register_function("studio_theme_qt", "stage_update", [write_fn](const BridgeArgs& args) -> BridgeArgs {
			const auto* bytes = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!bytes || bytes->empty())
				return {false, std::string{"no data"}};

			auto self_path = self_module_path();
			if (self_path.empty())
				return {false, std::string{"could not determine self path"}};

			auto target = self_path.parent_path() / "studio_theme_qt.dll";
			std::string target_str = target.string();
			return write_fn(BridgeArgs{target_str, *bytes});
		});
		if (!reg3) { m_log->debug("reg stage_update: {}", reg3.error()); }
	}

	void on_unload() override
	{
		if (auto* runtime = script_runtime()) {
			auto u1 = runtime->bridge().unregister_function("rml_updater", "write_file");
			(void)u1;
			auto u2 = runtime->bridge().unregister_function("rml_updater", "ping");
			(void)u2;
			auto u3 = runtime->bridge().unregister_function("studio_theme_qt", "stage_update");
			(void)u3;
		}
	}
};

static uint32_t detect_expected_loader_abi(void* ret_addr)
{
#ifdef _WIN32
	uint32_t detected_abi = 0;
	__try
	{
		const auto* p = reinterpret_cast<const uint8_t*>(ret_addr);
		for (int i = 0; i < 48; ++i)
		{
			if (p[i] == 0x83 && p[i + 1] == 0xF8) {
				detected_abi = p[i + 2];
				break;
			}
			if (p[i] == 0x3D) {
				detected_abi = *reinterpret_cast<const uint32_t*>(&p[i + 1]);
				break;
			}
			if (p[i] == 0x81 && p[i + 1] == 0xF8) {
				detected_abi = *reinterpret_cast<const uint32_t*>(&p[i + 2]);
				break;
			}
			if (p[i] == 0x83 && p[i + 1] == 0x7D) {
				detected_abi = p[i + 3];
				break;
			}
			if (p[i] == 0x83 && p[i + 1] == 0x7C && p[i + 2] == 0x24) {
				detected_abi = p[i + 4];
				break;
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		detected_abi = 0;
	}

	if (detected_abi >= 1 && detected_abi <= 255)
	{
		return detected_abi;
	}
#else
	(void)ret_addr;
#endif

#ifdef RML_ABI_VERSION
	return RML_ABI_VERSION;
#else
	return 6;
#endif
}

extern "C"
{
	RML_MOD_ABI_EXPORT ModBase* start_mod()
	{
		return new rml_updater();
	}

	RML_MOD_ABI_EXPORT void uninstall_mod(const ModBase* mod)
	{
		delete mod;
	}

	RML_MOD_ABI_EXPORT uint32_t rml_mod_abi_version()
	{
#ifdef _WIN32
		return detect_expected_loader_abi(_ReturnAddress());
#else
		return 6;
#endif
	}
}
