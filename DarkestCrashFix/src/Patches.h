#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// 定位规则和片段生成只依赖映射好的 PE 映像和 Xbyak，tools/test_patch.cpp 直接编译这份代码
namespace Patches
{
	inline constexpr std::size_t FragmentSize = 0x30;

	// BuffFX 的 vector 首次扩容容量下限（元素个数）。256 * 0x18 = 6144 B = 0x1800
	inline constexpr std::uint32_t BuffFXMinCapacity = 256;

	// 按内存布局映射好的 PE 映像，所有位置都用 RVA 表示
	class Image
	{
	public:
		explicit Image(std::uintptr_t a_base);

		[[nodiscard]] std::uintptr_t base() const noexcept { return _base; }
		[[nodiscard]] std::uint32_t  size() const noexcept { return _imageSize; }
		[[nodiscard]] std::uint32_t  textBegin() const noexcept { return _textBegin; }
		[[nodiscard]] std::uint32_t  textEnd() const noexcept { return _textEnd; }
		[[nodiscard]] std::uint32_t  dataBegin() const noexcept { return _dataBegin; }
		[[nodiscard]] std::uint32_t  dataEnd() const noexcept { return _dataEnd; }

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
		std::uint32_t  _dataBegin{ 0 };
		std::uint32_t  _dataEnd{ 0 };
	};

	struct Site
	{
		char          key{ 0 };
		const char*   name{ "" };
		std::uint32_t function{ 0 };  // 补丁点所在函数的入口
		std::uint32_t hook{ 0 };      // 被覆盖指令的起点
		std::uint32_t cover{ 0 };     // 被覆盖的字节数
		std::uint32_t exit{ 0 };      // 空指针 / 无效时的去向，B、C、D、F 使用

		// 只有 A 使用
		int           objectReg{ 0 };
		int           valueReg{ 0 };
		std::uint32_t typeOffset{ 0 };

		// 只有 F 使用：技能位图基址全局与技能计数全局的 RVA
		std::uint32_t bitmapGlobalRva{ 0 };
		std::uint32_t countGlobalRva{ 0 };
	};

	// 直接内存修补（不依赖 trampoline）：用于把固定大小的 widget 字符串池重定向到更大的堆缓冲区
	struct DirectPatch
	{
		char          key{ 0 };
		const char*   name{ "" };
		std::uint32_t leaOpcodeRva{ 0 };   // 要翻转的 lea 0x8D -> 0x8B 的字节位置（同一 rip 位移，读取值而非地址）
		std::uint32_t capImmRva{ 0 };       // cmp r8d, imm32 处 imm32 的起点，改成新的容量上限
		std::uint32_t bufferGlobalRva{ 0 }; // 旧数组首 8 字节，改为存放新缓冲区的指针
		std::uint32_t indexGlobalRva{ 0 };  // s_widget_strings_allocation_index，用于搬迁已有内容
		std::uint32_t bufferSize{ 0 };      // 新缓冲区大小（同时作为新的容量上限）
	};

	// 等长原位改写（不依赖 trampoline）：把游戏函数内部定长字节块整块换成等价逻辑。
	// 只用于片段完全自包含（不含相对寻址、不需要重定位）的场合，覆盖长度必须与片段长度一致。
	struct InPlacePatch
	{
		char          key{ 0 };
		const char*   name{ "" };
		std::uint32_t function{ 0 };  // 所在函数入口，仅供日志与排障
		std::uint32_t hook{ 0 };      // 被替换字节块的起点
		std::uint32_t cover{ 0 };     // 被替换的字节数，与生成的片段等长
	};

	struct Plan
	{
		std::vector<Site>          sites;    // 有任何一处失败时为空
		std::vector<DirectPatch>   directs;  // 直接修补点，同样遵循全有或全无
		std::vector<InPlacePatch>  inplaces; // 等长原位改写点，同样遵循全有或全无
		std::vector<std::string>   errors;   // 每个失败点一条
	};

	[[nodiscard]] Plan Locate(const Image& a_image);

	// 在 a_memory 生成补丁片段，a_memory 至少 FragmentSize 字节，且与目标的距离不超过 2GB。返回片段长度
	std::size_t Emit(const Site& a_site, const Image& a_image, void* a_memory);

	// 生成写入补丁点的 jmp rel32，余下的字节用 nop 填满
	[[nodiscard]] std::vector<std::uint8_t> MakeJump(const Site& a_site, const Image& a_image, const void* a_fragment);

	// 把直接修补点落到已经映射好的映像上（a_base 是映像基址，a_newBuffer 是新的堆缓冲区）。
	// 调用方负责刷新指令缓存并释放资源；新缓冲区必须在进程生命周期内保持有效。
	void ApplyDirect(const DirectPatch& a_patch, std::uintptr_t a_base, void* a_newBuffer);

	// 在原位改写点生成片段字节（a_memory 至少 a_patch.cover 字节），返回片段长度
	std::size_t EmitInPlace(const InPlacePatch& a_patch, void* a_memory);

	// 把原位改写落到已经映射好的映像上；调用方负责保证没有线程正在执行该段代码
	void ApplyInPlace(const InPlacePatch& a_patch, std::uintptr_t a_base);
}
