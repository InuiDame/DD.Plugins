// 修复方案来自局师傅的《暗黑地牢》64 位主程序通用修复补丁工具包 v3，这里改为在内存中打补丁。
// 工具包的 A~E 对应这里的 A~E（跳板补丁），工具包的 F（BuffFX 上游钳容）对应这里的 H。
#include "Patches.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <Windows.h>

#include <xbyak/xbyak.h>

namespace Patches
{
	namespace
	{
		using namespace std::literals;

		struct LocateError : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		// ===================== 特征码 =====================
		constexpr auto HeirloomSwitchPattern = "85 ?? 74 ?? 83 ?? 02 74 ?? 83 ?? 01 74 ?? 83 ?? 01 75 ??"sv;
		constexpr auto DirtyFlagPattern =
			"80 B9 ?? 00 00 00 00 "  // cmp     byte ptr [rcx+disp], 0
			"74 ?? "                 // je      short lea
			"48 83 C1 38 "           // add     rcx, 38h
			"E8 ?? ?? ?? ?? "        // call    sub
			"C6 ?? ?? ?? ?? ?? ?? "  // mov     byte ptr [reg+disp], 0
			"48 8D ?? 38"sv;         // lea     rc?, [reg+38h]
		constexpr auto TeleportPattern =
			"48 8B 09 "     // mov     rcx, [rcx]
			"44 39 41 ?? "  // cmp     [rcx+disp], r8d
			"0F 85"sv;      // jne     fail
		constexpr auto TeleportPrefixedPattern = "45 33 ?? 48 8B 09 44 39 41 ?? 0F 85"sv;
		constexpr auto BuffFXPattern =
			"48 8B 41 08 "           // mov     rax, [rcx+8]
			"C6 40 ?? 01 "           // mov     byte ptr [rax+disp], 1
			"48 8B 41 08 "           // mov     rax, [rcx+8]
			"48 8B 48 08 "           // mov     rcx, [rax+8]
			"C6 81 ?? ?? ?? ?? ?? "  // mov     byte ptr [rcx+disp], 1
			"C3"sv;                  // retn
		constexpr std::uint32_t BuffFXHookOffset = 16;  // 落在 mov byte ptr [rcx+disp32], 1 这条写脏标记指令

		// ===================== G：widget 字符串池溢出断言 =====================
		// AllocateWidgetString 把界面字符串写进一个固定 32KB（0x8000）的全局池，池满时触发
		// "Out of widget string" 断言（gui.cpp:1276）。工会 / 英雄技能过多时池会耗尽。
		// 池是 .data 里的静态数组，被唯一一条 `lea rcx,[rip]` 引用。把这条 lea 翻成 mov（同一 rip
		// 位移，改为读取值），并在数组首 8 字节写入一个更大的堆缓冲区指针，再把容量上限一起抬高，
		// 即可在不触碰游戏其余逻辑的前提下消除断言。
		constexpr auto WidgetStringCapPattern =
			"44 8B 05 ?? ?? ?? ?? "  // mov     r8d, [rip+disp]   s_widget_strings_allocation_index
			"41 FF C0 "              // inc     r8d
			"44 03 C7 "              // add     r8d, edi           + 源串长度
			"41 81 F8 00 80 00 00 "  // cmp     r8d, 0x8000        <- 容量上限
			"72 40"sv;               // jb      short 分配分支
		constexpr auto WidgetStringLeaPattern =
			"48 63 1D ?? ?? ?? ?? "  // movsxd  rbx, [rip+disp]     s_widget_strings_allocation_index
			"48 8D 0D ?? ?? ?? ?? "  // lea     rcx, [rip+disp]     池基址（HOOK：翻转 0x8D->0x8B）
			"48 03 D9"sv;            // add     rbx, rcx            rbx = 池基址 + 游标

		constexpr std::uint32_t WidgetStringOriginalPoolSize = 0x8000;  // 原池：32KB 静态数组
		constexpr std::uint32_t WidgetStringPoolSize = 0x100000;         // 新池 / 新容量上限：1MB

		// ===================== H：BuffFX 上游防扩容搬家 =====================
		// ActorFx::entries 是 std::vector<ActiveFxEntry>（元素 0x18 字节），从容量 0 起步、按 1.5 倍增长。
		// 每次扩容都要搬家，ActorFx::Update 之前注册进来的「&entries[i]」随即变成悬垂指针——这是 E 点
		// 所兜底的那类脏标记写入崩溃的上游根源。这里改上游：把 _Emplace_reallocate 算出的新容量下限
		// 钳到 256（6144 B = 0x1800），首次扩容就一次拿到足够容量，单次 Update 的注册循环里不再扩容；
		// 超过 256 个之后仍会按 1.5 倍扩容，所以 E 点的下游守卫必须保留，H 不能取代它。
		//
		// 该实例与其它 vector<0x18> 共用 COMDAT，所以用两把锁定位：ActorFx::Update 内唯一的 push_back
		// 慢路径 call 作上游锚点，call 目标函数内的 37 字节容量块作下游锁，并要求完整容量块在整个
		// .text 里只命中一处。两者一致才动手，任何一环不符就整体放弃。
		//
		// 注意容量块本身并不唯一：any vector<T>::_Emplace_reallocate 的「1.5 倍增长」都是同样的
		// 37 字节，全程序能命中 23 处。真正把它区别开来的是紧随其后那条 0x18 步长后缀
		// （lea rax,[rsi+rsi*2] / lea r13,[rax*8]），所以特征码把后缀一起带上，只认 0x18 这一个实例。
		constexpr auto BuffFXCapPattern =
			"48 8B C6 "           // mov     rax, rsi          ; max_size
			"48 2B C2 "           // sub     rax, rdx          ; max_size - cap/2
			"48 3B C8 "           // cmp     rcx, rax          ; cap
			"77 1A "              // ja      capacity          ; 1.5 倍会溢出，退回 max_size
			"48 8D 04 0A "        // lea     rax, [rdx+rcx]    ; 1.5 * cap
			"49 8B CE "           // mov     rcx, r14          ; size + 1
			"49 3B C6 "           // cmp     rax, r14
			"48 0F 43 C8 "        // cmovae  rcx, rax          ; max(1.5cap, size+1)
			"48 3B CE "           // cmp     rcx, rsi
			"0F 87 ?? ?? ?? ?? "  // ja      length_error
			"48 8B F1 "           // mov     rsi, rcx          ; 新容量
			"48 8D 04 76 "        // lea     rax, [rsi+rsi*2]  ; 容量 * 3
			"4C 8D 2C C5 00 00 00 00"sv;  // lea r13, [rax*8]  ; 元素 0x18 字节
		constexpr std::uint32_t BuffFXCapSize = 37;         // 被替换字节块的长度
		constexpr std::uint32_t BuffFXStrideSize = 12;      // 块后的 0x18 步长后缀

		// 已应用过 H 的形态（片段前 14 字节），只用于给出明确诊断，不参与正常定位
		constexpr auto BuffFXCapAppliedPattern = "48 8D 04 0A 49 8B F6 49 3B C6 48 0F 43 F0"sv;

		// 上游锚点：push_back 慢路径里「复制新元素 -> 推进 _Mylast -> 调用 _Emplace_reallocate」的形态。
		// add qword [..], 18h 两种寻址宽度长度不同，mov rcx 也有 rdi / r12 两种，故拆段匹配。
		constexpr auto BuffFXPushHead =
			"0F 10 44 24 ?? "     // movups  xmm0, [rsp+?]
			"0F 11 02 "           // movups  [rdx], xmm0
			"F2 0F 10 4C 24 ?? "  // movsd   xmm1, [rsp+?]
			"F2 0F 11 4A 10"sv;   // movsd   [rdx+10h], xmm1
		constexpr auto BuffFXPushAddShort = "48 83 47 08 18"sv;          // add qword [rdi+8], 18h
		constexpr auto BuffFXPushAddLong = "48 83 87 08 01 00 00 18"sv;  // add qword [rdi+108h], 18h
		constexpr std::uint32_t BuffFXPushHeadSize = 19;
		constexpr std::uint32_t BuffFXPushCallOffset = 10;  // 「EB 0D + lea r8 + mov rcx」之后是 call

		// H 的原地片段：与容量块等长（37 字节），rsi = max(1.5 * cap, size + 1, BuffFXMinCapacity)，
		// 之后照原样进入「容量 * 0x18 = 字节数」的后缀并调用游戏原版 operator new。
		// 与原块相比去掉了两段 max_size 溢出检查——正常容量范围内不可达，代价换来容量下限。
		constexpr std::array<std::uint8_t, BuffFXCapSize> BuffFXCapFragment{ {
			0x48, 0x8D, 0x04, 0x0A,                                      // lea     rax, [rdx+rcx]  ; 1.5 * cap
			0x49, 0x8B, 0xF6,                                            // mov     rsi, r14        ; size + 1
			0x49, 0x3B, 0xC6,                                            // cmp     rax, r14
			0x48, 0x0F, 0x43, 0xF0,                                      // cmovae  rsi, rax        ; max(1.5cap, size+1)
			0xB8,
			static_cast<std::uint8_t>(BuffFXMinCapacity & 0xFF),         // mov     eax, 容量下限
			static_cast<std::uint8_t>((BuffFXMinCapacity >> 8) & 0xFF),
			static_cast<std::uint8_t>((BuffFXMinCapacity >> 16) & 0xFF),
			static_cast<std::uint8_t>((BuffFXMinCapacity >> 24) & 0xFF),
			0x48, 0x3B, 0xF0,                                            // cmp     rsi, rax
			0x48, 0x0F, 0x42, 0xF0,                                      // cmovb   rsi, rax        ; max(..., 容量下限)
			0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,        // 9 字节 nop
			0x66, 0x90,                                                  // 2 字节 nop，合计 37 字节
		} };
		static_assert(BuffFXMinCapacity == 256, "the 37-byte block only fits a 256 capacity floor");
	
		// 前置声明：本段 G 定位函数早于下方字节读取与特征码搜索助手定义，先行声明以保持自包含
		template <class T>
		T Read(const std::uint8_t* a_bytes) noexcept;
		const std::uint8_t* Bytes(const Image& a_image, std::uint32_t a_rva, std::uint32_t a_size);
		std::uint32_t FindOne(const Image& a_image, std::string_view a_pattern);

		// 统计 .text 里把 a_target 当作 [rip+disp] 操作数的指令条数，用于确认池基址只被引用一次
		std::uint32_t CountRipRefs(const Image& a_image, std::uint32_t a_target)
		{
			constexpr std::uint32_t Ops[] = { 0x8B, 0x8D, 0x89, 0x8A, 0x03, 0x2B, 0x23, 0x0B, 0xC7 };
			std::uint32_t count = 0;
			auto          rva = a_image.textBegin();
			const auto    end = a_image.textEnd();
			while (rva + 7 <= end) {
				const auto* c = a_image.bytes(rva);
				std::uint32_t p = 0;
				if (c[0] >= 0x40 && c[0] <= 0x4F) {
					if (rva + 8 > end) {
						break;
					}
					p = 1;
				}
				bool  hit = false;
				for (const auto op : Ops) {
					if (c[p] != op) {
						continue;
					}
					const auto modrm = c[p + 1];
					if ((modrm & 0xC7) != 0x05) {
						break;  // 不是 [rip]，长度至少 2
					}
					const auto disp = Read<std::int32_t>(c + p + 2);
					const auto target = rva + p + 1 + 1 + 4 + static_cast<std::uint32_t>(disp);
					if (target == a_target) {
						++count;
					}
					hit = true;
					break;
				}
				rva += hit ? (p + 1 + 1 + 4) : 1;
			}
			return count;
		}

		DirectPatch LocateWidgetString(const Image& a_image)
		{
			const auto cap = FindOne(a_image, WidgetStringCapPattern);
			const auto lea = FindOne(a_image, WidgetStringLeaPattern);

			// 两条特征码必须在同一函数内，且 lea 在 cap 之后
			if (lea <= cap || lea - cap > 0x200) {
				throw LocateError("widget string cap and pool lea are not in the same function");
			}

			const auto leaStart = lea + 7;  // "48 8D 0D" 起点
			const auto* code = Bytes(a_image, leaStart, 7);
			if (code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x0D) {
				throw LocateError("pool base load is not a rip-relative lea rcx");
			}

			const auto  disp = Read<std::int32_t>(code + 3);
			const auto  bufferGlobalRva = leaStart + 7 + static_cast<std::uint32_t>(disp);
			const auto  leaOpcodeRva = leaStart + 1;  // 0x8D 字节

			// 紧随其前的 "48 63 1D disp32" 是 movsxd rbx, [rip+索引全局]，同样以 7 字节指令解出
			const auto indexGlobalRva = lea + 7 + static_cast<std::uint32_t>(Read<std::int32_t>(Bytes(a_image, lea + 3, 4)));

			if (CountRipRefs(a_image, bufferGlobalRva) != 1) {
				throw LocateError("pool base is referenced by more than one instruction");
			}

			// 池基址必须落在可写的 .data 节内，且原 32KB 池完整位于该节，避免指向代码或越界
			if (bufferGlobalRva < a_image.dataBegin() || bufferGlobalRva + WidgetStringOriginalPoolSize > a_image.dataEnd()) {
				throw LocateError("pool base is not fully inside a writable data section");
			}
			if (indexGlobalRva < a_image.dataBegin() || indexGlobalRva + 4 > a_image.dataEnd()) {
				throw LocateError("widget string index is not inside a writable data section");
			}

			const auto capImmRva = cap + 16;  // "44 8B 05.. / 41 FF C0 / 44 03 C7 / 41 81 F8" 之后才是 imm32 起点

			DirectPatch patch;
			patch.key = 'G';
			patch.name = "widgetstring";
			patch.leaOpcodeRva = leaOpcodeRva;
			patch.capImmRva = capImmRva;
			patch.bufferGlobalRva = bufferGlobalRva;
			patch.indexGlobalRva = indexGlobalRva;
			patch.bufferSize = WidgetStringPoolSize;
			return patch;
		}

		// ===================== F：英雄技能位图遍历崩溃 =====================
		// 工会 / 英雄页打开时，游戏按全局载入的技能位图基址逐 DWORD 查位，判定某技能是否可用。
		// 当该全局被写成 -1（STL 失效迭代器哨兵，mod 英雄技能过多时触发）时，test [rdx], eax
		// 直接解引用 -1 触发 AV。函数入口载入该基址，进入即校验其是否为合法用户态指针，
		// 非法（0 / 负数 / 过低）时经真实 ret 安全返回 0（该技能位判定为“未命中”）。
		// 注意：被覆盖的 mov 是 rip 相对寻址，不能在片段里原样重放，必须改为绝对地址载入。
		constexpr auto SkillBitmapPattern =
			"4C 8B 0D ?? ?? ?? ?? "  // mov     r9, [rip+disp]   技能计数 / 索引全局
			"33 C9 "                 // xor     ecx, ecx
			"48 8B 15 ?? ?? ?? ?? "  // mov     rdx, [rip+disp]  技能位图基址全局（HOOK 起点）
			"41 B0 01 "              // mov     r8b, 1
			"4D 85 C9 "              // test    r9, r9
			"79 ??"sv;               // jns     short
		constexpr std::uint32_t SkillBitmapHookOffset = 9;  // 相对 AOB 起点的偏移，落在 mov rdx,[rip]

		constexpr std::uint32_t SectionHeaderSize = 40;
		constexpr std::uint32_t FunctionBackLimit = 0x4000;
		constexpr std::uint32_t FunctionForwardLimit = 0x4000;

		// 用户态地址的上限和最低的有效地址
		constexpr std::uint64_t UserAddressLimit = 0x00007FFFFFFFFFFF;
		constexpr std::uint32_t MinValidAddress = 0x10000;

		constexpr std::array ExternalPatchSections{ ".dxfx"sv, ".dxfs"sv, ".zaas"sv, ".zaas2"sv, ".zaas3"sv };

		// ===================== 字节读取 =====================
		template <class T>
		T Read(const std::uint8_t* a_bytes) noexcept
		{
			T value;
			std::memcpy(std::addressof(value), a_bytes, sizeof(T));
			return value;
		}

		std::string_view SectionName(const std::uint8_t* a_header) noexcept
		{
			const auto* name = reinterpret_cast<const char*>(a_header);
			return { name, ::strnlen(name, 8) };
		}

		bool InText(const Image& a_image, std::uint32_t a_rva, std::uint32_t a_size) noexcept
		{
			return a_rva >= a_image.textBegin() && a_rva < a_image.textEnd() && a_size <= a_image.textEnd() - a_rva;
		}

		const std::uint8_t* Bytes(const Image& a_image, std::uint32_t a_rva, std::uint32_t a_size)
		{
			// 不能写成 a_rva + a_size > textEnd：a_rva 接近 4GB 时会回绕
			if (!InText(a_image, a_rva, a_size)) {
				throw LocateError(std::format("{:X}+{:X} is outside .text", a_rva, a_size));
			}

			return a_image.bytes(a_rva);
		}

		int RexR(std::uint8_t a_rex) noexcept { return (a_rex & 4) ? 8 : 0; }
		int RexB(std::uint8_t a_rex) noexcept { return (a_rex & 1) ? 8 : 0; }
		int ModrmMod(std::uint8_t a_modrm) noexcept { return a_modrm >> 6; }
		int ModrmReg(std::uint8_t a_modrm) noexcept { return (a_modrm >> 3) & 7; }
		int ModrmRm(std::uint8_t a_modrm) noexcept { return a_modrm & 7; }

		// rax、rcx、rdx、r8~r11 不能跨调用保存值
		bool IsVolatile(int a_reg) noexcept
		{
			return a_reg <= 2 || (a_reg >= 8 && a_reg <= 11);
		}

		// ===================== 特征码搜索 =====================
		std::vector<int> ParsePattern(std::string_view a_pattern)
		{
			std::vector<int> bytes;
			for (std::size_t i = 0; i + 1 < a_pattern.size();) {
				if (a_pattern[i] == ' ') {
					++i;
					continue;
				}

				const auto token = a_pattern.substr(i, 2);
				int        value = -1;
				if (token != "??"sv) {
					std::from_chars(token.data(), token.data() + 2, value, 16);
				}
				bytes.push_back(value);
				i += 2;
			}

			return bytes;
		}

		// 通配符较多，逐字节比对比 KMP 更稳妥。命中之间互不重叠
		std::vector<std::uint32_t> FindAll(const Image& a_image, std::string_view a_pattern)
		{
			const auto pattern = ParsePattern(a_pattern);
			const auto size = static_cast<std::uint32_t>(pattern.size());

			std::vector<std::uint32_t> hits;
			for (auto rva = a_image.textBegin(); rva + size <= a_image.textEnd();) {
				const auto* code = a_image.bytes(rva);

				std::uint32_t i = 0;
				while (i < size && (pattern[i] < 0 || pattern[i] == code[i])) {
					++i;
				}

				if (i == size) {
					hits.push_back(rva);
					rva += size;
				} else {
					++rva;
				}
			}

			return hits;
		}

		std::uint32_t FindOne(const Image& a_image, std::string_view a_pattern)
		{
			const auto hits = FindAll(a_image, a_pattern);
			if (hits.size() != 1) {
				throw LocateError(std::format("pattern matched {} times", hits.size()));
			}

			return hits.front();
		}

		// 在指定位置按特征码匹配（不扫描全段），用于「固定头 + 变长中段 + 固定尾」的分段匹配
		bool MatchesAt(const Image& a_image, std::uint32_t a_rva, std::string_view a_pattern)
		{
			const auto pattern = ParsePattern(a_pattern);
			const auto size = static_cast<std::uint32_t>(pattern.size());
			if (!InText(a_image, a_rva, size)) {
				return false;
			}

			const auto* code = a_image.bytes(a_rva);
			for (std::uint32_t i = 0; i < size; ++i) {
				if (pattern[i] >= 0 && pattern[i] != code[i]) {
					return false;
				}
			}

			return true;
		}

		// 函数之间以 int3 填充对齐，填充段之后的第一个字节就是函数入口
		std::uint32_t FunctionStart(const Image& a_image, std::uint32_t a_rva)
		{
			const auto limit = a_rva > a_image.textBegin() + FunctionBackLimit ? a_rva - FunctionBackLimit : a_image.textBegin();

			auto k = a_rva - 1;
			while (k > limit && *a_image.bytes(k) == 0xCC) {
				--k;
			}
			if (k < a_rva - 1) {
				return k + 1;
			}

			for (auto j = k; j > limit; --j) {
				if (*a_image.bytes(j) == 0xCC && *a_image.bytes(j + 1) != 0xCC) {
					return j + 1;
				}
			}

			throw LocateError(std::format("no int3 padding before {:X}", a_rva));
		}

		// 函数结尾取入口之后第一处连续的 int3
		std::uint32_t FunctionEnd(const Image& a_image, std::uint32_t a_function)
		{
			const auto limit = std::min(a_function + FunctionForwardLimit, a_image.textEnd());
			for (auto rva = a_function; rva + 2 <= limit; ++rva) {
				if (*a_image.bytes(rva) == 0xCC && *a_image.bytes(rva + 1) == 0xCC) {
					return rva;
				}
			}

			return a_function + FunctionForwardLimit;
		}

		// ===================== A：自定义祖产货币回城结算崩溃 =====================
		// 结算函数按 [obj+disp] 处的 type 走 4 路 switch，type 为 2、3 时把第二个参数 val 当作
		// 货币数据的指针解引用。自定义货币结算时 val 为空。读取 type 前判空，为空时把 type 写成 0，
		// 走取常量 0 的安全分支。

		bool HasDereference(const std::uint8_t* a_code, std::size_t a_size, int a_reg) noexcept
		{
			for (std::size_t i = 0; i < a_size; ++i) {
				auto         p = i;
				std::uint8_t rex = 0;
				if (a_code[p] >= 0x40 && a_code[p] <= 0x4F) {
					rex = a_code[p++];
				}
				if (p >= a_size) {
					return false;
				}

				const auto op = a_code[p++];
				if (op == 0x0F && p < a_size && (a_code[p] == 0xB6 || a_code[p] == 0xB7 || a_code[p] == 0xBE || a_code[p] == 0xBF)) {
					++p;
				} else if (op != 0x8B && op != 0x8A && op != 0x3B && op != 0x3A && op != 0x39 && op != 0x38 && op != 0x8D) {
					continue;
				}
				if (p >= a_size) {
					return false;
				}

				const auto modrm = a_code[p];
				if (ModrmMod(modrm) != 0 || ModrmRm(modrm) == 4 || ModrmRm(modrm) == 5) {
					continue;
				}
				if ((ModrmRm(modrm) | RexB(rex)) == a_reg) {
					return true;
				}
			}

			return false;
		}

		// test rcx, rcx / je / mov rax, [rcx] / 至多 8 字节 / call [rax]
		bool HasGuardedVirtualCall(const std::uint8_t* a_code, std::size_t a_size) noexcept
		{
			constexpr std::size_t Head = 8;
			constexpr std::size_t MaxGap = 8;

			for (std::size_t i = 0; i + Head + 2 <= a_size; ++i) {
				if (std::memcmp(a_code + i, "\x48\x85\xC9\x74", 4) != 0 || std::memcmp(a_code + i + 5, "\x48\x8B\x01", 3) != 0) {
					continue;
				}

				for (std::size_t gap = 0; gap <= MaxGap && i + Head + gap + 2 <= a_size; ++gap) {
					if (a_code[i + Head + gap] == 0xFF && a_code[i + Head + gap + 1] == 0x10) {
						return true;
					}
				}
			}

			return false;
		}

		Site TryHeirloom(const Image& a_image, std::uint32_t a_switch)
		{
			// switch 的四条指令针对同一个寄存器
			const auto* sw = Bytes(a_image, a_switch, 19);
			const auto  reg = sw[1] & 7;
			if (sw[1] != (0xC0 | (reg << 3) | reg) || sw[5] != (0xE8 | reg) || sw[10] != (0xE8 | reg) || sw[15] != (0xF8 | reg)) {
				throw LocateError("switch does not use a single register");
			}

			// 只接受 mov ecx, [obj+disp8] / test ecx, ecx，形态不同说明不是目标版本
			const auto  hook = a_switch - 3;
			const auto* mov = Bytes(a_image, hook, 5);
			if (mov[0] != 0x8B || ModrmMod(mov[1]) != 1 || ModrmReg(mov[1]) != 1 || ModrmRm(mov[1]) == 4 || mov[2] > 0x7F ||
				mov[3] != 0x85 || mov[4] != 0xC9) {
				throw LocateError("unexpected read of the type field");
			}

			// 函数序言把第二个参数 rdx 存进了 val
			const auto  function = FunctionStart(a_image, hook);
			const auto* prolog = Bytes(a_image, function, 0x60);
			int         valueReg = -1;
			for (std::size_t i = 0; i + 3 <= 0x60 && valueReg < 0;) {
				if (prolog[i] < 0x40 || prolog[i] > 0x4F || prolog[i + 1] != 0x8B || prolog[i + 2] < 0xC0) {
					++i;
					continue;
				}

				if ((ModrmRm(prolog[i + 2]) | RexB(prolog[i])) == 2) {
					valueReg = ModrmReg(prolog[i + 2]) | RexR(prolog[i]);
				}
				i += 3;
			}

			const auto objectReg = ModrmRm(mov[1]);
			if (valueReg < 0 || IsVolatile(valueReg) || IsVolatile(objectReg)) {
				throw LocateError("value or object register is not preserved");
			}

			// switch 的分支里确实会解引用 val
			if (!HasDereference(Bytes(a_image, a_switch, 0x40), 0x40, valueReg)) {
				throw LocateError("switch does not dereference the value");
			}

			Site site;
			site.function = function;
			site.hook = hook;
			site.cover = 5;
			site.objectReg = objectReg;
			site.valueReg = valueReg;
			site.typeOffset = mov[2];
			return site;
		}

		Site LocateHeirloom(const Image& a_image)
		{
			std::vector<Site> candidates;
			for (const auto hit : FindAll(a_image, HeirloomSwitchPattern)) {
				try {
					candidates.push_back(TryHeirloom(a_image, hit));
				} catch (const LocateError&) {
					continue;
				}
			}

			if (candidates.empty()) {
				throw LocateError("no matching settlement function");
			}
			if (candidates.size() == 1) {
				return candidates.front();
			}

			// 结算函数会克隆货币定义，函数体内有一处被判空保护的虚调用
			std::vector<Site> strong;
			for (const auto& candidate : candidates) {
				const auto size = std::min(FunctionEnd(a_image, candidate.function) - candidate.function, 0x800u);
				if (HasGuardedVirtualCall(Bytes(a_image, candidate.function, size), size)) {
					strong.push_back(candidate);
				}
			}

			if (strong.size() != 1) {
				throw LocateError(std::format("{} functions match, cannot pick one", candidates.size()));
			}

			return strong.front();
		}

		// ===================== B：无主 ActorDot 崩溃（part 1）=====================
		// 遍历 DoT 列表的函数不判空就读 [rcx+disp]，施法者离场后读档时 rcx 为空。入口判空，
		// 为空时走函数自己的「记录日志并返回 false」出口。日志要用保存第二个参数的寄存器，
		// 所以把这条赋值提到判空之前。

		bool IsMovR64FromMemory(const std::uint8_t* a_code) noexcept
		{
			return (a_code[0] == 0x48 || a_code[0] == 0x4C) && a_code[1] == 0x8B && a_code[2] >= 0x40 && a_code[2] <= 0x7F;
		}

		// mov rdx, r<log> / lea rcx, [rip+disp] / call / xor al, al，返回相对 a_code 的偏移
		std::optional<std::size_t> FindLogExit(const std::uint8_t* a_code, std::size_t a_size) noexcept
		{
			constexpr std::size_t Length = 17;

			for (std::size_t i = 0; i + Length <= a_size; ++i) {
				const auto* p = a_code + i;
				if (p[0] == 0x48 && p[1] == 0x8B && p[2] >= 0xD0 && p[2] <= 0xD7 && p[3] == 0x48 && p[4] == 0x8D && p[5] == 0x0D &&
					p[10] == 0xE8 && p[15] == 0x32 && p[16] == 0xC0) {
					return i;
				}
			}

			return std::nullopt;
		}

		Site LocateNoOwner(const Image& a_image)
		{
			constexpr std::uint32_t TripleSize = 11;

			std::vector<Site> candidates;
			for (auto rva = a_image.textBegin(); rva + TripleSize <= a_image.textEnd();) {
				const auto* code = a_image.bytes(rva);

				// mov r64, [rcx+disp8] / mov r<log>, rdx / mov r64, [rcx+disp8]
				if (!IsMovR64FromMemory(code) || (code[4] != 0x48 && code[4] != 0x4C) || code[5] != 0x8B || code[6] < 0xC0 ||
					!IsMovR64FromMemory(code + 7)) {
					++rva;
					continue;
				}

				const auto current = rva;
				rva += TripleSize;

				if (ModrmRm(code[2]) != 1 || ModrmRm(code[6]) != 2) {
					continue;
				}

				const auto logReg = ModrmReg(code[6]) | RexR(code[4]);
				if (IsVolatile(logReg) && logReg != 2) {
					continue;
				}

				std::uint32_t function;
				try {
					function = FunctionStart(a_image, current);
				} catch (const LocateError&) {
					continue;
				}

				const auto end = FunctionEnd(a_image, function);
				if (current <= function || current >= end) {
					continue;
				}

				const auto  size = std::min(end - function, 0x3000u);
				const auto* body = Bytes(a_image, function, size);
				const auto  exit = FindLogExit(body, size);
				if (!exit || ModrmRm(body[*exit + 2]) != logReg) {
					continue;
				}

				Site site;
				site.function = function;
				site.hook = current;
				site.cover = 7;
				site.exit = function + static_cast<std::uint32_t>(*exit);
				candidates.push_back(site);
			}

			if (candidates.size() != 1) {
				throw LocateError(std::format("{} functions match", candidates.size()));
			}

			return candidates.front();
		}

		// ===================== C：ActorDot 脏标记崩溃（part 2）=====================
		// 函数直接读 [rcx+disp] 处的 DoT 脏标记，传送重返战斗后残留的 DoT 引用为空。判空，
		// 为空时跳过脏标记处理和紧随其后同样以该对象为基址的调用，进入只用第二个参数的清理段。

		Site LocateDirtyFlag(const Image& a_image)
		{
			const auto  base = FindOne(a_image, DirtyFlagPattern);
			const auto* code = Bytes(a_image, base, 29);

			const auto flagOffset = Read<std::int32_t>(code + 2);
			if (code[24] != 0x00 || Read<std::int32_t>(code + 20) != flagOffset) {
				throw LocateError("dirty flag read and clear do not match");
			}
			if (static_cast<std::int8_t>(code[8]) != 16) {
				throw LocateError("je does not reach the lea");
			}

			// lea 之后走过一次 call，落在以 mov rcx, [reg+38h] 开头的清理段上
			constexpr std::uint32_t SegmentSize = 0x20;

			const auto    lea = base + 25;
			const auto*   segment = Bytes(a_image, lea, SegmentSize);
			std::uint32_t cleanup = 0;
			for (std::uint32_t k = 0; k + 5 < SegmentSize && !cleanup; ++k) {
				if (segment[k] != 0xE8) {
					continue;
				}

				auto candidate = lea + k + 5;
				while (candidate < lea + SegmentSize && *Bytes(a_image, candidate, 1) == 0x90) {
					++candidate;
				}

				const auto* head = Bytes(a_image, candidate, 4);
				if (head[0] == 0x48 && head[1] == 0x8B && ModrmMod(head[2]) == 1 && ModrmReg(head[2]) == 1 && head[3] == 0x38) {
					cleanup = candidate;
				}
			}

			if (!cleanup) {
				throw LocateError("cleanup block not found");
			}

			const auto function = FunctionStart(a_image, base);
			if (cleanup <= function || cleanup >= FunctionEnd(a_image, function)) {
				throw LocateError("cleanup block is outside the function");
			}

			Site site;
			site.function = function;
			site.hook = base;
			site.cover = 7;
			site.exit = cleanup;
			return site;
		}

		// ===================== D：传送 ActorDot 崩溃 =====================
		// 传送判定解引用战斗对象后比较字段，传送重返同一场战斗、尚未读档时残留 DoT 的持有者为空。
		// 解引用后判空，为空时跳到紧随其后那条 jne 的真实目标，它会正确设置返回值。

		Site LocateTeleport(const Image& a_image)
		{
			auto hits = FindAll(a_image, TeleportPattern);
			if (hits.size() > 1) {
				if (const auto prefixed = FindAll(a_image, TeleportPrefixedPattern); prefixed.size() == 1) {
					hits = { prefixed.front() + 3 };
				}
			}
			if (hits.size() != 1) {
				throw LocateError(std::format("pattern matched {} times", hits.size()));
			}

			const auto  hook = hits.front();
			const auto* code = Bytes(a_image, hook, 13);
			const auto  fail = hook + 13 + Read<std::int32_t>(code + 9);

			const auto function = FunctionStart(a_image, hook);
			if (fail <= function || fail >= FunctionEnd(a_image, function)) {
				throw LocateError("failure branch is outside the function");
			}

			Site site;
			site.function = function;
			site.hook = hook;
			site.cover = 7;
			site.exit = fail;
			return site;
		}

		// ===================== E：BuffFX 叠加崩溃 =====================
		// 小函数把 buff 脏标记写到指针链末端，多重 BuffFX 叠加时中间环节可能为空或已失效。
		// 写入前校验地址，不合法时直接返回。

		Site LocateBuffFX(const Image& a_image)
		{
			const auto  base = FindOne(a_image, BuffFXPattern);
			const auto  hook = base + BuffFXHookOffset;
			const auto* code = Bytes(a_image, hook, 8);

			const auto offset = Read<std::int32_t>(code + 2);
			if (code[6] != 0x01 || offset < 0 || offset >= 0x10000) {
				throw LocateError("unexpected dirty flag write");
			}

			Site site;
			site.function = base;
			site.hook = hook;
			site.cover = 7;
			return site;
		}

	// ===================== F：技能位图基址 -1 防御 =====================
	Site TrySkillBitmap(const Image& a_image, std::uint32_t a_match)
	{
		// 只接受位于函数入口（前面是 int3 填充）的匹配；同一段逻辑会被内联进更大的函数，
		// 那些副本前面是 ret / 代码，不是函数边界，不能作为干净的 hook 点。
		if (*a_image.bytes(a_match - 1) != 0xCC) {
			throw LocateError("skill bitmap match is not at a function boundary");
		}

		const auto hook = a_match + SkillBitmapHookOffset;
		const auto* code = Bytes(a_image, hook, 7);
		if (code[0] != 0x48 || code[1] != 0x8B || code[2] != 0x15) {
			throw LocateError("skill bitmap base load is not a rip-relative mov rdx");
		}

		const auto disp = Read<std::int32_t>(code + 3);
		const auto bitmapGlobalRva = hook + 7 + static_cast<std::uint32_t>(disp);

		// 技能计数全局就在 mov rdx 前面的那条 mov r9 里，同样是 rip 相对
		const auto* head = Bytes(a_image, a_match, 7);
		if (head[0] != 0x4C || head[1] != 0x8B || head[2] != 0x0D) {
			throw LocateError("skill count load is not a rip-relative mov r9");
		}
		const auto countDisp = Read<std::int32_t>(head + 3);
		const auto countGlobalRva = a_match + 7 + static_cast<std::uint32_t>(countDisp);

		const auto function = FunctionStart(a_image, hook);
		const auto end = FunctionEnd(a_image, function);
		if (hook <= function || hook + 7 >= end) {
			throw LocateError("hook is outside the function");
		}

		// 确认函数体包含崩溃指令 test [rdx], eax（85 02）—— 这正是 minidump 里的崩点
		bool hasCrash = false;
		for (auto rva = hook; rva + 2 <= end && !hasCrash; ++rva) {
			if (*a_image.bytes(rva) == 0x85 && *(a_image.bytes(rva) + 1) == 0x02) {
				hasCrash = true;
			}
		}
		if (!hasCrash) {
			throw LocateError("function does not contain the skill-bit test that crashes");
		}

		// 用函数真实出口（末尾的 ret）作为安全返回点，对有无序言都安全
		std::uint32_t exit = 0;
		for (auto rva = end; rva > function; --rva) {
			if (*a_image.bytes(rva - 1) == 0xC3) {
				exit = rva - 1;
				break;
			}
		}
		if (!exit || exit <= hook) {
			throw LocateError("function has no ret to return through");
		}

		Site site;
		site.function = function;
		site.hook = hook;
		site.cover = 7;
		site.exit = exit;
		site.bitmapGlobalRva = bitmapGlobalRva;
		site.countGlobalRva = countGlobalRva;
		return site;
	}

	Site LocateSkillBitmap(const Image& a_image)
	{
		std::vector<Site> candidates;
		for (const auto match : FindAll(a_image, SkillBitmapPattern)) {
			try {
				candidates.push_back(TrySkillBitmap(a_image, match));
			} catch (const LocateError&) {
				continue;
			}
		}

		if (candidates.empty()) {
			throw LocateError("no matching skill bitmap function");
		}
		if (candidates.size() == 1) {
			return candidates.front();
		}

		// 仍有多个时（不应发生），取函数体最小的叶子函数，跳过内联进大函数的副本
		std::sort(candidates.begin(), candidates.end(),
			[](const Site& a, const Site& b) { return (a.exit - a.function) < (b.exit - b.function); });
		return candidates.front();
	}

	// ===================== H：BuffFX 上游防扩容搬家 =====================
	// 上游锚点唯一 + 下游 37 字节容量块唯一 + 周边形态自洽，三者同时成立才动手。
	std::optional<std::uint32_t> MatchBuffFXPush(const Image& a_image, std::uint32_t a_rva)
	{
		if (!MatchesAt(a_image, a_rva, BuffFXPushHead)) {
			return std::nullopt;
		}

		// add qword [rdi+8], 18h 与 add qword [rdi+108h], 18h 两种寻址宽度，长度不同
		auto p = a_rva + BuffFXPushHeadSize;
		if (MatchesAt(a_image, p, BuffFXPushAddShort)) {
			p += 5;
		} else if (MatchesAt(a_image, p, BuffFXPushAddLong)) {
			p += 8;
		} else {
			return std::nullopt;
		}

		// jmp short +0D / lea r8, [rsp+?] / mov rcx, (rdi|r12) / call rel32
		if (!InText(a_image, p, 15)) {
			return std::nullopt;
		}

		const auto* c = a_image.bytes(p);
		if (c[0] != 0xEB || c[1] != 0x0D || c[2] != 0x4C || c[3] != 0x8D || c[4] != 0x44 || c[5] != 0x24) {
			return std::nullopt;
		}

		const bool moveRcx = (c[7] == 0x48 && c[8] == 0x8B && c[9] == 0xCF) ||
			(c[7] == 0x49 && c[8] == 0x8B && c[9] == 0xCC);
		if (!moveRcx || c[10] != 0xE8) {
			return std::nullopt;
		}

		return p + BuffFXPushCallOffset;
	}

	InPlacePatch LocateBuffFXCap(const Image& a_image)
	{
		// 锁 1：上游锚点在 .text 里必须只有一处
		std::vector<std::uint32_t> calls;
		for (const auto rva : FindAll(a_image, BuffFXPushHead)) {
			if (const auto call = MatchBuffFXPush(a_image, rva)) {
				calls.push_back(*call);
			}
		}
		if (calls.size() != 1) {
			throw LocateError(std::format("{} buff FX push anchor(s) matched, expected exactly 1", calls.size()));
		}

		const auto callRva = calls.front();
		const auto emplace = callRva + 5 + static_cast<std::uint32_t>(Read<std::int32_t>(Bytes(a_image, callRva + 1, 4)));

		// _Emplace_reallocate 以 int3 填充对齐，入口前一字节必须是 0xCC
		if (!InText(a_image, emplace - 1, 2)) {
			throw LocateError(std::format("call target {:X} is outside .text", emplace));
		}
		if (*Bytes(a_image, emplace - 1, 1) != 0xCC) {
			throw LocateError(std::format("call target {:X} is not a function entry", emplace));
		}

		const auto end = FunctionEnd(a_image, emplace);

		// 锁 2：容量块在全 .text 里必须只有一处，且落在锚点函数体内
		const auto hits = FindAll(a_image, BuffFXCapPattern);
		if (hits.size() != 1) {
			if (hits.empty() && FindAll(a_image, BuffFXCapAppliedPattern).size() == 1) {
				throw LocateError("the BuffFX capacity block is already clamped");
			}
			throw LocateError(std::format("{} buff FX capacity block(s) matched, expected exactly 1", hits.size()));
		}

		const auto hook = hits.front();
		if (hook <= emplace || hook + BuffFXCapSize + BuffFXStrideSize >= end) {
			throw LocateError(std::format("capacity block {:X} is outside the anchor function {:X}..{:X}", hook, emplace, end));
		}

		// 锁 3：块内那条 ja 的目标是 max_size 溢出出口，必须仍落在同一函数体内
		const auto* ja = Bytes(a_image, hook + 28, 6);
		if (ja[0] != 0x0F || ja[1] != 0x87) {
			throw LocateError("the capacity block does not end with a ja to the overflow exit");
		}
		const auto overflow = hook + 34 + static_cast<std::uint32_t>(Read<std::int32_t>(ja + 2));
		if (overflow <= emplace || overflow > end) {
			throw LocateError(std::format("overflow exit {:X} is outside the anchor function", overflow));
		}

		// 锁 4：块后紧接游戏原版 operator new 的调用序列（0x18 步长后缀已含在特征码里）
		if (!MatchesAt(a_image, hook + BuffFXCapSize + BuffFXStrideSize, "49 8B CD E8")) {
			throw LocateError("the operator new call sequence after the capacity block does not match");
		}

		const auto allocSite = hook + BuffFXCapSize + BuffFXStrideSize + 3;
		if (!InText(a_image, allocSite + 1, 4)) {
			throw LocateError("the operator new call is truncated");
		}
		const auto allocTarget = allocSite + 5 + static_cast<std::uint32_t>(Read<std::int32_t>(Bytes(a_image, allocSite + 1, 4)));
		if (!InText(a_image, allocTarget, 1)) {
			throw LocateError(std::format("operator new target {:X} is outside .text", allocTarget));
		}

		InPlacePatch patch;
		patch.function = emplace;
		patch.hook = hook;
		patch.cover = BuffFXCapSize;
		return patch;
	}

	// ===================== 汇总 =====================
	struct Fix
	{
			char        key;
			const char* name;
			Site (*locate)(const Image&);
		};

		constexpr std::array<Fix, 6> Fixes{ {
			{ 'A', "heirloom", LocateHeirloom },
			{ 'B', "no_owner", LocateNoOwner },
			{ 'C', "part2", LocateDirtyFlag },
			{ 'D', "teleport", LocateTeleport },
			{ 'E', "bufffx", LocateBuffFX },
			{ 'F', "skillbitmap", LocateSkillBitmap },
		} };

		// 被改写的字节区间。跳板点与原地改写点都要参与「跳转不得落入覆盖区中部」的检查
		struct Span
		{
			char          key;
			std::uint32_t hook;
			std::uint32_t cover;
		};

		// 按字节扫描 .text 里的相对跳转。有跳转落在被覆盖指令的中间时不能打补丁
		std::string FindJumpIntoSites(const Image& a_image, const std::vector<Span>& a_spans)
		{
			const auto check = [&](std::uint32_t a_source, std::int64_t a_target) -> std::string {
				for (const auto& span : a_spans) {
					if (a_target > span.hook && a_target < static_cast<std::int64_t>(span.hook) + span.cover) {
						return std::format("{}: jump from {:X} lands inside the patched bytes at {:X}", span.key, a_source, a_target);
					}
				}
				return {};
			};

			const auto* text = a_image.bytes(a_image.textBegin());
			const auto  begin = static_cast<std::int64_t>(a_image.textBegin());
			const auto  size = a_image.textEnd() - a_image.textBegin();

			for (std::uint32_t i = 0; i < size; ++i) {
				const auto op = text[i];

				std::uint32_t length = 0;
				if (op == 0xE8 || op == 0xE9) {
					length = 5;
				} else if (op == 0x0F && i + 1 < size && text[i + 1] >= 0x80 && text[i + 1] <= 0x8F) {
					length = 6;
				}

				if (length && i + length <= size) {
					auto error = check(a_image.textBegin() + i, begin + i + length + Read<std::int32_t>(text + i + length - 4));
					if (!error.empty()) {
						return error;
					}
				}

				if (((op >= 0x70 && op <= 0x7F) || op == 0xEB) && i + 2 <= size) {
					auto error = check(a_image.textBegin() + i, begin + i + 2 + static_cast<std::int8_t>(text[i + 1]));
					if (!error.empty()) {
						return error;
					}
				}
			}

			return {};
		}

		// ===================== 补丁片段 =====================
		// 被覆盖的指令已由定位规则限定为不含相对寻址的形式，可以原样搬进片段
		class Fragment final : public Xbyak::CodeGenerator
		{
		public:
			Fragment(const Site& a_site, const Image& a_image, void* a_memory) :
				Xbyak::CodeGenerator(FragmentSize, a_memory),
				_site(a_site),
				_image(a_image)
			{
				switch (a_site.key) {
				case 'A':
					GuardHeirloom();
					break;
				case 'B':
					GuardNoOwner();
					break;
				case 'C':
					GuardDirtyFlag();
					break;
				case 'D':
					GuardTeleport();
					break;
				case 'E':
					GuardBuffFX();
					break;
				case 'F':
					GuardSkillBitmap();
					break;
				default:
					throw std::logic_error("unknown fix");
				}
			}

		private:
			void Replay(std::uint32_t a_offset, std::uint32_t a_size)
			{
				for (std::uint32_t i = 0; i < a_size; ++i) {
					db(_image.bytes(_site.hook)[a_offset + i]);
				}
			}

			const void* Absolute(std::uint32_t a_rva) const noexcept
			{
				return reinterpret_cast<const void*>(_image.base() + a_rva);
			}

			void JumpBack() { jmp(Absolute(_site.hook + _site.cover), T_NEAR); }

			void GuardHeirloom()
			{
				Xbyak::Label replay;

				const Xbyak::Reg64 value{ _site.valueReg };
				const Xbyak::Reg64 object{ _site.objectReg };

				test(value, value);
				jnz(replay);
				mov(dword[object + _site.typeOffset], 0);
				L(replay);
				Replay(0, _site.cover);
				JumpBack();
			}

			void GuardNoOwner()
			{
				Replay(4, 3);  // mov r<log>, rdx
				test(rcx, rcx);
				jz(Absolute(_site.exit));
				Replay(0, 4);  // mov r64, [rcx+disp8]
				JumpBack();
			}

			void GuardDirtyFlag()
			{
				test(rcx, rcx);
				jz(Absolute(_site.exit));
				Replay(0, _site.cover);
				JumpBack();
			}

			void GuardTeleport()
			{
				Replay(0, 3);  // mov rcx, [rcx]
				test(rcx, rcx);
				jz(Absolute(_site.exit));
				Replay(3, 4);  // cmp [rcx+disp8], r8d
				JumpBack();
			}

			void GuardBuffFX()
			{
				Xbyak::Label done;

				test(rcx, rcx);
				jz(done);
				mov(rax, UserAddressLimit);
				cmp(rcx, rax);
				ja(done);
				cmp(rcx, MinValidAddress);
				jb(done);
				Replay(0, _site.cover);
				L(done);
				ret();
			}

			// 入口校验技能位图基址：被覆盖的 mov 是 rip 相对寻址，不能在片段里原样重放，
			// 改为按绝对地址读取全局。基址非法（0 / 负数 / 过低）时经真实 ret 返回 0。
			void GuardSkillBitmap()
			{
				Xbyak::Label done;

				const auto global = Absolute(_site.bitmapGlobalRva);
				mov(rax, reinterpret_cast<std::uintptr_t>(global));
				mov(rdx, qword[rax]);  // rdx = 技能位图基址，等价于被覆盖的 mov rdx,[rip+disp]

				test(rdx, rdx);
				js(done);  // 符号位为 1：负数 / 用户空间之上的坏地址（含 -1 哨兵）
				cmp(rdx, MinValidAddress);
				jb(done);  // 0 或过低地址：无效

				JumpBack();  // 合法：从 hook+cover 继续原函数

				L(done);
				xor_(eax, eax);                 // 返回 0（该技能位判定为“未命中”）
				jmp(Absolute(_site.exit));       // 经真实出口安全返回
			}

			const Site&  _site;
			const Image& _image;
		};
	}

	// ===================== Image =====================
	Image::Image(std::uintptr_t a_base) :
		_base(a_base)
	{
		const auto pe = Read<std::uint32_t>(bytes(0x3C));
		const auto optionalHeader = pe + 24;

		_sectionCount = Read<std::uint16_t>(bytes(pe + 6));
		_imageSize = Read<std::uint32_t>(bytes(optionalHeader + 56));
		_sectionTable = optionalHeader + Read<std::uint16_t>(bytes(pe + 20));

		for (std::uint32_t i = 0; i < _sectionCount; ++i) {
			const auto* header = bytes(_sectionTable + i * SectionHeaderSize);
			if (SectionName(header) == ".text"sv) {
				_textBegin = Read<std::uint32_t>(header + 12);
				_textEnd = _textBegin + Read<std::uint32_t>(header + 8);
			} else if (SectionName(header) == ".data"sv) {
				_dataBegin = Read<std::uint32_t>(header + 12);
				_dataEnd = _dataBegin + Read<std::uint32_t>(header + 8);
			}
		}
	}

	std::string Image::ExternalPatchSection() const
	{
		for (std::uint32_t i = 0; i < _sectionCount; ++i) {
			const auto name = SectionName(bytes(_sectionTable + i * SectionHeaderSize));
			if (std::ranges::find(ExternalPatchSections, name) != ExternalPatchSections.end()) {
				return std::string{ name };
			}
		}

		return {};
	}

	// ===================== 对外接口 =====================
	Plan Locate(const Image& a_image)
	{
		Plan plan;
		for (const auto& fix : Fixes) {
			try {
				auto site = fix.locate(a_image);
				site.key = fix.key;
				site.name = fix.name;
				plan.sites.push_back(site);
			} catch (const LocateError& e) {
				plan.errors.push_back(std::format("{} ({}): {}", fix.key, fix.name, e.what()));
			}
		}

		try {
			auto direct = LocateWidgetString(a_image);
			direct.key = 'G';
			direct.name = "widgetstring";
			plan.directs.push_back(direct);
		} catch (const LocateError& e) {
			plan.errors.push_back(std::format("G (widgetstring): {}", e.what()));
		}

		try {
			auto inplace = LocateBuffFXCap(a_image);
			inplace.key = 'H';
			inplace.name = "bufffx_cap";
			plan.inplaces.push_back(inplace);
		} catch (const LocateError& e) {
			plan.errors.push_back(std::format("H (bufffx_cap): {}", e.what()));
		}

		if (plan.errors.empty()) {
			std::vector<Span> spans;
			spans.reserve(plan.sites.size() + plan.inplaces.size());
			for (const auto& site : plan.sites) {
				spans.push_back({ site.key, site.hook, site.cover });
			}
			for (const auto& patch : plan.inplaces) {
				spans.push_back({ patch.key, patch.hook, patch.cover });
			}

			if (auto error = FindJumpIntoSites(a_image, spans); !error.empty()) {
				plan.errors.push_back(std::move(error));
			}
		}

		// 不完整的修复没有意义，任何一处失败都不打补丁
		if (!plan.errors.empty()) {
			plan.sites.clear();
			plan.directs.clear();
			plan.inplaces.clear();
		}

		return plan;
	}

	void ApplyDirect(const DirectPatch& a_patch, std::uintptr_t a_base, void* a_newBuffer)
	{
		const auto write = [&](void* a_address, const void* a_source, std::size_t a_size) {
			// 保护失败时页面仍是只读，memcpy 会直接 AV；DllMain 里没人能接住 SEH，所以先抛
			DWORD old = 0;
			if (!::VirtualProtect(a_address, a_size, PAGE_EXECUTE_READWRITE, &old)) {
				throw std::runtime_error(std::format("cannot unprotect {:p} ({} bytes, error {})",
					a_address, a_size, ::GetLastError()));
			}

			std::memcpy(a_address, a_source, a_size);
			::FlushInstructionCache(::GetCurrentProcess(), a_address, a_size);
			::VirtualProtect(a_address, a_size, old, &old);
		};

		// 0) 搬迁旧池里已经写好的内容：安装时机未必早于第一次分配，索引非零时若不搬，
		//    翻转后 rcx 会指向旧池里的字符串字节而不是合法缓冲区。
		const auto index = *reinterpret_cast<const std::uint32_t*>(a_base + a_patch.indexGlobalRva);
		if (index != 0) {
			// 索引超界（大于原池）说明定位到了错误的全局，宁可放弃也不做越界读取
			if (index > WidgetStringOriginalPoolSize) {
				throw std::runtime_error("widget string index is larger than the original pool");
			}
			std::memcpy(a_newBuffer, reinterpret_cast<const void*>(a_base + a_patch.bufferGlobalRva), index);
		}

		// 1) 把旧数组首 8 字节改成新缓冲区的指针
		write(reinterpret_cast<void*>(a_base + a_patch.bufferGlobalRva),
			std::addressof(a_newBuffer), sizeof(a_newBuffer));

		// 2) lea rcx,[rip] -> mov rcx,[rip]：同一 rip 位移，从“取地址”变成“取值”
		const std::uint8_t mov = 0x8B;
		write(reinterpret_cast<void*>(a_base + a_patch.leaOpcodeRva),
			std::addressof(mov), sizeof(mov));

		// 3) 抬高容量上限：cmp r8d, 0x8000 -> cmp r8d, bufferSize
		const std::uint32_t newCap = a_patch.bufferSize;
		write(reinterpret_cast<void*>(a_base + a_patch.capImmRva),
			std::addressof(newCap), sizeof(newCap));
	}

	std::size_t Emit(const Site& a_site, const Image& a_image, void* a_memory)
	{
		const Fragment fragment{ a_site, a_image, a_memory };
		return fragment.getSize();
	}

	std::size_t EmitInPlace(const InPlacePatch& a_patch, void* a_memory)
	{
		switch (a_patch.key) {
		case 'H':
			static_assert(BuffFXCapFragment.size() == BuffFXCapSize, "the BuffFX cap fragment must be 37 bytes");
			if (a_patch.cover != BuffFXCapFragment.size()) {
				throw std::logic_error("the BuffFX cap patch must cover exactly 37 bytes");
			}

			std::memcpy(a_memory, BuffFXCapFragment.data(), BuffFXCapFragment.size());
			return BuffFXCapFragment.size();
		default:
			throw std::logic_error("unknown in-place fix");
		}
	}

	void ApplyInPlace(const InPlacePatch& a_patch, std::uintptr_t a_base)
	{
		std::array<std::uint8_t, BuffFXCapSize> bytes{};
		const auto size = EmitInPlace(a_patch, bytes.data());
		if (size != a_patch.cover) {
			throw std::runtime_error(std::format("in-place fragment of {} is {} bytes, expected {}",
				a_patch.key, size, a_patch.cover));
		}

		// 与其它补丁一样在安装阶段（DllMain）写入，此时还没有别线程执行游戏代码。
		// 37 字节可能跨页，但 .text 全段同属性，恢复首页的旧属性即可。
		auto* const address = reinterpret_cast<void*>(a_base + a_patch.hook);
		DWORD       old = 0;
		if (!::VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) {
			throw std::runtime_error(std::format("cannot unprotect the in-place patch site of {} (error {})",
				a_patch.key, ::GetLastError()));
		}

		std::memcpy(address, bytes.data(), size);
		::FlushInstructionCache(::GetCurrentProcess(), address, size);
		if (!::VirtualProtect(address, size, old, &old)) {
			// 字节已经写进去了，这里只能如实报告：页面停在可写状态，但补丁本身是有效的
			throw std::runtime_error(std::format(
				"in-place patch of {} was written but its page protection could not be restored (error {})",
				a_patch.key, ::GetLastError()));
		}
	}

	std::vector<std::uint8_t> MakeJump(const Site& a_site, const Image& a_image, const void* a_fragment)
	{
		constexpr std::size_t JumpSize = 5;

		const auto source = static_cast<std::int64_t>(a_image.base() + a_site.hook + JumpSize);
		const auto displacement = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(a_fragment)) - source;
		const auto rel32 = static_cast<std::int32_t>(displacement);
		if (rel32 != displacement) {
			throw std::runtime_error(std::format("fragment of {} is out of rel32 range", a_site.key));
		}

		std::vector<std::uint8_t> jump(a_site.cover, 0x90);
		jump[0] = 0xE9;
		std::memcpy(jump.data() + 1, std::addressof(rel32), sizeof(rel32));
		return jump;
	}
}
