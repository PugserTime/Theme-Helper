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

		// Expose write_file for modern scripts
		bridge.register_function("rml_updater", "write_file", write_fn);

		// Ping to let Luau know the disk helper is active
		bridge.register_function("rml_updater", "ping", [](const BridgeArgs&) -> BridgeArgs {
			return {true};
		});

		// Compatibility alias: if scripts call studio_theme_qt's stage_update, route it here
		bridge.register_function("studio_theme_qt", "stage_update", [this, write_fn](const BridgeArgs& args) -> BridgeArgs {
			const auto* bytes = args.size() > 0 ? std::get_if<std::string>(&args[0]) : nullptr;
			if (!bytes || bytes->empty())
				return {false, std::string{"no data"}};

			// Find our own folder and write studio_theme_qt.dll directly
			HMODULE mod = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                   reinterpret_cast<LPCWSTR>(&rml_updater::register_bridge), &mod);
			wchar_t buf[MAX_PATH]{};
			GetModuleFileNameW(mod, buf, MAX_PATH);

			namespace fs = std::filesystem;
			fs::path self_path(buf);
			fs::path target = self_path.parent_path() / "studio_theme_qt.dll";

			std::string target_str = target.string();
			return write_fn(BridgeArgs{target_str, *bytes});
		});
	}

	void on_unload() override
	{
		if (auto* runtime = script_runtime()) {
			runtime->bridge().unregister_function("rml_updater", "write_file");
			runtime->bridge().unregister_function("rml_updater", "ping");
			runtime->bridge().unregister_function("studio_theme_qt", "stage_update");
		}
	}
};

// Inspects the caller instruction inside roblox_modloader.dll to dynamically
// extract the exact ABI version the loader is comparing against.
static uint32_t detect_expected_loader_abi(void* ret_addr)
{
#ifdef _WIN32
	uint32_t detected_abi = 0;
	__try
	{
		const auto* p = reinterpret_cast<const uint8_t*>(ret_addr);
		// Scan up to 48 bytes after the call instruction for common comparison opcodes
		for (int i = 0; i < 48; ++i)
		{
			// 1. cmp eax, imm8  (83 F8 <imm8>)
			if (p[i] == 0x83 && p[i + 1] == 0xF8) {
				detected_abi = p[i + 2];
				break;
			}
			// 2. cmp eax, imm32 (3D <imm32>)
			if (p[i] == 0x3D) {
				detected_abi = *reinterpret_cast<const uint32_t*>(&p[i + 1]);
				break;
			}
			// 3. cmp eax, imm32 (81 F8 <imm32>)
			if (p[i] == 0x81 && p[i + 1] == 0xF8) {
				detected_abi = *reinterpret_cast<const uint32_t*>(&p[i + 2]);
				break;
			}
			// 4. cmp [rbp+disp8], imm8 (83 7D <disp8> <imm8>)
			if (p[i] == 0x83 && p[i + 1] == 0x7D) {
				detected_abi = p[i + 3];
				break;
			}
			// 5. cmp [rsp+disp8], imm8 (83 7C 24 <disp8> <imm8>)
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

	// If detected within reasonable range (1 - 255), return it
	if (detected_abi >= 1 && detected_abi <= 255)
	{
		return detected_abi;
	}
#endif

	// Fallback to compile-time header version or current known default (6)
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

	// Dynamically detects the loader's expected ABI and returns it
	RML_MOD_ABI_EXPORT uint32_t rml_mod_abi_version()
	{
#ifdef _WIN32
		return detect_expected_loader_abi(_ReturnAddress());
#else
		return 6;
#endif
	}
}
