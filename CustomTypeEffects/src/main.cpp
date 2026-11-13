#include "Hooks.h"
#include "Rules.h"

// DDModLoader 通过 dbghelp.dll 代理 LoadLibraryW 加载本 DLL，沿用 DD.Plugins 的 DllMain 协议
BOOL APIENTRY DllMain(HMODULE a_hModule, DWORD a_ul_reason_for_call, LPVOID a_lpReserved)
{
	if (a_ul_reason_for_call == DLL_PROCESS_ATTACH) {
		::DisableThreadLibraryCalls(a_hModule);

		dku::Logger::Init(Plugin::NAME, std::to_string(Plugin::Version));
		INFO("game type : {}", dku::Hook::GetProcessName(::GetCurrentProcessId()));

		Rules::Load();
		Hooks::Install();
	}

	return TRUE;
}
