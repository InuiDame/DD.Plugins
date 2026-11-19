#pragma once

#include <cstddef>
#include <cstdint>

// 《暗黑地牢》里 spine-c 2.1 运行时的内存视图。
//
// 偏移来自 knowledge/dd1/spine.md 的「结构偏移」表，并用反汇编核对过：
//   spSlot_create      8558C0  0x40 字节，color 从 slotData+0x18 拷到 slot+0x10
//   spSlot_setAttachment 8559A0  写 +0x20 attachment、+0x38 time、+0x2C 0
//   spAnimation_apply  851100  按 timeline vtable 逐个 apply
//   spine-c 时间线      见 Timeline 注释里的类型号
// 这里只声明插件读写的字段，其余留成保留区，偏移由 static_assert 守住。

namespace SpineFix::spine
{
	// ---------------- 时间线类型号（这个 build 的枚举值，不是上游 2.1 的顺序） ----------------
	// 3 = slot attachment（切片显示/隐藏），4 = slot color，
	// 6 = drawOrder（层级调整）；5/7 是骨骼时间线。
	constexpr std::int32_t TimelineTypeAttachment = 3;
	constexpr std::int32_t TimelineTypeDrawOrder  = 6;

	struct Slot
	{
		void*         data;        // +0x00 spSlotData*
		void*         bone;        // +0x08 spBone*
		float         r;           // +0x10
		float         g;           // +0x14
		float         b;           // +0x18
		float         a;           // +0x1C
		void*         attachment;  // +0x20 spAttachment*，null 表示这个切片不画
		std::uint32_t reserved0;   // +0x28
		std::uint32_t reserved1;   // +0x2C spSlot_setAttachment 清零
		std::uint32_t reserved2;   // +0x30
		std::uint32_t reserved3;   // +0x34
		std::uint32_t attachTime;  // +0x38 spSlot_setAttachment 写 skeleton->time
		std::uint32_t reserved4;   // +0x3C
	};
	static_assert(sizeof(Slot) == 0x40);
	static_assert(offsetof(Slot, attachment) == 0x20);
	static_assert(offsetof(Slot, attachTime) == 0x38);

	// custom 自研 SlotData 的前几个字段：name、boneData、attachmentName、color(+0x18)、
	// blend/加法标志(+0x28)。spSlot_create 从 +0x18 取 color，就是这一版布局。
	struct SlotData
	{
		std::byte     head[0x28];
		std::uint32_t blend;  // +0x28 原版 2.1 是 additive(0/1)，本插件扩成 0..3
	};
	static_assert(offsetof(SlotData, blend) == 0x28);

	struct Skeleton
	{
		void*         data;        // +0x00 spSkeletonData*
		std::int32_t  bonesCount;  // +0x08
		std::int32_t  padding0;    // +0x0C
		void**        bones;       // +0x10
		void*         root;        // +0x18
		std::int32_t  slotsCount;  // +0x20
		std::int32_t  padding1;    // +0x24
		Slot**        slots;       // +0x28
		Slot**        drawOrder;   // +0x30 渲染顺序，是 slots 的一个排列
		std::int32_t  ikCount;     // +0x38
		std::int32_t  padding2;    // +0x3C
		void**        ik;          // +0x40
		void*         skin;        // +0x48
		float         r;           // +0x50
		float         g;           // +0x54
		float         b;           // +0x58
		float         a;           // +0x5C
		float         time;        // +0x60
		std::int32_t  flipX;       // +0x64
		std::int32_t  flipY;       // +0x68
		float         x;           // +0x6C
		float         y;           // +0x70
	};
	static_assert(offsetof(Skeleton, slotsCount) == 0x20);
	static_assert(offsetof(Skeleton, slots) == 0x28);
	static_assert(offsetof(Skeleton, drawOrder) == 0x30);
	static_assert(offsetof(Skeleton, a) == 0x5C);

	struct Animation
	{
		const char*   name;            // +0x00
		float         duration;        // +0x08
		std::int32_t  timelinesCount;  // +0x0C
		void**        timelines;       // +0x10
	};
	static_assert(sizeof(Animation) == 0x18);
	static_assert(offsetof(Animation, timelinesCount) == 0x0C);
	static_assert(offsetof(Animation, timelines) == 0x10);

	struct Timeline
	{
		std::int32_t type;      // +0x00 见 TimelineType*
		std::int32_t padding;   // +0x04
		void*        vtable;    // +0x08 [0]=apply [8]=dispose
		std::int32_t frameCount;  // +0x10
		std::int32_t padding1;    // +0x14
		float*       frames;      // +0x18
	};
	static_assert(offsetof(Timeline, type) == 0x00);
	static_assert(offsetof(Timeline, frameCount) == 0x10);
	static_assert(offsetof(Timeline, frames) == 0x18);

	// type 3：851460 create(count)、851520 setFrame(i, time, name)，apply 84FE60
	struct AttachmentTimeline : Timeline
	{
		std::int32_t        slotIndex;  // +0x20
		std::int32_t        padding2;   // +0x24
		const char* const*  names;      // +0x28，元素为 null 表示隐藏
	};
	static_assert(sizeof(AttachmentTimeline) == 0x30);
	static_assert(offsetof(AttachmentTimeline, slotIndex) == 0x20);
	static_assert(offsetof(AttachmentTimeline, names) == 0x28);

	// type 6：8518D0 create(count, slotsCount)、8519A0 setFrame(i, time, order)，apply 8502D0
	struct DrawOrderTimeline : Timeline
	{
		const std::int32_t* const* orders;      // +0x20 每帧一个 slotsCount 长度的槽位下标表，null 表示初始顺序
		std::int32_t               orderSlots;  // +0x28
		std::int32_t               padding2;    // +0x2C
	};
	static_assert(sizeof(DrawOrderTimeline) == 0x30);
	static_assert(offsetof(DrawOrderTimeline, orders) == 0x20);
	static_assert(offsetof(DrawOrderTimeline, orderSlots) == 0x28);

	struct TrackEntry
	{
		void*        padding0;   // +0x00
		void*        padding1;   // +0x08
		void*        padding2;   // +0x10
		Animation*   animation;  // +0x18
		std::int32_t loop;       // +0x20
		float        delay;      // +0x24
		float        time;       // +0x28
		float        lastTime;   // +0x2C
		float        endTime;    // +0x30
		float        timeScale;  // +0x34
	};
	static_assert(offsetof(TrackEntry, animation) == 0x18);
	static_assert(offsetof(TrackEntry, time) == 0x28);

	struct AnimationState
	{
		void*         padding0;       // +0x00
		void*         padding1;       // +0x08
		void*         padding2;       // +0x10
		std::int32_t  tracksCount;    // +0x18
		std::int32_t  padding3;       // +0x1C
		TrackEntry**  tracks;         // +0x20
		void*         rendererObject;  // +0x28 游戏在这里存 SpineSprite*
	};
	static_assert(offsetof(AnimationState, tracksCount) == 0x18);
	static_assert(offsetof(AnimationState, tracks) == 0x20);

	// SpineSprite 的条目，0x30 字节，见 257690 AddSpineData / 258540 Update
	struct SpriteEntry
	{
		std::uint64_t handle[2];   // +0x00 ResourceHandle<SpineData>
		Skeleton*     skeleton;    // +0x10
		AnimationState* state;     // +0x18
		std::uint64_t texture[2];  // +0x20 贴图重定向用的 ResourceHandle
	};
	static_assert(sizeof(SpriteEntry) == 0x30);
	static_assert(offsetof(SpriteEntry, skeleton) == 0x10);
	static_assert(offsetof(SpriteEntry, state) == 0x18);

	// SpineSprite 头部，大小 0xD8，2580A0 构造
	struct Sprite
	{
		void*         animation;  // +0x00 当前 spAnimation*
		float         time;       // +0x08 累计时间
		std::int32_t  index;      // +0x0C 当前条目下标
		float         bounds[4];  // +0x10 xmin/xmax/ymin/ymax
		SpriteEntry*  entries;    // +0x20
		SpriteEntry*  entriesEnd; // +0x28
	};
	static_assert(offsetof(Sprite, index) == 0x0C);
	static_assert(offsetof(Sprite, entries) == 0x20);
	static_assert(offsetof(Sprite, entriesEnd) == 0x28);

	// 取当前条目；sprite 没有条目时返回 null
	inline SpriteEntry* CurrentEntry(Sprite* a_sprite) noexcept
	{
		if (!a_sprite || a_sprite->entries == a_sprite->entriesEnd) {
			return nullptr;
		}
		if (a_sprite->index < 0) {
			return nullptr;
		}
		return a_sprite->entries + a_sprite->index;
	}
}
