#include "Hooks.h"

#include "CaveAsm.h"
#include "Config.h"
#include "Game.h"
#include "Wheel.h"

#include <xbyak/xbyak.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

// PCH.h 提供 DKUtil / Win32 / 日志宏；本文件是唯一接触 DKUtil 与游戏内存的实现。
//
// P4 新架构（零入口 detour，消除 B3/B4）：
//   (1) 输入 cave      @0x371F0A：Xbyak 纯整数 stub，捕获原始 wheel.y 入 SPSC 队列
//   (2) 每格统计 cave  @0x7EBF70：Xbyak stub jmp 进 C++ 门控，恒定放行、只计数
//   (3) 纯数据补丁：每帧把 start_y = base - current 以绝对值写入 .data 数组 [2]
// v9：彻底移除自绘滚动条（不创建任何窗口、不链接 gdi32、不处理鼠标消息）。
//     饰品库等界面本来就有游戏原生滚动条，DLL 只负责滚动本身。
// 不再有 GridHook/CellHook 的 write_detour 转调，因此不存在 stolen prolog 的
// rsp 基准指令被逐字重放导致栈帧错位的问题（B3 消失）。

namespace Hooks
{
	namespace
	{
		using namespace DKUtil::Alias;

		// v6 新增目标（p7-layouts.md，均 [静态] 反汇编确证）：
		//  0x2BE38A0  地牢背包 Panel_Inventory 的 6 元素布局数组（0x736320 lea r8），[2]=start_y
		//  0x2B8EBAC  工会 GuildDisplay 的 skill_pos.y 起始 Y（0x683829 addss xmm2,[rip]）
		//  0x2B8EBB4  工会 skill_spacing.y = 91（行高，用于工会动态范围）
		constexpr std::uint32_t kLayoutRaid   = 0x2BE38A0;
		constexpr std::uint32_t kGuildStartY = 0x2B8EBAC;
		constexpr std::uint32_t kGuildStepY  = 0x2B8EBB4;
		constexpr std::uint32_t kLayoutStore = Game::kLayoutStore;
		constexpr std::uint32_t kLayoutParty = Game::kLayoutParty;
		constexpr std::uint32_t kOffStartY   = 0x08; // 布局数组 [2]
		constexpr std::uint32_t kOffRowH     = 0x10; // 布局数组 [4] offset_y

		// 三个锚点的指令字节 + rip 相对目标，做「全有或全无」之外的额外版本校验。
		// 不改原有 28 项枚举，失败同样一个 hook 都不装。
		bool VerifyExtra(std::uintptr_t a_base)
		{
			const auto rd32 = [](std::uintptr_t a_p) {
				std::uint32_t v{};
				std::memcpy(&v, reinterpret_cast<const void*>(a_p), 4);
				return v;
			};
			struct Anchor
			{
				std::uint32_t site;
				std::uint8_t  prefix[4];
				std::uint32_t prefixLen; // lea 只有 3 字节前缀，第 4 字节是 disp32 低字节
				std::uint32_t insnLen;
				std::uint32_t expectRva;
			};
			constexpr Anchor anchors[]{
				{ 0x736320, { 0x4C, 0x8D, 0x05, 0 }, 3, 7, kLayoutRaid },   // lea r8,[rip+..]
				{ 0x683803, { 0xF3, 0x0F, 0x10, 0x15 }, 4, 8, kGuildStepY }, // movss xmm2,[rip+..]
				{ 0x683829, { 0xF3, 0x0F, 0x58, 0x15 }, 4, 8, kGuildStartY } // addss xmm2,[rip+..]
			};
			for (const auto& a : anchors) {
				const auto p = a_base + a.site;
				const auto* b = reinterpret_cast<const std::uint8_t*>(p);
				for (std::size_t i = 0; i < a.prefixLen; ++i) {
					if (b[i] != a.prefix[i]) {
						WARN("extra anchor @+{:X} byte[{}] mismatch", a.site, i);
						return false;
					}
				}
				const auto rel = static_cast<std::int32_t>(rd32(p + a.insnLen - 4));
				const auto target = static_cast<std::uint32_t>(
					static_cast<std::int64_t>(a.site) + a.insnLen + rel);
				if (target != a.expectRva) {
					WARN("extra anchor @+{:X} rip target +{:X} != expected +{:X}",
						a.site, target, a.expectRva);
					return false;
				}
			}
			// v10：两个网格渲染器入口的 stolen 字节必须逐字命中，
			// 否则 epilog 重放会执行错误指令（全有或全无）。
			if (std::memcmp(reinterpret_cast<const void*>(a_base + Game::kGridRva),
					Game::kGridBytes.data(), Game::kGridBytes.size()) != 0 ||
				std::memcmp(reinterpret_cast<const void*>(a_base + Game::kGrid2Rva),
					Game::kGrid2Bytes.data(), Game::kGrid2Bytes.size()) != 0) {
				WARN("extra anchor: grid entry bytes mismatch (+{:X} / +{:X})",
					Game::kGridRva, Game::kGrid2Rva);
				return false;
			}
			return true;
		}

		// v11 工会两个入口的字节 / 长特征码 / 虚表槽校验。
		// **只在 guild_scroll=1 时调用**；失败只把 g_guildEnabled 置 false，
		// 绝不 return —— 否则会连带禁用已实机验收的 v10 四个 cave（p26 审核 S3）。
		bool VerifyGuild(std::uintptr_t a_base, const Game::ImageView& a_image)
		{
			constexpr std::uint8_t kShow22[22]{
				0xF3, 0x0F, 0x10, 0x4F, 0x68, 0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x54,
				0xC7, 0x44, 0x24, 0x58, 0x00, 0x00, 0x80, 0x3F, 0x48, 0x8D, 0x4C };
			constexpr std::uint8_t kPre32[32]{
				0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
				0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x28, 0xF7, 0xFF, 0xFF,
				0x48, 0x81, 0xEC, 0x98, 0x09, 0x00, 0x00, 0x0F, 0x29, 0x70 };
			if (std::memcmp(a_image.bytes(ScrollAsm::kGuildRefreshRva), kPre32, sizeof(kPre32)) != 0 ||
				std::memcmp(a_image.bytes(0x7F5ED8), kShow22, sizeof(kShow22)) != 0) {
				WARN("v11 guild entry bytes mismatch; guild hooks skipped (v10 untouched)");
				return false;
			}
			// 裸 6B stolen 在 .text 有 26 处命中，必须用 22B 长特征码定位。
			const auto hits = Game::FindAll(a_image,
				"F3 0F 10 4F 68 F3 0F 11 4C 24 54 C7 44 24 58 00 00 80 3F 48 8D 4C");
			if (hits.size() != 1 || hits.front() != 0x7F5ED8u) {
				WARN("v11 guild Show signature hits={} (expected 1 @+7F5ED8); "
				     "guild hooks skipped (v10 untouched)", hits.size());
				return false;
			}
			// GuildDisplay 虚表 slot23 必须就是 0x66E9F0，否则 hook A 判据失效。
			if (a_image.u64(ScrollAsm::kGuildVtSlot23) != 0x14066E9F0ULL) {
				WARN("v11 guild vtable slot23 mismatch; guild hooks skipped (v10 untouched)");
				return false;
			}
			// 运行时 stolen 字节再核一次（防止别的插件已改过这两处）。
			if (std::memcmp(reinterpret_cast<const void*>(a_base + ScrollAsm::kGuildRefreshRva),
					ScrollAsm::kGuildRefreshBytes, sizeof(ScrollAsm::kGuildRefreshBytes)) != 0 ||
				std::memcmp(reinterpret_cast<const void*>(a_base + ScrollAsm::kGuildShiftRva),
					ScrollAsm::kGuildShiftBytes, sizeof(ScrollAsm::kGuildShiftBytes)) != 0) {
				WARN("v11 guild runtime stolen bytes mismatch; guild hooks skipped (v10 untouched)");
				return false;
			}
			return true;
		}

		std::uintptr_t g_base{ 0 };
		Config::Data   g_cfg{};
		Wheel::State   g_state{};
		Wheel::Params  g_params{};

		// 纯数据补丁的 base 自适应状态（S1）：无 per-call 保存/恢复、无全局单例指针、
		// 不依赖任何函数退出路径。每帧读回当前值，若与上次写入的绝对值不同即认为
		// 布局加载器重载了布局，刷新 base。
		struct LayoutTrack
		{
			float base{ 0.f };
			float lastWritten{ 0.f };
			bool  haveBase{ false };
		};
		LayoutTrack g_trackStore{}; // 0x2B8DB10 Provision store
		LayoutTrack g_trackParty{}; // 0x2B8DB30 Provision party
		LayoutTrack g_trackRaid{};   // 0x2BE38A0 地牢背包
		// v8：工会 0x2B8EBAC 不再写入（共享常量 + 一次性快照导致英雄网格错位）。
		// 该地址仅保留在 DIAG 里做只读观测。
		std::uint32_t g_peakCells{ 0 }; // 仅 tick 线程访问：最近一批次格子数峰值（动态范围用）

		// v5：每帧驱动改为独立 tick 线程，不再依赖「每格绘制函数是否被调用」。
		// 线程写、cave 读的状态一律用原子量。
		std::atomic<bool>          g_tickStop{ false };
		std::atomic<std::uint32_t> g_firstRow{ 0 };   // 线程写 / CellGate 读
		std::atomic<std::uint32_t> g_cellIndex{ 0 };  // CellGate 累加 / 线程取走统计格子数
		std::atomic<std::uint64_t> g_cellHits{ 0 };   // 诊断：每格 cave 门控命中次数
		std::atomic<std::int64_t>  g_wheelSum{ 0 };   // 诊断：累计消费到的 wheel.y
		std::atomic<std::uint64_t> g_tickCount{ 0 };  // 诊断：tick 次数
		std::atomic<std::uint32_t> g_lastIdx{ 0 };    // 诊断：最近一次每格调用的 param_3
		std::uint64_t              g_lastDiagMs{ 0 }; // 仅 tick 线程访问
		int                        g_totalRows{ 0 };  // 仅 tick 线程访问：当前列表总行数（动态范围用）

		// ---- v10 面板 gate + 逐格裁剪状态（跨线程一律原子）----
		// g_activeLayout 由两个网格渲染器入口的只读 cave 写：
		//   0 = 非目标面板（含城镇饰品库 0x6A6739 -> 0x2B8E8EC）→ 每格 cave 零干预
		//   1 = 0x2B8DB10 store / 2 = 0x2B8DB30 party / 3 = 0x2BE38A0 地牢背包
		// MSVC x64 的 atomic<uint32_t> 是 4 字节对齐整数，cave 用普通 mov dword
		// 写入与 store(relaxed) 等价；读侧用 load(acquire)。
		std::atomic<std::uint32_t> g_activeLayout{ 0 };
		std::atomic<std::uint32_t> g_maxRow{ 0 }; // 观测到的最大 row（真实总行数-1）
		// 每套布局各自的裁剪首行（索引 1=store / 2=party / 3=raid），<0 = 静止零干预。
		// 三套布局的 [4] 行高可能不同；用单一 first 会让非活跃布局的窗口错位（p18 应修项）。
		std::atomic<std::int32_t> g_clipFirst[4]{ -1, -1, -1, -1 };
		// 0 = 只裁上界（不裁下界）。真实可见行数静态无法确证，硬编码会裁掉
		// 仍在可视区的格子并让点击失效，故生产默认 0。
		std::atomic<std::uint32_t> g_visibleRows{ 0 };
		std::atomic<std::uint32_t> g_p5Src{ 0 };         // 诊断：param_5 命中槽位 1/2/3
		std::atomic<std::uint32_t> g_p5Fail{ 0 };        // 诊断：三个候选都不匹配
		std::atomic<std::uint32_t> g_skipCount{ 0 };     // 诊断：被裁剪的格子数

		// ---- v11 工会技能栏滚轮（实验性，独立开关）----
		// 两个新 cave 只读写这张 DLL 侧表 + 自己的栈帧/xmm1，绝不写游戏对象、
		// 绝不写 0x2B8EBAC（v6 实机证伪点）。v11 实机证明 hook A(0x66E9F0) 从未
		// 执行（gBeat 恒 0），v11.1 起改由 hook B(0x7F5EDD, Base::Show 每帧必经)
		// 在遇到 GuildDisplay 本体时刷新；hook A 保留为备用 + 诊断。
		// 每帧刷新使 0x682B90 切英雄 clear+析构不会留下悬垂命中。
		alignas(64) ScrollAsm::GuildTable g_guild{};
		Wheel::State  g_guildState{};
		Wheel::Params g_guildParams{};
		bool          g_guildEnabled{ false };  // 校验通过 && cfg 打开
		bool          g_guildActive{ false };   // 仅 tick 线程访问，供输入路由
		// p28 应修 1：活跃窗口收紧到 3 tick（≈48ms）。hook A 在工会显示时每帧
		// 都会刷新 lastBeat，3 tick 足够容纳一次帧错位；窗口越短，离开工会后
		// 残留 slots[] 与堆地址复用叠加造成跨界面误伤的机会越小。
		constexpr std::uint32_t kGuildActiveTicks = 3;
		constexpr float         kGuildOffscreen = -100000.f; // 有限值，避免 inf/NaN

		// v11.2：补给/背包「面板激活」门控。30 tick ≈ 480ms 宽限窗口，
		// 用来吸收 16ms tick 线程与游戏渲染帧之间的相位差（否则补给界面里
		// 第一格滚轮可能因某一 tick 恰好 cells=0 被吞掉）。仅 tick 线程访问。
		constexpr std::uint32_t kPanelTtlTicks = 30;
		std::uint32_t g_lastPanelTick{ 0 };
		bool          g_panelSeen{ false };

		// param_5 判定 / 行号解码 / 窗口判定都在 Wheel 纯逻辑模块里
		// （生产 CellGate 与离线单测共用同一份实现，见 Wheel.h）。
		void UpdateMax(std::atomic<std::uint32_t>& a_atom, std::uint32_t a_v) noexcept
		{
			auto cur = a_atom.load(std::memory_order_relaxed);
			while (cur < a_v && !a_atom.compare_exchange_weak(
				cur, a_v, std::memory_order_relaxed, std::memory_order_relaxed)) {
			}
		}

		// ---------------- 鼠标区域判定（三重 gate 之一，保持不坏）----------------
		bool MouseInRect(const Config::Rect& a_rect)
		{
			if (a_rect.w <= 0.f || a_rect.h <= 0.f) {
				return true;
			}
			// v5：默认全屏矩形直接放行。后台线程做 Win32 窗口/进程判定时，
			// 任何一步返回异常都会把 inRect 判成 false，从而静默禁用滚轮；
			// 只有用户把矩形收紧到真实网格区域时才需要它。
			if (a_rect.x <= 0.f && a_rect.y <= 0.f &&
				a_rect.w >= 1920.f && a_rect.h >= 1080.f) {
				return true;
			}
			POINT screen{};
			if (!::GetCursorPos(&screen)) {
				return true;
			}
			HWND hwnd = ::WindowFromPoint(screen);
			if (!hwnd) {
				return true;
			}
			DWORD pid = 0;
			::GetWindowThreadProcessId(hwnd, &pid);
			if (pid != ::GetCurrentProcessId()) {
				return false; // 鼠标不在游戏窗口
			}
			POINT client = screen;
			if (!::ScreenToClient(hwnd, &client)) {
				return true;
			}
			RECT cr{};
			if (!::GetClientRect(hwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) {
				return true;
			}
			const float x = static_cast<float>(client.x) * 1920.f / static_cast<float>(cr.right);
			const float y = static_cast<float>(client.y) * 1080.f / static_cast<float>(cr.bottom);
			return x >= a_rect.x && x < a_rect.x + a_rect.w &&
			       y >= a_rect.y && y < a_rect.y + a_rect.h;
		}

		// ---------------- 纯数据补丁（S1 / 布局数组语义已 [静态] 复核）----------------
		// 0x2B8DB10 / 0x2B8DB30 是 Provision store / party 的布局数组，
		// 元素：[0]int 列数 [1]float start_x [2]float start_y [3]float offset_x
		//       [4]float offset_y [5]int is_centred（0x7E9F80 汇编 + 0x628A90 描述表）。
		// 写绝对值、不保存/恢复；磁盘值 0 / 非有限 / 列数<1 时跳过，绝不写脏数据。
		// v6：泛化到「布局数组 [2]」与「工会单个 float 全局」两类目标。
		// a_isArray=true 时额外校验 [0] 列数有效；工会全局没有列数字段。
		void WriteOffset(std::uint32_t a_rva, std::uint32_t a_byteOff,
			LayoutTrack& a_track, bool a_isArray)
		{
			auto* target = reinterpret_cast<float*>(g_base + a_rva + a_byteOff);
			const float curY = *target;
			if (curY == 0.f || !std::isfinite(curY)) {
				return; // 布局未加载（磁盘值为 0）/ 异常：跳过，绝不写脏数据
			}
			if (a_isArray) {
				const std::int32_t cols = *reinterpret_cast<const std::int32_t*>(g_base + a_rva);
				if (cols < 1 || cols > 1024) {
					return;
				}
			}
			const float current = Wheel::InjectOffset(g_state);
			// base 自适应：读回值与上次写入的绝对值不同 -> 布局加载器重载 -> 刷新 base
			if (!a_track.haveBase || std::fabs(curY - a_track.lastWritten) > 1e-3f) {
				a_track.base = curY;
				a_track.haveBase = true;
			}
			const float shifted = a_track.base - current;
			if (!std::isfinite(shifted)) {
				return;
			}
			*target = shifted; // 绝对值写入，无保存/恢复、不依赖函数退出路径
			a_track.lastWritten = shifted;
		}

		// ---------------- v11 工会独立状态机 ----------------
		// tick 线程只读写 DLL 自己的表，绝不碰游戏对象；两个 cave 都在游戏主线程。
		// 绘制/命中/tooltip 同源已由 p25-b 静态闭环确证：改 Base::Show 栈帧里的 Y
		// 会让 0x274CA0 快照矩阵、0x273800 重放、0x273600 逆变换一起跟随。
		void UpdateGuild(std::int64_t a_wheel)
		{
			if (!g_guildEnabled) {
				g_guild.active.store(0u, std::memory_order_relaxed);
				g_guildActive = false;
				return;
			}
			const std::uint32_t tick = g_guild.beatTick.fetch_add(1, std::memory_order_relaxed) + 1;
			const std::uint32_t last = g_guild.lastBeat.load(std::memory_order_relaxed);
			std::uint32_t n = g_guild.count.load(std::memory_order_relaxed);
			if (n > ScrollAsm::kGuildMaxSlots) {
				n = ScrollAsm::kGuildMaxSlots;
			}
			const bool active = (tick - last) <= kGuildActiveTicks && n > 0;
			g_guild.active.store(active ? 1u : 0u, std::memory_order_relaxed);
			g_guildActive = active;
			if (!active) {
				// p28 应修 1：离开工会立刻让 hook B 遍历 0 个条目。即使 active
				// 还有一帧滞后，也不会命中残留 slots[]，杜绝跨界面误伤。
				g_guild.count.store(0u, std::memory_order_relaxed);
			}

			const auto bitsToFloat = [](std::uint32_t a_b) noexcept {
				float f{ 0.f };
				std::memcpy(&f, &a_b, sizeof(f));
				return f;
			};
			// 行高优先取运行时技能槽 Y 的相邻差（完全不硬编码 91）；
			// 退化时读 0x2B8EBB4（磁盘为 0，运行时由布局加载器填充）；
			// 最后才用 cfg 回退。p26 审核明确禁止硬编码 91。
			float rowH = 0.f;
			if (n >= 2) {
				const float y0 = bitsToFloat(g_guild.baseY[0]);
				const float y1 = bitsToFloat(g_guild.baseY[1]);
				if (std::isfinite(y0) && std::isfinite(y1) && y1 > y0) {
					rowH = y1 - y0;
				}
			}
			if (!(rowH > 0.f && std::isfinite(rowH))) {
				rowH = *reinterpret_cast<const float*>(g_base + kGuildStepY);
			}
			if (!(rowH > 0.f && std::isfinite(rowH))) {
				rowH = g_cfg.guildRowHeight > 0.f ? g_cfg.guildRowHeight : 1.f;
			}

			g_guildParams.step = g_cfg.guildStep;
			g_guildParams.smooth = g_cfg.smooth;
			g_guildParams.visibleRows = g_cfg.guildVisibleRows > 0 ? g_cfg.guildVisibleRows : 1;
			g_guildParams.rowHeight = rowH;
			g_guildParams.invert = g_cfg.invert;
			g_guildParams.columns = 1; // 工会技能单列线性排布（p25-c 确证）
			g_guildParams.enabled = true;
			g_guildParams.dryRun = false;
			g_guildParams.inRect = true;
			g_guildParams.frameTtl = 1;
			const float rawMax = static_cast<float>(
				n > static_cast<std::uint32_t>(g_guildParams.visibleRows)
					? n - static_cast<std::uint32_t>(g_guildParams.visibleRows) : 0u) * rowH;
			const float cap = g_cfg.guildCap > 0.f && std::isfinite(g_cfg.guildCap)
				? g_cfg.guildCap : 2000.f;
			g_guildParams.fixedMax = std::clamp(
				std::isfinite(rawMax) ? rawMax : 0.f, 0.f, cap);
			Wheel::RecomputeMax(g_guildState, g_guildParams);
			// 输入单点路由：wheel 只送给活跃的那套状态机，不会双驱动补给/背包。
			Wheel::Tick(g_guildState, g_guildParams, active ? a_wheel : 0, true, true);

			const float cur = g_guildState.current;
			std::uint32_t clipped = 0;
			for (std::uint32_t i = 0; i < ScrollAsm::kGuildMaxSlots; ++i) {
				float off = 0.f;
				if (active && i < n && cur > 0.5f) {
					const float y0 = bitsToFloat(g_guild.baseY[0]);
					const float yi = bitsToFloat(g_guild.baseY[i]);
					// 平移后 Y' = yi - cur；Y' < y0 即已滚出网格上沿。
					// 有限屏幕外负值把图标 + 命中 + tooltip 一起移出可视区
					// （p25-b 方案 b；禁止用 inf，也禁止跳 0x7F643C 公共尾声）。
					if (std::isfinite(yi) && std::isfinite(y0) && (yi - cur) < y0) {
						off = kGuildOffscreen;
						++clipped;
					} else {
						off = -cur;
					}
				}
				g_guild.offset[i] = off; // 未激活 / 静止时全 0 = 零干预
			}
			g_guild.clipped.store(clipped, std::memory_order_relaxed);
		}

		// ---------------- 每帧状态机（v5：独立 tick 线程驱动）----------------
		// v4 把 RunFrame 挂在 CellGate 里，只有当 0x7EBF70 被逐格调用且返回地址
		// 精确命中白名单时才会执行；一旦不命中，队列不消费、布局不写、滚轮完全
		// 静默失效（实机验收反馈正是这个现象）。现在由独立线程无条件驱动。
		void FrameTick()
		{
			g_tickCount.fetch_add(1, std::memory_order_relaxed);

			const std::int64_t wheel = Wheel::Drain();
			if (wheel != 0) {
				g_wheelSum.fetch_add(wheel, std::memory_order_relaxed);
			}
			// v11：先算工会活跃判据与每槽偏移，再决定 wheel 送给哪套状态机。
			UpdateGuild(wheel);

			// 取走本 tick 周期内每格 cave 统计到的格子数；用峰值做动态范围。
			const std::uint32_t cells = g_cellIndex.exchange(0, std::memory_order_relaxed);
			g_state.cellCount = cells;
			g_state.cellIndex = 0;
			if (cells > 0) {
				g_peakCells = cells;
			} else if (g_peakCells > 0) {
				--g_peakCells; // 界面关闭后慢衰减，避免旧峰值永久残留
			}

			// v10：列数/行高改从「当前活跃布局数组」读，不再永远用补给 store 的
			// 那一份（v9 日志里 cols=9 rowH=170 在背包界面也是它，动态范围因此失真）。
			const std::uint32_t active = g_activeLayout.load(std::memory_order_acquire);
			// v11.2：v10 把 Wheel::Tick 的 layoutHit 硬编码 true，且每帧无条件
			// 写三套布局数组 ⇒ 在城镇/工会滚滚轮也会改写补给 start_y，
			// 下次打开补给商店就看到「已经被翻页」。这里用真实的面板信号
			// （网格入口 cave 的 active 布局 + 逐格 cave 的 cells）做门控。
			const std::uint32_t tickNow = static_cast<std::uint32_t>(
				g_tickCount.load(std::memory_order_relaxed));
			const bool panelHit = active != 0 || cells > 0;
			if (panelHit) {
				g_lastPanelTick = tickNow;
				g_panelSeen = true;
			}
			const bool panelRecent = !g_cfg.gateWheelByPanel ||
				(g_panelSeen && (tickNow - g_lastPanelTick) <= kPanelTtlTicks);
			const std::uint32_t lr = active == 2 ? kLayoutParty
			                       : active == 3 ? kLayoutRaid
			                       : kLayoutStore;
			const auto* store = reinterpret_cast<const float*>(g_base + lr);
			int cols = g_cfg.columns;
			const std::int32_t c = *reinterpret_cast<const std::int32_t*>(store);
			if (c >= 1 && c <= 1024) {
				cols = c;
			}
			// 行高优先取运行时布局数组 [4] offset_y，取不到才回退 cfg。
			float rowH = *reinterpret_cast<const float*>(
				reinterpret_cast<const std::uint8_t*>(store) + kOffRowH);
			if (!(rowH > 0.f && std::isfinite(rowH))) {
				rowH = g_cfg.rowHeight > 0.f ? g_cfg.rowHeight : 1.f;
			}

			// v10 动态范围：优先用 param_5 观测到的**真实最大行号** + 1。
			// 这是「按物品数量增加下拉范围」的确证来源（仿 RealmInventory 0x6A57F0
			// 的 ceil(N/cols)*行高 - 可视高），不依赖与游戏帧不同步的异步计数。
			// 界面关闭后 cells 归零，maxRow 慢衰减避免旧面板峰值永久残留。
			if (cells == 0 && g_maxRow.load(std::memory_order_relaxed) > 0) {
				g_maxRow.fetch_sub(1, std::memory_order_relaxed);
			}
			const std::uint32_t maxRow = g_maxRow.load(std::memory_order_relaxed);
			g_totalRows = 0;
			if (maxRow > 0 && maxRow < 100000u) {
				g_totalRows = static_cast<int>(maxRow + 1);
			} else if (g_peakCells > 0 && cols > 0) {
				g_totalRows = static_cast<int>((g_peakCells + static_cast<std::uint32_t>(cols) - 1) /
				                              static_cast<std::uint32_t>(cols));
			}
			float autoMax = 0.f;
			if (g_cfg.autoRange && g_totalRows > 0) {
				const int vis = g_cfg.visibleRows > 0 ? g_cfg.visibleRows : 1;
				const int over = g_totalRows > vis ? g_totalRows - vis : 0;
				autoMax = static_cast<float>(over) * rowH;
			}
			float effMax = g_cfg.scrollRange;
			if (g_cfg.autoRange && autoMax > 0.f) {
				effMax = std::min(autoMax, g_cfg.maxRange);
			}
			if (!(effMax > 0.f && std::isfinite(effMax))) {
				effMax = g_cfg.scrollRange;
			}

			g_params.step = g_cfg.step;
			g_params.smooth = g_cfg.smooth;
			g_params.visibleRows = g_cfg.visibleRows > 0 ? g_cfg.visibleRows : 1;
			g_params.rowHeight = rowH;
			g_params.invert = g_cfg.invert;
			g_params.columns = cols > 0 ? cols : 1;
			g_params.frameTtl = g_cfg.frameTtl;
			g_params.fixedMax = effMax;
			g_params.enabled = g_cfg.enabled && Config::AnyPanelEnabled(g_cfg);
			g_params.dryRun = !g_params.enabled;
			g_params.inRect = MouseInRect(g_cfg.panels[Config::Provision].rect);

			// v8：恢复 v6 实机验证过的连续平滑滚动。v7 的「整行量化 +
			// 按 param_3 跳格裁剪」已被实机证伪（整屏物品与左边饰品栏一起
			// 消失），整段删除，不再有任何代码路径跳过格子绘制。
			Wheel::RecomputeMax(g_state, g_params);
			// v11.2：非目标界面丢弃滚轮并立刻归零状态机，杜绝残留偏移被写进
			// 布局数组（「打开补给前滚轮 → 打开后已翻页」的根因）。
			// 工会活跃时补给/背包仍不消费，避免双驱动。
			if (g_cfg.gateWheelByPanel && !panelRecent) {
				g_state.target = 0.f;
				g_state.current = 0.f;
			}
			const std::int64_t routedWheel =
				g_guildActive ? 0 : (panelRecent ? wheel : 0);
			Wheel::Tick(g_state, g_params, routedWheel, panelRecent, g_params.inRect);
			g_firstRow.store(g_state.firstRow, std::memory_order_release);

			// v10 裁剪窗口：first = ceil(current / rowH)。
			// 平移后格子 Y = base - current + row*rowH；row >= ceil(current/rowH)
			// 时 Y >= base（不会画到网格上方），窗口下界同理。
			// 用 ceil 而不是整行量化 => current 仍连续平滑，不会「直接跳一行」。
			// 静止 / 关闭裁剪 / DRY-RUN 时置 -1，CellGate 一律放行（零干预）。
			g_visibleRows.store(0, std::memory_order_relaxed); // 0 = 只裁上界
			// 三套布局各自算 first：它们的 [4] 行高可能不同，用活跃布局的
			// 单一值会让另一套布局的窗口错位（p18 应修项）。
			const std::uint32_t layoutRvas[4]{ 0, kLayoutStore, kLayoutParty, kLayoutRaid };
			g_clipFirst[0].store(-1, std::memory_order_release);
			for (std::uint32_t i = 1; i <= 3; ++i) {
				float rh = *reinterpret_cast<const float*>(
					reinterpret_cast<const std::uint8_t*>(g_base + layoutRvas[i]) + kOffRowH);
				if (!(rh > 0.f && std::isfinite(rh))) {
					rh = rowH; // 布局未加载：回退到活跃布局的行高
				}
				if (!g_cfg.clipOutOfWindow || g_params.dryRun ||
					!(g_state.current > 0.5f) || !(rh > 0.f)) {
					g_clipFirst[i].store(-1, std::memory_order_release);
				} else {
					g_clipFirst[i].store(
						static_cast<std::int32_t>(std::ceil(g_state.current / rh)),
						std::memory_order_release);
				}
			}

			// v11.2：门控开启且当前没有目标面板时**完全不碰布局数组**，
			// 让游戏自己管理 start_y；否则 DLL 会把非目标界面滚出的偏移残留下来。
			if (!g_params.dryRun && (!g_cfg.gateWheelByPanel || panelRecent)) {
				// 只写三套「布局数组 [2] 起始 Y」：出征补给 store/party 与
				// 地牢背包，这三者是同构的、且补给已实机验证生效。
			// v8 起不写工会 0x2B8EBAC：它是所有英雄共享的常量，写它会让
			// 不同技能数的英雄网格与名字横幅错位（v6 实机证伪）。
			// p22/p23 静态调研 + 独立对抗审核已判定工会滚轮 NO-GO：
			// 无技能区专属父容器、无原生滚动态、SackScroll 亦不支持工会；
			// guild 开关仅为兼容旧配置保留，恒不生效。详见 docs/adr/0001。
				WriteOffset(kLayoutStore, kOffStartY, g_trackStore, true);
				WriteOffset(kLayoutParty, kOffStartY, g_trackParty, true);
				WriteOffset(kLayoutRaid,  kOffStartY, g_trackRaid,  true);
			}

			// 无条件低频诊断（每秒一条，不依赖 debug 开关），用于定位运行期断点。
			const std::uint64_t now = ::GetTickCount64();
			if (now - g_lastDiagMs >= 1000) {
				g_lastDiagMs = now;
				const float yStore = *reinterpret_cast<const float*>(g_base + kLayoutStore + kOffStartY);
				const float yRaid  = *reinterpret_cast<const float*>(g_base + kLayoutRaid + kOffStartY);
				const float yGuild = *reinterpret_cast<const float*>(g_base + kGuildStartY);
				INFO("[DIAG] ticks={} wheelSum={} qWrite={} dropped={} cellHits={} cells={} peak={} "
				     "cols={} rowH={} rows={} autoMax={} max={} storeY={} raidY={} guildY={} base={} "
				     "target={} cur={} first={} lastIdx={} inRect={} enabled={} "
				     "active={} clip={} p5src={} p5fail={} skipped={} "
				     "gActive={} gN={} gCur={} gMax={} gHit={} gClip={} gBeat={} "
				     "gACalls={} gBCalls={} gSeenB={} gVtA={:X} gVtB={:X} pRecent={}",
					g_tickCount.load(std::memory_order_relaxed),
					g_wheelSum.load(std::memory_order_relaxed),
					Wheel::g_queue.write.load(std::memory_order_relaxed),
					Wheel::Dropped(),
					g_cellHits.load(std::memory_order_relaxed),
					g_state.cellCount, g_peakCells, g_params.columns, rowH, g_totalRows,
					autoMax, effMax, yStore, yRaid, yGuild, g_trackStore.base,
					g_state.target, g_state.current,
					g_state.firstRow, g_lastIdx.load(std::memory_order_relaxed),
					g_params.inRect, g_params.enabled,
					active, g_clipFirst[active > 3 ? 0 : active].load(std::memory_order_relaxed),
					g_p5Src.load(std::memory_order_relaxed),
					g_p5Fail.load(std::memory_order_relaxed),
					g_skipCount.load(std::memory_order_relaxed),
					g_guildActive, g_guild.count.load(std::memory_order_relaxed),
					g_guildState.current, g_guildState.max,
					g_guild.hits.load(std::memory_order_relaxed),
					g_guild.clipped.load(std::memory_order_relaxed),
					g_guild.lastBeat.load(std::memory_order_relaxed),
					g_guild.callsA.load(std::memory_order_relaxed),
					g_guild.callsB.load(std::memory_order_relaxed),
					g_guild.seenB.load(std::memory_order_relaxed),
					g_guild.lastVtA.load(std::memory_order_relaxed),
					g_guild.lastVtB.load(std::memory_order_relaxed),
					panelRecent);
			}
		}

		// ---------------- 每格统计 cave 的 C++ 门控（由 CellCaveAsm 的 stub call）----------------
		// v8：恒定返回 false = 放行（stub 重放 stolen 5B 并跳回 0x7EBF75）。
		// 这里只累计「本帧绘制了多少格」，用于按物品数动态扩大下拉范围。
		// 绝不返回 true（那会让 stub 跳过整个格子函数，造成实机物品/饰品消失）。
		bool CellGate(std::uintptr_t a_retAddr, std::uint32_t a_param5) noexcept
		{
			(void)a_retAddr;
			// 面板判据 = 网格渲染器入口 cave 用 r8 布局地址写的标志（零误伤）。
			// 返回地址白名单是函数内部地址、所有调用者共享，区分不了面板，
			// 正是 v7 连饰品栏一起裁空的根因 → 弃用。
			if (!g_cfg.enabled || !Config::AnyPanelEnabled(g_cfg)) {
				return false;
			}
			const std::uint32_t active = g_activeLayout.load(std::memory_order_acquire);
			if (active == 0 || active > 3) {
				return false; // 非目标面板（含饰品库）：原样放行，绝不裁剪
			}
			// param_5 来自确证槽位 [R0+0x28]（7EC1EF mov eax,[rbp+0xc70]），
			// 不再在多个候选里猜。位域特征只做 sanity check：不匹配就放行，
			// 零破坏降级，并把失败计数写进 [DIAG]。
			if (!Wheel::LooksLikeParam5(a_param5)) {
				g_p5Fail.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const std::uint32_t row = Wheel::DecodeRow(a_param5);
			g_p5Src.store(4, std::memory_order_relaxed); // 4 = 固定确证槽位
			g_lastIdx.store(row, std::memory_order_relaxed);
			UpdateMax(g_maxRow, row); // 真实总行数 = maxRow+1（按数量扩大下拉范围）
			g_cellHits.fetch_add(1, std::memory_order_relaxed);
			g_cellIndex.fetch_add(1, std::memory_order_relaxed);

			if (!g_cfg.clipOutOfWindow) {
				return false;
			}
			const std::int32_t first = g_clipFirst[active].load(std::memory_order_acquire);
			if (first < 0) {
				return false; // 静止（current≈0）：零干预，画面与原版完全一致
			}
			// Y = base - current + row*rowH；first = ceil(current/rowH) 保证
			// row>=first 时 Y>=base（不上溢），且 current 仍连续平滑。
			// visible=0 => 只裁上界，绝不裁掉仍可视的格子（p18 应修项）。
			const std::uint32_t vis = g_visibleRows.load(std::memory_order_relaxed);
			const bool draw = Wheel::ShouldDrawRow(row, first, vis);
			if (!draw) {
				g_skipCount.fetch_add(1, std::memory_order_relaxed);
			}
			if (g_cfg.debugSampling) {
				DEBUG("[SAMPLE] active={} row={} first={} vis={} skip={}",
					active, row, first, vis, !draw);
			}
			return !draw; // true = 已滚出上沿：stub 直接 ret，不绘制也不注册热点
		}

		std::filesystem::path GetExePath()
		{
			std::wstring buf(MAX_PATH, L'\0');
			for (;;) {
				const auto len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
				if (len < buf.size()) {
					buf.resize(len);
					return buf;
				}
				buf.resize(buf.size() * 2);
			}
		}

		void ApplyNativeStep()
		{
			// 默认关；只改 .rdata 的 30.0f，影响所有已原生支持滚轮的面板，不满足本需求。
			if (!g_cfg.patchNativeStep || !std::isfinite(g_cfg.nativeStep)) {
				return;
			}
			auto* addr = reinterpret_cast<std::uint32_t*>(g_base + Game::kConst30);
			DWORD old = 0;
			if (!::VirtualProtect(addr, sizeof(*addr), PAGE_READWRITE, &old)) {
				WARN("patch_native_step: VirtualProtect failed");
				return;
			}
			std::uint32_t bits;
			const float v = g_cfg.nativeStep;
			std::memcpy(&bits, &v, sizeof(bits));
			*addr = bits;
			::VirtualProtect(addr, sizeof(*addr), old, &old);
			WARN("patch_native_step=1: overwrote 0xEE63DC with {} (only speeds up natively-scrollable panels)", v);
		}

		// 预分配 stub 并校验 E9 rel32 的 ±2GB 范围（p4-recheck A1）。
		// DKUtil 的 Trampoline 分配地址上界可能超出 int32，直接 static_cast 会静默
		// 截断造成跳到错误地址。预检必须在写任何 patch 之前完成，保证全有或全无。
		void* PrepareCave(std::uint32_t a_rva, std::size_t a_size)
		{
			auto* mem = dku::Hook::Trampoline::Allocate(a_size);
			if (!mem) {
				WARN("cave stub allocation failed @+{:X}", a_rva);
				return nullptr;
			}
			const auto src = g_base + a_rva;
			const auto delta = static_cast<std::int64_t>(
				reinterpret_cast<std::intptr_t>(mem)) -
				static_cast<std::int64_t>(static_cast<std::intptr_t>(src + 5));
			if (delta < std::numeric_limits<std::int32_t>::min() ||
				delta > std::numeric_limits<std::int32_t>::max()) {
				WARN("cave stub @+{:X} out of E9 rel32 range ({}); no hooks installed", a_rva, delta);
				return nullptr;
			}
			return mem;
		}

		// 把 a_rva 处 a_stolen 字节换成 E9 rel32 -> 预分配好的 Xbyak stub。
		// cave 是 jmp（不是 call），跳进 stub 时 rsp 仍等于函数入口 rsp。
		void CommitCave(std::uint32_t a_rva, std::uint32_t a_stolen,
			const void* a_code, std::size_t a_size, void* a_mem)
		{
			auto* mem = static_cast<std::uint8_t*>(a_mem);
			std::memcpy(mem, a_code, a_size);

			std::uint8_t patch[16]{};
			patch[0] = 0xE9;
			const auto src = g_base + a_rva;
			const auto delta = static_cast<std::int64_t>(
				reinterpret_cast<std::intptr_t>(mem)) -
				static_cast<std::int64_t>(static_cast<std::intptr_t>(src + 5));
			const std::int32_t rel = static_cast<std::int32_t>(delta); // 已在 PrepareCave 校验
			std::memcpy(patch + 1, &rel, sizeof(rel));
			for (std::size_t i = 5; i < a_stolen; ++i) {
				patch[i] = 0x90;
			}
			dku::Hook::WriteData(src, patch, a_stolen);
			::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<const void*>(src), a_stolen);
		}
	}

	void Install(const Config::Data& a_cfg)
	{
		g_cfg = a_cfg;

		if (!g_cfg.enabled) {
			INFO("disabled by config (enabled=0); no hooks installed");
			return;
		}

		g_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));

		// ---- 全有或全无校验（基于磁盘映射映像，S3：不用运行时内存值）----
		std::vector<std::uint8_t> disk;
		if (!Game::MapDiskImage(GetExePath(), disk)) {
			WARN("Incompatible game version, no hooks installed (PE disk image parse failed)");
			return;
		}
		Game::ImageView image{};
		if (!Game::InitImageView(image, disk.data())) {
			WARN("Incompatible game version, no hooks installed (PE image parse failed)");
			return;
		}
		const bool sha1Ok = Game::CheckExeSha1(GetExePath());
		const auto report = Game::Verify(image, sha1Ok);
		INFO("ScrollWheel game verification:\n{}", report.Format());
		if (!report.AllOk()) {
			WARN("Incompatible game version, no hooks installed (one or more checks failed; see list above)");
			return;
		}

		// v6 新布局锚点同样做全有或全无校验：字节 + rip 相对目标不符就一个 hook 都不装。
		if (!VerifyExtra(g_base)) {
			WARN("Incompatible game version, no hooks installed (extra layout anchors mismatch)");
			return;
		}
		INFO("extra anchors OK: raid=+{:X} guildStartY=+{:X} guildStepY=+{:X}",
			kLayoutRaid, kGuildStartY, kGuildStepY);

		// v10：0x7EADE0 的长特征码必须在 .text 恰好命中一次且落在常量 RVA。
		// 裸 5B stolen 有 326 处命中，不能用来定位（p16 §5）。
		const auto grid2Hits = Game::FindAll(image, Game::kSigGrid2);
		if (grid2Hits.size() != 1 || grid2Hits.front() != Game::kGrid2Rva) {
			WARN("Incompatible game version, no hooks installed "
			     "(grid2 signature hits={}, expected 1 @+{:X})", grid2Hits.size(), Game::kGrid2Rva);
			return;
		}
		INFO("grid2 unique signature verified @+{:X}", grid2Hits.front());

		std::uint32_t cave = 0, grid = 0, cell = 0;
		if (!Game::Locate(image, report, cave, grid, cell)) {
			WARN("Incompatible game version, no hooks installed (unique signature locate failed)");
			return;
		}
		// grid 仍参与 28 项校验/13 条特征码唯一性复核，但不再 hook 0x7E9F80（零入口 detour）。
		INFO("verified hook RVAs: cave=+{:X} grid=+{:X} (not hooked) cell=+{:X}", cave, grid, cell);

		// ---- 安装 ----
		dku::Hook::Trampoline::AllocTrampoline(1 << 12);

		// (1) 输入 cave @0x371F0A（B1 偏移 0x64 / B2 wrap 契约）
		ScrollAsm::InputParams ip{};
		ip.queueAddr = reinterpret_cast<std::uint64_t>(&Wheel::g_queue);
		ip.resumeAddr = g_base + Game::kCaveResume;
		ip.offWrite = Wheel::kOffWrite;
		ip.offRead = Wheel::kOffRead;
		ip.offRing = Wheel::kOffRing;
		ip.offDropped = Wheel::kOffDropped;
		ip.mask = Wheel::kMask;
		ip.capacity = Wheel::kCapacity;
		ScrollAsm::InputCaveAsm inputAsm(ip);

		ScrollAsm::CellParams cp{};
		cp.gateFn = reinterpret_cast<std::uint64_t>(&CellGate);
		cp.resumeAddr = g_base + Game::kCellResume;
		ScrollAsm::CellCaveAsm cellAsm(cp); // 构造本身无副作用，未提交前不修改任何游戏内存

		// v10：两个网格渲染器入口各一个「只读」面板 gate cave。
		// r8 比对的是运行时的绝对地址（ASLR 安全），白名单只含 3 套目标布局；
		// 其余 10 个调用点（含城镇饰品库 0x6A6739）一律写 0，逐格 cave 零干预。
		ScrollAsm::GridEntryParams gp{};
		gp.flagAddr = reinterpret_cast<std::uint64_t>(&g_activeLayout);
		gp.resumeAddr = g_base + Game::kGridResume;
		gp.target1 = g_base + kLayoutStore;
		gp.target2 = g_base + kLayoutParty;
		gp.target3 = g_base + kLayoutRaid;
		gp.stolen = Game::kGridBytes.data();
		gp.stolenLen = Game::kGridStolen;
		ScrollAsm::GridEntryAsm gridAsm(gp);

		ScrollAsm::GridEntryParams gp2 = gp;
		gp2.resumeAddr = g_base + Game::kGrid2Resume;
		gp2.stolen = Game::kGrid2Bytes.data();
		gp2.stolenLen = Game::kGrid2Stolen;
		ScrollAsm::GridEntryAsm grid2Asm(gp2);

		// 全有或全无：先给所有要装的 cave 预分配 stub 并校验 rel32 范围，
		// 全部通过后才写 patch，避免「装了一半」的状态（p4-recheck A1）。
		const bool needCell = Config::AnyPanelEnabled(g_cfg);
		void* inputMem = PrepareCave(cave, inputAsm.getSize());
		void* gridMem = needCell ? PrepareCave(Game::kGridRva, gridAsm.getSize()) : nullptr;
		void* grid2Mem = needCell ? PrepareCave(Game::kGrid2Rva, grid2Asm.getSize()) : nullptr;
		void* cellMem = needCell ? PrepareCave(cell, cellAsm.getSize()) : nullptr;
		if (!inputMem || (needCell && (!gridMem || !grid2Mem || !cellMem))) {
			WARN("cave preparation failed, no hooks installed");
			return;
		}

		// (1) 输入 cave @0x371F0A（B1 偏移 0x64 / B2 wrap 契约）
		CommitCave(cave, Game::kCaveStolen, inputAsm.getCode(), inputAsm.getSize(), inputMem);
		INFO("input cave installed @+{:X}: reads [rsp+0x64]=wheel.y, wrap producer", cave);

		// (2) v10 面板 gate + 逐格裁剪：三个 cave 一起提交
		if (needCell) {
			CommitCave(Game::kGridRva, Game::kGridStolen,
				gridAsm.getCode(), gridAsm.getSize(), gridMem);
			CommitCave(Game::kGrid2Rva, Game::kGrid2Stolen,
				grid2Asm.getCode(), grid2Asm.getSize(), grid2Mem);
			CommitCave(cell, Game::kCellStolen, cellAsm.getCode(), cellAsm.getSize(), cellMem);
			INFO("panel-gate caves installed @+{:X} / +{:X} (r8 layout whitelist, read-only); "
			     "per-cell clipping cave @+{:X} uses param_5 row",
				Game::kGridRva, Game::kGrid2Rva, cell);

			// v5：独立的每帧驱动。不依赖 0x7EBF70 是否被调用、也不依赖任何游戏
			// 函数时序，因此队列一定会被消费、布局一定会被写。
			std::thread([] {
				::Sleep(300); // 等 DllMain / loader lock 释放，避免在加载器锁内跑逻辑
				while (!g_tickStop.load(std::memory_order_relaxed)) {
					try {
						FrameTick();
					} catch (...) {
						// 不让异常逃出线程导致进程终止
					}
					::Sleep(16);
				}
			}).detach();
			INFO("frame tick thread started (16ms); [DIAG] line written every 1s");
		} else {
			INFO("all panels disabled (default safe mode): clipping cave NOT installed, "
			     "layout NOT written, rendering NOT touched; state machine in DRY-RUN standby");
		}

		// ---- v11 工会（实验性）：校验 / 锚点 / PrepareCave 全部包在开关内 ----
		// 校验失败或预分配失败只跳过工会 hook，绝不 return——否则会连带禁用
		// 已实机验收的 v10 四个 cave（p26 对抗审核 S3 的硬要求）。
		if (g_cfg.guildScroll) {
			if (!VerifyGuild(g_base, image)) {
				WARN("v11 guild scroll DISABLED by verification (v10 hooks remain installed)");
			} else {
				ScrollAsm::GuildParams rp{};
				// ★ v11.2 ASLR 修复：cave 判据必须是「运行时」vtable 地址。
				// v11/v11.1 硬编码 0x140EBB8D8（PE ImageBase），实机基址却是
				// 0x7FF7D7DD0000 ⇒ gSeenB 恒 0、工会完全失效。由 C++ 用真实
				// 模块基址算出后写进表，cave 只从表里读，不内嵌任何绝对 VA。
				g_guild.expectedVptr = g_base + ScrollAsm::kGuildVtableRva;
				INFO("v11.2 guild runtime vtable = 0x{:X} (base 0x{:X} + RVA 0x{:X})",
					g_guild.expectedVptr, g_base, ScrollAsm::kGuildVtableRva);
				rp.tableAddr = reinterpret_cast<std::uint64_t>(&g_guild);
				rp.resumeAddr = g_base + ScrollAsm::kGuildRefreshResume;
				ScrollAsm::GuildParams sp{ rp.tableAddr, g_base + ScrollAsm::kGuildShiftResume };
				ScrollAsm::GuildRefreshAsm refreshAsm(rp);
				ScrollAsm::GuildShiftAsm shiftAsm(sp);
				void* rm = PrepareCave(ScrollAsm::kGuildRefreshRva, refreshAsm.getSize());
				void* sm = PrepareCave(ScrollAsm::kGuildShiftRva, shiftAsm.getSize());
				if (!rm || !sm) {
					WARN("v11 guild cave preparation failed; guild hooks skipped (v10 untouched)");
				} else {
					CommitCave(ScrollAsm::kGuildRefreshRva, sizeof(ScrollAsm::kGuildRefreshBytes),
						refreshAsm.getCode(), refreshAsm.getSize(), rm);
					CommitCave(ScrollAsm::kGuildShiftRva, sizeof(ScrollAsm::kGuildShiftBytes),
						shiftAsm.getCode(), shiftAsm.getSize(), sm);
					g_guildEnabled = true;
					INFO("v11 guild caves installed @+{:X} (per-frame slot table) / +{:X} "
					     "(stack-frame Y shift); experimental, guild_scroll=0 disables both",
						ScrollAsm::kGuildRefreshRva, ScrollAsm::kGuildShiftRva);
				}
			}
		} else {
			INFO("v11 guild scroll disabled (guild_scroll=0): no guild hooks installed");
		}

		ApplyNativeStep();

		INFO("panels: provision={} guild={} raid_inventory={} (0 = DRY-RUN, no injection)",
			g_cfg.panels[Config::Provision].enabled, g_cfg.panels[Config::Guild].enabled,
			g_cfg.panels[Config::RaidInventory].enabled);
		if (g_cfg.panels[Config::Guild].enabled) {
			WARN("legacy guild=1 key has no effect (ADR 0001); v11 uses the independent "
			     "guild_scroll switch, which never writes 0x2B8EBAC");
		}
		INFO("v9: no custom scrollbar (game-native bars are left untouched)");
	}
}
