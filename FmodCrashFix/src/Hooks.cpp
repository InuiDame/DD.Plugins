#include "Hooks.h"

namespace Hooks
{
	using namespace DKUtil::Alias;

	namespace
	{
		namespace Asm = dku::Hook::Assembly;

		// ===================== FMOD 结构 =====================
		constexpr std::ptrdiff_t ItemOwnerOffset = 0x20;  // 依赖条目 -> 持有者
		constexpr std::ptrdiff_t OwnerModelOffset = 0x8;  // 持有者 -> model，崩溃时为空

		// ===================== 补丁位置 =====================
		constexpr std::string_view RebindPattern{
			"48 8B 3B "        // mov     rdi, [rbx]
			"41 8B 06 "        // mov     eax, [r14]
			"39 47 28 "        // cmp     [rdi+28h], eax
			"75 ?? "           // jne     next
			"8B 47 38 "        // mov     eax, [rdi+38h]
			"90 "              // nop
			"85 C0 "           // test    eax, eax
			"75 ?? "           // jne     next
			"48 8B 4F 20 "     // mov     rcx, [rdi+20h]
			"4C 8D 4C 24 40 "  // lea     r9, [rsp+40h]
			"45 33 C0 "        // xor     r8d, r8d
			"4C 89 7C 24 40 "  // mov     [rsp+40h], r15
			"48 8B D5 "        // mov     rdx, rbp
			"E8 ?? ?? ?? ?? "  // call    Resolve
			"85 C0 "           // test    eax, eax
			"75 ??"            // jne     exit
		};

		constexpr std::size_t PatchOffset = 19;  // mov rcx, [rdi+20h]
		constexpr std::size_t PatchSize = 17;    // 到 mov rdx, rbp 之前

		// jmp qword ptr [rip+0]，后接 8 字节目标地址
		constexpr std::array<std::uint8_t, 6> AbsoluteJump{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };

		// ===================== 全局变量与定义 =====================
		constexpr std::uint32_t LogLimit = 32;

		std::atomic<std::uint32_t> SkippedCount{ 0 };

		// 由 FMOD 异步线程调用，返回 true 时跳过该条目
		bool ShouldSkip(const void* a_item) noexcept
		{
			const auto* owner = *dku::Hook::adjust_pointer<const void* const>(a_item, ItemOwnerOffset);
			if (owner && *dku::Hook::adjust_pointer<const void* const>(owner, OwnerModelOffset)) {
				return false;
			}

			if (const auto count = ++SkippedCount; count <= LogLimit) {
				WARN("Skipped FMOD dependency without a model (item: {:X}, owner: {:X}, total: {})",
					AsAddress(a_item), AsAddress(owner), count);
			}

			return true;
		}

		// 补丁点上易失寄存器都没有存活的值，rsp 按 16 字节对齐
		struct SkipGuard final : Xbyak::CodeGenerator
		{
			SkipGuard(std::uintptr_t a_resume, std::uintptr_t a_skip)
			{
				Xbyak::Label skip;
				Xbyak::Label resumeAddress;
				Xbyak::Label skipAddress;

				sub(rsp, 0x20);
				mov(rcx, rdi);
				mov(rax, reinterpret_cast<std::uintptr_t>(&ShouldSkip));
				call(rax);
				add(rsp, 0x20);
				test(al, al);
				jnz(skip);

				// 被补丁覆盖的原指令
				mov(rcx, ptr[rdi + ItemOwnerOffset]);
				lea(r9, ptr[rsp + 0x40]);
				xor_(r8d, r8d);
				mov(ptr[rsp + 0x40], r15);
				jmp(qword[rip + resumeAddress]);

				L(skip);
				jmp(qword[rip + skipAddress]);

				L(resumeAddress);
				dq(a_resume);
				L(skipAddress);
				dq(a_skip);
			}
		};
	}

	// ===================== 初始化 =====================
	void Install()
	{
		const auto base = AsAddress(::GetModuleHandleW(L"fmodstudio.dll"));
		if (!base) {
			WARN("fmodstudio.dll is not loaded, patch is not installed");
			return;
		}

		dku::Hook::Module fmod{ base };
		const auto [text, textSize] = fmod.section(dku::Hook::Module::Section::textx);

		auto* match = Asm::search_pattern(RebindPattern, text, textSize);
		if (!match) {
			WARN("Incompatible FMOD version {}, patch is not installed", fmod.version_string("."));
			return;
		}

		// 补丁点前一条 jne 的目标就是循环的下一项
		auto* const site = match + PatchOffset;
		const auto  next = AsAddress(site) + *std::bit_cast<const std::int8_t*>(site - 1);
		const auto  resume = AsAddress(site) + PatchSize;

		static SkipGuard guard{ resume, next };

		std::array<std::uint8_t, PatchSize> patch;
		patch.fill(0x90);
		std::memcpy(patch.data(), AbsoluteJump.data(), AbsoluteJump.size());
		const auto target = reinterpret_cast<std::uintptr_t>(guard.getCode());
		std::memcpy(patch.data() + AbsoluteJump.size(), std::addressof(target), sizeof(target));

		dku::Hook::WriteData(site, patch.data(), patch.size());
		::FlushInstructionCache(::GetCurrentProcess(), site, patch.size());

		INFO("Patched FMOD {} at fmodstudio.dll+{:X}", fmod.version_string("."), AsAddress(site) - base);
	}
}
