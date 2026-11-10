#pragma once
#include "common.h"

namespace dds {

// ---------------------------------------------------------------------------
// Verified engine facts (Darkest.exe win64 build, image base 0x140000000)
//   Actor::health                 float32 at actor + 0x10A8
//   Actor::health_damage_blocks   int32   at actor + 0x123C   (native "Aegis")
//   Actor::actor_class            pointer at actor + 0x0768
//   Actor::level                  int32   at actor + 0x0790
//   status_bars layout            float[] at image + 0x10AA794 (normal widths)
//                                 float[] at image + 0x10AA7A4 (shared widths)
// ---------------------------------------------------------------------------
constexpr uint32_t kOffHealth        = 0x10A8;
constexpr uint32_t kOffBlockCharges  = 0x123C;
constexpr uint32_t kOffActorClass    = 0x0768;
constexpr uint32_t kOffLevel         = 0x0790;

constexpr uint32_t kRvaBarWidths       = 0x10AA794;  // shared == false
constexpr uint32_t kRvaBarWidthsShared = 0x10AA7A4;  // shared == true

// Function prototypes -------------------------------------------------------
using ApplyHealthDamageFn = void* (*)(void* self, void* prng, float damage,
                                      const int* damageType, unsigned int sourceGuid,
                                      const int* sourceType, bool flag, void* attacker);
using BarDrawFn      = void (*)(void* self, Vec2 pos, void* actor, bool shared,
                                float health, float maxHealth, int rank, float dt);
using EffectApplyFn  = void (*)(void* prng, void* performer, void* target, const void* effect,
                                void* item, float f1, bool b1, float f2, int source,
                                unsigned int u, void* result1, bool b2, bool b3);
using EffectLookupFn = void* (*)(const char* name);
using ConsumeBlockFn  = bool (*)(void* self, int damageType, float damage);
using UpdateRoundBuffsFn = void (*)(void* battle);
using GetMaxHealthFn = float (*)(void* actorClass, int level, bool flag);
using MenuTransitionFn = void (*)(void* self);
using ResetStatsFn    = void (*)(void* self, int flags);
using BlockPopupFn    = void (*)(void* self, void* actor, int type, int flag);
using PanelTransitionFn = void (*)(void* self);

struct Game {
    ApplyHealthDamageFn applyHealthDamage = nullptr;
    ApplyHealthDamageFn applyHealthDamageMonster = nullptr;
    BarDrawFn           barDraw = nullptr;
    EffectApplyFn       effectApply = nullptr;
    EffectLookupFn      effectLookup = nullptr;
    ConsumeBlockFn      consumeBlock = nullptr;
    UpdateRoundBuffsFn  updateRoundBuffs = nullptr;
    GetMaxHealthFn      getMaxHealth = nullptr;
    ResetStatsFn        resetStats = nullptr;
    BlockPopupFn        blockPopup = nullptr;
    PanelTransitionFn   pageOpen = nullptr;
    PanelTransitionFn   pageClose = nullptr;
    MenuTransitionFn    menuPause = nullptr;
    MenuTransitionFn    menuUnpause = nullptr;
    bool ready = false;
};

extern Game g_game;

bool GameResolve();

// Main module (Darkest.exe) helpers -----------------------------------------
// 每帧都会调用，缓存下来避免反复查询模块
inline uintptr_t ModuleBase() {
    static const uintptr_t base = dku::Hook::Module::get().base();
    return base;
}
inline size_t ModuleSize() {
    return dku::Hook::Module::get().ntHeader()->OptionalHeader.SizeOfImage;
}

// Actor helpers -------------------------------------------------------------
inline float ActorHealth(void* actor) {
    return *(float*)((char*)actor + kOffHealth);
}
inline void  ActorSetHealth(void* actor, float v) {
    *(float*)((char*)actor + kOffHealth) = v;
}
inline int*  ActorBlockCharges(void* actor) {
    return (int*)((char*)actor + kOffBlockCharges);
}
float ActorMaxHealth(void* actor);
float BarWidth(int rank, bool shared);

} // namespace dds
