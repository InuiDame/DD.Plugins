#pragma once

#include <cstdint>

namespace Hooks
{
	// 缓存 DllMain 所在线程 ID（可选 rule-gated 模式用，v1 仅记录）。
	void CacheMainThread(std::uint32_t a_threadId) noexcept;

	// 方案 B：校验 4CB8C0 的 E8 rel32，改写为指向本模块 Detour 的 call，
	// detour 内算 mask、清零栈副本、直接 call g_base+4C5050。
	void Install();

	[[nodiscard]] bool IsArmed() noexcept;
}
