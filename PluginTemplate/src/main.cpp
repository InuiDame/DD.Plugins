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

		dku::Hook::Trampoline::AllocTrampoline(1 << 6);
	}

	return TRUE;
}
