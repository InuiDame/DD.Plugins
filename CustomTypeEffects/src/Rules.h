#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Rules
{
	using EffectIds = std::vector<std::string>;

	// 扫描数据文件，并读写 <gameRoot>\DDModLoader\cfg\CustomTypeEffects.cfg 里的屏蔽列表
	void Load();

	[[nodiscard]] bool Empty() noexcept;
	[[nodiscard]] bool HasType(std::string_view a_type) noexcept;
	[[nodiscard]] bool IsBlockedType(std::string_view a_type) noexcept;

	// 统一 .type 与 JSON 键里的类型名：ASCII 小写 + 去首尾空白
	[[nodiscard]] std::string NormalizeType(std::string_view a_text);

	[[nodiscard]] const EffectIds* FindBuff(std::int32_t a_idHash, std::string_view a_type) noexcept;
	[[nodiscard]] const EffectIds* FindTrinket(std::int32_t a_idHash, std::string_view a_type) noexcept;

	// 游戏把字符串 id 转成 int32 的算法：hash = hash * 53 + byte
	[[nodiscard]] std::int32_t Hash(std::string_view a_text) noexcept;

	[[nodiscard]] std::size_t BuffCount() noexcept;
	[[nodiscard]] std::size_t TrinketCount() noexcept;
	[[nodiscard]] std::size_t TypeCount() noexcept;
}
