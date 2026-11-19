#include "Hooks.h"

#include "Patches.h"

namespace Hooks
{
	namespace
	{
		namespace Trampoline = dku::Hook::Trampoline;

		void Apply(const Patches::Image& a_image, const Patches::Plan& a_plan)
		{
			const auto& sites = a_plan.sites;
			const auto& directs = a_plan.directs;
			const auto& inplaces = a_plan.inplaces;

			// 直接修补需要先准备好新缓冲区，任何一处分配失败都整体放弃
			std::vector<void*> buffers;
			for (const auto& direct : directs) {
				auto* buffer = ::VirtualAlloc(nullptr, direct.bufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
				if (!buffer) {
					throw std::runtime_error("failed to allocate the enlarged widget string pool");
				}
				buffers.push_back(buffer);
			}

			Trampoline::AllocTrampoline(sites.size() * Patches::FragmentSize);

			// 片段和跳转全部生成成功后才改写游戏代码，中途失败时游戏保持原样
			std::vector<std::vector<std::uint8_t>> jumps;
			for (const auto& site : sites) {
				auto* fragment = Trampoline::Allocate(Patches::FragmentSize);
				Patches::Emit(site, a_image, fragment);
				jumps.push_back(Patches::MakeJump(site, a_image, fragment));
			}

			for (std::size_t i = 0; i < sites.size(); ++i) {
				const auto address = a_image.base() + sites[i].hook;
				dku::Hook::WriteData(address, jumps[i].data(), jumps[i].size());
				::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<const void*>(address), jumps[i].size());
				INFO("Patched {} ({}) at Darkest.exe+{:X}", sites[i].key, sites[i].name, sites[i].hook);
			}

			for (const auto& inplace : inplaces) {
				Patches::ApplyInPlace(inplace, a_image.base());
				INFO("Patched {} ({}) at Darkest.exe+{:X}: BuffFX vector capacity floor raised to {}",
					inplace.key, inplace.name, inplace.hook, Patches::BuffFXMinCapacity);
			}

			for (std::size_t i = 0; i < directs.size(); ++i) {
				Patches::ApplyDirect(directs[i], a_image.base(), buffers[i]);
				INFO("Patched {} ({}) at Darkest.exe+{:X}: widget string pool enlarged to {:X}",
					directs[i].key, directs[i].name, directs[i].leaOpcodeRva, directs[i].bufferSize);
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

			Apply(image, plan);
			INFO("All {} crash fix sites are patched", plan.sites.size() + plan.directs.size() + plan.inplaces.size());
		} catch (const std::exception& e) {
			// 写入阶段才失败时，前面已经写进去的补丁不会回滚，日志里那些 Patched 行仍然有效
			WARN("Failed to install the crash fixes: {}. Patches already logged above remain applied", e.what());
		}
	}
}
