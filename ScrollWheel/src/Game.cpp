#include "Game.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace Game
{
	std::uint32_t ImageView::u32(std::uint32_t a_rva) const noexcept
	{
		std::uint32_t v;
		std::memcpy(&v, bytes(a_rva), sizeof(v));
		return v;
	}

	std::int32_t ImageView::i32(std::uint32_t a_rva) const noexcept
	{
		std::int32_t v;
		std::memcpy(&v, bytes(a_rva), sizeof(v));
		return v;
	}

	std::uint64_t ImageView::u64(std::uint32_t a_rva) const noexcept
	{
		std::uint64_t v;
		std::memcpy(&v, bytes(a_rva), sizeof(v));
		return v;
	}

	bool MapDiskImage(const std::filesystem::path& a_exe, std::vector<std::uint8_t>& a_out)
	{
		std::ifstream in(a_exe, std::ios::binary);
		if (!in.is_open()) {
			return false;
		}
		const std::vector<std::uint8_t> raw{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		if (raw.size() < 0x40) {
			return false;
		}
		const auto rd16 = [&raw](std::size_t o) -> std::uint16_t {
			return static_cast<std::uint16_t>(raw[o]) | (static_cast<std::uint16_t>(raw[o + 1]) << 8);
		};
		const auto rd32 = [&raw](std::size_t o) -> std::uint32_t {
			return static_cast<std::uint32_t>(raw[o]) | (static_cast<std::uint32_t>(raw[o + 1]) << 8) |
			       (static_cast<std::uint32_t>(raw[o + 2]) << 16) | (static_cast<std::uint32_t>(raw[o + 3]) << 24);
		};
		const auto pe = rd32(0x3C);
		if (pe + 24 + 2 > raw.size() || rd32(pe) != 0x00004550u) { // "PE\0\0"
			return false;
		}
		const auto opt = pe + 24;
		if (rd16(opt) != 0x20B) { // PE32+
			return false;
		}
		const auto nsec = rd16(pe + 6);
		const auto optSize = rd16(pe + 20);
		const auto imageSize = rd32(opt + 56);
		const auto headerSize = rd32(opt + 60);
		const auto secTable = opt + optSize;
		if (headerSize > raw.size() || imageSize == 0 || secTable + nsec * 40 > raw.size()) {
			return false;
		}
		a_out.assign(imageSize, 0);
		std::memcpy(a_out.data(), raw.data(), headerSize);
		for (std::uint16_t i = 0; i < nsec; ++i) {
			const auto h = secTable + static_cast<std::size_t>(i) * 40;
			const auto va = rd32(h + 12);
			const auto vsize = rd32(h + 8);
			const auto rawSize = rd32(h + 16);
			const auto rawPtr = rd32(h + 20);
			if (rawPtr > raw.size() || rawSize > raw.size() - rawPtr) {
				continue;
			}
			const auto n = std::min<std::uint32_t>(rawSize, vsize);
			if (va > imageSize || n > imageSize - va) {
				continue;
			}
			std::memcpy(a_out.data() + va, raw.data() + rawPtr, n);
		}
		return true;
	}

	bool IsGridCellReturn(std::uint64_t a_retAddr, std::uint64_t a_base) noexcept
	{
		return a_retAddr == a_base + kRetCellA || a_retAddr == a_base + kRetCellB;
	}

	bool InitImageView(ImageView& a_out, const void* a_mappedBase)
	{
		const auto* b = static_cast<const std::uint8_t*>(a_mappedBase);
		if (!b) {
			return false;
		}
		const auto rd16 = [b](std::size_t o) -> std::uint16_t {
			return static_cast<std::uint16_t>(b[o]) | (static_cast<std::uint16_t>(b[o + 1]) << 8);
		};
		const auto rd32 = [b](std::size_t o) -> std::uint32_t {
			return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
			       (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
		};

		const auto pe = rd32(0x3C);
		if (rd32(pe) != 0x00004550u) { // "PE\0\0"
			return false;
		}
		const auto opt = pe + 24;
		if (rd16(opt) != 0x20B) { // PE32+
			return false;
		}
		const auto nsec = rd16(pe + 6);
		const auto optSize = rd16(pe + 20);
		const auto secTable = opt + optSize;

		a_out.base = b;
		a_out.imageBase = static_cast<std::uint64_t>(rd32(opt + 24)) |
		                  (static_cast<std::uint64_t>(rd32(opt + 28)) << 32);
		a_out.textBegin = 0;
		a_out.textEnd = 0;
		for (std::uint16_t i = 0; i < nsec; ++i) {
			const auto h = secTable + static_cast<std::size_t>(i) * 40;
			if (std::memcmp(b + h, ".text", 5) == 0) {
				a_out.textBegin = rd32(h + 12);          // VirtualAddress
				a_out.textEnd = a_out.textBegin + rd32(h + 8); // VirtualSize
				break;
			}
		}
		return a_out.textEnd > a_out.textBegin;
	}

	std::vector<int> ParsePattern(std::string_view a_pattern)
	{
		std::vector<int> bytes;
		for (std::size_t i = 0; i + 1 < a_pattern.size();) {
			if (a_pattern[i] == ' ') {
				++i;
				continue;
			}
			const auto token = a_pattern.substr(i, 2);
			int value = -1;
			if (token != "??") {
				value = static_cast<int>(std::strtoul(std::string{ token }.c_str(), nullptr, 16));
			}
			bytes.push_back(value);
			i += 2;
		}
		return bytes;
	}

	std::vector<std::uint32_t> FindAll(const ImageView& a_image, std::string_view a_pattern)
	{
		const auto pattern = ParsePattern(a_pattern);
		const auto n = static_cast<std::uint32_t>(pattern.size());
		std::vector<std::uint32_t> hits;
		if (n == 0 || a_image.textEnd <= a_image.textBegin) {
			return hits;
		}
		for (auto rva = a_image.textBegin; rva + n <= a_image.textEnd;) {
			const auto* code = a_image.bytes(rva);
			std::uint32_t i = 0;
			while (i < n && (pattern[i] < 0 || pattern[i] == code[i])) {
				++i;
			}
			if (i == n) {
				hits.push_back(rva);
				rva += n;
			} else {
				++rva;
			}
		}
		return hits;
	}

	bool FindUnique(const ImageView& a_image, std::string_view a_pattern, std::uint32_t a_expected)
	{
		const auto hits = FindAll(a_image, a_pattern);
		return hits.size() == 1 && hits.front() == a_expected;
	}

	// ---------------- SHA-1（FIPS 180-4，纯 C++）----------------
	std::array<std::uint8_t, 20> Sha1(const std::uint8_t* a_data, std::size_t a_size)
	{
		const auto rol = [](std::uint32_t v, int n) -> std::uint32_t {
			return (v << n) | (v >> (32 - n));
		};
		std::uint32_t h[5]{ 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

		std::vector<std::uint8_t> msg(a_data, a_data + a_size);
		const std::uint64_t bitLen = static_cast<std::uint64_t>(a_size) * 8;
		msg.push_back(0x80);
		while (msg.size() % 64 != 56) {
			msg.push_back(0);
		}
		for (int k = 7; k >= 0; --k) {
			msg.push_back(static_cast<std::uint8_t>(bitLen >> (k * 8)));
		}

		for (std::size_t off = 0; off < msg.size(); off += 64) {
			std::uint32_t w[80];
			for (int t = 0; t < 16; ++t) {
				w[t] = (static_cast<std::uint32_t>(msg[off + 4 * t]) << 24) |
				       (static_cast<std::uint32_t>(msg[off + 4 * t + 1]) << 16) |
				       (static_cast<std::uint32_t>(msg[off + 4 * t + 2]) << 8) |
				       static_cast<std::uint32_t>(msg[off + 4 * t + 3]);
			}
			for (int t = 16; t < 80; ++t) {
				w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
			}
			std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
			for (int t = 0; t < 80; ++t) {
				std::uint32_t f, k;
				if (t < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
				else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
				else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
				else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
				const auto tmp = rol(a, 5) + f + e + k + w[t];
				e = d; d = c; c = rol(b, 30); b = a; a = tmp;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
		}

		std::array<std::uint8_t, 20> out{};
		for (int j = 0; j < 5; ++j) {
			out[4 * j] = static_cast<std::uint8_t>(h[j] >> 24);
			out[4 * j + 1] = static_cast<std::uint8_t>(h[j] >> 16);
			out[4 * j + 2] = static_cast<std::uint8_t>(h[j] >> 8);
			out[4 * j + 3] = static_cast<std::uint8_t>(h[j]);
		}
		return out;
	}

	std::string Sha1Hex(const std::uint8_t* a_data, std::size_t a_size)
	{
		const auto digest = Sha1(a_data, a_size);
		static constexpr char hex[] = "0123456789abcdef";
		std::string s;
		s.reserve(40);
		for (const auto byte : digest) {
			s.push_back(hex[byte >> 4]);
			s.push_back(hex[byte & 0xF]);
		}
		return s;
	}

	bool CheckExeSha1(const std::filesystem::path& a_exe)
	{
		std::ifstream in(a_exe, std::ios::binary);
		if (!in.is_open()) {
			return false;
		}
		const std::vector<std::uint8_t> data{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		return Sha1Hex(data.data(), data.size()) == kSha1;
	}

	// ---------------- 全有或全无校验 ----------------
	namespace
	{
		constexpr std::array<const char*, static_cast<int>(Check::Count)> kNames{
			"exe SHA1", "ImageBase", "cave 入口 9B", "grid 入口 7B", "cell 入口 5B",
			"F0 cave 特征码", "F0b 备用锚", "F1 0x403 分支", "F2 tick 基准", "F3 单格调用点",
			"F7b grid 特征码", "F6b cell 特征码", "F4 store 锚", "F5 party 锚", "F8 帧序列", "F9 清零序列", "F10 区域判定", "F11b Widget Show",
			"0xEE63DC=30.0f", "0xEE63B8=8.0f", "0xEE62C0=1.0f", "0xEE625C=0.5f", "0xEE6950 符号位", "累加器磁盘为 0",
			"布局数组地址(lea)", "原点全局地址(mov)", "调用点 rel32", "虚表内容"
		};

		bool BytesEqual(const ImageView& img, std::uint32_t rva, const std::uint8_t* expect, std::size_t n)
		{
			return std::memcmp(img.bytes(rva), expect, n) == 0;
		}

		std::uint32_t RelTarget(const ImageView& img, std::uint32_t a_call)
		{
			return a_call + 5 + static_cast<std::uint32_t>(img.i32(a_call + 1));
		}

		std::uint32_t RipTarget(const ImageView& img, std::uint32_t a_insn, std::uint32_t a_len)
		{
			return a_insn + a_len + static_cast<std::uint32_t>(img.i32(a_insn + a_len - 4));
		}
	}

	bool VerifyReport::AllOk() const noexcept
	{
		return std::all_of(ok.begin(), ok.end(), [](bool v) { return v; });
	}

	std::string VerifyReport::Format() const
	{
		std::string s;
		for (int i = 0; i < static_cast<int>(Check::Count); ++i) {
			s += "  ";
			s += ok[i] ? "[OK]   " : "[FAIL] ";
			s += kNames[i];
			s += "\n";
		}
		return s;
	}

	VerifyReport Verify(const ImageView& img, bool sha1Matches)
	{
		VerifyReport r;
		const auto set = [&r](Check c, bool v) { r.ok[static_cast<int>(c)] = v; };

		set(Check::Sha1, sha1Matches);
		set(Check::ImageBase, img.imageBase == kImageBase);
		set(Check::CaveBytes, BytesEqual(img, kCaveRva, kCaveBytes.data(), kCaveBytes.size()));
		set(Check::GridBytes, BytesEqual(img, kGridRva, kGridBytes.data(), kGridBytes.size()));
		set(Check::CellBytes, BytesEqual(img, kCellRva, kCellBytes.data(), kCellBytes.size()));

		set(Check::SigCave, FindUnique(img, kSigCave, kCaveRva));
		set(Check::SigCaveB, FindUnique(img, kSigCaveB, 0x371EFD));
		set(Check::SigBranch, FindUnique(img, kSigBranch, 0x371EA8));
		set(Check::SigTick, FindUnique(img, kSigTick, 0x3E1AD0));
		set(Check::SigCallSite, FindUnique(img, kSigCallSite, 0x7EA6BE));
		set(Check::SigGrid, FindUnique(img, kSigGrid, kGridRva));
		set(Check::SigCell, FindUnique(img, kSigCell, kCellRva));
		set(Check::SigLeaStore, FindUnique(img, kSigLeaStore, kLeaStore));
		set(Check::SigLeaParty, FindUnique(img, kSigLeaParty, kLeaParty));
		set(Check::SigFrame, FindUnique(img, kSigFrame, 0x236020));
		set(Check::SigClear, FindUnique(img, kSigClear, 0x23608E));
		set(Check::SigRegion, FindUnique(img, kSigRegion, 0x2749B0));
		set(Check::SigWidget, FindUnique(img, kSigWidget, 0x39B640));

		set(Check::Const30, img.u32(kConst30) == 0x41F00000u);
		set(Check::Const8, img.u32(kConst8) == 0x41000000u);
		set(Check::Const1, img.u32(kConst1) == 0x3F800000u);
		set(Check::ConstHalf, img.u32(kConstHalf) == 0x3F000000u);
		set(Check::ConstSign, img.u64(kConstSign) == 0x8000000080000000ull);
		set(Check::AccumZero, img.u32(kAccumY) == 0 && img.u32(kAccumX) == 0);

		set(Check::LayoutAddrs,
			RipTarget(img, kLeaStore, 7) == kLayoutStore &&
			RipTarget(img, kLeaParty, 7) == kLayoutParty);
		set(Check::OriginAddrs,
			RipTarget(img, kMovOriginStore, 7) == kOriginStore &&
			RipTarget(img, kMovOriginParty, 7) == kOriginParty);
		set(Check::CallRel32,
			RelTarget(img, kCallCell) == kCellRva &&
			RelTarget(img, kCallCell2) == kCellRva && // 第二条逐格路径 0x7EB5AD (B4)
			RelTarget(img, kCallGridStore) == kGridRva &&
			RelTarget(img, kCallGridParty) == kGridRva);
		set(Check::Vtable,
			img.u64(kVtableProv + 8 * 23) == 0x140695460ull &&
			img.u64(kVtableGuild + 8 * 23) == 0x14066E9F0ull &&
			img.u64(kVtableInv + 8 * 2) == 0x140735DB0ull &&
			img.u64(kVtableInv2) == 0x140738540ull);

		return r;
	}

	bool Locate(const ImageView& img, const VerifyReport& r,
		std::uint32_t& a_cave, std::uint32_t& a_grid, std::uint32_t& a_cell)
	{
		if (!r.AllOk()) {
			return false;
		}
		const auto h1 = FindAll(img, kSigCave);
		const auto h2 = FindAll(img, kSigGrid);
		const auto h3 = FindAll(img, kSigCell);
		if (h1.size() != 1 || h2.size() != 1 || h3.size() != 1 ||
			h1.front() != kCaveRva || h2.front() != kGridRva || h3.front() != kCellRva) {
			return false;
		}
		a_cave = h1.front();
		a_grid = h2.front();
		a_cell = h3.front();
		return true;
	}
}
