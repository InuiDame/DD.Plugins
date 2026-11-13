#pragma once

// 纯逻辑模块：不依赖 DKUtil / 游戏内存，可被离线单测直接编译运行。
// cave 的 Xbyak prolog 按固定偏移直接读写 SpscQueue，布局与对齐不得改动。

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Wheel
{
	// 单生产者(事件泵线程)/单消费者(每帧状态机)环形队列，进程启动时静态分配。
	struct alignas(64) SpscQueue
	{
		std::atomic<std::uint32_t> write{ 0 };            // 下一个写槽索引 0..255
		alignas(64) std::atomic<std::uint32_t> read{ 0 }; // 下一个读槽索引 0..255
		std::int32_t ring[256]{};                         // 原始 SDL_Event.wheel.y (Sint32)
		std::atomic<std::uint32_t> dropped{ 0 };          // 队列满丢弃计数
	};

	constexpr std::uint32_t kMask = 255;      // 256 槽，2 的幂
	constexpr std::uint32_t kCapacity = 256; // 索引制：满时最多存 255 个

	// 供 Xbyak prolog 使用的字段偏移（编译期固定）。
	constexpr std::size_t kOffWrite = offsetof(SpscQueue, write);
	constexpr std::size_t kOffRead = offsetof(SpscQueue, read);
	constexpr std::size_t kOffRing = offsetof(SpscQueue, ring);
	constexpr std::size_t kOffDropped = offsetof(SpscQueue, dropped);

	extern SpscQueue g_queue;

	// 生产者侧 C++ 实现（cave 汇编生产者与之同契约：write=(write+1)&mask）。
	// 队列满返回 false 并累计 dropped。
	bool Push(std::int32_t a_wheelY) noexcept;
	// 消费者侧：一次性 drain 全部，返回 Sint32 的 64 位累加（防溢出）。幂等：空队列返回 0。
	// 有界步数 (w-r)&mask，对 wrap 与单调 write 两种生产者契约都不死循环/不越界（B2）。
	std::int64_t Drain() noexcept;
	std::uint32_t Dropped() noexcept;
	void Reset() noexcept; // 仅离线单测使用

	// ---------------- 每帧状态机（纯逻辑，离线可测）----------------
	struct Params
	{
		float step{ 30.f };
		float smooth{ 8.f };
		int   visibleRows{ 5 };
		float rowHeight{ 40.f };
		bool  invert{ false };
		int   columns{ 7 };
		bool  enabled{ true };
		bool  dryRun{ false };  // 关闭的面板：走完整状态机但不注入
		bool  inRect{ true };   // 鼠标区域判定结果，由 Hooks 侧注入
		float fixedMax{ 0.f };  // >0 时直接用它作 max，不依赖每格 cave 统计（v5）
		std::uint32_t frameTtl{ 1 };
	};

	struct State
	{
		float target{ 0.f };
		float current{ 0.f };
		float max{ 0.f };
		std::uint32_t firstRow{ 0 };
		std::uint32_t frame{ 0 };
		std::uint32_t lastActiveFrame{ 0 };
		std::uint32_t cellCount{ 0 };  // 上一帧统计到的格子数
		std::uint32_t cellIndex{ 0 }; // 本帧逐格计数
		bool inGrid{ false };
	};

	// 消费队列 -> 激活判定(layout 命中 + 帧 TTL + 鼠标区域 + cfg 开关)
	//   -> target -= wheel*step -> clamp[0,max]/NaN/Inf 防护 -> current 平滑逼近 -> firstRow
	void Tick(State& a_state, const Params& a_params, std::int64_t a_wheel,
		bool a_layoutHit, bool a_mouseInRect) noexcept;

	// max 自适应：totalRows = ceil(cellCount/columns)，max = max(0,(totalRows-visibleRows)*rowHeight)
	void RecomputeMax(State& a_state, const Params& a_params) noexcept;

	// 虚拟裁剪：idx = cellIndex++，row = idx/columns；窗口外返回 false(不绘制、不注册热点)
	bool ShouldDrawCell(State& a_state, const Params& a_params) noexcept;

	float InjectOffset(const State& a_state) noexcept; // = current

	// ---------------- v10：param_5 位域与裁剪窗口（纯逻辑，生产/单测共用）----------------
	// 调用点编码：V = (((col&0x1f)|0x20)<<0x18) | (row&0xffffff)，param_5 = V*2
	// => bit30 恒 1、bit0 恒 0；row = (param_5>>1)&0xffffff。
	bool LooksLikeParam5(std::uint32_t a_v) noexcept;
	std::uint32_t DecodeRow(std::uint32_t a_param5) noexcept;
	// a_first < 0 表示静止（零干预，恒 true）。
	// a_visible == 0：**只裁上界**（跳过 row < first = 已滚出网格上方的格子），
	//   不裁下界。静态无法确证真实可见行数，硬编码 visible_rows 会把仍在
	//   可视区的格子一起裁掉（p18 应修项）→ 生产默认传 0。
	// a_visible > 0：额外跳过 row >= first+visible 的下方格子（需实机确认后开）。
	bool ShouldDrawRow(std::uint32_t a_row, std::int32_t a_first,
		std::uint32_t a_visible) noexcept;
}
