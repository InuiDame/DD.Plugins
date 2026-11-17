#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

// 单测与实现共用：json 只在 .cpp 里解引用，这里仅前向声明避免把大头文件带进每个 TU。
// nlohmann::json 是 basic_json<> 的别名，不能前向声明 class，故直接包含。
#include <nlohmann/json.hpp>

namespace Rules
{
	// ---- 三个互相独立的免疫位 ----
	enum : std::uint8_t
	{
		IMM_KILL     = 1 << 0,
		IMM_CONTROL  = 1 << 1,
		IMM_CAPTURE  = 1 << 2,
	};

	// ---- Effect 结构偏移（基准 exe SHA1 9db2d73b89aa034e2055df65a139403e18df641f）----
	// 由 effect 解析器 4F02D0 与消费者 4C5050 双向核对（见 design.md §1.3）。
	constexpr std::ptrdiff_t OFF_CONTROL          = 0x070;   // .control
	constexpr std::ptrdiff_t OFF_UNCONTROL        = 0x074;   // .uncontrol —— 只读/透传，永不被写
	constexpr std::ptrdiff_t OFF_KILL             = 0x1F0;   // .kill
	constexpr std::ptrdiff_t OFF_KILL_TYPES       = 0x1F8;   // .kill_enemy_types first
	constexpr std::ptrdiff_t OFF_KILL_TYPES_LAST  = 0x200;   // .kill_enemy_types last（清零目标：last=first）
	constexpr std::ptrdiff_t OFF_KILL_TYPES_END   = 0x208;   // .kill_enemy_types end（永远不动）
	constexpr std::ptrdiff_t OFF_CAPTURE          = 0x320;   // .capture
	constexpr std::ptrdiff_t OFF_CAPTURE_RMP      = 0x324;   // .capture_remove_from_party
	constexpr std::ptrdiff_t OFF_SWAP             = 0x3E0;   // .swap_source_and_target (bool)
	constexpr std::size_t    kEffectSize          = 0x618;

	// 仅供 offsetof 静态断言与单测哨兵填充；不在运行时实例化为拥有型对象。
	// 副本清零只按字节写这些字段，不调用任何构造/析构函数。
	struct EffectMirror
	{
		std::byte     _pad0[OFF_CONTROL];                              // 0x000
		std::int32_t  control;                                        // 0x070
		std::int32_t  uncontrol;                                      // 0x074
		std::byte     _pad1[OFF_KILL - (OFF_UNCONTROL + 4)];          // 0x078
		std::int32_t  kill;                                           // 0x1F0
		std::byte     _pad2[OFF_KILL_TYPES - (OFF_KILL + 4)];         // 0x1F4
		void*         killFirst;                                      // 0x1F8
		void*         killLast;                                       // 0x200
		void*         killEnd;                                        // 0x208
		std::byte     _pad3[OFF_CAPTURE - (OFF_KILL_TYPES_END + 8)];  // 0x210
		std::int32_t  capture;                                        // 0x320
		std::int32_t  captureRemoveFromParty;                         // 0x324
		std::byte     _pad4[OFF_SWAP - (OFF_CAPTURE_RMP + 4)];        // 0x328
		bool          swap;                                           // 0x3E0
		std::byte     _pad5[kEffectSize - (OFF_SWAP + 1)];            // 0x3E1
	};

	static_assert(sizeof(EffectMirror) == kEffectSize, "Effect layout drifted");
	static_assert(offsetof(EffectMirror, control) == OFF_CONTROL);
	static_assert(offsetof(EffectMirror, uncontrol) == OFF_UNCONTROL);
	static_assert(offsetof(EffectMirror, kill) == OFF_KILL);
	static_assert(offsetof(EffectMirror, killFirst) == OFF_KILL_TYPES);
	static_assert(offsetof(EffectMirror, killLast) == OFF_KILL_TYPES_LAST);
	static_assert(offsetof(EffectMirror, killEnd) == OFF_KILL_TYPES_END);
	static_assert(offsetof(EffectMirror, capture) == OFF_CAPTURE);
	static_assert(offsetof(EffectMirror, captureRemoveFromParty) == OFF_CAPTURE_RMP);
	static_assert(offsetof(EffectMirror, swap) == OFF_SWAP);

	// ---- 纯逻辑（单测直接调用）----

	// 游戏字符串哈希：h = h * 53 + byte，uint32 截断为 int32。
	[[nodiscard]] std::int32_t Hash(std::string_view a_text) noexcept;

	// 两级 mask 只是按位或。
	[[nodiscard]] constexpr std::uint8_t CombineMask(
		std::uint8_t a_classMask, std::uint8_t a_buffMask) noexcept
	{
		return static_cast<std::uint8_t>(a_classMask | a_buffMask);
	}

	// 单行 info 原始文本解析（照 DDShield 的 ParseInfoShield 逐行模式）：
	// 行首跳过空白后首个 token 为大小写不敏感的 "immune:" 即真实块（**不需要 //**）；
	// 另兼容旧写法 "// ... DD_IMMUNE:" / "// ... immune:"。
	// setMask=本行显式置位的模块，clearMask=本行显式清零的模块（.kill false / .capture 0）。
	// matched=false 表示本行不是 immune 块/标签。
	struct InfoLineEffect
	{
		std::uint8_t setMask{ 0 };
		std::uint8_t clearMask{ 0 };
		bool         matched{ false };
	};

	[[nodiscard]] InfoLineEffect ParseInfoLine(std::string_view a_line) noexcept;

	// 整段文本按行顺序解析：先 OR 置位、再清除显式 false（后者可覆盖前者的置位）。
	[[nodiscard]] std::uint8_t ParseInfoText(std::string_view a_text) noexcept;

	// buff JSON 条目解析：只认布尔值 true 的 immune_kill/immune_control/immune_capture。
	// 非布尔写 WARN 并忽略；未知字段不影响。
	[[nodiscard]] std::uint8_t ParseBuffFields(const nlohmann::json& a_entry) noexcept;

	// 条目是否显式写过任一 immune_* 字段（用于 high-priority 显式 false 覆盖 low-priority）。
	[[nodiscard]] bool HasImmuneFields(const nlohmann::json& a_entry) noexcept;

	// ---- 目标解析与副本清零（单测直接调用）----

	// swap==0 → 真 target 是第 3 参 (targetArg)；swap!=0 → 真 target 是第 2 参 (performer)。
	// 复刻 decomp_4C5050.txt:343-351。
	[[nodiscard]] const void* ResolveTarget(
		const void* a_performer, const void* a_targetArg, const std::byte* a_effect) noexcept;

	// 在 kEffectSize 栈副本上按 mask 清零对应字段。
	// KILL     : kill=0 且 kill_enemy_types.last=first（end 不动）
	// CONTROL  : 只写 +0x70；+0x74 .uncontrol 原样保留（硬约束）
	// CAPTURE  : +0x320 与 +0x324 都清零
	// mask==0  : 什么都不做（调用方应直接转发原指针）
	void ApplyImmunity(std::byte* a_copy, std::uint8_t a_mask) noexcept;

	// ---- 转发决策（单测直接调用；保证 g_original 只调用一次）----
	// Detour 的 __try/__except 只包插件自身逻辑，先产出本结构，再在受保护块之外
	// 统一调用一次原函数。异常时 a_failed=true，强制回退原指针（fail-open）。
	struct ForwardDecision
	{
		const void* effect{ nullptr };  // 最终传给 g_original 的 Effect*
		std::uint8_t mask{ 0 };         // 实际施加的 mask（0 = 原样转发）
		bool         usedCopy{ false };
		bool         failed{ false };   // 插件逻辑是否异常
	};

	[[nodiscard]] ForwardDecision DecideForwardEffect(
		const void* a_originalEffect, const void* a_copy,
		std::uint8_t a_mask, bool a_failed) noexcept;

	// ---- 从 ActorClass/MonsterClass 对象读职业 id（单测直接调用）----
	// a_classObj+8（HeroClass）/ +0x48（部分 MonsterClass）是**内嵌 char[0x40] 字符串本身**，
	// 不是 char* 指针。2026-11-17 实机 bug：旧实现二次解引用 *(char**)(cls+8) 导致
	// 每次 class id read failed。本函数按"直接读内嵌数组"实现，单测用假对象验证。
	// 读取在 SEH 内，失败清 a_out[0] 并返回 false（fail-open）。
	[[nodiscard]] bool ReadEmbeddedClassId(
		const void* a_classObj, std::ptrdiff_t a_offset,
		char* a_out, std::size_t a_cap) noexcept;

	// ---- class 级双 id 合并（单测直接调用）----
	// 哈希→mask 查询回调（找不到返回 0）。ClassMask 用查询 ClassRules 的适配器，
	// 单测用假表验证 +8 与 +0x48 两个 id 串同时命中时取 OR。
	using ClassMaskLookup = std::uint8_t (*)(std::int32_t a_hash, const void* a_context) noexcept;

	[[nodiscard]] std::uint8_t ClassMaskFromIds(
		std::initializer_list<std::string_view> a_ids,
		ClassMaskLookup a_lookup, const void* a_context) noexcept;

	// ---- 可选 rule-gated 模式的前置判定（纯谓词，v1 未接入 detour，见 README）----
	// rule_type==0(always) → true；==55(解析失败) → false；非主线程 → false；
	// 需要 Raid 的类型在无 Raid 时 → false。
	[[nodiscard]] bool RuleGatePrecheck(
		std::int32_t a_ruleType, bool a_isMainThread, bool a_raidPresent) noexcept;

	// ---- 数据装载与运行时查询 ----

	// 扫描 *.buffs.json 与 heroes/**/*.info.darkest、monsters/**/*.info.darkest 建表。
	void Load();

	[[nodiscard]] bool Empty() noexcept;
	[[nodiscard]] std::size_t BuffCount() noexcept;
	[[nodiscard]] std::size_t ClassHashCount() noexcept;

	// 运行时查表：class 级（虚表槽6 → ActorClass* → +8 与 +0x48 两个 id 串 → 各三哈希变体）
	// 与 buff 级（Actor+0x460 组，步长 0x418，Instance+0xA4）。
	// 所有指针读取用 SEH 包住，失败 fail-open 返回 0。
	[[nodiscard]] std::uint8_t ClassMask(const void* a_actor) noexcept;
	[[nodiscard]] std::uint8_t BuffMask(const void* a_actor) noexcept;
	[[nodiscard]] std::uint8_t Mask(const void* a_actor) noexcept;
}
