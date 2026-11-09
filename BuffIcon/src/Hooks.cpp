#include "Hooks.h"

#include "BuffIcons.h"

namespace Hooks
{
	using namespace DKUtil::Alias;

	namespace
	{
		namespace Asm = dku::Hook::Assembly;

		// ===================== 游戏结构 =====================
		constexpr std::ptrdiff_t OverlayIconsOffset = 0x1B0;  // Overlay -> std::vector<TrayIcon>，即 quirk 图标表
		constexpr std::ptrdiff_t ActorTrayStatesOffset = 0x78;
		constexpr std::ptrdiff_t ActorBuffsOffset = 0x460;  // Actor -> std::vector<BuffEntry>
		constexpr std::ptrdiff_t BuffEntrySize = 0x418;
		constexpr std::ptrdiff_t BuffInEntryOffset = 0x8;  // BuffEntry -> Buff
		constexpr std::ptrdiff_t BuffStatOffset = 0x0;     // Buff -> stat_type 枚举
		constexpr std::ptrdiff_t BuffAmountOffset = 0x48;  // Buff -> amount
		constexpr std::ptrdiff_t BuffIdHashOffset = 0xA4;  // Buff -> id 的哈希

		// tray 图标类型，类型 0 是唯一拥有多个子槽位的类型
		constexpr std::int32_t TrayQuirk = 0;
		constexpr std::int32_t TrayFirstDot = 6;
		constexpr std::int32_t TrayBuff = 13;
		constexpr std::int32_t TrayDebuff = 14;
		constexpr std::int32_t NoGroup = -1;

		// 类型 6~11（毒、流血、灼烧、压力、位移、愈合）对应的 stat 枚举，游戏直接扫描 buff 表判定
		constexpr std::array<std::int32_t, 6> DotStats{ 0x04, 0x05, 0x58, 0x22, 0x23, 0x24 };

		// 其余由 buff 查询结果决定是否显示的分组
		constexpr std::array<std::int32_t, 7> QueriedGroups{ 1, 2, 3, 16, 18, 20, 21 };

		struct ImageHandle
		{
			std::int32_t  index{ 0 };
			std::int32_t  generation{ -1 };
			std::uint64_t data{ 0 };
		};
		static_assert(sizeof(ImageHandle) == 0x10);

		struct TrayIcon
		{
			std::int32_t key{ 0 };
			ImageHandle  image{};
		};
		static_assert(sizeof(TrayIcon) == 0x18);

		template <class T>
		struct Vector
		{
			T* first;
			T* last;
			T* end;
		};

		// std::function 谓词的捕获对象，游戏只读取 +8 处的极性
		struct GroupPredicate
		{
			void* vftable{ nullptr };
			bool  positive{ false };
		};

		using CreateImage_t = void* (*)(ImageHandle*, const char*, std::int32_t);
		using ReallocIcons_t = TrayIcon* (*)(Vector<TrayIcon>*, TrayIcon*, const TrayIcon*);
		using IsListedInGroup_t = bool (*)(const GroupPredicate*, const std::byte*);

		CreateImage_t     CreateImage = nullptr;
		ReallocIcons_t    ReallocIcons = nullptr;
		IsListedInGroup_t IsListedInGroup = nullptr;

		// ===================== 全局变量与定义 =====================
		enum class Filter
		{
			None,
			HideCustom,  // 从原版分组中去掉拥有独立图标的 buff
			OnlyIcon     // 只保留使用指定图标的 buff
		};

		thread_local Filter       ActiveFilter = Filter::None;
		thread_local std::int32_t ActiveKey = 0;

		class ScopedFilter
		{
		public:
			explicit ScopedFilter(Filter a_filter, std::int32_t a_key = 0) noexcept :
				_filter(ActiveFilter),
				_key(ActiveKey)
			{
				ActiveFilter = a_filter;
				ActiveKey = a_key;
			}

			~ScopedFilter() noexcept
			{
				ActiveFilter = _filter;
				ActiveKey = _key;
			}

			ScopedFilter(const ScopedFilter&) = delete;
			ScopedFilter& operator=(const ScopedFilter&) = delete;

		private:
			Filter       _filter;
			std::int32_t _key;
		};

		Vector<TrayIcon>& IconsOf(void* a_overlay) noexcept
		{
			return *dku::Hook::adjust_pointer<Vector<TrayIcon>>(a_overlay, OverlayIconsOffset);
		}

		std::int32_t IdHashOf(const std::byte* a_buff) noexcept
		{
			return *dku::Hook::adjust_pointer<const std::int32_t>(a_buff, BuffIdHashOffset);
		}

		const BuffIcons::Icon* FindSlot(void* a_overlay, std::int32_t a_sub) noexcept
		{
			const auto& icons = IconsOf(a_overlay);
			if (a_sub < 0 || a_sub >= icons.last - icons.first) {
				return nullptr;
			}

			return BuffIcons::FindByKey(icons.first[a_sub].key);
		}

		// 返回该 buff 在原版 tray 中所属的分组，不在普通 buff/debuff 分组时返回 NoGroup
		std::int32_t GroupOf(const std::byte* a_entry) noexcept
		{
			static constexpr GroupPredicate positive{ nullptr, true };
			static constexpr GroupPredicate negative{ nullptr, false };

			if (IsListedInGroup(std::addressof(positive), a_entry)) {
				return TrayBuff;
			}

			if (IsListedInGroup(std::addressof(negative), a_entry)) {
				return TrayDebuff;
			}

			return NoGroup;
		}

		bool IsDotType(std::int32_t a_type) noexcept
		{
			return a_type >= TrayFirstDot && a_type < TrayFirstDot + static_cast<std::int32_t>(DotStats.size());
		}

		// 能被 buff_icon 接管的分组，其他分组（痴狂、标记等）不依赖 buff 列表决定显示
		bool IsReplaceableGroup(std::int32_t a_type) noexcept
		{
			return IsDotType(a_type) || a_type == TrayBuff || a_type == TrayDebuff ||
			       std::ranges::find(QueriedGroups, a_type) != QueriedGroups.end();
		}

		// 与游戏的判定一致：stat 匹配且 amount 大于 0
		std::int32_t DotGroupOf(const std::byte* a_entry) noexcept
		{
			const auto* buff = a_entry + BuffInEntryOffset;
			if (*dku::Hook::adjust_pointer<const float>(buff, BuffAmountOffset) <= 0.0f) {
				return NoGroup;
			}

			const auto stat = *dku::Hook::adjust_pointer<const std::int32_t>(buff, BuffStatOffset);
			const auto it = std::ranges::find(DotStats, stat);
			return it != DotStats.end() ? TrayFirstDot + static_cast<std::int32_t>(it - DotStats.begin()) : NoGroup;
		}

		bool HasUnreplacedDot(std::byte* a_actor, std::int32_t a_type) noexcept
		{
			const auto& buffs = *dku::Hook::adjust_pointer<Vector<std::byte>>(a_actor, ActorBuffsOffset);
			for (auto* entry = buffs.first; entry != buffs.last; entry += BuffEntrySize) {
				if (DotGroupOf(entry) == a_type && !BuffIcons::FindByBuff(IdHashOf(entry + BuffInEntryOffset))) {
					return true;
				}
			}

			return false;
		}

		bool IsFilteredOut(const std::byte* a_buff) noexcept
		{
			const auto* icon = BuffIcons::FindByBuff(IdHashOf(a_buff));
			if (ActiveFilter == Filter::OnlyIcon) {
				return !icon || icon->key != ActiveKey;
			}

			return icon != nullptr;
		}

		void AddCustomIcons(void* a_overlay)
		{
			const auto customIcons = BuffIcons::All();
			if (customIcons.empty()) {
				return;
			}

			auto&      icons = IconsOf(a_overlay);
			const auto count = static_cast<std::size_t>(icons.last - icons.first);

			// 借游戏自己的扩容函数预留空间，占位的空句柄没有引用计数，可以直接丢弃
			const TrayIcon placeholder{};
			while (static_cast<std::size_t>(icons.end - icons.first) < count + customIcons.size()) {
				ReallocIcons(std::addressof(icons), icons.last, std::addressof(placeholder));
			}
			icons.last = icons.first + count;

			for (const auto& icon : customIcons) {
				auto* slot = icons.last;
				slot->key = icon.key;
				CreateImage(std::addressof(slot->image), icon.path.c_str(), 0);
				++icons.last;
			}

			INFO("Added {} custom tray icon(s) to overlay", customIcons.size());
		}

		// ===================== Hook 函数 =====================
		class CreateOverlay
		{
		public:
			static void* Hook(void* a_overlay, void* a_arg2, void* a_arg3, void* a_arg4)
			{
				auto* result = Original(a_overlay, a_arg2, a_arg3, a_arg4);
				AddCustomIcons(a_overlay);
				return result;
			}

			static inline std::add_pointer_t<decltype(Hook)> Original{ nullptr };
		};

		class IsIconActive
		{
		public:
			static bool Hook(void* a_overlay, std::byte* a_actor, void* a_states, std::int32_t a_type, std::int32_t a_sub)
			{
				if (a_type == TrayQuirk) {
					const auto* icon = FindSlot(a_overlay, a_sub);
					const auto  active = Original(a_overlay, a_actor, a_states, a_type, a_sub);
					return icon ? FindGroup(a_overlay, a_actor, icon->key) != NoGroup : active;
				}

				if (IsDotType(a_type)) {
					// DOT 不经过 buff 查询，只能在原结果上再去掉已被接管的 buff
					return Original(a_overlay, a_actor, a_states, a_type, a_sub) && HasUnreplacedDot(a_actor, a_type);
				}

				ScopedFilter filter{ IsReplaceableGroup(a_type) ? Filter::HideCustom : Filter::None };
				return Original(a_overlay, a_actor, a_states, a_type, a_sub);
			}

			// 返回使用该图标的 buff 原本所属的 tray 分组，用于套用对应的 tooltip
			static std::int32_t FindGroup(void* a_overlay, std::byte* a_actor, std::int32_t a_key) noexcept
			{
				bool found = false;

				const auto& buffs = *dku::Hook::adjust_pointer<Vector<std::byte>>(a_actor, ActorBuffsOffset);
				for (auto* entry = buffs.first; entry != buffs.last; entry += BuffEntrySize) {
					const auto* icon = BuffIcons::FindByBuff(IdHashOf(entry + BuffInEntryOffset));
					if (!icon || icon->key != a_key) {
						continue;
					}

					found = true;

					if (const auto group = DotGroupOf(entry); group != NoGroup) {
						return group;
					}

					if (const auto group = GroupOf(entry); group != NoGroup) {
						return group;
					}
				}

				if (!found) {
					return NoGroup;
				}

				// 这些分组的判定没有副作用，让游戏只看使用该图标的 buff 来判断
				ScopedFilter filter{ Filter::OnlyIcon, a_key };
				auto*        states = a_actor + ActorTrayStatesOffset;
				for (const auto group : QueriedGroups) {
					if (Original(a_overlay, a_actor, states, group, 0)) {
						return group;
					}
				}

				return NoGroup;
			}

			static inline std::add_pointer_t<decltype(Hook)> Original{ nullptr };
		};

		class ShowTooltip
		{
		public:
			static bool Hook(
				void* a_overlay, std::byte* a_actor, std::int32_t a_type, std::uint64_t a_id,
				std::uint64_t a_pos, std::uint64_t a_size, std::int32_t a_arg7, void* a_context, std::int32_t a_sub)
			{
				if (a_type != TrayQuirk) {
					ScopedFilter filter{ IsReplaceableGroup(a_type) ? Filter::HideCustom : Filter::None };
					return Original(a_overlay, a_actor, a_type, a_id, a_pos, a_size, a_arg7, a_context, a_sub);
				}

				const auto* icon = FindSlot(a_overlay, a_sub);
				if (!icon) {
					return Original(a_overlay, a_actor, a_type, a_id, a_pos, a_size, a_arg7, a_context, a_sub);
				}

				const auto group = IsIconActive::FindGroup(a_overlay, a_actor, icon->key);
				if (group == NoGroup) {
					return false;
				}

				// 复用该分组原有的 tooltip，列表里只留下使用该图标的 buff
				ScopedFilter filter{ Filter::OnlyIcon, icon->key };
				return Original(a_overlay, a_actor, group, a_id, a_pos, a_size, a_arg7, a_context, 0);
			}

			static inline std::add_pointer_t<decltype(Hook)> Original{ nullptr };
		};

		class PlayIconPulse
		{
		public:
			// 原函数在 quirk 分支里按哈希查 quirk 库，自定义槽位查不到会访问空指针
			static void Hook(void* a_overlay, void* a_actor, std::int32_t a_type, std::int32_t a_sub)
			{
				if (a_type != TrayQuirk || !FindSlot(a_overlay, a_sub)) {
					Original(a_overlay, a_actor, a_type, a_sub);
				}
			}

			static inline std::add_pointer_t<decltype(Hook)> Original{ nullptr };
		};

		class QueryBuffs
		{
		public:
			static Vector<const std::byte*>* Hook(void* a_buffs, Vector<const std::byte*>* a_out, void* a_predicate)
			{
				auto* result = Original(a_buffs, a_out, a_predicate);
				if (ActiveFilter != Filter::None) {
					a_out->last = std::remove_if(a_out->first, a_out->last, IsFilteredOut);
				}
				return result;
			}

			static inline std::add_pointer_t<decltype(Hook)> Original{ nullptr };
		};

		std::uintptr_t ToRva(const void* a_addr)
		{
			return AsAddress(a_addr) - dku::Hook::Module::get().base();
		}
	}

	// ===================== 初始化 =====================
	void Install()
	{
		auto* createOverlay = Asm::search_pattern<
			"40 55 53 56 57 41 54 41 55 41 56 41 57 "  // push    rbp ... r15
			"48 8D AC 24 88 F9 FF FF "                 // lea     rbp, [rsp-678h]
			"48 81 EC 78 07 00 00 "                    // sub     rsp, 778h
			"48 8B 05 ?? ?? ?? ?? "                    // mov     rax, cs:__security_cookie
			"48 33 C4 "                                // xor     rax, rsp
			"48 89 85 60 06 00 00 "                    // mov     [rbp+660h], rax
			"48 8B F1">();                             // mov     rsi, rcx

		auto* isIconActive = Asm::search_pattern<
			"48 8B C4 "                       // mov     rax, rsp
			"48 89 58 08 "                    // mov     [rax+8], rbx
			"55 56 57 41 54 41 55 41 56 41 57 "  // push    rbp ... r15
			"48 8D 6C 24 B0 "                 // lea     rbp, [rsp-50h]
			"48 81 EC 50 01 00 00">();        // sub     rsp, 150h

		auto* showTooltip = Asm::search_pattern<
			"48 89 5C 24 08 "                    // mov     [rsp+8], rbx
			"55 56 57 41 54 41 55 41 56 41 57 "  // push    rbp ... r15
			"48 8D AC 24 10 EF FF FF">();        // lea     rbp, [rsp-10F0h]

		auto* playIconPulse = Asm::search_pattern<
			"40 55 53 56 57 41 56 "     // push    rbp, rbx, rsi, rdi, r14
			"48 8D AC 24 90 FE FF FF "  // lea     rbp, [rsp-170h]
			"48 81 EC 70 02 00 00 "     // sub     rsp, 270h
			"48 8B 05 ?? ?? ?? ??">();  // mov     rax, cs:__security_cookie

		auto* queryBuffs = Asm::search_pattern<
			"40 53 55 56 57 41 56 "  // push    rbx, rbp, rsi, rdi, r14
			"48 83 EC 50 "           // sub     rsp, 50h
			"48 8B 05 ?? ?? ?? ?? "  // mov     rax, cs:__security_cookie
			"48 33 C4 "              // xor     rax, rsp
			"48 89 44 24 40 "        // mov     [rsp+40h], rax
			"49 8B F0">();           // mov     rsi, r8

		auto* createImage = Asm::search_pattern<
			"40 53 55 56 57 41 56 "     // push    rbx, rbp, rsi, rdi, r14
			"48 83 EC 60 "              // sub     rsp, 60h
			"48 8B 05 ?? ?? ?? ?? "     // mov     rax, cs:__security_cookie
			"48 33 C4 "                 // xor     rax, rsp
			"48 89 44 24 58 "           // mov     [rsp+58h], rax
			"48 8B FA "                 // mov     rdi, rdx
			"48 8B F1 "                 // mov     rsi, rcx
			"48 89 4C 24 40 "           // mov     [rsp+40h], rcx
			"33 ED "                    // xor     ebp, ebp
			"89 6C 24 30 "              // mov     [rsp+30h], ebp
			"4C 8D 35 ?? ?? ?? ??">();  // lea     r14, [rip+disp]

		auto* isListedInGroup = Asm::search_pattern<
			"48 89 5C 24 08 "           // mov     [rsp+8], rbx
			"57 "                       // push    rdi
			"48 83 EC 20 "              // sub     rsp, 20h
			"44 8B 4A 08 "              // mov     r9d, [rdx+8]
			"48 8D 3D ?? ?? ?? ?? "     // lea     rdi, __ImageBase
			"48 8B D9">();              // mov     rbx, rcx

		auto* reallocCallSite = Asm::search_pattern<
			"4C 8D 44 24 78 "           // lea     r8, [rsp+78h]
			"48 8D 8E B0 01 00 00 "     // lea     rcx, [rsi+1B0h]
			"E8 ?? ?? ?? ??">();        // call    vector::_Emplace_reallocate

		// 结构偏移写死在下面的指令里，游戏更新改变内存布局时拒绝安装
		const auto layoutMatches =
			Asm::search_pattern<"48 81 C7 18 04 00 00 48 3B FD">() != nullptr &&  // add rdi, 418h; cmp rdi, rbp
			Asm::search_pattern<"48 8D 8E 60 04 00 00 48 8D 44 24 60">() != nullptr &&  // lea rcx, [rsi+460h]; lea rax, [rsp+60h]
			Asm::search_pattern<"48 89 B9 A4 00 00 00 66 89 B9 AC 00 00 00">() != nullptr &&  // Buff 构造函数中的 id 哈希字段
			// cmp [rax+rcx+8], stat; jne; movss xmm0, [rax+rcx+50h]，对应 DotStats 的六个枚举值
			Asm::search_pattern<"83 7C 08 08 04 75 0B F3 0F 10 44 08 50">() != nullptr &&
			Asm::search_pattern<"83 7C 08 08 05 75 0B F3 0F 10 44 08 50">() != nullptr &&
			Asm::search_pattern<"83 7C 08 08 58 75 0B F3 0F 10 44 08 50">() != nullptr &&
			Asm::search_pattern<"83 7C 08 08 22 75 0B F3 0F 10 44 08 50">() != nullptr &&
			Asm::search_pattern<"83 7C 08 08 23 75 0B F3 0F 10 44 08 50">() != nullptr &&
			Asm::search_pattern<"83 7C 08 08 24 75 0B F3 0F 10 44 08 50">() != nullptr;

		if (!createOverlay || !isIconActive || !showTooltip || !playIconPulse || !queryBuffs ||
			!createImage || !isListedInGroup || !reallocCallSite || !layoutMatches) {
			WARN("Incompatible game version, hooks are not installed "
				 "(CreateOverlay: {}, IsIconActive: {}, ShowTooltip: {}, PlayIconPulse: {}, QueryBuffs: {}, "
				 "CreateImage: {}, IsListedInGroup: {}, ReallocCallSite: {}, Layout: {})",
				createOverlay != nullptr, isIconActive != nullptr, showTooltip != nullptr, playIconPulse != nullptr,
				queryBuffs != nullptr, createImage != nullptr, isListedInGroup != nullptr, reallocCallSite != nullptr,
				layoutMatches);
			return;
		}

		INFO("CreateOverlay found at +{:X}", ToRva(createOverlay));
		INFO("IsIconActive found at +{:X}", ToRva(isIconActive));
		INFO("ShowTooltip found at +{:X}", ToRva(showTooltip));
		INFO("PlayIconPulse found at +{:X}", ToRva(playIconPulse));
		INFO("QueryBuffs found at +{:X}", ToRva(queryBuffs));

		// call 指令位于特征码第 12 字节，目标 = 下一条指令地址 + rel32
		const auto callAddr = AsAddress(reallocCallSite) + 12;
		const auto callTarget = callAddr + 5 + *std::bit_cast<const std::int32_t*>(callAddr + 1);

		CreateImage = std::bit_cast<CreateImage_t>(createImage);
		IsListedInGroup = std::bit_cast<IsListedInGroup_t>(isListedInGroup);
		ReallocIcons = std::bit_cast<ReallocIcons_t>(callTarget);

		dku::Hook::Trampoline::AllocTrampoline(1 << 10);

		CreateOverlay::Original = dku::Hook::write_detour<5>(AsAddress(createOverlay), CreateOverlay::Hook);
		IsIconActive::Original = dku::Hook::write_detour<7>(AsAddress(isIconActive), IsIconActive::Hook);
		ShowTooltip::Original = dku::Hook::write_detour<5>(AsAddress(showTooltip), ShowTooltip::Hook);
		PlayIconPulse::Original = dku::Hook::write_detour<5>(AsAddress(playIconPulse), PlayIconPulse::Hook);
		QueryBuffs::Original = dku::Hook::write_detour<5>(AsAddress(queryBuffs), QueryBuffs::Hook);

		INFO("All hooks installed and enabled successfully.");
	}
}
