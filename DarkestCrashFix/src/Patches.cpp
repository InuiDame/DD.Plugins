// 修复方案来自局师傅的《暗黑地牢》64 位主程序通用修复补丁工具包 v2，这里改为在内存中打补丁。
#include "Patches.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>

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

		constexpr std::uint32_t BuffFXHookOffset = 16;

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

		const std::uint8_t* Bytes(const Image& a_image, std::uint32_t a_rva, std::uint32_t a_size)
		{
			if (a_rva < a_image.textBegin() || a_rva + a_size > a_image.textEnd()) {
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

		// ===================== 汇总 =====================
		struct Fix
		{
			char        key;
			const char* name;
			Site (*locate)(const Image&);
		};

		constexpr std::array<Fix, 5> Fixes{ {
			{ 'A', "heirloom", LocateHeirloom },
			{ 'B', "no_owner", LocateNoOwner },
			{ 'C', "part2", LocateDirtyFlag },
			{ 'D', "teleport", LocateTeleport },
			{ 'E', "bufffx", LocateBuffFX },
		} };

		// 按字节扫描 .text 里的相对跳转。有跳转落在被覆盖指令的中间时不能打补丁
		std::string FindJumpIntoSites(const Image& a_image, const std::vector<Site>& a_sites)
		{
			const auto check = [&](std::uint32_t a_source, std::int64_t a_target) -> std::string {
				for (const auto& site : a_sites) {
					if (a_target > site.hook && a_target < static_cast<std::int64_t>(site.hook) + site.cover) {
						return std::format("{}: jump from {:X} lands inside the patched bytes at {:X}", site.key, a_source, a_target);
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
				break;
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

		if (plan.errors.empty()) {
			if (auto error = FindJumpIntoSites(a_image, plan.sites); !error.empty()) {
				plan.errors.push_back(std::move(error));
			}
		}

		// 不完整的修复没有意义，任何一处失败都不打补丁
		if (!plan.errors.empty()) {
			plan.sites.clear();
		}

		return plan;
	}

	std::size_t Emit(const Site& a_site, const Image& a_image, void* a_memory)
	{
		const Fragment fragment{ a_site, a_image, a_memory };
		return fragment.getSize();
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
