// 用真实的 Darkest.exe 验证定位规则和补丁片段，不需要启动游戏
// 用法：darkestcrashfix_test_patch <Darkest.exe>
#include "Patches.h"

#include <Windows.h>

#include <algorithm>
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

		// 定位规则允许 0 <= offset < 0x10000，但下面两个桩只有 0x100 字节；越界会踩到栈上别的对象，
		// 而且断言可能照样通过，所以先明确拒绝
		Check(offset >= 0 && offset < 0x100, "E: dirty flag offset fits the test harness");
		if (offset < 0 || offset >= 0x100) {
			return;
		}

		Chain chain;
		Check(Faulted(function, chain.Prepare(nullptr)), "unpatched: null target faults");

		const auto jump = Patches::MakeJump(a_site, a_image, a_fragment);
		std::memcpy(a_mapping.base + a_site.hook, jump.data(), jump.size());

		Check(!Faulted(function, chain.Prepare(chain.target)) && chain.target[offset] == 1, "patched: valid target is written");
		Check(!Faulted(function, chain.Prepare(nullptr)), "patched: null target is skipped");
		Check(!Faulted(function, chain.Prepare(reinterpret_cast<void*>(0x8000))), "patched: low address is skipped");
		Check(!Faulted(function, chain.Prepare(reinterpret_cast<void*>(0x0000800000000000))), "patched: address above user space is skipped");
	}

	// 技能位图防御：未打补丁且基址为 -1 时会崩；打补丁后返回 0 且不再崩
	bool FaultedFn(std::uint64_t (*a_function)())
	{
		__try {
			a_function();
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return true;
		}
	}

	// G：widget 字符串池溢出断言。把池基址加载从 lea（取地址）翻成 mov（取值），并在数组首 8
	// 字节写入更大的堆缓冲区指针，同时抬高容量上限。验证定位、翻转语义和 ApplyDirect 落地。
	void TestWidgetString(const Patches::DirectPatch& a_direct, const Patches::Image& a_image)
	{
		const auto* leaByte = a_image.bytes(a_direct.leaOpcodeRva);
		Check(*leaByte == 0x8D, "G: pool base load opcode is lea (0x8D)");

		const auto capImm = *reinterpret_cast<const std::uint32_t*>(a_image.bytes(a_direct.capImmRva));
		Check(capImm == 0x8000, "G: original capacity limit is 0x8000");

		const auto poolSlot = a_image.base() + a_direct.bufferGlobalRva;
		Check(a_direct.bufferGlobalRva < a_image.textBegin() || a_direct.bufferGlobalRva >= a_image.textEnd(),
			"G: pool base lives in a data section, not .text");
		Check(a_image.dataBegin() != 0 && a_image.dataEnd() != 0, "G: .data section was located");

		const auto* head8 = reinterpret_cast<const std::uint64_t*>(poolSlot);
		Check(*head8 == 0, "G: pool head (array first 8 bytes) is zero-initialized");

		// 索引全局必须被定位到，且与池同在一个可写数据节
		auto* indexGlobal = reinterpret_cast<std::uint32_t*>(a_image.base() + a_direct.indexGlobalRva);
		Check(a_direct.indexGlobalRva >= a_image.dataBegin() && a_direct.indexGlobalRva + 4 <= a_image.dataEnd(),
			"G: widget string index global lives in the data section");
		Check(*indexGlobal == 0, "G: widget string index is zero before any allocation");

		// ApplyDirect 是原地改写，测试完必须回滚：否则后续的 Locate 会因为 lea 已被翻成 mov、
		// cap 已被抬高而失配（正是之前 "pattern matched 0 times" 的来源）。
		const std::uint8_t  savedLea = *leaByte;
		const std::uint32_t savedCap = capImm;
		const std::uint64_t savedHead = *head8;

		// 用绝对寻址复现 AllocateWidgetString 的“基址 + 索引”计算，避开 rip 相对位移在测试进程里
		// 可能跨出 ±2GB 的问题。flip=false 用 lea（取基址这个地址），flip=true 用 mov（取基址处存的指针）。
		alignas(16) static std::uint64_t s_outRcx = 0;
		alignas(16) static std::uint64_t s_outRbx = 0;
		auto* scratch = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

		const auto putU32 = [](std::uint8_t* p, std::uint32_t v) { std::memcpy(p, std::addressof(v), 4); return p + 4; };
		const auto putU64 = [](std::uint8_t* p, std::uint64_t v) { std::memcpy(p, std::addressof(v), 8); return p + 8; };

		const auto build = [&](bool a_flip, std::size_t& a_len) {
			std::uint8_t* p = scratch;
			*p++ = 0xBB; p = putU32(p, 0x1234);                                                  // mov ebx, 0x1234  (索引)
			*p++ = 0x48; *p++ = 0xB8; p = putU64(p, poolSlot);                                    // mov rax, poolSlot
			*p++ = 0x48; *p++ = a_flip ? 0x8B : 0x8D; *p++ = 0x08;                                // (mov|lea) rcx, [rax]
			*p++ = 0x48; *p++ = 0x01; *p++ = 0xD9;                                                // add rcx, rbx (opcode 01: r/m=rcx 为目標)
			*p++ = 0x49; *p++ = 0xB8; p = putU64(p, reinterpret_cast<std::uint64_t>(std::addressof(s_outRcx)));  // mov r8, &s_outRcx
			*p++ = 0x49; *p++ = 0x89; *p++ = 0x08;                                                // mov [r8], rcx
			*p++ = 0x49; *p++ = 0xB9; p = putU64(p, reinterpret_cast<std::uint64_t>(std::addressof(s_outRbx)));  // mov r9, &s_outRbx
			*p++ = 0x49; *p++ = 0x89; *p++ = 0x19;                                                // mov [r9], rbx
			*p++ = 0xC3;                                                                           // ret
			a_len = static_cast<std::size_t>(p - scratch);
		};

		const auto run = [&](bool a_flip) -> bool {
			std::size_t len = 0;
			build(a_flip, len);
			::FlushInstructionCache(::GetCurrentProcess(), scratch, len);
			__try {
				reinterpret_cast<void(*)()>(scratch)();
				return false;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return true;
			}
		};

		// 原语义：lea 取池基址这个地址，加索引后 rcx = 池基址 + 索引
		Check(!run(false) && s_outRcx == poolSlot + 0x1234 && s_outRbx == 0x1234,
			"G: original lea loads the pool base address");

		// 翻转后语义：mov 取池基址处存放的指针，加索引后 rcx = 新指针 + 索引。
		// 顺便模拟“安装时池里已经有内容”（索引非零），验证 ApplyDirect 会先搬迁旧内容。
		constexpr std::uint32_t Existing = 0x20;
		constexpr char          ExistingText[Existing] =
			"existing widget strings before";
		auto* poolBase = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(poolSlot));
		std::memcpy(poolBase, ExistingText, Existing);
		*indexGlobal = Existing;

		const auto newBuffer = ::VirtualAlloc(nullptr, a_direct.bufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		Check(newBuffer != nullptr, "G: enlarged pool buffer allocated");
		Patches::ApplyDirect(a_direct, a_image.base(), newBuffer);

		Check(*leaByte == 0x8B, "G: ApplyDirect flipped lea -> mov (0x8B)");
		const auto capNow = *reinterpret_cast<const std::uint32_t*>(a_image.bytes(a_direct.capImmRva));
		Check(capNow == a_direct.bufferSize, "G: ApplyDirect raised the capacity limit");
		Check(*head8 == reinterpret_cast<std::uint64_t>(newBuffer), "G: ApplyDirect stored the new buffer pointer in the array head");
		Check(std::memcmp(newBuffer, ExistingText, Existing) == 0,
			"G: ApplyDirect migrated the strings already in the old pool");

		// 该断言同时交叉验证 bufferGlobalRva 正确：ApplyDirect 写入的指针必须被同一地址的 mov 读出
		Check(!run(true) && s_outRcx == reinterpret_cast<std::uint64_t>(newBuffer) + 0x1234 && s_outRbx == 0x1234,
			"G: flipped mov loads the new buffer pointer");

		// 回滚到原始镜像：先还原指令字节与容量，再清空池首指针与索引（newBuffer 即将释放，
		// 留着会让后续 Locate 看到非零的池首，误导 "zero-initialized" 断言）。
		*const_cast<std::uint8_t*>(leaByte) = savedLea;
		*const_cast<std::uint32_t*>(reinterpret_cast<const std::uint32_t*>(a_image.bytes(a_direct.capImmRva))) = savedCap;
		*const_cast<std::uint64_t*>(head8) = savedHead;
		*indexGlobal = 0;
		std::memset(poolBase, 0, Existing);
		::FlushInstructionCache(::GetCurrentProcess(), a_image.bytes(a_direct.leaOpcodeRva), 1);

		Check(*leaByte == 0x8D && *reinterpret_cast<const std::uint32_t*>(a_image.bytes(a_direct.capImmRva)) == 0x8000 && *head8 == 0,
			"G: image is restored after the direct patch test");

		::VirtualFree(scratch, 0, MEM_RELEASE);
		::VirtualFree(newBuffer, 0, MEM_RELEASE);
	}

	void TestSkillBitmap(const Patches::Site& a_site, const Patches::Image& a_image, Mapping& a_mapping, const void* a_fragment)
	{
		const auto function = reinterpret_cast<std::uint64_t (*)()>(a_image.base() + a_site.function);
		const auto bitmapGlobal = reinterpret_cast<std::uint64_t*>(a_image.base() + a_site.bitmapGlobalRva);
		const auto countGlobal = reinterpret_cast<std::uint32_t*>(a_image.base() + a_site.countGlobalRva);

		alignas(16) std::uint8_t buffer[0x100]{};

		*countGlobal = 1;  // 让遍历循环至少进入一次，触发位图读取

		// 必要性：位图基址为 -1 且未打补丁时，原函数会解引用 -1 崩
		*bitmapGlobal = 0xFFFFFFFFFFFFFFFFull;
		Check(FaultedFn(function), "unpatched: -1 bitmap faults");

		const auto jump = Patches::MakeJump(a_site, a_image, a_fragment);
		std::memcpy(a_mapping.base + a_site.hook, jump.data(), jump.size());

		// 防御：位图基址为 -1 时安全返回 0，不再崩
		Check(!FaultedFn(function) && function() == 0, "patched: -1 bitmap returns 0 without fault");

		// 合法性：位图基址指向真实缓冲区时不阻断、不崩
		*bitmapGlobal = reinterpret_cast<std::uint64_t>(buffer);
		Check(!FaultedFn(function), "patched: valid bitmap returns without fault");
	}

	// H：BuffFX 上游钳容。片段是 37 字节固定机器码，不引用任何地址，因此可以放进独立缓冲区
	// 直接执行，验证它算出的新容量，而不必真的去调用游戏的 _Emplace_reallocate。
	void TestBuffFXCap(const Patches::InPlacePatch& a_patch, const Patches::Image& a_image, Mapping& a_mapping)
	{
		std::printf("H hook=%x cover=%u\n", a_patch.hook, a_patch.cover);

		Check(a_patch.cover == 37, "H: capacity block is 37 bytes");
		Check(a_patch.function != 0 && a_patch.function < a_patch.hook, "H: capacity block is inside its anchor function");

		// 原始形态：块首是 mov rax, rsi（rsi = max_size），末尾是 mov rsi, rcx
		const auto* original = a_image.bytes(a_patch.hook);
		Check(original[0] == 0x48 && original[1] == 0x8B && original[2] == 0xC6, "H: block starts with mov rax, rsi");
		Check(original[34] == 0x48 && original[35] == 0x8B && original[36] == 0xF1, "H: block ends with mov rsi, rcx");

		std::uint8_t fragment[64]{};
		const auto   size = Patches::EmitInPlace(a_patch, fragment);
		Check(size == a_patch.cover, "H: fragment is exactly as long as the block it replaces");

		// 片段必须与工具包 build_min_cap_patch(256) 逐字节一致，避免后续改动悄悄偏离参考实现
		const std::uint8_t reference[37] = {
			0x48, 0x8D, 0x04, 0x0A, 0x49, 0x8B, 0xF6, 0x49, 0x3B, 0xC6, 0x48, 0x0F, 0x43, 0xF0,
			0xB8, 0x00, 0x01, 0x00, 0x00, 0x48, 0x3B, 0xF0, 0x48, 0x0F, 0x42, 0xF0,
			0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x90,
		};
		Check(std::memcmp(fragment, reference, sizeof(reference)) == 0, "H: fragment matches the toolkit's 37-byte clamp");

		// 独立执行片段：按 _Emplace_reallocate 的现场摆好 rsi（max_size）、rdx（capacity）、
		// rcx（capacity/2）、r14（size+1），读完 rsi 就返回。
		auto* scratch = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!scratch) {
			Check(false, "H: scratch page allocated");
			return;
		}

		alignas(16) static std::uint64_t s_capacity = 0;

		const auto putU64 = [](std::uint8_t* a_p, std::uint64_t a_value) {
			std::memcpy(a_p, std::addressof(a_value), 8);
			return a_p + 8;
		};

		const auto run = [&](std::uint64_t a_capacity, std::uint64_t a_size) -> std::uint64_t {
			std::uint8_t* p = scratch;
			*p++ = 0x56;                                                   // push rsi
			*p++ = 0x41; *p++ = 0x56;                                      // push r14
			*p++ = 0x48; *p++ = 0xBE; p = putU64(p, 0x0AAAAAAAAAAAAAAAull);  // mov rsi, max_size
			*p++ = 0x48; *p++ = 0xBA; p = putU64(p, a_capacity);            // mov rdx, capacity
			*p++ = 0x48; *p++ = 0x89; *p++ = 0xD1;                          // mov rcx, rdx
			*p++ = 0x48; *p++ = 0xD1; *p++ = 0xE9;                          // shr rcx, 1
			*p++ = 0x49; *p++ = 0xBE; p = putU64(p, a_size + 1);            // mov r14, size + 1
			std::memcpy(p, fragment, size);
			p += size;
			*p++ = 0x48; *p++ = 0xB8; p = putU64(p, reinterpret_cast<std::uint64_t>(std::addressof(s_capacity)));  // mov rax, &out
			*p++ = 0x48; *p++ = 0x89; *p++ = 0x30;                          // mov [rax], rsi
			*p++ = 0x41; *p++ = 0x5E;                                      // pop r14
			*p++ = 0x5E;                                                   // pop rsi
			*p++ = 0xC3;                                                   // ret

			::FlushInstructionCache(::GetCurrentProcess(), scratch, static_cast<std::size_t>(p - scratch));
			__try {
				reinterpret_cast<void (*)()>(scratch)();
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
			return s_capacity;
		};

		struct Case
		{
			std::uint64_t capacity;
			std::uint64_t size;
			std::uint64_t want;
			const char*   what;
		};

		const Case cases[] = {
			{ 0, 0, 256, "an empty vector jumps straight to 256" },
			{ 0, 1, 256, "capacity 0 with one element jumps to 256" },
			{ 1, 1, 256, "capacity 1 jumps straight to 256" },
			{ 100, 100, 256, "1.5x below the floor is raised to 256" },
			{ 170, 170, 256, "1.5x of 170 stays under the floor" },
			{ 171, 171, 256, "1.5x of 171 reaches the floor exactly" },
			{ 172, 172, 258, "1.5x just above the floor is kept" },
			{ 200, 200, 300, "1.5x above the floor is kept" },
			{ 256, 300, 384, "growth from the floor is unchanged" },
			{ 1000, 5000, 5001, "a larger new size wins over 1.5x" },
			{ 1, 5000, 5001, "a larger new size wins over the floor" },
		};

		for (const auto& c : cases) {
			const auto got = run(c.capacity, c.size);
			const char* name = c.what;
			Check(got == c.want && got >= c.size + 1, name);
		}

		// 落地：写到映射映像上必须一字不差，并且再定位时必须报「已打过」
		const std::uint8_t saved[37] = { original[0], original[1], original[2], original[3], original[4], original[5],
			original[6], original[7], original[8], original[9], original[10], original[11], original[12], original[13],
			original[14], original[15], original[16], original[17], original[18], original[19], original[20], original[21],
			original[22], original[23], original[24], original[25], original[26], original[27], original[28], original[29],
			original[30], original[31], original[32], original[33], original[34], original[35], original[36] };

		Patches::ApplyInPlace(a_patch, a_image.base());
		Check(std::memcmp(a_mapping.base + a_patch.hook, fragment, size) == 0, "H: ApplyInPlace writes the fragment");

		const auto applied = Patches::Locate(a_image);
		Check(applied.sites.empty() && applied.directs.empty() && applied.inplaces.empty() &&
				applied.errors.size() == 1 && applied.errors.front().starts_with("H ("),
			"H: a clamped block is reported instead of patched twice");

		// 回滚，后续的定位测试仍然要看到原始字节
		std::memcpy(a_mapping.base + a_patch.hook, saved, sizeof(saved));
		::FlushInstructionCache(::GetCurrentProcess(), a_mapping.base + a_patch.hook, sizeof(saved));
		Check(std::memcmp(a_image.bytes(a_patch.hook), saved, sizeof(saved)) == 0, "H: image is restored after the in-place test");

		// 反向：块内改一个字节，H 必须定位失败，而一处失败就等于整体不打补丁
		auto* const block = a_mapping.base + a_patch.hook;
		block[3] ^= 0xFF;
		const auto broken = Patches::Locate(a_image);
		Check(broken.sites.empty() && broken.directs.empty() && broken.inplaces.empty() && broken.errors.size() == 1,
			"H: a corrupted capacity block cancels every patch");
		block[3] ^= 0xFF;

		::VirtualFree(scratch, 0, MEM_RELEASE);
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
	Check(plan.errors.empty() && plan.sites.size() == 6 && plan.inplaces.size() == 1,
		"all six sites and the BuffFX capacity block are located");
	if (plan.sites.size() != 6 || plan.inplaces.size() != 1) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}

	if (plan.directs.empty()) {
		Check(false, "widget string direct patch (G) is located");
	} else {
		TestWidgetString(plan.directs.front(), image);
	}

	TestBuffFXCap(plan.inplaces.front(), image, mapping);

	std::puts("fragments:");
	std::uint8_t* buffFX = nullptr;
	std::uint8_t* skillBitmap = nullptr;
	for (std::size_t i = 0; i < plan.sites.size(); ++i) {
		const auto& site = plan.sites[i];
		const auto  rva = mapping.fragments + static_cast<std::uint32_t>(i * Patches::FragmentSize);
		auto*       fragment = mapping.base + rva;
		const auto  size = Patches::Emit(site, image, fragment);
		PrintFragment(site, rva, fragment, size);
		if (site.key == 'E') {
			buffFX = fragment;
		} else if (site.key == 'F') {
			skillBitmap = fragment;
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

	// 按 key 取定位结果，不依赖 Fixes 数组的顺序
	const auto findSite = [&](char a_key) -> const Patches::Site& {
		const auto it = std::find_if(plan.sites.begin(), plan.sites.end(),
			[&](const Patches::Site& a_site) { return a_site.key == a_key; });
		return *it;
	};

	// 只要一处对不上就整体放弃
	const auto& teleport = findSite('D');
	mapping.base[teleport.hook] ^= 0xFF;
	const auto broken = Patches::Locate(image);
	Check(broken.sites.empty() && broken.directs.empty() && broken.errors.size() == 1, "one mismatching site cancels every patch");
	mapping.base[teleport.hook] ^= 0xFF;

	const auto limit = *reinterpret_cast<const std::uint64_t*>(buffFX + 7);
	Check(limit == 0x00007FFFFFFFFFFF, "BuffFX guard uses the user address limit");

	TestBuffFX(findSite('E'), image, mapping, buffFX);

	TestSkillBitmap(findSite('F'), image, mapping, skillBitmap);

	std::printf("%d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
