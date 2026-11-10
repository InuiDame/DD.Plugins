#include "hooks.h"
#include "config.h"
#include "game.h"
#include "render.h"
#include "shield.h"

namespace dds {
namespace {

ApplyHealthDamageFn o_applyActor = nullptr;
ApplyHealthDamageFn o_applyMonster = nullptr;
BarDrawFn           o_barDraw = nullptr;
EffectApplyFn       o_effectApply = nullptr;
UpdateRoundBuffsFn  o_roundBuffs = nullptr;
ResetStatsFn        o_resetStats = nullptr;
BlockPopupFn        o_blockPopup = nullptr;
ConsumeBlockFn      o_consumeBlock = nullptr;
MenuTransitionFn    o_menuPause = nullptr;
MenuTransitionFn    o_menuUnpause = nullptr;

void* DamageEntry(void* self, void* prng, float damage, const int* dt, unsigned src,
                  const int* st, bool flag, void* attacker, ApplyHealthDamageFn orig) {
    if (!orig) return nullptr;
    void* const outerAttacker = t_damageAttacker;
    t_damageAttacker = attacker;
    ++t_inDamage;
    void* result = orig(self, prng, damage, dt, src, st, flag, attacker);
    --t_inDamage;
    t_damageAttacker = outerAttacker;
    ShieldEndHit(self, attacker);
    return result;
}

void* DetourActorDamage(void* self, void* prng, float damage, const int* dt, unsigned src,
                        const int* st, bool flag, void* attacker) {
    return DamageEntry(self, prng, damage, dt, src, st, flag, attacker, o_applyActor);
}

void* DetourMonsterDamage(void* self, void* prng, float damage, const int* dt, unsigned src,
                          const int* st, bool flag, void* attacker) {
    return DamageEntry(self, prng, damage, dt, src, st, flag, attacker, o_applyMonster);
}

// Called by ApplyHealthDamageInternal with the FINAL (post-protection) damage
// right before the health subtraction, so this is the correct place to decide
// how much the shield eats.  Returning true makes the engine skip the damage
// and run its own "Blocked" presentation.
bool DetourConsumeBlock(void* self, int damageType, float damage) {
    bool nativeBlocked = o_consumeBlock ? o_consumeBlock(self, damageType, damage) : false;
    (void)damageType;
    if (nativeBlocked) return true;                       // real Aegis charge used
    if (!g_cfg.enabled || t_inDamage <= 0 || damage <= 0.0f) return false;

    float credit = 0.0f;
    AbsorbKind kind = ShieldAbsorbFinal(self, damage, credit);
    if (kind == AbsorbKind::Full) return true;
    if (kind == AbsorbKind::Partial) {
        // The engine is about to subtract the full `damage`; give back the part
        // that the shield did not cover so the net health loss is the absorbed
        // amount.  The bar hook ignores this refund when mirroring heals.
        ActorSetHealth(self, ActorHealth(self) + credit);
        if (ShieldState* st = FindShield(self)) st->pendingHealthCredit += credit;
    }
    return false;
}

void DetourBarDraw(void* self, Vec2 pos, void* actor, bool shared,
                   float health, float maxHealth, int rank, float dt) {
    if (o_barDraw) o_barDraw(self, pos, actor, shared, health, maxHealth, rank, dt);
    if (g_cfg.enabled && actor) ShieldOnActorBar(actor, pos, rank, shared);
}

void DetourEffectApply(void* prng, void* performer, void* target, const void* effect,
                       void* item, float f1, bool b1, float f2, int source,
                       unsigned int u, void* result1, bool b2, bool b3) {
    if (g_cfg.enabled && effect) ShieldOnEffectApply(target, effect, performer);
    if (o_effectApply)
        o_effectApply(prng, performer, target, effect, item, f1, b1, f2, source, u, result1, b2, b3);
}

void DetourRoundBuffs(void* battle) {
    if (o_roundBuffs) o_roundBuffs(battle);
    if (g_cfg.enabled) ShieldOnRound();
}

void DetourBlockPopup(void* self, void* actor, int type, int flag) {
    if (g_cfg.enabled) ShieldOnBlockPopup(type);
    if (o_blockPopup) o_blockPopup(self, actor, type, flag);
}

PanelTransitionFn   o_pageOpen = nullptr;
PanelTransitionFn   o_pageClose = nullptr;

void DetourPageOpen(void* self) {
    g_panelOpen = true;
    DEBUG("panel open {}", self);
    if (o_pageOpen) o_pageOpen(self);
}

void DetourPageClose(void* self) {
    g_panelOpen = false;
    DEBUG("panel close {}", self);
    if (o_pageClose) o_pageClose(self);
}

void DetourResetStats(void* self, int flags) {
    if (o_resetStats) o_resetStats(self, flags);
    if (g_cfg.enabled) ShieldOnStatsReset(self);
}

// Pause menu -----------------------------------------------------------------
void DetourMenuPause(void* self) {
    g_menuOpen = true;
    if (o_menuPause) o_menuPause(self);
}

void DetourMenuUnpause(void* self) {
    g_menuOpen = false;
    if (o_menuUnpause) o_menuUnpause(self);
}

template <std::size_t N, class F>
int Detour(F target, F hook, F& original) {
    if (!target) return 0;
    original = dku::Hook::write_detour<N>(AsAddress(target), hook);
    return 1;
}

// SDL_GL_SwapWindow ---------------------------------------------------------
// SDL2.dll 的导出只是动态 API 跳板 jmp qword ptr [rip+disp]，真实地址在跳转表里。
// 它离游戏模块超过 2GB，且 rip 相对指令不能原样搬走，所以在 SDL2.dll 附近单独分配
// trampoline 做长跳：
//   跳板 -> jmp rel32 -> trampoline -> OnSwapWindow -> epilog 尾跳到跳转表里的真实函数
void OnSwapWindow() {
    __try {
        ShieldTick();
        render::OnSwap();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WARN("exception in render hook");
    }
}

// Buff 规则 -----------------------------------------------------------------
// actorStatus（对方）和 is_actor_status（自身）共用同一段状态判定，走到这里时
// rdi 已是要检查的角色，r15 是 Buff，sil 存判定结果。接管 shielded 这个状态后跳到
// 规则结算入口，is_false_rule 由游戏继续处理
constexpr std::uint32_t kOffBuffRuleStringHash = 0xF4;

constexpr std::uint32_t HashName(std::string_view name) {
    std::uint32_t hash = 0;
    for (const unsigned char c : name) {
        hash = hash * 53 + c;
    }
    return hash;
}

constexpr std::uint32_t kStatusShielded = HashName("shielded");

// 返回 -1 表示不是 shielded，交回游戏原有的判定
int CheckBuffStatus(void* actor, const char* buff) {
    const auto hash = *reinterpret_cast<const std::uint32_t*>(buff + kOffBuffRuleStringHash);
    return hash == kStatusShielded ? ShieldStatus(actor) : -1;
}

struct StatusProlog : Xbyak::CodeGenerator {
    StatusProlog() {
        mov(rcx, rdi);
        mov(rdx, r15);
    }
};

struct StatusEpilog : Xbyak::CodeGenerator {
    explicit StatusEpilog(std::uintptr_t tail) {
        Xbyak::Label notShielded;
        test(eax, eax);
        js(notShielded, T_NEAR);
        mov(sil, al);
        mov(rax, tail);
        jmp(rax);
        L(notShielded);
    }
};

bool HookBuffStatus() {
    auto* site = dku::Hook::Assembly::search_pattern<
        "41 83 FE 2A "             // cmp     r14d, 2Ah
        "48 0F 44 FB "             // cmove   rdi, rbx
        "41 8B 87 F4 00 00 00 "    // mov     eax, [r15+0F4h]
        "0F 57 F6 "                // xorps   xmm6, xmm6
        "3B 05 ?? ?? ?? ?? "       // cmp     eax, stunned
        "75 15 "                   // jnz     short
        "F3 0F 10 87 90 10 00 00 " // movss   xmm0, [rdi+1090h]
        "0F 2F C6 "                // comiss  xmm0, xmm6
        "76 08 "                   // jbe     short
        "40 B6 01 "                // mov     sil, 1
        "E9">();                   // jmp     结算入口
    if (!site) {
        WARN("buff rule status branch not found; shielded buff rules are disabled");
        return false;
    }

    const auto address = AsAddress(site);
    const auto tail = address + 0x2F + *std::bit_cast<const std::int32_t*>(address + 0x2B);
    InstallBuffStatusCave(address, 8, 15, tail);

    INFO("hooked buff rule status at +{:X}", address - ModuleBase());
    return true;
}

constexpr std::ptrdiff_t kSwapThunkSize = 7;

struct SwapProlog : Xbyak::CodeGenerator {
    // 保存 SDL_Window*，入口处 rsp 模 16 余 8，压栈后 call 时正好对齐
    SwapProlog() { push(rcx); }
};

struct SwapEpilog : Xbyak::CodeGenerator {
    explicit SwapEpilog(std::uintptr_t slot) {
        pop(rcx);
        mov(rax, slot);
        jmp(qword[rax]);
    }
};

} // namespace

void InstallBuffStatusCave(std::uintptr_t site, std::ptrdiff_t from, std::ptrdiff_t to, std::uintptr_t tail) {
    using Flag = dku::Hook::HookFlag;

    StatusProlog prolog;
    StatusEpilog epilog{ tail };
    auto handle = dku::Hook::AddCaveHook(site, { from, to }, FUNC_INFO(CheckBuffStatus), &prolog, &epilog,
        { Flag::kSkipNOP, Flag::kRestoreAfterEpilog });
    handle->Enable();
}

std::uint32_t ShieldedStatusHash() {
    return kStatusShielded;
}

bool HookSwapWindow() {
    HMODULE sdl = GetModuleHandleW(L"SDL2.dll");
    const auto* thunk = sdl ? std::bit_cast<const std::uint8_t*>(GetProcAddress(sdl, "SDL_GL_SwapWindow")) : nullptr;
    if (!thunk || thunk[0] != 0x48 || thunk[1] != 0xFF || thunk[2] != 0x25) {
        WARN("SDL_GL_SwapWindow is not the expected thunk; shield bars will not be drawn");
        return false;
    }

    const auto slot = AsAddress(thunk) + kSwapThunkSize + *std::bit_cast<const std::int32_t*>(thunk + 3);

    // PageAlloc 会把 DKUtil 的 trampoline 切到新区域，必须排在游戏函数的 Hook 之后
    dku::Hook::Trampoline::GetTrampoline().PageAlloc(1 << 8, AsAddress(thunk));

    SwapProlog prolog;
    SwapEpilog epilog{ slot };
    auto handle = dku::Hook::AddCaveHook(AsAddress(thunk), { 0, kSwapThunkSize }, FUNC_INFO(OnSwapWindow), &prolog, &epilog);
    handle->Enable();

    INFO("hooked SDL_GL_SwapWindow export at {}", static_cast<const void*>(thunk));
    return true;
}

bool HooksInstall() {
    dku::Hook::Trampoline::AllocTrampoline(1 << 10);

    // 偷取长度落在指令边界上，这些字节都包含在特征码里，不含 rip 相对寻址
    int count = 0;
    count += Detour<7>(g_game.applyHealthDamage, DetourActorDamage, o_applyActor);          // mov rax, rsp; mov [rax+10h], rbx
    count += Detour<5>(g_game.applyHealthDamageMonster, DetourMonsterDamage, o_applyMonster);  // mov rax, rsp; push rbp; push rbx
    count += Detour<5>(g_game.consumeBlock, DetourConsumeBlock, o_consumeBlock);            // mov [rsp+10h], rbx
    count += Detour<5>(g_game.barDraw, DetourBarDraw, o_barDraw);                           // mov [rsp+20h], rbx
    count += Detour<5>(g_game.effectApply, DetourEffectApply, o_effectApply);               // push rbp; push rbx; push rsi; push rdi
    count += Detour<5>(g_game.updateRoundBuffs, DetourRoundBuffs, o_roundBuffs);            // push rbx; push rbp; push rsi; push rdi
    count += Detour<5>(g_game.resetStats, DetourResetStats, o_resetStats);                  // push rbp; push rbx; push rsi; push rdi
    count += Detour<5>(g_game.blockPopup, DetourBlockPopup, o_blockPopup);                  // mov [rsp+20h], rbx
    count += Detour<5>(g_game.pageOpen, DetourPageOpen, o_pageOpen);                        // push rbp; push rbx; push rsi; push rdi
    count += Detour<5>(g_game.pageClose, DetourPageClose, o_pageClose);                     // mov [rsp+10h], rbx
    count += Detour<5>(g_game.menuPause, DetourMenuPause, o_menuPause);                     // mov r11, rsp; push rbx; push rbp
    count += Detour<8>(g_game.menuUnpause, DetourMenuUnpause, o_menuUnpause);               // mov r11, rsp; push rbx; sub rsp, 70h
    INFO("HooksInstall: {} game hooks installed", count);

    HookBuffStatus();

    const bool swapHooked = HookSwapWindow();
    return count > 0 || swapHooked;
}

} // namespace dds
