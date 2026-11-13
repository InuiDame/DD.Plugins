#include "Hooks.h"

#include "Rules.h"

namespace Hooks
{
	using namespace DKUtil::Alias;

	namespace
	{
		namespace Asm = dku::Hook::Assembly;

		// ===================== 游戏结构 =====================
		// 基准 exe: Darkest.exe  SHA1 9db2d73b89aa034e2055df65a139403e18df641f  (ImageBase 0x140000000)
		constexpr std::ptrdiff_t ActorBuffsOffset      = 0x460;   // Actor -> Vector<BuffEntry>
		constexpr std::ptrdiff_t BuffEntrySize         = 0x418;
		constexpr std::ptrdiff_t BuffInEntryOffset     = 0x8;     // BuffEntry -> Buff
		constexpr std::ptrdiff_t BuffIdHashOffset      = 0xA4;    // Buff -> id hash
		constexpr std::ptrdiff_t SkillTypeOffset       = 0x50;    // ActorCombatSkill -> .type (char[0x40])
		constexpr std::size_t   SkillTypeCapacity      = 0x40;
		constexpr std::ptrdiff_t InventoryBeginOffset  = 0x1288;  // Actor -> 背包首元素
		constexpr std::ptrdiff_t InventoryEndOffset    = 0x1290;  // Actor -> 背包尾后元素
		constexpr std::ptrdiff_t InventoryItemSize     = 0x5A8;
		constexpr std::ptrdiff_t ItemTrinketHashOffset = 0x88;    // Item -> trinket id hash

		template <class T>
		struct Vector
		{
			T* first;
			T* last;
			T* end;
		};

		// 4F3E50 的反汇编显示字符串只走 rdx、rcx 进函数后被立即清零，因此需要一个占位首参
		using EffectLookup_t = void* (*)(void*, const char*);
		// 239AA0: Vector<T*>::_Emplace_reallocate(vec, where, &value)
		using VectorPush_t = void* (*)(Vector<void*>*, void*, void* const*);
		// 479CD0: 某个 buff 在当前 actor / skill / 上下文下是否生效
		using BuffActive_t = bool (*)(void*, void*, void*, void*, void*);
		// 479470: 统一的技能附加效果收集函数
		//   rcx = Actor(执行者), rdx = Vector<Effect*>* (输出), r8 = ActorCombatSkill*,
		//   r9 = 上下文, [rsp+0x28] = crit 标志；返回 rdx
		using CollectEffects_t = Vector<void*>* (*)(void*, Vector<void*>*, void*, void*, bool);

		EffectLookup_t EffectLookup = nullptr;
		VectorPush_t   VectorPush = nullptr;
		BuffActive_t   BuffActive = nullptr;

		template <class T>
		T* Field(void* a_base, std::ptrdiff_t a_offset) noexcept
		{
			return reinterpret_cast<T*>(static_cast<std::byte*>(a_base) + a_offset);
		}

		std::string_view ReadSkillType(const void* a_skill) noexcept
		{
			const auto* text = reinterpret_cast<const char*>(
				static_cast<const std::byte*>(a_skill) + SkillTypeOffset);
			std::size_t len = 0;
			while (len < SkillTypeCapacity && text[len] != '\0') {
				++len;
			}
			return { text, len };
		}

		void Push(Vector<void*>* a_out, void* a_effect) noexcept
		{
			if (a_out->last == a_out->end) {
				// 借用游戏自己的扩容函数，保证分配/释放都在游戏 CRT 内完成
				VectorPush(a_out, a_out->last, std::addressof(a_effect));
			} else {
				*a_out->last = a_effect;
				++a_out->last;
			}
		}

		void AppendIds(Vector<void*>* a_out, const Rules::EffectIds& a_ids, std::string_view a_owner)
		{
			for (const auto& id : a_ids) {
				auto* effect = EffectLookup(nullptr, id.c_str());
				if (!effect) {
					WARN("Undefined additional effect \"{}\" referenced by {} with a custom .type", id, a_owner);
					continue;
				}
				Push(a_out, effect);
			}
		}

		void AppendCustomEffects(void* a_actor, Vector<void*>* a_out, void* a_skill, void* a_context)
		{
			if (Rules::Empty() || !a_actor || !a_out || !a_skill) {
				return;
			}

			const auto type = Rules::NormalizeType(ReadSkillType(a_skill));
			if (type.empty() || Rules::IsBlockedType(type) || !Rules::HasType(type)) {
				return;
			}

			// ---- buff ----
			const auto& buffs = *Field<Vector<std::byte>>(a_actor, ActorBuffsOffset);
			if (buffs.first && buffs.last && buffs.last > buffs.first) {
				for (auto* entry = buffs.first; entry < buffs.last; entry += BuffEntrySize) {
					auto*       buff = entry + BuffInEntryOffset;
					const auto  hash = *Field<const std::int32_t>(buff, BuffIdHashOffset);
					const auto* ids = Rules::FindBuff(hash, type);
					if (!ids) {
						continue;
					}
					// 与游戏原版触发器保持同一个 buff 生效判定
					if (!BuffActive(a_actor, buff, a_actor, a_context, a_skill)) {
						continue;
					}
					AppendIds(a_out, *ids, "a buff");
				}
			}

			// ---- trinket ----
			// 484580 用 GetItemFromSlotIndex(actor+0x1280, slot) 遍历背包；这里直接按同一布局枚举
			auto* items = *Field<std::byte*>(a_actor, InventoryBeginOffset);
			auto* itemsEnd = *Field<std::byte*>(a_actor, InventoryEndOffset);
			if (items && itemsEnd && itemsEnd > items) {
				for (auto* item = items; item < itemsEnd; item += InventoryItemSize) {
					if (*Field<const std::int32_t>(item, 0) <= 0) {  // 空槽
						continue;
					}
					const auto  hash = *Field<const std::int32_t>(item, ItemTrinketHashOffset);
					const auto* ids = Rules::FindTrinket(hash, type);
					if (ids) {
						AppendIds(a_out, *ids, "a trinket");
					}
				}
			}
		}

		// ===================== Hook =====================
		class CollectEffects
		{
		public:
			static Vector<void*>* Hook(
				void* a_actor, Vector<void*>* a_out, void* a_skill, void* a_context, bool a_crit)
			{
				// 先让原版收集 attack / melee / ranged / trinket 等全部效果，再把自定义类型的追加进去
				auto* result = Original(a_actor, a_out, a_skill, a_context, a_crit);
				AppendCustomEffects(a_actor, a_out, a_skill, a_context);
				return result;
			}

			static inline CollectEffects_t Original{ nullptr };
		};

		std::uintptr_t ToRva(const void* a_addr)
		{
			return AsAddress(a_addr) - dku::Hook::Module::get().base();
		}
	}

	// ===================== 初始化 =====================
	void Install()
	{
		// 四个函数入口，全部在基准 exe 上唯一命中
		auto* collect = Asm::search_pattern<
			"48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 41 57 "
			"48 8D 6C 24 E9 48 81 EC C8 00 00 00">();  // 479470

		auto* effectLookup = Asm::search_pattern<
			"48 83 EC 28 33 C9 4C 8B C2 38 0A 74 16">();  // 4F3E50

		auto* vectorPush = Asm::search_pattern<
			"48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 "
			"48 83 EC 50 4C 89 44 24 20 48 8B EA 48 8B D9 "
			"4C 8B 31 48 8B 41 08 49 2B C6 48 83 F8 F8">();  // 239AA0

		auto* buffPredicate = Asm::search_pattern<
			"40 55 53 56 57 41 54 41 55 41 56 41 57 "
			"48 8D 6C 24 E9 48 81 EC E8 00 00 00">();  // 479CD0

		// 结构偏移写死在指令里，游戏更新导致布局变化时拒绝安装，避免读坏内存
		const auto layoutMatches =
			Asm::search_pattern<"48 81 C7 18 04 00 00 48 3B FD">() != nullptr &&                     // BuffEntry 0x418
			Asm::search_pattern<"48 8D 8E 60 04 00 00 48 8D 44 24 60">() != nullptr &&               // Actor+0x460
			Asm::search_pattern<"48 89 B9 A4 00 00 00 66 89 B9 AC 00 00 00">() != nullptr &&         // Buff+0xA4
			Asm::search_pattern<"49 83 C6 50 4C 89 75 97">() != nullptr &&                           // Skill+0x50
			Asm::search_pattern<"41 B8 40 00 00 00 48 8D 15 ?? ?? ?? ?? "
			                    "48 8D 78 50 48 8B CF E8">() != nullptr &&                            // .type char[0x40]
			Asm::search_pattern<"83 B9 A4 12 00 00 00">() != nullptr &&                               // Actor+0x12A4
			Asm::search_pattern<"8B 90 88 00 00 00 41 39 56 40 74 11 "
			                    "49 81 C6 88 04 00 00">() != nullptr &&                               // Item+0x88 / 0x488
			Asm::search_pattern<"4D 8B 95 88 12 00 00 4D 8B 8D 90 12 00 00 "
			                    "4D 2B CA 49 C1 F9 03">() != nullptr;                                 // 背包 0x1288/0x1290

		if (!collect || !effectLookup || !vectorPush || !buffPredicate || !layoutMatches) {
			WARN("Incompatible game version, hook NOT installed "
				 "(Collect: {}, EffectLookup: {}, VectorPush: {}, BuffActive: {}, Layout: {})",
				collect != nullptr, effectLookup != nullptr, vectorPush != nullptr,
				buffPredicate != nullptr, layoutMatches);
			return;
		}

		EffectLookup = std::bit_cast<EffectLookup_t>(effectLookup);
		VectorPush = std::bit_cast<VectorPush_t>(vectorPush);
		BuffActive = std::bit_cast<BuffActive_t>(buffPredicate);

		dku::Hook::Trampoline::AllocTrampoline(1 << 10);

		// 前 5 字节正好是一条完整指令 (mov [rsp+10h], rdx)，无 rip 相对 / 无分支，可安全搬进 trampoline
		CollectEffects::Original = dku::Hook::write_detour<5>(AsAddress(collect), CollectEffects::Hook);

		INFO("CollectEffects detour installed at +{:X} "
			 "(EffectLookup +{:X}, VectorPush +{:X}, BuffActive +{:X})",
			ToRva(collect), ToRva(effectLookup), ToRva(vectorPush), ToRva(buffPredicate));
		INFO("Custom type triggers active: {} buff(s), {} trinket(s), {} type(s)",
			Rules::BuffCount(), Rules::TrinketCount(), Rules::TypeCount());
	}
}
