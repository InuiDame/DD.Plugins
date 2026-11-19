#pragma once

#include "SpineLayout.h"

namespace SpineFix::timeline
{
	// 游戏侧依赖：按名字取附件（853330 spSkeleton_getAttachmentForSlotIndex）、
	// 挂附件（8559A0 spSlot_setAttachment）。离线测试里换成假的实现。
	struct RepairContext
	{
		void* (*resolve)(void* a_skeleton, std::int32_t a_slotIndex, const char* aName) = nullptr;
		void (*attach)(void* a_slot, void* a_attachment) = nullptr;

		[[nodiscard]] bool Usable() const noexcept { return resolve && attach; }
	};

	struct RepairStats
	{
		std::int32_t attachments{ 0 };
		std::int32_t drawOrders{ 0 };
	};

	// 最后一个 frames[i] <= a_time 的下标；时间早于首帧时返回 -1（和 spine 2.1 一样保持原状）
	std::int32_t FindFrame(const float* a_frames, std::int32_t a_count, float a_time) noexcept;

	// 把一条 attachment 时间线按时间补到 skeleton 上；返回 true 表示改动了状态
	bool RepairAttachment(spine::Skeleton& a_skeleton, const spine::AttachmentTimeline& a_timeline, float a_time,
		const RepairContext& a_context) noexcept;

	// 把一条 drawOrder 时间线按时间补到 skeleton 上；返回 true 表示改动了状态
	bool RepairDrawOrder(spine::Skeleton& a_skeleton, const spine::DrawOrderTimeline& a_timeline, float a_time) noexcept;

	// 遍历一个动画的全部时间线，只处理 attachment / drawOrder
	RepairStats RepairAnimation(spine::Skeleton& a_skeleton, const spine::Animation& a_animation, float a_time,
		const RepairContext& a_context) noexcept;

	// 对一个 sprite 当前条目的全部轨道做修补（轨道从 0 到 tracksCount-1，高层轨道后应用）
	RepairStats RepairSprite(const spine::Sprite& a_sprite, const RepairContext& a_context) noexcept;
}
