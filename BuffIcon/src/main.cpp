#include "BuffIcons.h"
#include "Hooks.h"

// ===================== DllMain =====================
BOOL APIENTRY DllMain(HMODULE a_hModule, DWORD a_ul_reason_for_call, LPVOID a_lpReserved)
{
	if (a_ul_reason_for_call == DLL_PROCESS_ATTACH) {
#ifndef NDEBUG
		while (!IsDebuggerPresent()) {
			Sleep(100);
		}
#endif

		dku::Logger::Init(Plugin::NAME, std::to_string(Plugin::Version));
		INFO("game type : {}", dku::Hook::GetProcessName(::GetCurrentProcessId()));

		BuffIcons::Load();
		Hooks::Install();
	}

	return TRUE;
}
