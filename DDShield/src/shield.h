#pragma once
#include "common.h"

namespace dds {

struct ShieldGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    int   rank = 0;
    bool  shared = false;
    bool  valid = false;
};

struct ShieldState {
    float cur = 0.0f;
    float max = 0.0f;
    float lastHealth = -1.0f;
    float regen = 0.0f;
    int   regenTurns = 0;
    float decay = 0.0f;
    int   decayTurns = 0;
    bool  initialized = false;
    DWORD lastSeen = 0;
    float pendingHealthCredit = 0.0f;   // health we added to soften a partial absorb
    float baseMax = 0.0f;               // class/config cap before buffs
    float effectMax = 0.0f;             // accumulated cap from skill effects
    float buffMaxAdd = 0.0f;
    float buffMaxPct = 0.0f;
    float buffGain = 0.0f;
    float buffRegen = 0.0f;
    float buffDecay = 0.0f;
    DWORD lastBuffScan = 0;
    float piercePct = 0.0f;             // 下一次命中穿透护盾的百分比
    void* pierceAttacker = nullptr;     // 穿透只对施加它的攻击者生效
    int   action = 0;                   // Actor action/animation enum
    int   loggedAction = -999;
    ShieldGeometry geo;
};

struct ShieldEffectSpec {
    std::string op;
    float value = 0.0f;
    int   duration = 0;
};

using ShieldMap = std::unordered_map<void*, ShieldState>;

ShieldMap& Shields();

// Depth counter set while the game is applying health damage.
extern thread_local int t_inDamage;

// 当前正在结算的伤害来源
extern thread_local void* t_damageAttacker;

// True while the in-game pause/menu panel is open (its own pause/unpause
// transitions are hooked so the overlay can hide behind it).
extern bool g_menuOpen;
extern bool g_panelOpen;

// Lifecycle / engine integration ------------------------------------------
void  ShieldLoadEffects(const std::wstring& jsonPath);

// Reads the custom `shield:` block from heroes/*/*.info.darkest (base, mods
// and workshop) so shield can be authored next to weapon/armour.
void  ShieldLoadClasses(const std::wstring& gameRoot);

// Reads buff definitions (shared/buffs/*.buffs.json) that carry custom
// shield_* members and applies them while the buff id is active on an actor.
void  ShieldLoadBuffs(const std::wstring& gameRoot);
void  ShieldOnStatsReset(void* actor);
void  ShieldOnBlockPopup(int type);
bool  ShieldActingNow();
void  ShieldTick();                        // once per frame
void  ShieldOnActorBar(void* actor, Vec2 pos, int rank, bool shared);
void  ShieldOnRound();
void  ShieldOnEffectApply(void* target, const void* effect, void* performer);

// 一次命中结算完毕，清掉该攻击者留下的穿透
void  ShieldEndHit(void* actor, void* attacker);

// buff 规则的 shielded 状态：护盾值大于 0 时返回 1，否则返回 0
int   ShieldStatus(void* actor);

enum class AbsorbKind { None, Full, Partial };

// Absorbs `damage` (the final, post-mitigation damage about to be applied).
// Full   -> whole hit absorbed; caller should treat the hit as blocked.
// Partial-> shield drained; `creditOut` is the part of the hit that must be
//           refunded to health so the net loss equals the absorbed amount.
AbsorbKind ShieldAbsorbFinal(void* actor, float damage, float& creditOut);

ShieldState* FindShield(void* actor);
void ShieldPrune(DWORD now);

bool  ShieldHasEffects();

} // namespace dds
