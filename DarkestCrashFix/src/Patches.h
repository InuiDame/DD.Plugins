#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 定位规则和片段生成只依赖映射好的 PE 映像和 Xbyak，tools/test_patch.cpp 直接编译这份代码
namespace Patches
{
	inline constexpr std::size_t FragmentSize = 0x30;

	// 按内存布局映射好的 PE 映像，所有位置都用 RVA 表示
	class Image
	{
	public:
		explicit Image(std::uintptr_t a_base);

		[[nodiscard]] std::uintptr_t base() const noexcept { return _base; }
		[[nodiscard]] std::uint32_t  size() const noexcept { return _imageSize; }
		[[nodiscard]] std::uint32_t  textBegin() const noexcept { return _textBegin; }
		[[nodiscard]] std::uint32_t  textEnd() const noexcept { return _textEnd; }

		[[nodiscard]] const std::uint8_t* bytes(std::uint32_t a_rva) const noexcept
		{
			return reinterpret_cast<const std::uint8_t*>(_base + a_rva);
		}

		// 主程序被独立补丁工具改过时返回其节区名，否则返回空串
		[[nodiscard]] std::string ExternalPatchSection() const;

	private:
		std::uintptr_t _base;
		std::uint32_t  _imageSize{ 0 };
		std::uint32_t  _sectionTable{ 0 };
		std::uint32_t  _sectionCount{ 0 };
		std::uint32_t  _textBegin{ 0 };
		std::uint32_t  _textEnd{ 0 };
	};

	struct Site
	{
		char          key{ 0 };
		const char*   name{ "" };
		std::uint32_t function{ 0 };  // 补丁点所在函数的入口
		std::uint32_t hook{ 0 };      // 被覆盖指令的起点
		std::uint32_t cover{ 0 };     // 被覆盖的字节数
		std::uint32_t exit{ 0 };      // 空指针时的去向，只有 B、C、D 使用

		// 只有 A 使用
		int           objectReg{ 0 };
		int           valueReg{ 0 };
		std::uint32_t typeOffset{ 0 };
	};

	struct Plan
	{
		std::vector<Site>        sites;   // 有任何一处失败时为空
		std::vector<std::string> errors;  // 每个失败点一条
	};

	[[nodiscard]] Plan Locate(const Image& a_image);

	// 在 a_memory 生成补丁片段，a_memory 至少 FragmentSize 字节，且与目标的距离不超过 2GB。返回片段长度
	std::size_t Emit(const Site& a_site, const Image& a_image, void* a_memory);

	// 生成写入补丁点的 jmp rel32，余下的字节用 nop 填满
	[[nodiscard]] std::vector<std::uint8_t> MakeJump(const Site& a_site, const Image& a_image, const void* a_fragment);
}
