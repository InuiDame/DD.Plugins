#include "Hooks.h"

namespace {

bool DirExists(const std::wstring& path)
{
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ModuleDir(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring s(path);
    const std::size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? s : s.substr(0, slash + 1);
}

// Darkest.exe 在 <root>\_windows\win64，数据目录在 <root> 下。
std::wstring FindGameRoot(const std::wstring& exeDir)
{
    std::wstring dir = exeDir;
    for (int i = 0; i < 5; ++i) {
        if (DirExists(dir + L"mods") && DirExists(dir + L"heroes")) return dir;
        if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        const std::size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        dir = dir.substr(0, slash + 1);
    }
    return exeDir;
}

void OnAttach(HMODULE self)
{
    const std::wstring gameRoot = FindGameRoot(ModuleDir(nullptr));
    INFO("HeroLevelCap loading, game root: {}", std::filesystem::path(gameRoot).string());

    HeroLevelCap::CapMap caps;
    HeroLevelCap::LoadRules(gameRoot, caps);
    if (caps.empty()) {
        INFO("no level_cap blocks found; default cap is {}", HeroLevelCap::kDefaultCap);
    }

    HeroLevelCap::Hooks::Install(caps);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE a_hModule, DWORD a_reason, LPVOID)
{
    if (a_reason == DLL_PROCESS_ATTACH) {
#ifndef NDEBUG
        while (!::IsDebuggerPresent()) ::Sleep(100);
#endif
        dku::Logger::Init(Plugin::NAME, std::to_string(Plugin::Version));
        INFO("game type: {}", dku::Hook::GetProcessName(::GetCurrentProcessId()));

        ::DisableThreadLibraryCalls(a_hModule);
        dku::Hook::Trampoline::AllocTrampoline(1 << 6);

        OnAttach(a_hModule);
    }
    return TRUE;
}
