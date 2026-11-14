#include "Hooks.h"

#include "Patches.h"

namespace Hooks
{
	namespace
	{
		namespace Trampoline = dku::Hook::Trampoline;

		void Apply(const Patches::Image& a_image, const std::vector<Patches::Site>& a_sites, const std::vector<Patches::DirectPatch>& a_directs)
		{
			// 直接修补需要先准备好新缓冲区，任何一处分配失败都整体放弃
			std::vector<void*> buffers;
			for (const auto& direct : a_directs) {
				auto* buffer = ::VirtualAlloc(nullptr, direct.bufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				if (!buffer) {
					throw std::runtime_error("failed to allocate the enlarged widget string pool");
				}
				buffers.push_back(buffer);
			}

			Trampoline::AllocTrampoline(a_sites.size() * Patches::FragmentSize);

			// 片段和跳转全部生成成功后才改写游戏代码，中途失败时游戏保持原样
			std::vector<std::vector<std::uint8_t>> jumps;
			for (const auto& site : a_sites) {
				auto* fragment = Trampoline::Allocate(Patches::FragmentSize);
				Patches::Emit(site, a_image, fragment);
				jumps.push_back(Patches::MakeJump(site, a_image, fragment));
			}

			for (std::size_t i = 0; i < a_sites.size(); ++i) {
				const auto address = a_image.base() + a_sites[i].hook;
				dku::Hook::WriteData(address, jumps[i].data(), jumps[i].size());
				::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<const void*>(address), jumps[i].size());
				INFO("Patched {} ({}) at Darkest.exe+{:X}", a_sites[i].key, a_sites[i].name, a_sites[i].hook);
			}

			for (std::size_t i = 0; i < a_directs.size(); ++i) {
				Patches::ApplyDirect(a_directs[i], a_image.base(), buffers[i]);
				INFO("Patched {} ({}) at Darkest.exe+{:X}: widget string pool enlarged to {:X}",
					a_directs[i].key, a_directs[i].name, a_directs[i].leaOpcodeRva, a_directs[i].bufferSize);
			}
		}
	}

	// ===================== 初始化 =====================
	void Install()
	{
		try {
			const Patches::Image image{ dku::Hook::Module::get().base() };

			if (const auto section = image.ExternalPatchSection(); !section.empty()) {
				INFO("Darkest.exe was already modified by a patch tool (section {}), nothing is patched", section);
				return;
			}

			const auto plan = Patches::Locate(image);
			if (!plan.errors.empty()) {
				WARN("Incompatible game version, nothing is patched");
				for (const auto& error : plan.errors) {
					WARN("  {}", error);
				}
				return;
			}

			Apply(image, plan.sites, plan.directs);
			INFO("All {} crash fix sites are patched", plan.sites.size() + plan.directs.size());
		} catch (const std::exception& e) {
			WARN("Failed to install the crash fixes, nothing is patched: {}", e.what());
		}
	}
}
