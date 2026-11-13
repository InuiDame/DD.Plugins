#pragma once

// 逆向基准常量、唯一特征码、全有或全无校验。
// 纯 C++：只依赖「按 RVA 映射好的 PE 映像」，不依赖 DKUtil / 游戏运行时，
// 因此 src/Game.cpp 可被离线单测(tools/test_scroll.cpp)直接编译，对映射的 exe 跑同一套校验。

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Game
{
	// ---------------- 基准 ----------------
	constexpr std::string_view kSha1 = "9db2d73b89aa034e2055df65a139403e18df641f";
	constexpr std::uint64_t kImageBase = 0x140000000ull;

	// ---------------- 3 个 hook 的 RVA 与偷取字节 ----------------
	constexpr std::uint32_t kCaveRva = 0x371F0A;      // 事件泵 0x403 分支内
	constexpr std::uint32_t kCaveResume = 0x371F13;   // epilog 跳回（rip 相对 movss 起点）
	constexpr std::uint32_t kCaveStolen = 9;
	constexpr std::uint32_t kGridRva = 0x7E9F80;      // 网格渲染器 A（5 参）
	constexpr std::uint32_t kGridResume = 0x7E9F87;
	constexpr std::uint32_t kGridStolen = 7;
	// v10：网格渲染器 B（6 参）。p17 确证 A/B 是互斥分支，各自都是
	// 「逐格绘制 + 注册热点」的完整渲染器；补给两区 + 地牢背包会走 B。
	// 两个入口都装「只读 cave」，用 r8 的布局数组地址判面板（零误伤）。
	constexpr std::uint32_t kGrid2Rva = 0x7EADE0;
	constexpr std::uint32_t kGrid2Resume = 0x7EADE5;
	constexpr std::uint32_t kGrid2Stolen = 5;
	constexpr std::uint32_t kCellRva = 0x7EBF70;      // InventoryButtonInGrid（9 参）
	constexpr std::uint32_t kCellResume = 0x7EBF75;
	constexpr std::uint32_t kCellStolen = 5;
	// 两条逐格调用路径 [静态]：0x7EA6CD (in 0x7E9F80) / 0x7EB5AD (in 0x7EADE0) 的
	// E8 call 返回地址。返回地址门控据此覆盖两条路径，消除 B4。
	constexpr std::uint32_t kCallCell2 = 0x7EB5AD;
	constexpr std::uint32_t kRetCellA = 0x7EA6D2;
	constexpr std::uint32_t kRetCellB = 0x7EB5B2;
	// Provision 布局数组元素（[静态]：0x7E9F80 汇编 + 0x628A90 布局描述表确证）
	constexpr std::uint32_t kLayoutColumnsOff = 0x00; // int number_of_columns
	constexpr std::uint32_t kLayoutStartYOff = 0x08;  // float start_y  <-- 数据补丁写这里
	constexpr std::uint32_t kLayoutCentredOff = 0x14; // int is_centred

	// ---------------- 布局 / 常量 / 虚表 ----------------
	constexpr std::uint32_t kLayoutStore = 0x2B8DB10;        // Provision store 布局数组
	constexpr std::uint32_t kLayoutParty = 0x2B8DB30;        // Provision party 布局数组
	constexpr std::uint32_t kOriginStore = 0x2B8DB08;
	constexpr std::uint32_t kOriginParty = 0x2B8DB28;
	constexpr std::uint32_t kCallCell = 0x7EA6CD;            // 循环内单格 call
	constexpr std::uint32_t kLeaStore = 0x695B79;            // lea r8,[2B8DB10]
	constexpr std::uint32_t kLeaParty = 0x695E1C;            // lea r8,[2B8DB30]
	constexpr std::uint32_t kCallGridStore = 0x695B80;
	constexpr std::uint32_t kCallGridParty = 0x695E23;
	constexpr std::uint32_t kMovOriginStore = 0x695A06;
	constexpr std::uint32_t kMovOriginParty = 0x695CDE;
	constexpr std::uint32_t kConst30 = 0xEE63DC;
	constexpr std::uint32_t kConst8 = 0xEE63B8;
	constexpr std::uint32_t kConst1 = 0xEE62C0;
	constexpr std::uint32_t kConstHalf = 0xEE625C;
	constexpr std::uint32_t kConstSign = 0xEE6950;
	constexpr std::uint32_t kAccumY = 0x118D754;
	constexpr std::uint32_t kAccumX = 0x118D784;
	constexpr std::uint32_t kVtableProv = 0xEBDDE8;
	constexpr std::uint32_t kVtableGuild = 0xEBB8D8;
	constexpr std::uint32_t kVtableInv = 0xECAC68;
	constexpr std::uint32_t kVtableInv2 = 0xECAC58;

	// 偷取的入口字节（精确比对，不作定位用）
	constexpr std::array<std::uint8_t, 9> kCaveBytes{ 0x66, 0x0F, 0x6E, 0x44, 0x24, 0x34, 0x0F, 0x5B, 0xC0 };
	constexpr std::array<std::uint8_t, 7> kGridBytes{ 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10 };
	constexpr std::array<std::uint8_t, 5> kGrid2Bytes{ 0x4C, 0x89, 0x4C, 0x24, 0x20 };
	constexpr std::array<std::uint8_t, 5> kCellBytes{ 0x48, 0x89, 0x5C, 0x24, 0x20 };

	// v10：0x7EADE0 的长特征码（p16 §5 实测全 .text 唯一命中；裸 5B 有 326 命中，不可用）。
	constexpr std::string_view kSigGrid2 =
		"4C 89 4C 24 20 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 B8 11 00 00 E8 ?? ?? ?? ??";

	// ---------------- 唯一特征码（全 .text 实测 uniq=1，见 p3-progress.md）----------------
	constexpr std::string_view kSigCave =       // F0   @0x371F0A
		"66 0F 6E 44 24 34 0F 5B C0 F3 0F 11 0D ?? ?? ?? ?? F3 0F 58 D0 F3 0F 11 15 ?? ?? ?? ??";
	constexpr std::string_view kSigCaveB =      // F0b  @0x371EFD
		"66 0F 6E 44 24 30 0F 5B C0 F3 0F 58 C8 66 0F 6E 44 24 34 0F 5B C0 F3 0F 11 0D ?? ?? ?? ?? F3 0F 58 D0";
	constexpr std::string_view kSigBranch =     // F1   @0x371EA8 (0x403 分支入口)
		"8B 05 66 B6 E1 00 32 C9 2B C7 75 04 40 0F B6 CF";
	constexpr std::string_view kSigTick =       // F2   @0x3E1AD0 (基准校验，不 hook)
		"48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 C8 FC FF FF 48 81 EC F8 03 00 00";
	constexpr std::string_view kSigCallSite =   // F3   @0x7EA6BE (单格调用点)
		"45 8B 8D 78 03 00 00 45 8B C6 48 8B D3 33 C9 E8 ?? ?? ?? ?? 84 C0 4C 0F 45 FB";
	constexpr std::string_view kSigGrid =       // F7b  @0x7E9F80
		"48 8B C4 48 89 58 10 4C 89 48 20 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 38 FB FF";
	constexpr std::string_view kSigCell =       // F6b  @0x7EBF70
		"48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 F3 FF FF";
	constexpr std::string_view kSigLeaStore =   // F4   @0x695B79
		"4C 8D 05 90 7F 4F 02 E8 FB 43 15 00 E9";
	constexpr std::string_view kSigLeaParty =   // F5   @0x695E1C
		"4C 8D 05 0D 7D 4F 02 E8 58 41 15 00 E9";
	constexpr std::string_view kSigFrame =      // F8   @0x236020 (基准)
		"41 B0 01 B9 1B 00 00 00 0F B6 F8";
	constexpr std::string_view kSigClear =      // F9   @0x23608E (基准)
		"89 2D BC 76 F5 00 89 2D FA 76 F5 00 89 2D E4 76 F5 00 89 2D AE 76 F5 00";
	constexpr std::string_view kSigRegion =     // F10  @0x2749B0 (区域判定)
		"4C 89 44 24 18 48 89 54 24 10 57 48 83 EC";
	constexpr std::string_view kSigWidget =     // F11b @0x39B640 (Widget Show)
		"40 53 55 56 57 41 56 48 83 EC 60 44 8B 05 A2 9C";

	// ---------------- 映射 PE 映像 ----------------
	struct ImageView
	{
		const std::uint8_t* base{ nullptr };
		std::uint32_t textBegin{ 0 };
		std::uint32_t textEnd{ 0 };
		std::uint64_t imageBase{ 0 };

		const std::uint8_t* bytes(std::uint32_t a_rva) const noexcept { return base + a_rva; }
		std::uint32_t u32(std::uint32_t a_rva) const noexcept;
		std::int32_t i32(std::uint32_t a_rva) const noexcept;
		std::uint64_t u64(std::uint32_t a_rva) const noexcept;
	};

	// 从按 RVA 映射的 PE 映像解析 .text 范围与 ImageBase。
	bool InitImageView(ImageView& a_out, const void* a_mappedBase);

	// 按 PE 节把磁盘 exe 文件展开成 RVA 映像（纯标准库、只读）。
	// 生产侧与离线单测共用，使 28 项校验(含 AccumZero)基于磁盘映射值而非
	// 运行时内存值 —— 消除 S3：加载器在 UI 跑过一帧后才加载 DLL 时，
	// 0x118D754/0x118D784 运行时累加器可能非零，磁盘值恒为 0。
	bool MapDiskImage(const std::filesystem::path& a_exe, std::vector<std::uint8_t>& a_out);

	// 返回地址门控：a_retAddr 是否落在两条逐格绘制路径的 call 之后（B4）。
	bool IsGridCellReturn(std::uint64_t a_retAddr, std::uint64_t a_base) noexcept;

	// ---------------- 特征码搜索 ----------------
	std::vector<int> ParsePattern(std::string_view a_pattern);
	std::vector<std::uint32_t> FindAll(const ImageView& a_image, std::string_view a_pattern);
	// 恰好命中一次且地址等于 a_expected。
	bool FindUnique(const ImageView& a_image, std::string_view a_pattern, std::uint32_t a_expected);

	// ---------------- SHA1（纯 C++，生产侧与单测共用）----------------
	std::array<std::uint8_t, 20> Sha1(const std::uint8_t* a_data, std::size_t a_size);
	std::string Sha1Hex(const std::uint8_t* a_data, std::size_t a_size);
	bool CheckExeSha1(const std::filesystem::path& a_exe);

	// ---------------- 全有或全无校验 ----------------
	enum class Check : int
	{
		Sha1, ImageBase, CaveBytes, GridBytes, CellBytes,
		SigCave, SigCaveB, SigBranch, SigTick, SigCallSite,
		SigGrid, SigCell, SigLeaStore, SigLeaParty, SigFrame, SigClear, SigRegion, SigWidget,
		Const30, Const8, Const1, ConstHalf, ConstSign, AccumZero,
		LayoutAddrs, OriginAddrs, CallRel32, Vtable,
		Count
	};

	struct VerifyReport
	{
		std::array<bool, static_cast<int>(Check::Count)> ok{};

		bool AllOk() const noexcept;
		bool Item(Check a_c) const noexcept { return ok[static_cast<int>(a_c)]; }
		std::string Format() const; // 逐项打印 [OK]/[FAIL]，供 WARN 日志
	};

	// sha1Matches 由调用方(CheckExeSha1)传入；Verify 校验其余全部项并逐项记录。
	VerifyReport Verify(const ImageView& a_image, bool a_sha1Matches);

	// 用唯一特征码定位 3 个 hook 的实际地址（并再次确认与常量 RVA 一致）。
	// 返回 false 表示定位失败 -> 一个 hook 都不装。
	bool Locate(const ImageView& a_image, const VerifyReport& a_report,
		std::uint32_t& a_cave, std::uint32_t& a_grid, std::uint32_t& a_cell);
}
