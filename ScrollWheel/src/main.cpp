#include "Config.h"
#include "Hooks.h"

namespace
{
	std::filesystem::path GetGameRoot()
	{
		std::wstring buf(MAX_PATH, L'\0');
		for (;;) {
			const auto len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
			if (len < buf.size()) {
				buf.resize(len);
				break;
			}
			buf.resize(buf.size() * 2);
		}
		// <gameRoot>\_windows\win64\Darkest.exe -> 上溯三级
		return std::filesystem::path{ buf }.parent_path().parent_path().parent_path();
	}
}

BOOL APIENTRY DllMain(HMODULE a_hModule, DWORD a_ul_reason_for_call, LPVOID a_lpReserved)
{
	if (a_ul_reason_for_call == DLL_PROCESS_ATTACH) {
		::DisableThreadLibraryCalls(a_hModule);

		dku::Logger::Init(Plugin::NAME, std::to_string(Plugin::Version));
		INFO("game type : {}", dku::Hook::GetProcessName(::GetCurrentProcessId()));

		const auto cfgPath = GetGameRoot() / "DDModLoader" / "cfg" / "ScrollWheel.cfg";
		Config::Data cfg;
		if (!Config::Load(cfgPath, cfg)) {
			WARN("cannot load/generate cfg, using built-in defaults: {}", cfgPath.string());
		}
		if (cfg.debugLog) {
			dku::Logger::EnableDebug();
		}

		Hooks::Install(cfg);
	}

	return TRUE;
}
