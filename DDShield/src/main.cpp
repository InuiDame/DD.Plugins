#include "config.h"
#include "game.h"
#include "hooks.h"
#include "loc.h"
#include "render.h"
#include "shield.h"

using namespace dds;

namespace {

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Darkest.exe lives in <root>\_windows\win64, while mods/localization live in
// the install root.  Walk up until we find it.
std::wstring FindGameRoot(std::wstring dir) {
    for (int i = 0; i < 4; ++i) {
        if (DirExists(dir + L"mods") || DirExists(dir + L"localization"))
            return dir;
        std::wstring trimmed = dir;
        if (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/'))
            trimmed.pop_back();
        size_t slash = trimmed.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        dir = trimmed.substr(0, slash + 1);
    }
    return L"";
}

std::wstring DirOfModule(HMODULE mod) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s.resize(slash + 1);
    return s;
}

void Load(HMODULE self) {
    const std::wstring dllDir = DirOfModule(self);
    const std::wstring gameDir = DirOfModule(nullptr);
    INFO("DDShield loading from {}", ToUtf8(dllDir));
    INFO("game dir: {}", ToUtf8(gameDir));

    const std::wstring configPath = dllDir + L"DDShield.ini";
    if (!g_cfg.Load(configPath))
        WARN("DDShield.ini not found, using defaults");
    if (g_cfg.debugLog)
        dku::Logger::EnableDebug();
    INFO("config: enabled={} barShow={} height={:.1f} gap={:.1f} autoScale={}",
        g_cfg.enabled, g_cfg.barShow, g_cfg.barHeight, g_cfg.barGap, g_cfg.autoScale);
    INFO("config: actionAllowCount={} barHoldMs={} mirrorHeal={:.2f}",
        g_cfg.actionAllowCount, g_cfg.barHoldMs, g_cfg.mirrorHeal);

    std::wstring gameRoot = FindGameRoot(gameDir);
    if (gameRoot.empty()) gameRoot = gameDir;
    INFO("game root: {}", ToUtf8(gameRoot));
    loc::Init(dllDir, gameRoot);
    render::SetConfigPath(configPath);
    ShieldLoadClasses(gameRoot);
    ShieldLoadBuffs(gameRoot);
    ShieldLoadEffects(dllDir + L"DDShield.effects.json");

    if (!g_cfg.enabled) {
        INFO("disabled by config");
        return;
    }

    if (!GameResolve()) {
        WARN("signature resolution failed; no hooks installed");
        return;
    }
    HooksInstall();
    INFO("init complete");
}

} // namespace

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

        Load(a_hModule);
    }

    return TRUE;
}
