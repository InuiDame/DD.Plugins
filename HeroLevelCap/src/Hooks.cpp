#include "Hooks.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace HeroLevelCap::Hooks {
namespace {

using namespace DKUtil::Alias;

// 基准：_windows/win64/Darkest.exe SHA1 9db2d73b89aa034e2055df65a139403e18df641f
// 入口偷 6 字节：40 57          push r15
//                48 83 EC 30    sub rsp,30h
constexpr std::array<std::uint8_t, 17> kGetLevelPattern{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x80, 0xB9, 0x06,
    0x13, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x74,
};
constexpr std::array<std::uint8_t, 6> kStolenBytes{ 0x40, 0x57, 0x48, 0x83, 0xEC, 0x30 };

// Roster::System::OnRaidFinish 里的 XP 封顶：
//   3B C2           cmp eax,edx        (edx = 阈值表最后一项)
//   0F 42 D0        cmovb edx,eax
//   89 91 30 13 00 00                  mov [rcx+1330h],edx
constexpr std::array<std::uint8_t, 21> kClampPattern{
    0x49, 0x8B, 0x0E, 0x8B, 0x81, 0x30, 0x13, 0x00, 0x00, 0x3B, 0xC2,
    0x0F, 0x42, 0xD0, 0x89, 0x91, 0x30, 0x13, 0x00, 0x00, 0x49,
};
constexpr std::size_t kClampOffset = 9;  // 3B C2 在特征码里的位置
constexpr std::array<std::uint8_t, 5> kExpectedClamp{ 0x3B, 0xC2, 0x0F, 0x42, 0xD0 };
// mov edx,eax + 3 NOP：XP 不再被 48 截断，等级由本插件按职业上限计算。
constexpr std::array<std::uint8_t, 5> kClampPatch{ 0x8B, 0xD0, 0x90, 0x90, 0x90 };

// UI::CampaignAssets::Load（7BB9B0）里等级图标加载循环：
//   FF C3           inc ebx
//   83 FB 07        cmp ebx,7        <- 只加载 lvl0..lvl6
//   0F 82 ...       jb  循环开头
// 把边界改成 kIconBoundValue，才会去加载 lvl7 及以上的 PNG。
constexpr std::array<std::uint8_t, 7> kIconLoopPattern{ 0xFF, 0xC3, 0x83, 0xFB, 0x07, 0x0F, 0x82 };
constexpr std::size_t kIconBoundOffset = 4;  // cmp 的 imm8
constexpr std::uint8_t kIconBoundValue = static_cast<std::uint8_t>(kMaxCap + 1);

// 名册卡片底图加载循环（62DB90 内，紧跟在 res%d 拼名之后）：
//   FF C3           inc ebx
//   83 FB 07        cmp ebx,7        <- 只加载 rosterelement_res1..6
//   7C B0           jl  循环开头
// 这个容器是按 level-1 下标取用的（6AFCE4: lea edx,[rbx-1]; shl rdx,4; add rdx,[容器]），
// 只有 6 项时 7 级会读到容器末尾之外，所以必须一起放宽。
constexpr std::array<std::uint8_t, 9> kRosterLoopPattern{
    0xFF, 0xC3, 0x83, 0xFB, 0x07, 0x7C, 0xB0, 0x48, 0x8D,
};
constexpr std::size_t kRosterBoundOffset = 4;  // cmp 的 imm8

// OnRaidFinish 里的等级变化判断：
//   3B FE           cmp edi,esi     (edi=加xp前等级, esi=加xp后等级)
//   0F 84 A0 01 00 00              je 58792C  (相等就跳过技能刷新/等级条更新)
// 原版靠"整数等级变化"触发升级刷新。装了本插件后，英雄在达到上限后 XP 继续增长，
// GetResolveLevel 两次都返回同一上限值（例如 7），游戏会认为等级没变而跳过刷新，
// 导致等级条和技能永远停在旧状态。把这些 je 改成 NOP，让升级刷新每次都执行；
// RefreshSkills(5D4950) 和 HeroLogItem 写入都是幂等的，重复执行无副作用。
constexpr std::array<std::uint8_t, 10> kLevelChangePattern{
    0x3B, 0xFE, 0x0F, 0x84, 0xA0, 0x01, 0x00, 0x00, 0x49, 0x8B,
};
constexpr std::size_t kLevelChangeJumpOffset = 2;  // 0F 84 在特征码里的位置
constexpr std::array<std::uint8_t, 6> kLevelChangePatch{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };

// 运行时地址（RVA）
constexpr std::uintptr_t kRvaRosterVariables = 0x118E0B8;  // 指针 -> vector<uint32>（阈值表）
constexpr std::uintptr_t kRvaHeroVtable = 0xE8A288;        // .?AVHero@@
constexpr std::uintptr_t kHeroVtableSlotGetLevel = 0x168;  // Actor::GetResolveLevel

// 结构偏移
constexpr std::ptrdiff_t kOffActorXp = 0x1328;
constexpr std::ptrdiff_t kOffActorNoLevel = 0x1306;
constexpr std::ptrdiff_t kOffActorClass = 0x1320;
constexpr std::ptrdiff_t kOffClassId = 0x8;  // ActorClass+8 是内嵌 char[0x40]
// XP 是 uint32，但游戏用 int32 做加法；超过这个量级直接按上限处理，避免溢出回绕。
constexpr std::uint32_t kXpSanityLimit = 1'000'000u;

using GetLevelFn = float(__cdecl*)(void* self);

GetLevelFn g_original = nullptr;
CapMap g_caps;
std::vector<std::uint32_t> g_thresholds;
std::atomic<std::uint64_t> g_hits{ 0 };
std::atomic<std::uint64_t> g_clamped{ 0 };
std::atomic<std::uint64_t> g_aboveVanilla{ 0 };
std::atomic<std::uint64_t> g_xpLimited{ 0 };

// DllMain 时游戏还没加载 roster.variables.json，阈值表要等第一次调用时再取。
std::once_flag g_thresholdOnce;
std::uintptr_t g_moduleBase = 0;
std::uintptr_t g_levelSite = 0;  // Actor::GetResolveLevel 绝对地址，用于确认对象类型
int g_maxCap = kDefaultCap;

std::uint8_t* FindUnique(const std::uint8_t* text, std::size_t size,
    const std::uint8_t* pattern, std::size_t length, const char* tag)
{
    if (!text || length == 0 || length > size) return nullptr;
    const std::uint8_t* end = text + size - length;
    std::size_t count = 0;
    std::uint8_t* first = nullptr;
    for (const std::uint8_t* p = text; p <= end; ++p) {
        if (std::memcmp(p, pattern, length) != 0) continue;
        ++count;
        if (count == 1) first = const_cast<std::uint8_t*>(p);
        if (count > 1) break;
    }
    if (count == 0) {
        WARN("{}: pattern not found; NOT patching", tag);
        return nullptr;
    }
    if (count > 1) {
        WARN("{}: ambiguous ({} hits); NOT patching", tag, count);
        return nullptr;
    }
    return first;
}

// SEH 辅助：这些函数里只允许 POD，不能出现需要展开的 C++ 对象（C2712）。
bool TryReadThresholdRange(std::uintptr_t base, const std::uint32_t*& begin, std::size_t& count)
{
    begin = nullptr;
    count = 0;
    __try {
        auto** holder = reinterpret_cast<std::uint8_t**>(base + kRvaRosterVariables);
        std::uint8_t* const vectorObject = *holder;
        if (!vectorObject) return false;
        const auto* first = *reinterpret_cast<const std::uint8_t**>(vectorObject);
        const auto* last = *reinterpret_cast<const std::uint8_t**>(vectorObject + 8);
        if (!first || !last || last < first) return false;
        const std::uintptr_t bytes = static_cast<std::uintptr_t>(last - first);
        begin = reinterpret_cast<const std::uint32_t*>(first);
        count = static_cast<std::size_t>(bytes / 4);
        return count > 0 && count <= 64;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        begin = nullptr;
        count = 0;
        return false;
    }
}

void EnsureLiveThresholds()
{
    std::call_once(g_thresholdOnce, [] {
        const std::uint32_t* begin = nullptr;
        std::size_t count = 0;
        if (g_moduleBase && TryReadThresholdRange(g_moduleBase, begin, count)) {
            g_thresholds = BuildThresholds(begin, count, g_maxCap);
            INFO("live thresholds loaded: {} entries, last={}", count, g_thresholds.back());
        }
    });
}

bool TryReadVtableSlot(std::uintptr_t base, std::uintptr_t& slot)
{
    slot = 0;
    __try {
        const auto* vt = reinterpret_cast<const std::uint8_t*>(base + kRvaHeroVtable);
        slot = *reinterpret_cast<const std::uintptr_t*>(vt + kHeroVtableSlotGetLevel);
        return slot != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = 0;
        return false;
    }
}

bool TryReadActor(const void* self, bool& special, char id[64], bool& haveId, std::uint32_t& xp)
{
    special = false;
    haveId = false;
    xp = 0;
    __try {
        const auto* actor = static_cast<const std::uint8_t*>(self);
        special = actor[kOffActorNoLevel] != 0;
        if (special) return true;
        const void* const* cls = reinterpret_cast<const void* const*>(actor + kOffActorClass);
        haveId = *cls != nullptr && ReadInlineId(static_cast<const char*>(*cls) + kOffClassId, id);
        xp = *reinterpret_cast<const std::uint32_t*>(actor + kOffActorXp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 调用者探针：记录前若干个唯一返回地址（转成 RVA），用来确认游戏里到底是
// 谁在调用 GetResolveLevel（UI 显示 / 技能解锁 / 仅 OnRaidFinish）。
constexpr std::size_t kMaxCallers = 48;
std::uintptr_t g_callers[kMaxCallers]{};
std::atomic<int> g_callerCount{ 0 };

void RecordCaller()
{
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    std::uintptr_t rva = ret - g_moduleBase;
    // 只记录落在 .text 里的地址，过滤掉噪声。
    if (!g_moduleBase || rva < 0x1000 || rva > 0xc71000) return;
    const int n = g_callerCount.load(std::memory_order_relaxed);
    if (n >= kMaxCallers) return;
    for (int i = 0; i < n; ++i) {
        if (g_callers[i] == rva) return;  // 已记录过
    }
    g_callers[n] = rva;
    g_callerCount.fetch_add(1, std::memory_order_relaxed);
    INFO("caller of GetResolveLevel: +{:X}", rva);
}

bool WriteActorXp(void* self, std::uint32_t xp)
{
    __try {
        auto* actor = static_cast<std::uint8_t*>(self);
        *reinterpret_cast<std::uint32_t*>(actor + kOffActorXp) = xp;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 确认 self 真的是实现了 GetResolveLevel 的角色对象：虚表 +0x168 槽必须指向同一个函数。
// GetResolveLevel 会被通用 thunk（4849C0: mov rax,[rcx]; call [rax+0x168]）调用，
// this 不保证是 Hero；只有这个槽对得上，才能在 +0x1328 上安全写 4 字节。
bool IsActorObject(const void* self)
{
    __try {
        const auto* vt = *reinterpret_cast<const std::uint8_t* const*>(self);
        if (!vt || !g_levelSite) return false;
        const auto slot = *reinterpret_cast<const std::uintptr_t*>(vt + kHeroVtableSlotGetLevel);
        return slot == g_levelSite;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float GetLevelDetour(void* self)
{
    const std::uint64_t hit = ++g_hits;
    RecordCaller();
    if (!self || !g_original) return 0.0f;

    bool special = false;
    bool haveId = false;
    std::uint32_t xp = 0;
    char id[64] = {};
    if (!TryReadActor(self, special, id, haveId, xp) || special) {
        return g_original(self);
    }

    EnsureLiveThresholds();
    const int cap = haveId ? CapForClass(g_caps, id) : kDefaultCap;

    // 满级后 XP 不再增长（与原版满级行为一致）。
    // 原版由 OnRaidFinish 的 cmovb 把 XP 截断到"阈值表最后一项"，而本插件要按职业上限截断，
    // 只靠改表做不到（表是全局的），所以在这里把超额 XP 回写为该职业上限对应的阈值。
    // 写之前必须确认对象真的是角色对象：GetResolveLevel 会被通用 thunk（4849C0）调用，
    // this 不保证是 Hero，往非角色对象 +0x1328 写 4 字节会破坏堆（上次闪退的教训之一）。
    // 校验方式：对象虚表 + 0x168 槽必须指向同一个 GetResolveLevel。
    const std::size_t capIndex = static_cast<std::size_t>(std::clamp(cap, kMinCap, kMaxCap));
    if (capIndex < g_thresholds.size() && xp > g_thresholds[capIndex]
        && xp <= kXpSanityLimit && IsActorObject(self)) {
        const std::uint32_t limited = g_thresholds[capIndex];
        if (WriteActorXp(self, limited)) {
            if (++g_xpLimited <= 16) {
                INFO("xp capped at class limit: class={} {} -> {} (cap={})",
                    haveId ? id : "?", xp, limited, cap);
            }
            xp = limited;
        }
    }

    // 极端 XP 不参与插值，直接按上限返回，避免减法/索引在异常存档下回绕。
    const float level = xp > kXpSanityLimit
        ? static_cast<float>(cap)
        : ComputeLevel(xp, g_thresholds, cap);
    const int intLevel = static_cast<int>(level);

    // 原版 XP 上限是 48；超过它就说明 XP 截断补丁确实生效。前 32 条完整记录，便于排错。
    if (xp > kVanillaThresholds[std::size(kVanillaThresholds) - 1]) {
        if (++g_aboveVanilla <= 32) {
            INFO("XP above vanilla: class={} xp={} cap={} level={:.3f} intLevel={}",
                haveId ? id : "?", xp, cap, level, intLevel);
        }
    }
    if (level >= static_cast<float>(cap)) {
        if (++g_clamped <= 16) {
            INFO("cap hit: class={} xp={} cap={} level={:.2f}", haveId ? id : "?", xp, cap, level);
        }
    } else if (hit <= 8) {
        INFO("resolve level: class={} xp={} cap={} level={:.2f}", haveId ? id : "?", xp, cap, level);
    }
    return level;
}

}  // namespace

bool Install(const CapMap& caps)
{
    int maxCap = kDefaultCap;
    for (const auto& [id, cap] : caps) {
        if (cap > maxCap) maxCap = cap;
    }
    if (maxCap <= kDefaultCap) {
        INFO("no level_cap above {} in hero info files; game left untouched", kDefaultCap);
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    if (!base) {
        WARN("cannot get module base; NOT patching");
        return false;
    }

    dku::Hook::Module module{ base };
    const auto [text, textSize] = module.section(dku::Hook::Module::Section::textx);
    const auto* textPtr = reinterpret_cast<const std::uint8_t*>(text);
    auto* const levelSite = FindUnique(textPtr, textSize,
        kGetLevelPattern.data(), kGetLevelPattern.size(), "GetResolveLevel");
    auto* const clampSite = FindUnique(textPtr, textSize,
        kClampPattern.data(), kClampPattern.size(), "RaidFinish XpClamp");
    auto* const iconSite = FindUnique(textPtr, textSize,
        kIconLoopPattern.data(), kIconLoopPattern.size(), "ResolveLevelIconLoop");
    auto* const changeSite = FindUnique(textPtr, textSize,
        kLevelChangePattern.data(), kLevelChangePattern.size(), "LevelChangeCheck");
    auto* const rosterSite = FindUnique(textPtr, textSize,
        kRosterLoopPattern.data(), kRosterLoopPattern.size(), "RosterElementResLoop");
    if (!levelSite || !clampSite || !iconSite || !changeSite || !rosterSite) {
        WARN("Incompatible game version, NOT patching");
        return false;
    }

    // 全有或全无：所有校验先过，再写任何字节。
    const auto levelAddr = reinterpret_cast<std::uintptr_t>(levelSite);
    const auto clampAddr = reinterpret_cast<std::uintptr_t>(clampSite);
    const auto levelRva = levelAddr - base;
    const auto clampRva = clampAddr - base;
    bool verified = std::memcmp(levelSite, kStolenBytes.data(), kStolenBytes.size()) == 0;
    if (!verified) WARN("V1 stolen prologue mismatch at +{:X}", levelRva);
    verified = verified && std::memcmp(clampSite + kClampOffset,
        kExpectedClamp.data(), kExpectedClamp.size()) == 0;
    if (!verified) WARN("V2 XP clamp bytes mismatch at +{:X}", clampRva);

    // 同一性：英雄虚表槽位必须指向我们找到的函数。
    std::uintptr_t slot = 0;
    const bool vtableOk = TryReadVtableSlot(base, slot) && slot == levelAddr;
    if (!vtableOk) WARN("V3 Hero vtable slot 0x168 != GetResolveLevel (+{:X} vs +{:X})",
        slot ? slot - base : 0, levelRva);

    const std::uint32_t* liveBegin = nullptr;
    std::size_t liveCount = 0;
    const bool thresholdsOk = TryReadThresholdRange(base, liveBegin, liveCount);
    if (!thresholdsOk) WARN("V4 cannot read live resolve_level_thresholds; using vanilla table");

    const bool changeJumpOk = std::memcmp(changeSite + kLevelChangeJumpOffset,
        kLevelChangePattern.data() + kLevelChangeJumpOffset, kLevelChangePatch.size()) == 0;
    if (!changeJumpOk) WARN("V7 level-change jump bytes mismatch; NOT patching");

    if (!verified || !vtableOk || !changeJumpOk) {
        WARN("verification failed; NOT patching");
        return false;
    }

    g_caps = caps;
    g_moduleBase = base;
    g_levelSite = levelAddr;
    g_maxCap = maxCap;
    g_thresholds = BuildThresholds(liveBegin, liveCount, maxCap);

    g_original = dku::Hook::write_detour<6>(levelAddr, &GetLevelDetour);
    if (!g_original) {
        WARN("write_detour failed; NOT patching XP clamp");
        return false;
    }

    dku::Hook::WriteData(clampSite + kClampOffset, kClampPatch.data(), kClampPatch.size());
    const std::uint8_t iconBound = kIconBoundValue;
    dku::Hook::WriteData(iconSite + kIconBoundOffset, &iconBound, 1);
    dku::Hook::WriteData(rosterSite + kRosterBoundOffset, &iconBound, 1);
    dku::Hook::WriteData(changeSite + kLevelChangeJumpOffset,
        kLevelChangePatch.data(), kLevelChangePatch.size());
    ::FlushInstructionCache(::GetCurrentProcess(), clampSite + kClampOffset, kClampPatch.size());
    ::FlushInstructionCache(::GetCurrentProcess(), iconSite + kIconBoundOffset, 1);
    ::FlushInstructionCache(::GetCurrentProcess(), rosterSite + kRosterBoundOffset, 1);
    ::FlushInstructionCache(::GetCurrentProcess(), changeSite + kLevelChangeJumpOffset,
        kLevelChangePatch.size());
    const bool clampVerified = std::memcmp(clampSite + kClampOffset,
        kClampPatch.data(), kClampPatch.size()) == 0;
    const bool iconVerified = iconSite[kIconBoundOffset] == iconBound;
    const bool rosterVerified = rosterSite[kRosterBoundOffset] == iconBound;
    const bool changeVerified = std::memcmp(changeSite + kLevelChangeJumpOffset,
        kLevelChangePatch.data(), kLevelChangePatch.size()) == 0;
    if (!clampVerified) WARN("V5 XP clamp writeback mismatch; level cap may stay at 6");
    if (!iconVerified) WARN("V6 icon loop writeback mismatch; level 7+ icons may not load");
    if (!rosterVerified) WARN("V9 roster res loop writeback mismatch; level 7+ 名册底图可能越界");
    if (!changeVerified) WARN("V8 level-change jump writeback mismatch; skill/UI refresh may be skipped");

    INFO("installed: GetResolveLevel +{:X}, XP clamp +{:X} (writeback={}), icon loop +{:X} (bound={}, writeback={}), roster res loop +{:X} (writeback={}), level-change +{:X} (writeback={}), maxCap={}, thresholds={}",
        levelRva, clampRva, clampVerified, iconSite - textPtr + (text - base), iconBound,
        iconVerified, rosterSite - textPtr + (text - base), rosterVerified,
        changeSite - textPtr + (text - base) + kLevelChangeJumpOffset,
        changeVerified, maxCap, g_thresholds.size());
    return true;
}

}  // namespace HeroLevelCap::Hooks
