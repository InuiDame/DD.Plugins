#include "Hooks.h"

#include "Rules.h"

namespace Hooks
{
	using namespace DKUtil::Alias;

	namespace
	{
		namespace Asm = dku::Hook::Assembly;

		// ---- 基准 exe 常量（SHA1 9db2d73b89aa034e2055df65a139403e18df641f，RVA）----
		constexpr std::uintptr_t RVA_CALL_SITE = 0x4CB8C0;  // ApplyEffect 内唯一 call 4C5050
		constexpr std::uintptr_t RVA_ORIGINAL  = 0x4C5050;  // EffectApplyInternal（入口有第三方 hook，不打补丁）
		constexpr std::uintptr_t RVA_APPLY     = 0x4CB4D0;  // ApplyEffect 入口（只校验）

		// 4CB8C0 实测：E8 8B 97 FF FF → 目标 g_base+0x4C5050，全 .text 唯一。
		constexpr std::uint8_t CALL_PATTERN[5]{ 0xE8, 0x8B, 0x97, 0xFF, 0xFF };

		// V2：4CB4D0 入口 19 字节。
		constexpr std::uint8_t APPLY_PROLOGUE[19]{
			0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
			0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x48
		};

		// EffectApplyInternal 的 13 参签名。审核者对真实 exe 的 4CB4D0 反汇编实测（4CB872-8AE、
		// 4CB8B3-BD）确认 Windows x64 参数布局如下——两个 float 在**栈上**，不在 xmm：
		//   rcx=prng, rdx=performer, r8=target, r9=effect,
		//   [rsp+0x20]=item, [rsp+0x28]=roll(float), [rsp+0x30]=crit,
		//   [rsp+0x38]=scale(float), [rsp+0x40]=source, [rsp+0x48]=sourcedata,
		//   [rsp+0x50]=out, [rsp+0x58]=b1, [rsp+0x60]=b2。
		// 实现用与真实签名逐参同型的 C++ 函数让编译器按 ABI 编组，不依赖此 prose、不手写汇编。
		// （旧注释曾误写 roll→xmm0/scale→xmm1/item→[rsp+0x28]，已据实测更正。）
		using EffectApplyInternal_t = void (*)(
			void*, void*, void*, const void*, void*,
			float, bool, float, std::uint32_t, std::uint32_t, void*, bool, bool);

		std::uintptr_t        g_base = 0;
		bool                  g_armed = false;
		std::uint32_t         g_mainThread = 0;
		EffectApplyInternal_t g_original = nullptr;
		std::atomic<std::uint32_t> g_diagCount{ 0 };

		// 日志必须放在 __try 函数之外（含 noexcept 的函数里用 __try 时，函数体内不能出现
		// 需要 C++ unwinding 的对象；fmt 日志会构造这类对象）。用 noinline 隔离。
		__declspec(noinline) void DiagHit(std::uint8_t a_mask, bool a_swap) noexcept
		{
			const auto n = g_diagCount.fetch_add(1, std::memory_order_relaxed);
			if (n < 16) {
				INFO("[DIAG] immune detour hit: mask=0x{:X} swap={}",
					static_cast<unsigned>(a_mask), a_swap);
			}
		}

		__declspec(noinline) void DiagException() noexcept
		{
			const auto n = g_diagCount.fetch_add(1, std::memory_order_relaxed);
			if (n < 16) {
				ERROR("immune detour exception, forwarded original effect");
			}
		}

		// ===================== detour（方案 B 核心）=====================

		void Detour(
			void* a_prng, void* a_performer, void* a_targetArg, const void* a_effect, void* a_item,
			float a_roll, bool a_crit, float a_scale, std::uint32_t a_source,
			std::uint32_t a_sourcedata, void* a_out, bool a_b1, bool a_b2)
		{
			// 栈副本要活到 g_original 调用之后，故声明在受保护块之外（POD 数组、无析构）。
			alignas(8) std::byte copy[Rules::kEffectSize];
			Rules::ForwardDecision decision{};

			// __try/__except 只包插件自身逻辑：target 判定、mask 查询、memcpy、字段清零。
			// 异常时 decision 回退为"原指针 + mask=0"（fail-open），且此处绝不调用 g_original。
			__try {
				if (!a_effect) {
					decision = Rules::DecideForwardEffect(a_effect, nullptr, 0, false);
				} else {
					const auto* eff = static_cast<const std::byte*>(a_effect);
					const void* realTarget = Rules::ResolveTarget(a_performer, a_targetArg, eff);
					const auto  mask = Rules::Mask(realTarget);

					if (mask == 0) {
						decision = Rules::DecideForwardEffect(a_effect, nullptr, 0, false);
					} else {
						std::memcpy(copy, eff, Rules::kEffectSize);
						Rules::ApplyImmunity(copy, mask);
						DiagHit(mask, *reinterpret_cast<const bool*>(eff + Rules::OFF_SWAP));
						decision = Rules::DecideForwardEffect(a_effect, copy, mask, false);
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				DiagException();
				decision = Rules::DecideForwardEffect(a_effect, nullptr, 0, true);  // fail-open
			}

			// 唯一一次转发，位于受保护块之外：g_original 自身抛出的 SEH 异常（游戏内 int3 断言 / AV）
			// 照常向上传播，绝不会被上面的 __except 吞掉后再调一次，避免 effect 副作用双施加。
			if (g_original) {
				g_original(a_prng, a_performer, a_targetArg, decision.effect, a_item,
					a_roll, a_crit, a_scale, a_source, a_sourcedata, a_out, a_b1, a_b2);
			}
		}

		// ===================== 补丁辅助 =====================

		bool BytesMatch(std::uintptr_t a_addr, const std::uint8_t* a_pat, std::size_t a_n) noexcept
		{
			__try {
				return std::memcmp(reinterpret_cast<const void*>(a_addr), a_pat, a_n) == 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::size_t CountPattern(
			std::uintptr_t a_begin, std::size_t a_size,
			const std::uint8_t* a_pat, std::size_t a_n) noexcept
		{
			if (a_n == 0 || a_size < a_n) {
				return 0;
			}
			std::size_t count = 0;
			const auto* p = reinterpret_cast<const std::uint8_t*>(a_begin);
			for (std::size_t i = 0; i + a_n <= a_size; ++i) {
				if (std::memcmp(p + i, a_pat, a_n) == 0) {
					++count;
				}
			}
			return count;
		}

		std::uintptr_t RoundUp(std::uintptr_t a_v, std::uintptr_t a_align) noexcept
		{
			return (a_v + a_align - 1) & ~(a_align - 1);
		}
		std::uintptr_t RoundDown(std::uintptr_t a_v, std::uintptr_t a_align) noexcept
		{
			return a_v & ~(a_align - 1);
		}

		// 在 a_from ±2GB 内找一页可执行内存；失败返回 nullptr（绝不 FATAL / __fastfail）。
		std::byte* AllocNearPage(std::uintptr_t a_from, std::size_t a_size) noexcept
		{
			SYSTEM_INFO si{};
			::GetSystemInfo(&si);
			const auto gran = static_cast<std::uintptr_t>(si.dwAllocationGranularity);
			constexpr std::uintptr_t gb = std::uintptr_t{ 1 } << 30;
			constexpr std::uintptr_t range = gb * 2;
			const auto maxAddr = std::numeric_limits<std::uintptr_t>::max();

			std::uintptr_t min = a_from >= range ? RoundUp(a_from - range, gran) : 0;
			const auto max = a_from < (maxAddr - range) ? RoundDown(a_from + range, gran) : maxAddr;

			MEMORY_BASIC_INFORMATION mbi{};
			while (min < max) {
				if (!::VirtualQuery(reinterpret_cast<LPCVOID>(min), &mbi, sizeof(mbi))) {
					break;
				}
				const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				min = regionEnd;
				if (mbi.State == MEM_FREE) {
					const auto addr = RoundUp(reinterpret_cast<std::uintptr_t>(mbi.BaseAddress), gran);
					if (addr < regionEnd && a_size <= regionEnd - addr) {
						if (auto* p = ::VirtualAlloc(reinterpret_cast<LPVOID>(addr), a_size,
							                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
							return static_cast<std::byte*>(p);
						}
					}
				}
			}
			return nullptr;
		}

		void WriteNearCall(std::byte* a_site, std::uintptr_t a_target) noexcept
		{
			const auto disp = static_cast<std::int64_t>(a_target) -
			                  static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(a_site) + 5);
			const auto d32 = static_cast<std::int32_t>(disp);
			a_site[0] = static_cast<std::byte>(0xE8);
			std::memcpy(a_site + 1, &d32, sizeof(d32));
		}

		void WriteIndirectJump(std::byte* a_stub, std::uintptr_t a_target) noexcept
		{
			a_stub[0] = static_cast<std::byte>(0xFF);
			a_stub[1] = static_cast<std::byte>(0x25);
			const std::uint32_t zero = 0;
			std::memcpy(a_stub + 2, &zero, sizeof(zero));   // [rip+0]
			std::memcpy(a_stub + 6, &a_target, sizeof(a_target));  // abs64
		}
	}

	// ===================== 初始化 =====================

	void CacheMainThread(std::uint32_t a_threadId) noexcept
	{
		g_mainThread = a_threadId;
	}

	bool IsArmed() noexcept
	{
		return g_armed;
	}

	void Install()
	{
		INFO("dd_immune: validating signatures (plan B call-site detour) ...");

		if (Rules::Empty()) {
			INFO("dd_immune: no rules found, hook NOT installed (nothing to immunize)");
			return;
		}

		g_base = dku::Hook::Module::get().base();
		if (!g_base) {
			WARN("[FAIL] V0: cannot resolve game module base");
			return;
		}

		const auto callSite = g_base + RVA_CALL_SITE;
		const auto original = g_base + RVA_ORIGINAL;
		const auto applyEntry = g_base + RVA_APPLY;

		// ---- V1：4CB8C0 的 E8 rel32 必须解析到 4C5050，且全 .text 唯一 ----
		bool v1op = false, v1target = false, v1unique = false;
		std::uint8_t callBytes[5]{};
		std::memcpy(callBytes, reinterpret_cast<const void*>(callSite), sizeof(callBytes));
		v1op = callBytes[0] == 0xE8;
		if (v1op) {
			std::int32_t rel = 0;
			std::memcpy(&rel, callBytes + 1, sizeof(rel));
			v1target = (callSite + 5 + static_cast<std::int64_t>(rel) == original);
		}
		const auto [textBase, textSize] = dku::Hook::Module::get().section(dku::Hook::Module::Section::textx);
		const auto callHits = CountPattern(textBase, textSize, CALL_PATTERN, sizeof(CALL_PATTERN));
		v1unique = (callHits == 1);
		INFO("[{}] V1: call-site E8 op={}, target==4C5050 {}, .text matches={}",
			(v1op && v1target && v1unique) ? "OK" : "FAIL", v1op, v1target, callHits);

		// ---- V2：4CB4D0 入口字节 ----
		const bool v2 = BytesMatch(applyEntry, APPLY_PROLOGUE, sizeof(APPLY_PROLOGUE));
		INFO("[{}] V2: ApplyEffect prologue", v2 ? "OK" : "FAIL");

		// ---- V3：4C5050 入口——只校验、不打补丁 ----
		// 该入口常被 DDShield(MinHook, E9)/DDCore(E9) 占用，故接受 0x40(原版) 或 0xE9/0xE8(已被 hook)。
		// 它是信息项，不作为门禁：若把它设成硬门禁，插件在真实共存环境会静默失效，违背方案 B 初衷。
		const std::uint8_t origFirst = *reinterpret_cast<const std::uint8_t*>(original);
		const bool v3 = (origFirst == 0x40 || origFirst == 0xE9 || origFirst == 0xE8);
		INFO("[{}] V3: EffectApplyInternal entry first byte = 0x{:02X} (informational)",
			v3 ? "OK" : "WARN", origFirst);

		// ---- V4：13 参栈槽 [rsp+0x20..0x60] 由同签名 C++ 函数按 ABI 复刻。
		// 审核者对真实 exe 反汇编 4CB4D0 已独立复核，结论：签名逐参同型、无需手写汇编。
		INFO("[OK] V4: 13-param ABI by matching C++ signature (not independently disassembled)");

		// ---- V5：字段偏移由 Rules.h 的 static_assert 焊死（0x70/0x74/0x1F0/0x1F8/0x200/0x320/0x324/0x3E0）。
		// 不猜测消费者指令字节，避免误判导致拒绝安装（信息项）。
		INFO("[OK] V5: effect offsets pinned by static_assert in Rules.h");

		// ---- V6：buff 组布局 3 组字节（与 CustomTypeEffects/src/Hooks.cpp:180-182 同源） ----
		const bool v6 =
			Asm::search_pattern<"48 81 C7 18 04 00 00 48 3B FD">() != nullptr &&   // BuffEntry 0x418
			Asm::search_pattern<"48 8D 8E 60 04 00 00 48 8D 44 24 60">() != nullptr &&  // Actor+0x460
			Asm::search_pattern<"48 89 B9 A4 00 00 00 66 89 B9 AC 00 00 00">() != nullptr;  // Buff+0xA4
		INFO("[{}] V6: buff group layout (0x418 / Actor+0x460 / Buff+0xA4)",
			v6 ? "OK" : "FAIL");

		// ---- V7：vtable 槽 6 在运行时由 Rules::ClassMask 的 SEH 读取，安装期无 actor 可测（信息项） ----
		INFO("[OK] V7: vtable slot 6 checked at runtime under SEH (main thread {})", g_mainThread);

		// ---- 门禁：任一硬校验失败 → 跳过本 hook，Install 正常返回 ----
		if (!v1op || !v1target || !v1unique || !v2 || !v6) {
			WARN("dd_immune: incompatible game version, hook NOT installed (see [FAIL] items above)");
			return;
		}

		// ---- V8：±2GB / 近跳中继 ----
		const auto detourAddr = reinterpret_cast<std::uintptr_t>(&Detour);
		std::uintptr_t patchTarget = detourAddr;
		std::byte*     nearPage = nullptr;

		const auto disp = static_cast<std::int64_t>(detourAddr) -
		                  static_cast<std::int64_t>(callSite + 5);
		if (disp < -0x7FFFFFFFLL || disp > 0x7FFFFFFFLL) {
			nearPage = AllocNearPage(callSite, 16);
			if (!nearPage) {
				WARN("[FAIL] V8: detour out of rel32 range and near-page allocation failed, hook NOT installed");
				return;
			}
			WriteIndirectJump(nearPage, detourAddr);  // FF25 [abs64] -> Detour
			patchTarget = reinterpret_cast<std::uintptr_t>(nearPage);
			INFO("[OK] V8: near FF25 relay at +{:X} -> Detour", patchTarget - g_base);
		} else {
			INFO("[OK] V8: detour within rel32 range, direct E8");
		}

		// g_original 直接指向 4C5050（入口若已有 DDShield hook，会自然串联）。
		g_original = reinterpret_cast<EffectApplyInternal_t>(original);

		DWORD oldProtect = 0;
		if (!::VirtualProtect(reinterpret_cast<LPVOID>(callSite), 5,
			                  PAGE_EXECUTE_READWRITE, &oldProtect)) {
			WARN("[FAIL] V8: VirtualProtect(callSite) failed ({}), hook NOT installed", ::GetLastError());
			return;
		}

		WriteNearCall(reinterpret_cast<std::byte*>(callSite), patchTarget);

		DWORD restored = 0;
		::VirtualProtect(reinterpret_cast<LPVOID>(callSite), 5, oldProtect, &restored);
		::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(callSite), 5);

		g_armed = true;
		INFO("dd_immune: plan B detour installed at +{:X} -> {:X} (original +{:X})",
			RVA_CALL_SITE, patchTarget - g_base, RVA_ORIGINAL);
	}
}
