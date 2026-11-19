#pragma once

#include <vector>

#include "SpineLayout.h"

namespace SpineFix::render
{
	// slot 的混合模式，取值就是 .skel 里那个字节，见 Config.h 的说明
	enum class Blend : std::uint8_t
	{
		Normal = 0,
		Additive = 1,
		Multiply = 2,
		Screen = 3,
	};

	// 读 spSlotData+0x28。原版 2.1 这里只有 0/1，2/3 是本插件扩的。
	[[nodiscard]] Blend BlendOfSlot(const spine::Slot& a_slot) noexcept;

	// drawOrder 上连续、混合模式相同的一段可见切片，[begin, end) 是 drawOrder 下标
	struct Run
	{
		Blend        blend{ Blend::Normal };
		std::int32_t begin{ 0 };
		std::int32_t end{ 0 };

		[[nodiscard]] bool Empty() const noexcept { return end <= begin; }
	};

	// 按 drawOrder 切段；没有条目 / 没有切片时返回空表。
	// a_honorAdditive 为 false 时 additive 当正常混合，不单独成段。
	// 段数超过 a_maxRuns 时把后面的并进最后一段（同段内按 drawOrder 顺序画，层级不丢）。
	[[nodiscard]] std::vector<Run> BuildRuns(const spine::Sprite& a_sprite, std::size_t a_maxRuns,
		bool a_honorAdditive);
}
