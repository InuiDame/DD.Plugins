#include "Wheel.h"

#include <algorithm>
#include <cmath>

namespace Wheel
{
	SpscQueue g_queue;

	bool Push(std::int32_t a_wheelY) noexcept
	{
		const auto w = g_queue.write.load(std::memory_order_relaxed);
		const auto r = g_queue.read.load(std::memory_order_acquire);
		const auto nw = (w + 1) & kMask;
		if (nw == r) { // 队列满：直接丢弃，不覆盖未读项、不阻塞
			g_queue.dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		g_queue.ring[w] = a_wheelY;
		g_queue.write.store(nw, std::memory_order_release);
		return true;
	}

	std::int64_t Drain() noexcept
	{
		const auto w = g_queue.write.load(std::memory_order_acquire);
		auto r = g_queue.read.load(std::memory_order_relaxed);
		// B2 双保险：生产者契约统一为 wrap(0..255)，但消费者必须对「单调 write
		// (>255，旧 cave 行为)」和「wrap 跨越 255->0」都安全。
		// 若照字面用 while (r != w) { acc += ring[r & kMask]; ++r; }，当 w 是 wrap
		// 值而 r 单调增时 r 永远追不上 w（需 2^32 次溢出）→ 死循环。
		// 因此用有界步数 (w - r) & kMask（0..255），既保留单调 r 与 r&mask 索引，
		// 又保证循环次数有界、ring 索引永不越界、wrap 跨越正确。
		const std::uint32_t steps = (w - r) & kMask;
		std::int64_t acc = 0;
		for (std::uint32_t i = 0; i < steps; ++i) {
			// 索引只用 r + i；不要再 ++r，否则每轮跳 2 个槽（P4 修复复核发现）。
			acc += g_queue.ring[(r + i) & kMask];
		}
		g_queue.read.store(w, std::memory_order_release);
		return acc;
	}

	std::uint32_t Dropped() noexcept
	{
		return g_queue.dropped.load(std::memory_order_relaxed);
	}

	void Reset() noexcept
	{
		g_queue.write.store(0, std::memory_order_relaxed);
		g_queue.read.store(0, std::memory_order_relaxed);
		g_queue.dropped.store(0, std::memory_order_relaxed);
		for (auto& v : g_queue.ring) {
			v = 0;
		}
	}

	namespace
	{
		float Finite(float a_v, float a_fallback) noexcept
		{
			return std::isfinite(a_v) ? a_v : a_fallback;
		}
	}

	void Tick(State& s, const Params& p, std::int64_t wheel,
		bool layoutHit, bool mouseInRect) noexcept
	{
		++s.frame;
		if (layoutHit) {
			s.lastActiveFrame = s.frame;
		}
		const bool fresh = p.frameTtl == 0 || (s.frame - s.lastActiveFrame) <= p.frameTtl;
		const bool active = layoutHit && fresh && p.enabled && mouseInRect;

		// 极端滚轮(Sint32 极值)经 int64 累加后再转 float，先做 isfinite 防护。
		const float wy = Finite(static_cast<float>(wheel), 0.f) * (p.invert ? -1.f : 1.f);

		if (active && std::isfinite(wy) && wy != 0.f) {
			float t = s.target - wy * p.step; // p0 A4: offset -= wheel_y * step
			if (!std::isfinite(t)) {
				t = s.target;
			}
			s.max = Finite(s.max, 0.f);
			s.target = std::clamp(Finite(t, s.target), 0.f, std::max(0.f, s.max));
		}

		// 每帧 clamp + NaN/Inf 防护（无论是否有滚轮输入）。
		s.max = Finite(s.max, 0.f);
		s.target = std::clamp(Finite(s.target, 0.f), 0.f, std::max(0.f, s.max));
		s.current = Finite(s.current, s.target);

		// current 平滑逼近 target：alpha = clamp(smooth/60, 0, 1)，固定 60Hz 步长。
		const float alpha = std::clamp(p.smooth * (1.f / 60.f), 0.f, 1.f);
		const float d = (s.target - s.current) * alpha;
		const float next = Finite(s.current + d, s.current);
		s.current = std::clamp(next, 0.f, std::max(0.f, s.max));

		// firstRow = current / rowHeight（NaN/Inf/除零防护）。
		if (p.rowHeight > 0.f && std::isfinite(p.rowHeight)) {
			const float fr = s.current / p.rowHeight;
			s.firstRow = std::isfinite(fr) && fr > 0.f ? static_cast<std::uint32_t>(fr) : 0u;
		} else {
			s.firstRow = 0u;
		}
	}

	void RecomputeMax(State& s, const Params& p) noexcept
	{
		const int cols = p.columns > 0 ? p.columns : 1;
		const std::uint32_t totalRows = (s.cellCount + cols - 1) / cols;
		const int over = static_cast<int>(totalRows) - p.visibleRows;
		const float m = static_cast<float>(over > 0 ? over : 0) *
		                (p.rowHeight > 0.f && std::isfinite(p.rowHeight) ? p.rowHeight : 0.f);
		// v5：max 不再依赖「每格 cave 是否被调用」。若该 cave 没命中，cellCount=0
		// 会让 max=0，target 被 clamp 死，滚轮静默失效。cfg 的 scroll_range 优先。
		s.max = p.fixedMax > 0.f && std::isfinite(p.fixedMax) ? p.fixedMax : Finite(m, 0.f);
	}

	bool ShouldDrawCell(State& s, const Params& p) noexcept
	{
		if (!s.inGrid) {
			return true; // 不在注入网格：全部照常绘制
		}
		const std::uint32_t idx = s.cellIndex++;
		const int cols = p.columns > 0 ? p.columns : 1;
		const std::uint32_t row = idx / static_cast<std::uint32_t>(cols);
		if (row < s.firstRow || row >= s.firstRow + static_cast<std::uint32_t>(p.visibleRows)) {
			return false; // 窗口外：不绘制、不注册热点
		}
		return true;
	}

	float InjectOffset(const State& s) noexcept
	{
		return s.current;
	}

	bool LooksLikeParam5(std::uint32_t a_v) noexcept
	{
		// V 的 bit29 来自 (col&0x1f)|0x20 的 0x20 位，左移 1 后落到 bit30。
		return (a_v & 0x40000000u) != 0 && (a_v & 1u) == 0;
	}

	std::uint32_t DecodeRow(std::uint32_t a_param5) noexcept
	{
		return (a_param5 >> 1) & 0xffffffu;
	}

	bool ShouldDrawRow(std::uint32_t a_row, std::int32_t a_first,
		std::uint32_t a_visible) noexcept
	{
		if (a_first < 0) {
			return true; // 静止 / 裁剪关闭：零干预
		}
		if (a_row < static_cast<std::uint32_t>(a_first)) {
			return false; // 已滚出网格上沿：必裁（用户要求的不遮挡）
		}
		if (a_visible > 0 &&
			a_row >= static_cast<std::uint32_t>(a_first) + a_visible) {
			return false; // 可选：下界裁剪，默认关闭
		}
		return true;
	}
}
