#pragma once

namespace BuffIcons
{
	struct Icon
	{
		std::int32_t key;   // tray 槽位的标识，由解析后的文件路径哈希得到
		std::string  path;  // 传给游戏的 UTF-8 绝对路径，使用正斜杠
	};

	void Load();

	// 游戏用同样的算法把字符串 id 转成 int32
	[[nodiscard]] std::int32_t Hash(std::string_view a_text) noexcept;

	[[nodiscard]] std::span<const Icon> All() noexcept;
	[[nodiscard]] const Icon*           FindByBuff(std::int32_t a_buffHash) noexcept;
	[[nodiscard]] const Icon*           FindByKey(std::int32_t a_key) noexcept;
}
