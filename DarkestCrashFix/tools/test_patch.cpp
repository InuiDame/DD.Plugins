// 用真实的 Darkest.exe 验证定位规则和补丁片段，不需要启动游戏
// 用法：darkestcrashfix_test_patch <Darkest.exe>
#include "Patches.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
	int g_failures = 0;

	void Check(bool a_ok, const char* a_what)
	{
		std::printf("  [%s] %s\n", a_ok ? "PASS" : "FAIL", a_what);
		if (!a_ok) {
			++g_failures;
		}
	}

	template <class T>
	T ReadAs(const std::vector<char>& a_file, std::size_t a_offset)
	{
		T value;
		std::memcpy(std::addressof(value), a_file.data() + a_offset, sizeof(T));
		return value;
	}

	struct Mapping
	{
		std::uint8_t* base{ nullptr };
		std::uint32_t fragments{ 0 };  // 与独立补丁工具新增节区相同的 RVA，方便逐字节比对
		std::uint32_t sectionTable{ 0 };
	};

	// 按内存布局展开文件。多留一页放补丁片段
	Mapping Map(const std::vector<char>& a_file)
	{
		const auto pe = ReadAs<std::uint32_t>(a_file, 0x3C);
		const auto optionalHeader = pe + 24;
		const auto sectionCount = ReadAs<std::uint16_t>(a_file, pe + 6);
		const auto sectionAlign = ReadAs<std::uint32_t>(a_file, optionalHeader + 32);
		const auto imageSize = ReadAs<std::uint32_t>(a_file, optionalHeader + 56);
		const auto headerSize = ReadAs<std::uint32_t>(a_file, optionalHeader + 60);
		const auto sectionTable = optionalHeader + ReadAs<std::uint16_t>(a_file, pe + 20);

		const auto fragments = (imageSize + sectionAlign - 1) / sectionAlign * sectionAlign;
		auto*      base = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, fragments + 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!base) {
			return {};
		}

		std::memcpy(base, a_file.data(), headerSize);
		for (std::uint32_t i = 0; i < sectionCount; ++i) {
			const auto header = sectionTable + i * 40;
			std::memcpy(
				base + ReadAs<std::uint32_t>(a_file, header + 12),
				a_file.data() + ReadAs<std::uint32_t>(a_file, header + 20),
				ReadAs<std::uint32_t>(a_file, header + 16));
		}

		return { base, fragments, sectionTable };
	}

	void PrintFragment(const Patches::Site& a_site, std::uint32_t a_rva, const std::uint8_t* a_code, std::size_t a_size)
	{
		std::printf("%c hook=%x cover=%u frag=%x bytes=", a_site.key, a_site.hook, a_site.cover, a_rva);
		for (std::size_t i = 0; i < a_size; ++i) {
			std::printf("%02x", a_code[i]);
		}
		std::puts("");
	}

	using SetDirty_t = void (*)(void*);

	bool Faulted(SetDirty_t a_function, void* a_argument)
	{
		__try {
			a_function(a_argument);
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return true;
		}
	}

	// 函数只访问这条指针链：[rcx+8] -> [+8] -> 目标对象
	struct Chain
	{
		alignas(16) std::uint8_t holder[0x20]{};
		alignas(16) std::uint8_t link[0x100]{};
		alignas(16) std::uint8_t target[0x100]{};

		void* Prepare(void* a_target) noexcept
		{
			*reinterpret_cast<void**>(holder + 8) = link;
			*reinterpret_cast<void**>(link + 8) = a_target;
			return holder;
		}
	};

	void TestBuffFX(const Patches::Site& a_site, const Patches::Image& a_image, Mapping& a_mapping, const void* a_fragment)
	{
		const auto function = reinterpret_cast<SetDirty_t>(a_image.base() + a_site.function);
		const auto offset = *reinterpret_cast<const std::int32_t*>(a_image.bytes(a_site.hook) + 2);

		Chain chain;
		Check(Faulted(function, chain.Prepare(nullptr)), "unpatched: null target faults");

		const auto jump = Patches::MakeJump(a_site, a_image, a_fragment);
		std::memcpy(a_mapping.base + a_site.hook, jump.data(), jump.size());

		Check(!Faulted(function, chain.Prepare(chain.target)) && chain.target[offset] == 1, "patched: valid target is written");
		Check(!Faulted(function, chain.Prepare(nullptr)), "patched: null target is skipped");
		Check(!Faulted(function, chain.Prepare(reinterpret_cast<void*>(0x8000))), "patched: low address is skipped");
		Check(!Faulted(function, chain.Prepare(reinterpret_cast<void*>(0x0000800000000000))), "patched: address above user space is skipped");
	}
}

int wmain(int a_argc, wchar_t* a_argv[])
{
	if (a_argc != 2) {
		std::puts("usage: darkestcrashfix_test_patch <Darkest.exe>");
		return 2;
	}

	std::ifstream           stream{ a_argv[1], std::ios::binary };
	const std::vector<char> file{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
	if (file.empty()) {
		std::puts("failed to read the file");
		return 2;
	}

	auto mapping = Map(file);
	if (!mapping.base) {
		std::printf("failed to allocate: %lu\n", ::GetLastError());
		return 2;
	}

	const Patches::Image image{ reinterpret_cast<std::uintptr_t>(mapping.base) };
	const auto           plan = Patches::Locate(image);
	for (const auto& error : plan.errors) {
		std::printf("  %s\n", error.c_str());
	}
	Check(plan.errors.empty() && plan.sites.size() == 5, "all five sites are located");
	if (plan.sites.size() != 5) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}

	std::puts("fragments:");
	std::uint8_t* buffFX = nullptr;
	for (std::size_t i = 0; i < plan.sites.size(); ++i) {
		const auto& site = plan.sites[i];
		const auto  rva = mapping.fragments + static_cast<std::uint32_t>(i * Patches::FragmentSize);
		auto*       fragment = mapping.base + rva;
		const auto  size = Patches::Emit(site, image, fragment);
		PrintFragment(site, rva, fragment, size);
		if (site.key == 'E') {
			buffFX = fragment;
		}
	}

	std::puts("checks:");
	Check(image.ExternalPatchSection().empty(), "original executable has no patch section");

	auto* sectionName = mapping.base + mapping.sectionTable;
	char  savedName[8];
	std::memcpy(savedName, sectionName, sizeof(savedName));
	std::memcpy(sectionName, ".dxfs\0\0\0", sizeof(savedName));
	Check(image.ExternalPatchSection() == ".dxfs", "patched executable is detected");
	std::memcpy(sectionName, savedName, sizeof(savedName));

	// 只要一处对不上就整体放弃
	const auto& teleport = plan.sites[3];
	mapping.base[teleport.hook] ^= 0xFF;
	const auto broken = Patches::Locate(image);
	Check(broken.sites.empty() && broken.errors.size() == 1, "one mismatching site cancels every patch");
	mapping.base[teleport.hook] ^= 0xFF;

	const auto limit = *reinterpret_cast<const std::uint64_t*>(buffFX + 7);
	Check(limit == 0x00007FFFFFFFFFFF, "BuffFX guard uses the user address limit");

	TestBuffFX(plan.sites[4], image, mapping, buffFX);

	std::printf("%d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
