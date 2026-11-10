#include "game.h"

namespace dds {

Game g_game;

namespace {

namespace Asm = dku::Hook::Assembly;

template <class F>
int Resolve(const char* name, F& out, void* found) {
    out = std::bit_cast<F>(found);
    if (!found) {
        WARN("FAILED   {:<40} (signature not found)", name);
        return 0;
    }
    INFO("resolved {:<40} {:#x}", name, AsAddress(found));
    return 1;
}

} // namespace

bool GameResolve() {
    // 特征码在 Steam 和 _windowsnosteam 两个版本中都只命中一处。
    // 三个带回退的函数位移不同，改成通配符后会误中其他函数，所以两个版本各写一条
    void* pageOpen = Asm::search_pattern<
        "40 55 53 56 57 41 56 41 57 48 8D 6C 24 D1 48 81 EC D8 00 00 00 48 8B 05 24 F9 89 00 48 33 C4 48">();
    if (!pageOpen) {
        pageOpen = Asm::search_pattern<
            "40 55 53 56 57 41 56 41 57 48 8D 6C 24 D1 48 81 EC D8 00 00 00 48 8B 05 54 14 89 00 48 33 C4 48">();
    }

    void* resetStats = Asm::search_pattern<
        "40 55 53 56 57 48 8D 6C 24 F8 48 81 EC 08 01 00 00 48 8B 05 B8 24 C1 00">();
    if (!resetStats) {
        resetStats = Asm::search_pattern<
            "40 55 53 56 57 48 8D 6C 24 F8 48 81 EC 08 01 00 00 48 8B 05 E8 12 C0 00 48 33 C4 48 89 45 F0 8B">();
    }

    void* menuUnpause = Asm::search_pattern<
        "4C 8B DC 53 48 83 EC 70 48 8B D9 49 8D 43 B8 49 89 43 08 49 C7 43 F0 00 00 00 00 4D 8D 43 B8 48 8D 0D E2 56">();
    if (!menuUnpause) {
        menuUnpause = Asm::search_pattern<
            "4C 8B DC 53 48 83 EC 70 48 8B D9 49 8D 43 B8 49 89 43 08 49 C7 43 F0 00 00 00 00 4D 8D 43 B8 48 8D 0D 12 CA">();
    }

    int ok = 0;
    ok += Resolve("Actor::ApplyHealthDamageInternal", g_game.applyHealthDamage, Asm::search_pattern<
        "48 8B C4 48 89 58 10 4C 89 48 20 55 56 57 41 54 41 55 41 56 41 57 48 81">());
    ok += Resolve("Monster::ApplyHealthDamageInternal", g_game.applyHealthDamageMonster, Asm::search_pattern<
        "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 38 FE FF FF 48 81 "
        "EC 88 02 00 00 0F 29 70 A8 0F 29 78 98 44 0F 29 40 88 48 8B">());
    ok += Resolve("Overlay::DrawHealthBar", g_game.barDraw, Asm::search_pattern<
        "48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC">());
    ok += Resolve("EffectLookup", g_game.effectLookup, Asm::search_pattern<
        "48 83 EC 28 33 C9 4C 8B C2 38 0A 74 16 0F 1F 00">());
    ok += Resolve("EffectApplyInternal", g_game.effectApply, Asm::search_pattern<
        "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C8 DC FF">());
    ok += Resolve("Battle::UpdateRoundBuffs", g_game.updateRoundBuffs, Asm::search_pattern<
        "40 53 55 56 57 41 54 41 56 41 57 48 81 EC A0 00 00 00 48 8B E9 48 8B 05">());
    ok += Resolve("Actor::GetMaxHealth", g_game.getMaxHealth, Asm::search_pattern<
        "48 89 5C 24 08 48 89 7C 24 10 4C 8B 59 30 44 8B">());
    ok += Resolve("Actor::ConsumeHealthDamageBlock", g_game.consumeBlock, Asm::search_pattern<
        "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 40">());
    ok += Resolve("Actor::ResetStats", g_game.resetStats, resetStats);
    ok += Resolve("BlockPopup(blocked)", g_game.blockPopup, Asm::search_pattern<
        "48 89 5C 24 20 55 56 57 41 56 41 57 48 81 EC E0 00 00 00 48">());
    ok += Resolve("Panel::PageOpen", g_game.pageOpen, pageOpen);
    ok += Resolve("Panel::PageClose", g_game.pageClose, Asm::search_pattern<
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 60 48 8B D9 48 8B A9 F0 04 00 00">());
    ok += Resolve("Menu::OnPause", g_game.menuPause, Asm::search_pattern<
        "4C 8B DC 53 55 56 57 48 83 EC 68 48 8B F1 33 ED">());
    ok += Resolve("Menu::OnUnpause", g_game.menuUnpause, menuUnpause);

    g_game.ready = ok >= 4; // bar + damage hooks are the minimum
    INFO("GameResolve: {}/14 functions resolved", ok);
    return g_game.ready;
}

float ActorMaxHealth(void* actor) {
    if (!g_game.getMaxHealth || !actor) return 0.0f;
    float result = 0.0f;
    __try {
        void* actorClass = *(void**)((char*)actor + kOffActorClass);
        int level = *(int*)((char*)actor + kOffLevel);
        if (actorClass)
            result = g_game.getMaxHealth(actorClass, level, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0.0f;
    }
    return result;
}

float BarWidth(int rank, bool shared) {
    if (rank < 1 || rank > 4) return 0.0f;
    float* table = (float*)(ModuleBase() + (shared ? kRvaBarWidthsShared : kRvaBarWidths));
    float w = table[rank - 1];
    if (!(w > 0.0f) || w > 4000.0f) return 0.0f;
    return w;
}

} // namespace dds
