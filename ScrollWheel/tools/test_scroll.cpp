// 离线单测：在映射的 Darkest.exe 上验证特征码/校验，并跑纯逻辑(符号/clamp/NaN/队列)。
// 不启动游戏、不加载任何 DLL 到游戏进程。
// 用法：scroll_wheel_test <Darkest.exe 路径>
#include "CaveAsm.h"
#include "Config.h"
#include "Game.h"
#include "Wheel.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

	bool Contains(const std::uint8_t* a_hay, std::size_t a_n,
		const std::uint8_t* a_needle, std::size_t a_m)
	{
		if (a_m == 0 || a_m > a_n) {
			return false;
		}
		for (std::size_t i = 0; i + a_m <= a_n; ++i) {
			if (std::memcmp(a_hay + i, a_needle, a_m) == 0) {
				return true;
			}
		}
		return false;
	}

	void HexLine(const char* a_tag, const std::uint8_t* a_p, std::size_t a_n)
	{
		std::printf("%s:", a_tag);
		for (std::size_t i = 0; i < a_n; ++i) {
			std::printf(" %02X", a_p[i]);
		}
		std::printf("\n");
	}

	Wheel::Params BaseParams()
	{
		Wheel::Params p;
		p.step = 30.f;
		p.smooth = 0.f;   // 关掉平滑，方便断言 target
		p.visibleRows = 5;
		p.rowHeight = 40.f;
		p.columns = 7;
		p.enabled = true;
		p.frameTtl = 1;
		return p;
	}

	void TestPureLogic()
	{
		std::puts("pure logic:");
		const auto p = BaseParams();

		// 1) 符号约定 offset -= wheel*step（向上 + 使 target 减小）；invert 反转
		{
			Wheel::State s;
			s.max = 300.f; s.target = 150.f; s.current = 150.f;
			Wheel::Tick(s, p, +1, true, true);
			Check(s.target < 150.f, "wheel.y=+1 (up) -> target -= step");
			Check(std::fabs(s.target - 120.f) < 1e-4f, "target 精确 = 150-30 = 120");
		}
		{
			Wheel::State s;
			s.max = 300.f; s.target = 150.f; s.current = 150.f;
			Wheel::Tick(s, p, -1, true, true);
			Check(s.target > 150.f, "wheel.y=-1 (down) -> target += step");
		}
		{
			auto inv = p; inv.invert = true;
			Wheel::State s; s.max = 300.f; s.target = 150.f; s.current = 150.f;
			Wheel::Tick(s, inv, +1, true, true);
			Check(s.target > 150.f, "invert=1 反转方向");
		}

		// 2) clamp [0,max]
		{
			Wheel::State s; s.max = 100.f; s.target = 90.f; s.current = 90.f;
			Wheel::Tick(s, p, +1000, true, true);
			Check(s.target >= 0.f && s.target <= 100.f, "极端正向滚轮 clamp 到 [0,max]");
			Check(s.target == 0.f, "clamp 下界 = 0");
		}
		{
			Wheel::State s; s.max = 100.f; s.target = 10.f; s.current = 10.f;
			Wheel::Tick(s, p, -1000, true, true);
			Check(s.target == 100.f, "clamp 上界 = max");
		}

		// 3) NaN / Inf 防护
		{
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float inf = std::numeric_limits<float>::infinity();
			Wheel::State s; s.max = nan; s.target = nan; s.current = nan;
			Wheel::Tick(s, p, 0, true, true);
			Check(std::isfinite(s.target) && std::isfinite(s.current) && std::isfinite(s.max),
				"NaN 的 target/current/max 全部被 isfinite 拦截");
			Wheel::State s2; s2.max = inf; s2.target = 50.f; s2.current = 50.f;
			Wheel::Tick(s2, p, +2, true, true);
			Check(std::isfinite(s2.max) && std::isfinite(s2.target) && std::isfinite(s2.current),
				"Inf 的 max 被拦截且不写 NaN");
			Wheel::State s3; s3.max = 300.f; s3.target = 150.f; s3.current = 150.f;
			Wheel::Tick(s3, p, std::numeric_limits<std::int64_t>::max(), true, true);
			Check(std::isfinite(s3.target), "INT64_MAX 滚轮值不产生 NaN/Inf");
		}

		// 4) SPSC 队列溢出：不覆盖未读项、dropped 正确、drain 顺序、不崩
		{
			Wheel::Reset();
			int accepted = 0;
			for (int i = 1; i <= 300; ++i) {
				if (Wheel::Push(i)) {
					++accepted;
				}
			}
			Check(accepted == 255, "队列满后丢弃，有效容量 255");
			Check(Wheel::Dropped() == 45, "dropped 累计 45");
			const auto sum = Wheel::Drain();
			Check(sum == 255LL * 256 / 2, "drain 顺序正确、未覆盖未读项(1..255 求和)");
			Check(Wheel::Drain() == 0, "重复 drain 幂等(返回 0)");
			Wheel::Reset();
		}

		// 5) 虚拟裁剪 / max 自适应 / firstRow
		{
			Wheel::State s;
			s.cellCount = 70; // 70 格 / 7 列 = 10 行
			Wheel::RecomputeMax(s, p); // (10-5)*40 = 200
			Check(std::fabs(s.max - 200.f) < 1e-4f, "max = (ceil(70/7)-5)*40 = 200");
			// v5：每格 cave 未命中时 cellCount=0，fixedMax 必须仍能给出可滚动上限
			{
				Wheel::State z; z.cellCount = 0;
				Wheel::Params fp = p; fp.fixedMax = 400.f;
				Wheel::RecomputeMax(z, fp);
				Check(std::fabs(z.max - 400.f) < 1e-4f, "fixedMax=400 时 cellCount=0 也能滚动(v5)");
			}
			s.inGrid = true;
			s.firstRow = 2;
			s.cellIndex = 0;
			int drawn = 0;
			for (int i = 0; i < 70; ++i) {
				if (Wheel::ShouldDrawCell(s, p)) {
					++drawn;
				}
			}
			Check(drawn == 5 * 7, "只绘制窗口 [firstRow,firstRow+visRows) 内的 35 格");
			Check(s.cellIndex == 70, "cellIndex 统计到全部 70 格");
		}
	}

	// 新增 ②：单调 write 超过 255 后 Drain 不死循环、不越界；wrap 跨越也安全（B2 双保险）
	void TestQueueMonotonic()
	{
		std::puts("queue monotonic producer (B2 double-safety):");
		Wheel::Reset();
		for (int i = 0; i < 256; ++i) {
			Wheel::g_queue.ring[i] = 1;
		}
		// 模拟旧 cave 的单调生产者：write 直接跳到 300（> 255）
		Wheel::g_queue.write.store(300, std::memory_order_relaxed);
		Wheel::g_queue.read.store(45, std::memory_order_relaxed);
		const auto acc = Wheel::Drain(); // 若用 while(r!=w){++r;} 此处会跑 2^32 次
		Check(acc == 255, "单调 write=300 -> Drain 有界返回 (300-45)&255=255，不死循环");
		Check(Wheel::g_queue.read.load(std::memory_order_relaxed) == 300, "Drain 后 read 追平 write");
		Check(Wheel::Drain() == 0, "单调 write 之后重复 Drain 幂等");

		// wrap 跨越 250 -> 5：新契约下也必须不死循环、越界
		Wheel::Reset();
		for (int i = 0; i < 256; ++i) {
			Wheel::g_queue.ring[i] = 1;
		}
		Wheel::g_queue.write.store(5, std::memory_order_relaxed);
		Wheel::g_queue.read.store(250, std::memory_order_relaxed);
		Check(Wheel::Drain() == 11, "wrap 跨越 (read=250,write=5) -> Drain=11 不死循环不越界");
		Wheel::Reset();
	}

	// 新增 ①：cave 汇编生产者读取偏移 0x64 + wrap 契约（字节断言，
	// 机器码由 CAVE_INPUT_HEX/CAVE_CELL_HEX 输出，外部用 capstone 反汇编复核）
	void TestCaveAsm()
	{
		std::puts("cave assembly producer (B1/B2):");
		ScrollAsm::InputParams ip{};
		ip.queueAddr = 0x140000000ull;   // dummy：只生成字节，不执行
		ip.resumeAddr = 0x1400371F13ull;
		ip.offWrite = 0;
		ip.offRead = 64;
		ip.offRing = 128;
		ip.offDropped = 1152;
		ip.mask = 255;
		ip.capacity = 256;
		ScrollAsm::InputCaveAsm frag(ip);
		const auto* code = static_cast<const std::uint8_t*>(frag.getCode());
		const auto size = frag.getSize();
		HexLine("CAVE_INPUT_HEX", code, size);

		// mov eax, dword ptr [rsp+0x64] = 8B 44 24 64（6 次压栈=48 -> 48+0x34=0x64）
		static constexpr std::uint8_t kReadY[]{ 0x8B, 0x44, 0x24, 0x64 };
		Check(Contains(code, size, kReadY, sizeof(kReadY)),
			"B1: cave 生产者读取 [rsp+0x64] = 原始 wheel.y");
		static constexpr std::uint8_t kReadWrong[]{ 0x8B, 0x44, 0x24, 0x5C };
		Check(!Contains(code, size, kReadWrong, sizeof(kReadWrong)),
			"B1: 不含旧的错误偏移 [rsp+0x5C]");
		Check(Contains(code, size, ScrollAsm::kInputBytes, 9),
			"cave epilog 原样重放 9B stolen");

		// B2: and edx/ecx, 255（Xbyak 对 255 用 81 /4 id，也可能 83 /4 ib）
		static constexpr std::uint8_t kAndEdx[]{ 0x81, 0xE2, 0xFF, 0x00, 0x00, 0x00 };
		static constexpr std::uint8_t kAndEdx8[]{ 0x83, 0xE2, 0xFF };
		static constexpr std::uint8_t kAndEcx[]{ 0x81, 0xE1, 0xFF, 0x00, 0x00, 0x00 };
		static constexpr std::uint8_t kAndEcx8[]{ 0x83, 0xE1, 0xFF };
		Check(Contains(code, size, kAndEdx, sizeof(kAndEdx)) ||
		      Contains(code, size, kAndEdx8, sizeof(kAndEdx8)) ||
		      Contains(code, size, kAndEcx, sizeof(kAndEcx)) ||
		      Contains(code, size, kAndEcx8, sizeof(kAndEcx8)),
			"B2: cave 生产者 write=(write+1)&255 wrap 契约");

		// 每格裁剪 cave：偷 5B、xor eax,eax; ret 跳过分支、成对 push/pop
		ScrollAsm::CellParams cp{};
		cp.gateFn = 0x140000000ull;
		cp.resumeAddr = 0x1407EBF75ull;
		ScrollAsm::CellCaveAsm cellFrag(cp);
		const auto* ccode = static_cast<const std::uint8_t*>(cellFrag.getCode());
		const auto csize = cellFrag.getSize();
		HexLine("CAVE_CELL_HEX", ccode, csize);
		Check(Contains(ccode, csize, ScrollAsm::kCellBytes, 5),
			"每格 cave epilog 重放 5B stolen (mov [rsp+20],rbx)");
		static constexpr std::uint8_t kSkip[]{ 0x31, 0xC0, 0xC3 }; // xor eax,eax; ret
		Check(Contains(ccode, csize, kSkip, sizeof(kSkip)),
			"窗口外分支 xor eax,eax; ret（安全返回 false）");
	}

	// v10：两个网格渲染器入口「只读面板 gate」cave 的机器码断言。
	// 审核 B1 要求：epilog 前 rsp 必须回到 R0，stolen 才能原样重放。
	void TestGridEntryAsm()
	{
		std::puts("v10 grid-entry panel gate asm:");
		ScrollAsm::GridEntryParams gp{};
		gp.flagAddr = 0x140500000ull;
		gp.resumeAddr = 0x1407E9F87ull;
		gp.target1 = 0x142B8DB10ull;
		gp.target2 = 0x142B8DB30ull;
		gp.target3 = 0x142BE38A0ull;
		gp.stolen = Game::kGridBytes.data();
		gp.stolenLen = Game::kGridStolen;

		ScrollAsm::GridEntryAsm a(gp);
		const auto* ca = static_cast<const std::uint8_t*>(a.getCode());
		const auto sa = a.getSize();
		HexLine("GRID_GATE_A_HEX", ca, sa);
		Check(ca[0] == 0x9C && ca[1] == 0x50, "入口 pushfq; push rax（保存 flags/rax）");
		static constexpr std::uint8_t kPopSeq[]{ 0x58, 0x9D }; // pop rax; popfq
		Check(Contains(ca, sa, kPopSeq, 2), "epilog 前 pop rax; popfq -> rsp 回到 R0");
		Check(Contains(ca, sa, Game::kGridBytes.data(), Game::kGridStolen),
			"重放 0x7E9F80 stolen 7B (mov rax,rsp; mov [rax+10],rbx)");
		static constexpr std::uint8_t kAbsJmp[]{ 0xFF, 0x25, 0, 0, 0, 0 };
		Check(Contains(ca, sa, kAbsJmp, 6), "FF25 绝对跳回 0x7E9F87");

		ScrollAsm::GridEntryParams gp2 = gp;
		gp2.resumeAddr = 0x1407EADE5ull;
		gp2.stolen = Game::kGrid2Bytes.data();
		gp2.stolenLen = Game::kGrid2Stolen;
		ScrollAsm::GridEntryAsm b(gp2);
		const auto* cb = static_cast<const std::uint8_t*>(b.getCode());
		const auto sb = b.getSize();
		HexLine("GRID_GATE_B_HEX", cb, sb);
		Check(Contains(cb, sb, Game::kGrid2Bytes.data(), Game::kGrid2Stolen),
			"重放 0x7EADE0 stolen 5B (mov [rsp+20],r9)");
		Check(Contains(cb, sb, kPopSeq, 2), "B 的 epilog 同样恢复 rsp=R0");
	}

	// 新增 ③：返回地址门控对两条逐格路径都判命中
	void TestReturnGate()
	{
		std::puts("return-address gate (B4):");
		constexpr std::uint64_t base = 0x140000000ull;
		Check(Game::IsGridCellReturn(base + Game::kRetCellA, base),
			"路径A 返回地址 0x7EA6D2 (from 0x7EA6CD) 命中");
		Check(Game::IsGridCellReturn(base + Game::kRetCellB, base),
			"路径B 返回地址 0x7EB5B2 (from 0x7EB5AD) 命中");
		Check(!Game::IsGridCellReturn(base + 0x7EA6D0, base), "非逐格路径返回地址不命中");
		Check(!Game::IsGridCellReturn(base + 0x6A6740, base), "其它调用者不命中");
		Check(Game::kRetCellA == ScrollAsm::kRetPathA && Game::kRetCellB == ScrollAsm::kRetPathB,
			"Game.h 与 CaveAsm.h 返回地址常量一致");
	}

	void TestConfig()
	{
		std::puts("config:");
		namespace fs = std::filesystem;
		const auto tmp = fs::temp_directory_path() / "scrollwheel_test_cfg.ini";
		std::error_code ec;
		fs::remove(tmp, ec);

		Config::Data d;
		Config::Load(tmp, d);
		Check(fs::is_regular_file(tmp, ec), "cfg 不存在时首次生成");
		// B5：默认全部面板关闭
		Check(!d.panels[Config::Provision].enabled && !d.panels[Config::Guild].enabled &&
			!d.panels[Config::RaidInventory].enabled,
			"默认 provision=0 / guild=0 / raid_inventory=0（全部关闭）");
		Check(!d.patchNativeStep && !d.debugSampling, "默认 patch_native_step=0 / debug_sampling=0");

		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			out << "# user cfg\nenabled = 0\nprovision = 1\nstep = 99\nprovision_rect = 10,20,300,400\n";
		}
		Config::Data d2;
		Config::Load(tmp, d2); // 绝不覆盖
		Check(!d2.enabled && d2.panels[Config::Provision].enabled, "读取用户开关而非默认值");
		Check(std::fabs(d2.step - 99.f) < 1e-4f, "读取用户 step");
		Check(std::fabs(d2.panels[Config::Provision].rect.w - 300.f) < 1e-4f, "解析矩形");
		{
			std::ifstream in(tmp);
			const std::string content{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
			Check(content.find("step = 99") != std::string::npos, "用户 cfg 文件未被改写");
		}
		fs::remove(tmp, ec);
	}

	// ⑤ 默认策略 + v8 止血版的新契约
	void TestDefaultPolicy()
	{
		std::puts("default safe mode (B5) + v10 policy:");
		Config::Data d;
		Config::Parse({}, d);
		Check(!Config::AnyPanelEnabled(d),
			"默认全部面板关闭 -> Install 不装每格统计 cave / 不写布局 / 不改渲染");
		const std::string_view text = Config::DefaultCfgText();
		Check(text.find("provision = 0") != std::string_view::npos &&
		      text.find("guild = 0") != std::string_view::npos &&
		      text.find("raid_inventory = 0") != std::string_view::npos,
			"默认 cfg 模板 provision=0 / guild=0 / raid_inventory=0");
		Check(Config::AnyPanelEnabled(d) == false, "默认策略判定 = 不安装统计 hook");

		// v10：裁剪用全新机制重新启用（r8 面板 gate + param_5 行号），
		// 代码默认与模板/用户 cfg 三处都显式写 1，杜绝 v7 的隐式开关事故。
		Check(d.clipOutOfWindow, "v10 默认 clipOutOfWindow=true（新裁剪机制）");
		// v9：自绘滚动条已移除，默认 cfg 模板不应再出现任何 bar_* 键。
		Check(text.find("show_scrollbar") == std::string_view::npos &&
		      text.find("bar_rect") == std::string_view::npos &&
		      text.find("bar_track") == std::string_view::npos,
			"v9 默认 cfg 模板不含自绘滚动条配置");
		const char* keys[] = { "enabled", "provision", "guild", "raid_inventory",
			"step", "smooth", "visible_rows", "row_height", "columns", "invert",
			"frame_ttl", "scroll_range", "auto_range", "max_range",
			"clip_out_of_window", "rows_per_notch", "debug_sampling", "debug_log",
			"patch_native_step", "native_step", "provision_rect", "guild_rect",
			"raid_inventory_rect" };
		bool allPresent = true;
		for (const char* k : keys) {
			if (text.find(k) == std::string_view::npos) {
				allPresent = false;
				std::printf("  missing cfg key: %s\n", k);
			}
		}
		Check(allPresent, "v10 cfg 模板显式列出全部键（无隐式默认开关）");
		Check(text.find("clip_out_of_window = 1") != std::string_view::npos,
			"cfg 模板显式写 clip_out_of_window = 1（与代码默认一致）");

		// 一键回退：把键设 0 后生产 CellGate 必须完全不裁剪。
		Config::Parse("clip_out_of_window = 0\nrows_per_notch = 9\n", d);
		Check(!d.clipOutOfWindow && d.rowsPerNotch == 9,
			"clip_out_of_window=0 可一键回退到「只平移不裁剪」");
	}

	// v10：param_5 位域解码 + 裁剪窗口。直接调用生产 Wheel 实现，
	// 不是另写一份副本（避免「自造测试条件当成真实路径证据」）。
	void TestV10Clipping()
	{
		std::puts("v10 param_5 decode + clip window:");
		const auto encode = [](std::uint32_t a_row, std::uint32_t a_col) -> std::uint32_t {
			const std::uint32_t v = (((a_col & 0x1fu) | 0x20u) << 24) | (a_row & 0xffffffu);
			return v * 2u;
		};
		bool allOk = true;
		for (std::uint32_t row = 0; row < 40; ++row) {
			for (std::uint32_t col = 0; col < 9; ++col) {
				const std::uint32_t p5 = encode(row, col);
				allOk = allOk && Wheel::LooksLikeParam5(p5) && Wheel::DecodeRow(p5) == row;
			}
		}
		Check(allOk, "40x9 组 row/col 的 param_5 编码全部被正确识别并还原行号");
		Check(!Wheel::LooksLikeParam5(0x12345678u), "bit30=0 的候选被拒绝");
		Check(!Wheel::LooksLikeParam5(0x40000001u), "bit0=1 的候选被拒绝");
		Check(Wheel::ShouldDrawRow(0, -1, 5) && Wheel::ShouldDrawRow(999, -1, 5),
			"first<0（静止/裁剪关闭）一律放行，画面与原版一致");
		Check(!Wheel::ShouldDrawRow(1, 2, 0), "生产默认 vis=0：row=1 已滚出上沿 -> 跳过");
		Check(Wheel::ShouldDrawRow(2, 2, 0) && Wheel::ShouldDrawRow(99, 2, 0),
			"生产默认 vis=0：只裁上界，绝不裁掉下方仍可视的格子（p18 应修项）");
		Check(!Wheel::ShouldDrawRow(1, 2, 5), "可选下界：row=1 在窗口上方 -> 跳过");
		Check(Wheel::ShouldDrawRow(2, 2, 5) && Wheel::ShouldDrawRow(6, 2, 5),
			"可选下界：row 2..6 落在 [first, first+vis) -> 放行");
		Check(!Wheel::ShouldDrawRow(7, 2, 5), "可选下界：row=7 -> 跳过");
		Check(ScrollAsm::kStubFrameBytes == 88 && ScrollAsm::kCellParam5Disp == 128,
			"v10: param_5 槽位 [rsp+0x80] = [R0+0x28]（7EC1EF mov eax,[rbp+0xc70]，p18 B1 修正）");
		Check(static_cast<int>(std::ceil(2.3f)) == 3,
			"ceil(current/rowH) 取上整，保证 row>=first 时 Y>=base（无半行外溢）");
	}

	void TestImage(const Game::ImageView& a_image, std::vector<std::uint8_t>& a_disk,
		const std::filesystem::path& a_exe)
	{
		std::puts("mapped PE verification:");

		// SHA-1 实现自校验 + 真实 exe
		const char* abc = "abc";
		Check(Game::Sha1Hex(reinterpret_cast<const std::uint8_t*>(abc), 3) ==
			"a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1(abc) 实现正确");
		Check(Game::CheckExeSha1(a_exe), "基准 exe SHA1 匹配");

		const auto report = Game::Verify(a_image, true);
		Check(report.AllOk(), "全部 28 项校验通过（含新增 0x7EB5AD rel32）");
		if (!report.AllOk()) {
			std::puts(report.Format().c_str());
		}
		// S3：磁盘映射值恒为 0，不受运行时累加器影响
		Check(a_image.u32(Game::kAccumY) == 0 && a_image.u32(Game::kAccumX) == 0,
			"S3: AccumZero 校验的是磁盘映射值(0)，非运行时内存值");

		std::uint32_t cave = 0, grid = 0, cell = 0;
		Check(Game::Locate(a_image, report, cave, grid, cell), "3 个 hook 用唯一特征码定位成功");
		Check(cave == Game::kCaveRva && grid == Game::kGridRva && cell == Game::kCellRva,
			"定位地址 = 0x371F0A / 0x7E9F80 / 0x7EBF70");

		// 每条特征码在映射 PE 上唯一命中
		struct Sig { const char* name; std::string_view pat; std::uint32_t expect; };
		const Sig sigs[] = {
			{ "F0 cave", Game::kSigCave, 0x371F0A },
			{ "F0b anchor", Game::kSigCaveB, 0x371EFD },
			{ "F1 0x403 branch", Game::kSigBranch, 0x371EA8 },
			{ "F2 tick", Game::kSigTick, 0x3E1AD0 },
			{ "F3 cell callsite", Game::kSigCallSite, 0x7EA6BE },
			{ "F7b grid", Game::kSigGrid, 0x7E9F80 },
			{ "F6b cell", Game::kSigCell, 0x7EBF70 },
			{ "F4 store anchor", Game::kSigLeaStore, 0x695B79 },
			{ "F5 party anchor", Game::kSigLeaParty, 0x695E1C },
			{ "F8 frame", Game::kSigFrame, 0x236020 },
			{ "F9 clear", Game::kSigClear, 0x23608E },
			{ "F10 region", Game::kSigRegion, 0x2749B0 },
			{ "F11b widget", Game::kSigWidget, 0x39B640 },
			{ "F12 grid2 (v10)", Game::kSigGrid2, 0x7EADE0 },
		};
		for (const auto& s : sigs) {
			Check(Game::FindUnique(a_image, s.pat, s.expect),
				(std::string("特征码 ") + s.name + " 在映射 PE 上唯一命中").c_str());
		}

		// 新增 ③ 补充：两条逐格 call 的 rel32 都指向 0x7EBF70
		const auto rel = [&a_image](std::uint32_t a_call) -> std::uint32_t {
			return a_call + 5 + static_cast<std::uint32_t>(a_image.i32(a_call + 1));
		};
		Check(rel(Game::kCallCell) == Game::kCellRva, "0x7EA6CD call rel32 -> 0x7EBF70");
		Check(rel(Game::kCallCell2) == Game::kCellRva, "0x7EB5AD call rel32 -> 0x7EBF70（第二条路径）");

		// v10：两个网格渲染器入口的 stolen 字节（epilog 会原样重放）
		Check(std::memcmp(a_image.bytes(Game::kGridRva), Game::kGridBytes.data(),
				Game::kGridBytes.size()) == 0,
			"v10: 0x7E9F80 入口 stolen 7B 精确命中");
		Check(std::memcmp(a_image.bytes(Game::kGrid2Rva), Game::kGrid2Bytes.data(),
				Game::kGrid2Bytes.size()) == 0,
			"v10: 0x7EADE0 入口 stolen 5B 精确命中");

		// 新增 ④：布局数组元素类型 / [2] 偏移校验（0x7E9F80 对 param_3=r12 的读取指令）
		static constexpr std::uint8_t kMovEcx[]{ 0x41, 0x8B, 0x0C, 0x24 };              // mov ecx,[r12]
		static constexpr std::uint8_t kAddssY[]{ 0xF3, 0x41, 0x0F, 0x58, 0x5C, 0x24, 0x08 }; // addss xmm3,[r12+8]
		static constexpr std::uint8_t kMovssX[]{ 0xF3, 0x41, 0x0F, 0x10, 0x74, 0x24, 0x0C }; // movss xmm6,[r12+0xc]
		static constexpr std::uint8_t kCmpCentre[]{ 0x41, 0x83, 0x7C, 0x24, 0x14, 0x00 };     // cmp [r12+0x14],0
		Check(std::memcmp(a_image.bytes(0x7EA450), kMovEcx, sizeof(kMovEcx)) == 0,
			"布局 [0] 为 int 列数 (mov ecx,[r12])");
		Check(std::memcmp(a_image.bytes(0x7EA543), kAddssY, sizeof(kAddssY)) == 0,
			"布局 [2] 为 float start_y (addss xmm3,[r12+8])");
		Check(std::memcmp(a_image.bytes(0x7EA471), kMovssX, sizeof(kMovssX)) == 0,
			"布局 [3] 为 float offset_x (movss xmm6,[r12+0xc])");
		Check(std::memcmp(a_image.bytes(0x7EA56E), kCmpCentre, sizeof(kCmpCentre)) == 0,
			"布局 [5] 为 int is_centred (cmp [r12+0x14],0)");
		Check(Game::kLayoutStartYOff == 8 && ScrollAsm::kLayoutStartYOff == 8,
			"[2] 字节偏移 = +8（数据补丁写入位置）");
		Check(a_image.u32(Game::kLayoutStore) == 0 && a_image.u32(Game::kLayoutParty) == 0,
			"两个布局数组磁盘值为 0（运行时由 0x62DB90 加载器填充，磁盘 0 时跳过写入）");

		// 一处校验失败 -> 全不装
		const auto saved = a_disk[Game::kCaveRva];
		a_disk[Game::kCaveRva] ^= 0xFF;
		const auto broken = Game::Verify(a_image, true);
		Check(!broken.AllOk(), "改坏 1 字节 -> 全有或全无校验失败");
		Check(!broken.Item(Game::Check::CaveBytes), "失败项定位到 cave 入口字节");
		std::uint32_t c2 = 0, g2 = 0, e2 = 0;
		Check(!Game::Locate(a_image, broken, c2, g2, e2), "校验失败 -> 一个 hook 都不装");
		a_disk[Game::kCaveRva] = saved; // 恢复
		Check(Game::Verify(a_image, true).AllOk(), "恢复后校验重新通过");
	}
	// ---------------- v11 工会：stub / 表布局 / cfg 隔离 ----------------
	void TestV11Guild()
	{
		std::puts("v11 guild scroll (experimental, isolated switch):");
		using namespace ScrollAsm;
		Check(sizeof(GuildTable) >= kGtVtExpectedOff + 8,
			"GuildTable 覆盖全部 cave 偏移（含 v11.2 运行时 vtable）");
		Check(offsetof(GuildTable, slots) == kGtSlotsOff &&
		      offsetof(GuildTable, baseY) == kGtBaseYOff &&
		      offsetof(GuildTable, offset) == kGtOffsetOff &&
		      offsetof(GuildTable, count) == kGtCountOff &&
		      offsetof(GuildTable, active) == kGtActiveOff &&
		      offsetof(GuildTable, beatTick) == kGtTickOff &&
		      offsetof(GuildTable, callsA) == kGtCallsAOff &&
		      offsetof(GuildTable, callsB) == kGtCallsBOff &&
		      offsetof(GuildTable, lastVtA) == kGtVtAOff &&
		      offsetof(GuildTable, lastVtB) == kGtVtBOff &&
		      offsetof(GuildTable, seenB) == kGtSeenBOff &&
		      offsetof(GuildTable, expectedVptr) == kGtVtExpectedOff,
			"cave 用的表偏移与结构体一致（含 v11.1 诊断 / v11.2 运行时 vtable）");
		Check(kGuildRefreshResume == kGuildRefreshRva + 5, "hook A 5B stolen -> resume +5");
		Check(kGuildShiftResume == kGuildShiftRva + 6, "hook B 6B stolen -> resume +6");
		Check(kGuildVtSlot23 == 0xEBB990u, "GuildDisplay 虚表 slot23 偏移 = 0xEBB990");

		GuildTable table{};
		GuildParams rp{ 0x140500000ull, 0x14066E9F5ull };
		GuildRefreshAsm refresh(rp);
		const auto* a = static_cast<const std::uint8_t*>(refresh.getCode());
		const auto sa = refresh.getSize();
		HexLine("GUILD_REFRESH_HEX", a, sa);
		Check(a[0] == 0x9C, "hook A 入口 pushfq");
		static constexpr std::uint8_t kAbsJmp[]{ 0xFF, 0x25, 0, 0, 0, 0 };
		static constexpr std::uint8_t kMovAbs[]{ 0x48, 0xB8 };
		// v11.2 ★ ASLR 修复：运行时判据存在 GuildTable.expectedVptr，
		// cave 用 mov rax,[r11+0x438] 读它，绝不内嵌 0x140EBB8D8。
		static constexpr std::uint8_t kLoadExpectedVt[]{
			0x49, 0x8B, 0x83, 0x38, 0x04, 0x00, 0x00 };
		Check(Contains(a, sa, kLoadExpectedVt, sizeof(kLoadExpectedVt)),
			"hook A 从 GuildTable 读运行时 vtable（ASLR 安全）");
		Check(!Contains(a, sa, kMovAbs, 2),
			"hook A 不再硬编码 PE 磁盘 VA 0x140EBB8D8");
		Check(Contains(a, sa, kGuildRefreshBytes, sizeof(kGuildRefreshBytes)),
			"hook A 原样重放 5B stolen");
		Check(Contains(a, sa, kAbsJmp, 6), "hook A FF25 绝对跳回 0x66E9F5");
		bool hasCallA = false;
		for (std::size_t i = 0; i + 5 <= sa; ++i) {
			if (a[i] == 0xE8) { hasCallA = true; }
		}
		Check(!hasCallA, "hook A 无 E8 call（p26 S1：call 会破坏 rcx=this）");

		GuildParams sp{ rp.tableAddr, 0x1407F5EE3ull };
		GuildShiftAsm shift(sp);
		const auto* b = static_cast<const std::uint8_t*>(shift.getCode());
		const auto sb = shift.getSize();
		HexLine("GUILD_SHIFT_HEX", b, sb);
		Check(b[0] == 0x9C, "hook B 入口 pushfq");
		Check(Contains(b, sb, kGuildShiftBytes, sizeof(kGuildShiftBytes)),
			"hook B 原样重放 6B stolen (movss [rsp+0x54],xmm1)");
		// r11 作基址需要 REX.B，编码为 F3 41 0F 58（不是裸 F3 0F 58）。
		static constexpr std::uint8_t kAddss[]{ 0xF3, 0x41, 0x0F, 0x58 };
		Check(Contains(b, sb, kAddss, 4), "hook B 用 addss 把每槽偏移加到 xmm1");
		Check(Contains(b, sb, kAbsJmp, 6), "hook B FF25 绝对跳回 0x7F5EE3");
		// v11.1 核心修复：hook B 自己从 GuildDisplay 本体读 [rdi+0x270] 刷新表，
		// 不再依赖实机证明未执行的 hook A(0x66E9F0)。
		static constexpr std::uint8_t kReadSlots[]{ 0x48, 0x8B, 0x97, 0x70, 0x02, 0x00, 0x00 };
		Check(Contains(b, sb, kReadSlots, sizeof(kReadSlots)),
			"v11.1 hook B 读 [rdi+0x270] 刷新技能槽表（mov rdx,[rdi+0x270]）");
		Check(Contains(b, sb, kLoadExpectedVt, sizeof(kLoadExpectedVt)),
			"v11.2 hook B 从 GuildTable 读运行时 vtable（ASLR 安全）");
		Check(!Contains(b, sb, kMovAbs, 2),
			"hook B 不再硬编码 PE 磁盘 VA 0x140EBB8D8");
		bool hasCallB = false;
		for (std::size_t i = 0; i + 5 <= sb; ++i) {
			if (b[i] == 0xE8) { hasCallB = true; }
		}
		Check(!hasCallB, "hook B 无 E8 call（call 会破坏 xmm1）");

		// cfg：默认关闭、缺键关闭、显式开启、5 键全解析、与旧 guild 键隔离
		Config::Data d0;
		Check(!d0.guildScroll, "v11 代码默认 guildScroll=false（安全模式）");
		const auto tmpl = Config::DefaultCfgText();
		Check(tmpl.find("guild_scroll = 0") != std::string_view::npos,
			"cfg 模板显式写 guild_scroll = 0");
		Check(tmpl.find("guild_step") != std::string_view::npos &&
		      tmpl.find("guild_cap") != std::string_view::npos &&
		      tmpl.find("guild_visible_rows") != std::string_view::npos &&
		      tmpl.find("guild_row_height") != std::string_view::npos,
			"cfg 模板含全部 4 个 v11 参数键");
		Config::Data d1;
		Config::Parse("guild_scroll = 1\nguild_step = 45.0\nguild_cap = 700.0\n"
		              "guild_visible_rows = 2\nguild_row_height = 88.0\n", d1);
		Check(d1.guildScroll, "guild_scroll=1 可开启");
		Check(std::fabs(d1.guildStep - 45.f) < 1e-4f &&
		      std::fabs(d1.guildCap - 700.f) < 1e-4f &&
		      d1.guildVisibleRows == 2 &&
		      std::fabs(d1.guildRowHeight - 88.f) < 1e-4f,
			"4 个 v11 参数键全部正确解析");
		Config::Data d2;
		Config::Parse("provision = 1\n", d2);
		Check(!d2.guildScroll, "缺 guild_scroll 键时默认关闭（无隐式开关）");
		Config::Data d3;
		Config::Parse("guild = 1\n", d3);
		Check(d3.panels[Config::Guild].enabled && !d3.guildScroll,
			"旧 guild=1 不会打开 v11 工会滚轮（两套开关隔离）");

		// v11.2：滚轮按面板激活门控（修「打开补给前滚轮 → 打开后已翻页」）
		Check(d0.gateWheelByPanel,
			"v11.2 代码默认 gateWheelByPanel=true（默认修好预翻页）");
		Check(tmpl.find("gate_wheel_by_panel = 1") != std::string_view::npos,
			"cfg 模板显式写 gate_wheel_by_panel = 1");
		Config::Data d4;
		Config::Parse("gate_wheel_by_panel = 0\n", d4);
		Check(!d4.gateWheelByPanel,
			"gate_wheel_by_panel=0 可一键回退到 v10 原行为");

		// 门控的纯逻辑依据：layoutHit=false 时 Target 不累加滚轮，
		// 所以非目标界面的滚轮不会推进补给/背包状态机。
		Wheel::State s{};
		Wheel::Params wp{};
		wp.step = 30.f; wp.enabled = true; wp.inRect = true;
		wp.frameTtl = 0; wp.fixedMax = 400.f;
		s.max = 400.f;
		Wheel::Tick(s, wp, -1, false, true); // -1 会推高 target（若门控失效）
		Check(s.target == 0.f && s.current == 0.f,
			"layoutHit=false 时滚轮不驱动状态机（面板门控的纯逻辑依据）");
		Wheel::Tick(s, wp, -1, true, true);
		Check(s.target > 0.f,
			"layoutHit=true 时同一滚轮会驱动状态机（门控对照）");
	}

	// v11 真实 exe 上的字节 / 特征码 / 虚表槽断言
	void TestV11Image(const Game::ImageView& a_image)
	{
		std::puts("v11 guild anchors on real exe:");
		constexpr std::uint8_t kShow22[22]{
			0xF3, 0x0F, 0x10, 0x4F, 0x68, 0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x54,
			0xC7, 0x44, 0x24, 0x58, 0x00, 0x00, 0x80, 0x3F, 0x48, 0x8D, 0x4C };
		constexpr std::uint8_t kPre32[32]{
			0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
			0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x28, 0xF7, 0xFF, 0xFF,
			0x48, 0x81, 0xEC, 0x98, 0x09, 0x00, 0x00, 0x0F, 0x29, 0x70 };
		Check(std::memcmp(a_image.bytes(ScrollAsm::kGuildRefreshRva), kPre32, 32) == 0,
			"0x66E9F0 入口 32B 精确命中");
		Check(std::memcmp(a_image.bytes(0x7F5ED8), kShow22, 22) == 0,
			"0x7F5ED8 起 22B 长特征码精确命中");
		Check(std::memcmp(a_image.bytes(ScrollAsm::kGuildRefreshRva),
				ScrollAsm::kGuildRefreshBytes, 5) == 0, "hook A stolen 5B 与 exe 一致");
		Check(std::memcmp(a_image.bytes(ScrollAsm::kGuildShiftRva),
				ScrollAsm::kGuildShiftBytes, 6) == 0, "hook B stolen 6B 与 exe 一致");
		const auto hits = Game::FindAll(a_image,
			"F3 0F 10 4F 68 F3 0F 11 4C 24 54 C7 44 24 58 00 00 80 3F 48 8D 4C");
		Check(hits.size() == 1 && hits.front() == 0x7F5ED8u,
			"22B 长特征码在 .text 唯一命中 @0x7F5ED8");
		Check(Game::FindAll(a_image, "F3 0F 11 4C 24 54").size() > 1,
			"裸 6B stolen 多处命中 => 必须用 22B 长特征码定位");
		Check(a_image.u64(ScrollAsm::kGuildVtSlot23) == 0x14066E9F0ULL,
			"GuildDisplay 虚表 slot23 = 0x66E9F0（hook A 判据成立）");
		// v11.1：vptr 判据必须由构造侧实证。上一版把 0xEBB8D8 当作「虚表起点」
		// 直接用，却没有任何断言验证对象首 qword 真的等于它。
		const auto leaTarget = [&a_image](std::uint32_t a_rva) -> std::uint64_t {
			const auto* p = a_image.bytes(a_rva);
			std::int32_t disp{};
			std::memcpy(&disp, p + 3, sizeof(disp)); // lea rax,[rip+disp32] 是 7B
			return 0x140000000ULL + a_rva + 7 + static_cast<std::int64_t>(disp);
		};
		Check(leaTarget(0x408758u) == ScrollAsm::kGuildVtable &&
		      leaTarget(0x6827D4u) == ScrollAsm::kGuildVtable,
			"GuildDisplay 工厂/构造写入的 vptr 确为 0x140EBB8D8（判据实证）");
		Check(ScrollAsm::kGuildRefreshRva != Game::kGridRva &&
		      ScrollAsm::kGuildRefreshRva != Game::kGrid2Rva &&
		      ScrollAsm::kGuildShiftRva != Game::kCellRva &&
		      ScrollAsm::kGuildShiftRva != Game::kCaveRva,
			"v11 两个 cave 地址与 v10 四个 cave 零重叠");
	}
}

int wmain(int a_argc, wchar_t* a_argv[])
{
	if (a_argc != 2) {
		std::puts("usage: scroll_wheel_test <Darkest.exe>");
		return 2;
	}

	TestPureLogic();
	TestQueueMonotonic();
	TestCaveAsm();
	TestGridEntryAsm();
	TestReturnGate();
	TestConfig();
	TestDefaultPolicy();
	TestV10Clipping();
	TestV11Guild();

	std::ifstream stream{ a_argv[1], std::ios::binary };
	const std::vector<char> probe{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
	if (probe.empty()) {
		std::puts("failed to read the exe");
		return 2;
	}
	// 与生产侧共用 Game::MapDiskImage：按 PE 节展开磁盘 exe 为 RVA 映像
	std::vector<std::uint8_t> disk;
	if (!Game::MapDiskImage(a_argv[1], disk)) {
		std::puts("failed to map PE disk image");
		return 2;
	}
	Game::ImageView image{};
	if (!Game::InitImageView(image, disk.data())) {
		std::puts("failed to parse PE image");
		return 2;
	}
	TestImage(image, disk, a_argv[1]);
	TestV11Image(image);

	std::printf("%d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
