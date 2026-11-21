#include <cmath>

#include "Timeline.h"

namespace SpineFix::timeline
{
	// 按时间找帧：非线性时间线只有「最后一个不晚于 t 的帧」这一条语义，和 spine 2.1 的
	// Animation.c 一致（先判早于首帧，再二分，最后夹到末帧）。
	std::int32_t FindFrame(const float* a_frames, std::int32_t a_count, float a_time) noexcept
	{
		if (!a_frames || a_count <= 0) {
			return -1;
		}
		if (a_time < a_frames[0]) {
			return -1;
		}
		if (a_time >= a_frames[a_count - 1]) {
			return a_count - 1;
		}

		std::int32_t low = 0;
		std::int32_t high = a_count - 1;
		while (low < high) {
			const std::int32_t mid = (low + high + 1) / 2;
			if (a_frames[mid] <= a_time) {
				low = mid;
			} else {
				high = mid - 1;
			}
		}
		return low;
	}

	bool RepairAttachment(spine::Skeleton& a_skeleton, const spine::AttachmentTimeline& a_timeline, float a_time,
		const RepairContext& a_context) noexcept
	{
		if (!a_context.Usable()) {
			return false;
		}

		const std::int32_t frame = FindFrame(a_timeline.frames, a_timeline.frameCount, a_time);
		if (frame < 0 || !a_timeline.names) {
			return false;
		}
		if (a_timeline.slotIndex < 0 || a_timeline.slotIndex >= a_skeleton.slotsCount) {
			return false;
		}
		if (!a_skeleton.slots) {
			return false;
		}

		auto* slot = a_skeleton.slots[a_timeline.slotIndex];
		if (!slot) {
			return false;
		}

		const char* name = a_timeline.names[frame];
		// 名字为 null 表示这一帧要把切片隐藏掉
		// 名字在皮肤里找不到时也是「什么都不画」，和 spine 一样
		void* wanted = nullptr;
		if (name && name[0] != '\0') {
			wanted = a_context.resolve(&a_skeleton, a_timeline.slotIndex, name);
		}

		if (slot->attachment == wanted) {
			return false;
		}

		a_context.attach(slot, wanted);
		return true;
	}

	bool RepairDrawOrder(spine::Skeleton& a_skeleton, const spine::DrawOrderTimeline& a_timeline,
		float a_time) noexcept
	{
		const std::int32_t frame = FindFrame(a_timeline.frames, a_timeline.frameCount, a_time);
		if (frame < 0 || !a_timeline.orders || !a_skeleton.slots || !a_skeleton.drawOrder) {
			return false;
		}

		const std::int32_t count = a_skeleton.slotsCount;
		if (count <= 0) {
			return false;
		}

		const std::int32_t* order = a_timeline.orders[frame];
		std::int32_t        limit = a_timeline.orderSlots;
		if (limit <= 0 || limit > count) {
			limit = count;
		}

		bool changed = false;
		for (std::int32_t i = 0; i < limit; ++i) {
			const std::int32_t index = order ? order[i] : i;
			if (index < 0 || index >= count) {
				continue;
			}
			auto* wanted = a_skeleton.slots[index];
			if (a_skeleton.drawOrder[i] != wanted) {
				a_skeleton.drawOrder[i] = wanted;
				changed = true;
			}
		}

		// 数据比 skeleton 短时，剩下的位置保持初始顺序，避免残留上一帧的排列
		for (std::int32_t i = limit; i < count; ++i) {
			if (a_skeleton.drawOrder[i] != a_skeleton.slots[i]) {
				a_skeleton.drawOrder[i] = a_skeleton.slots[i];
				changed = true;
			}
		}

		return changed;
	}

	RepairStats RepairAnimation(spine::Skeleton& a_skeleton, const spine::Animation& a_animation, float a_time,
		const RepairContext& a_context) noexcept
	{
		RepairStats stats;
		if (!a_animation.timelines || a_animation.timelinesCount <= 0) {
			return stats;
		}

		for (std::int32_t i = 0; i < a_animation.timelinesCount; ++i) {
			auto* timeline = static_cast<spine::Timeline*>(a_animation.timelines[i]);
			if (!timeline) {
				continue;
			}

			switch (timeline->type) {
			case spine::TimelineTypeAttachment:
				if (RepairAttachment(a_skeleton, *reinterpret_cast<spine::AttachmentTimeline*>(timeline), a_time,
						a_context)) {
					++stats.attachments;
				}
				break;
			case spine::TimelineTypeDrawOrder:
				if (RepairDrawOrder(a_skeleton, *reinterpret_cast<spine::DrawOrderTimeline*>(timeline), a_time)) {
					++stats.drawOrders;
				}
				break;
			default:
				break;
			}
		}

		return stats;
	}

	RepairStats RepairSprite(const spine::Sprite& a_sprite, const RepairContext& a_context) noexcept
	{
		RepairStats stats;
		if (!a_context.Usable()) {
			return stats;
		}

		auto* entry = spine::CurrentEntry(const_cast<spine::Sprite*>(&a_sprite));
		if (!entry || !entry->skeleton || !entry->state) {
			return stats;
		}

		auto& state = *entry->state;
		if (!state.tracks || state.tracksCount <= 0) {
			return stats;
		}

		for (std::int32_t i = 0; i < state.tracksCount; ++i) {
			auto* track = state.tracks[i];
			if (!track || !track->animation) {
				continue;
			}

			// 游戏的 spAnimation_apply（851100）在 loop 时会对 time 取 fmod，
			// 但 track->time 本身一直累加。修补必须和它用同一个回卷后的时间，
			// 否则第二遍开始所有时间线都会被夹到末帧，帧动画看起来只播一次。
			auto& animation = *track->animation;
			float time = track->time;
			if (track->loop && animation.duration > 0.0f) {
				time = std::fmod(time, animation.duration);
			}

			const auto one = RepairAnimation(*entry->skeleton, animation, time, a_context);
			stats.attachments += one.attachments;
			stats.drawOrders += one.drawOrders;
		}

		return stats;
	}
}
