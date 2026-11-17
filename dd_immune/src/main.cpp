#include "Hooks.h"
#include "Rules.h"

// DDModLoader 通过 dbghelp.dll 代理 LoadLibraryW 加载本 DLL，沿用 DD.Plugins 的 DllMain 协议。
// 注意：DllMain 里不做重活之外的线程创建；规则扫描是一次性磁盘读，hook 安装是一次性内联补丁，
// 与 CustomTypeEffects / PluginTemplate 的既有做法一致。
BOOL APIENTRY DllMain(HMODULE a_hModule, DWORD a_ul_reason_for_call, LPVOID a_lpReserved)
{
	if (a_ul_reason_for_call == DLL_PROCESS_ATTACH) {
		::DisableThreadLibraryCalls(a_hModule);

		// DllMain 主线程 ID，供可选 rule-gated 模式的主线程门使用（v1 不启用，见 README）。
		Hooks::CacheMainThread(::GetCurrentThreadId());

		dku::Logger::Init(Plugin::NAME, std::to_string(Plugin::Version));
		INFO("game type : {}", dku::Hook::GetProcessName(::GetCurrentProcessId()));

		Rules::Load();
		Hooks::Install();

		INFO("dd_immune ready: {} buff rule(s), {} class hash(es), armed={}",
			Rules::BuffCount(), Rules::ClassHashCount(), Hooks::IsArmed());
	}

	return TRUE;
}
