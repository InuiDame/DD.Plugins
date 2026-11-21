// 离线验证时间线修补与分段逻辑，不需要游戏：xmake build spinefix_test && build\...\spinefix_test.exe
//
// 用假的内存布局喂给 Timeline.cpp / Fix.cpp，覆盖：
// 帧查找的边界、attachment 的显示/隐藏、drawOrder 的排列与尾部补齐、
// 多轨道、混合模式分段与段数上限。

#include <cstdio>
#include <cstring>
#include <vector>

#include "Fix.h"
#include "Timeline.h"

namespace
{
	using namespace SpineFix;

	int g_checks = 0;
	int g_failures = 0;

	void Check(bool a_condition, const char* a_what)
	{
		++g_checks;
		if (!a_condition) {
			++g_failures;
			std::printf("FAIL  %s\n", a_what);
		}
	}

	template <class T>
	void CheckEqual(const T& a_actual, const T& a_expected, const char* a_what)
	{
		++g_checks;
		if (!(a_actual == a_expected)) {
			++g_failures;
			std::printf("FAIL  %s (got %lld, want %lld)\n", a_what, static_cast<long long>(a_actual),
				static_cast<long long>(a_expected));
		}
	}

	// 假的附件对象，只用地址区分
	char g_body = 0;
	char g_weapon = 0;
	char g_other = 0;

	void* FakeResolve(void*, std::int32_t a_slotIndex, const char* a_name)
	{
		(void)a_slotIndex;
		if (std::strcmp(a_name, "body") == 0) {
			return &g_body;
		}
		if (std::strcmp(a_name, "weapon") == 0) {
			return &g_weapon;
		}
		// 不存在的名字：spine 的行为是什么都不画
		return nullptr;
	}

	void FakeAttach(void* a_slot, void* a_attachment)
	{
		static_cast<spine::Slot*>(a_slot)->attachment = a_attachment;
	}

	timeline::RepairContext FakeContext()
	{
		timeline::RepairContext context;
		context.resolve = FakeResolve;
		context.attach = FakeAttach;
		return context;
	}

	// 一组槽位 + drawOrder 的夹具
	struct Fixture
	{
		explicit Fixture(int a_slotCount) :
			slotData(static_cast<std::size_t>(a_slotCount)),
			slots(static_cast<std::size_t>(a_slotCount)),
			slotPtrs(static_cast<std::size_t>(a_slotCount)),
			drawOrder(static_cast<std::size_t>(a_slotCount))
		{
			for (int i = 0; i < a_slotCount; ++i) {
				const auto index = static_cast<std::size_t>(i);
				slots[index].data = &slotData[index];
				slotPtrs[index] = &slots[index];
				drawOrder[index] = &slots[index];
			}
			skeleton.slotsCount = a_slotCount;
			skeleton.slots = slotPtrs.data();
			skeleton.drawOrder = drawOrder.data();
		}

		std::vector<spine::SlotData> slotData;
		std::vector<spine::Slot>     slots;
		std::vector<spine::Slot*>    slotPtrs;
		std::vector<spine::Slot*>    drawOrder;
		spine::Skeleton              skeleton{};
	};

	void TestFindFrame()
	{
		const float frames[] = { 0.0f, 1.0f, 2.0f };
		CheckEqual(timeline::FindFrame(frames, 3, -0.5f), -1, "FindFrame before first frame");
		CheckEqual(timeline::FindFrame(frames, 3, 0.0f), 0, "FindFrame at first frame");
		CheckEqual(timeline::FindFrame(frames, 3, 0.5f), 0, "FindFrame between 0 and 1");
		CheckEqual(timeline::FindFrame(frames, 3, 1.0f), 1, "FindFrame exactly on 1");
		CheckEqual(timeline::FindFrame(frames, 3, 1.999f), 1, "FindFrame just before last");
		CheckEqual(timeline::FindFrame(frames, 3, 2.0f), 2, "FindFrame at last frame");
		CheckEqual(timeline::FindFrame(frames, 3, 100.0f), 2, "FindFrame past the end");
		CheckEqual(timeline::FindFrame(nullptr, 3, 1.0f), -1, "FindFrame null frames");
		CheckEqual(timeline::FindFrame(frames, 0, 1.0f), -1, "FindFrame empty");
	}

	void TestAttachment()
	{
		Fixture fixture(3);
		auto context = FakeContext();

		const char* names[] = { "body", nullptr, "missing" };
		float       frames[] = { 1.0f, 2.0f, 3.0f };

		spine::AttachmentTimeline attachment{};
		attachment.type = spine::TimelineTypeAttachment;
		attachment.frameCount = 3;
		attachment.frames = frames;
		attachment.slotIndex = 1;
		attachment.names = names;

		Check(!timeline::RepairAttachment(fixture.skeleton, attachment, 0.5f, context),
			"attachment before the first frame leaves state alone");
		Check(fixture.slots[1].attachment == nullptr, "attachment before the first frame stays null");

		Check(timeline::RepairAttachment(fixture.skeleton, attachment, 1.0f, context), "attachment shows slot");
		Check(fixture.slots[1].attachment == &g_body, "attachment points at the resolved attachment");
		Check(!timeline::RepairAttachment(fixture.skeleton, attachment, 1.5f, context),
			"attachment is idempotent inside the same frame");

		Check(timeline::RepairAttachment(fixture.skeleton, attachment, 2.0f, context), "attachment hides slot");
		Check(fixture.slots[1].attachment == nullptr, "attachment becomes null for a null name");

		// 名字在皮肤里查不到时也当作隐藏
		fixture.slots[1].attachment = &g_body;
		Check(timeline::RepairAttachment(fixture.skeleton, attachment, 3.0f, context),
			"attachment with an unknown name changes state");
		Check(fixture.slots[1].attachment == nullptr, "unknown attachment name hides the slot");

		// 越界的槽位下标不能崩
		attachment.slotIndex = 99;
		Check(!timeline::RepairAttachment(fixture.skeleton, attachment, 1.0f, context), "out of range slot ignored");

		// 改别的槽位不影响本槽位
		attachment.slotIndex = 2;
		attachment.names = names;
		Check(timeline::RepairAttachment(fixture.skeleton, attachment, 1.0f, context), "other slot repaired");
		Check(fixture.slots[2].attachment == &g_body, "other slot got the attachment");
		Check(fixture.slots[1].attachment == nullptr, "previous slot untouched");
		(void)g_other;
	}

	void TestDrawOrder()
	{
		Fixture fixture(4);
		auto&   order = fixture.drawOrder;

		const std::int32_t permuted[] = { 3, 1, 0, 2 };
		const std::int32_t* orders[] = { permuted };
		float               frames[] = { 0.5f };

		spine::DrawOrderTimeline drawOrder{};
		drawOrder.type = spine::TimelineTypeDrawOrder;
		drawOrder.frameCount = 1;
		drawOrder.frames = frames;
		drawOrder.orders = orders;
		drawOrder.orderSlots = 4;

		Check(!timeline::RepairDrawOrder(fixture.skeleton, drawOrder, 0.25f), "drawOrder before first frame ignored");
		Check(order[0] == &fixture.slots[0], "drawOrder unchanged before the keyframe");

		Check(timeline::RepairDrawOrder(fixture.skeleton, drawOrder, 1.0f), "drawOrder applied");
		for (int i = 0; i < 4; ++i) {
			Check(order[i] == &fixture.slots[permuted[i]], "drawOrder follows the keyframe permutation");
		}
		Check(!timeline::RepairDrawOrder(fixture.skeleton, drawOrder, 2.0f), "drawOrder is idempotent");

		// 数据比 skeleton 短时，尾部回到初始顺序
		drawOrder.orderSlots = 2;
		Check(timeline::RepairDrawOrder(fixture.skeleton, drawOrder, 2.0f), "short drawOrder repairs the tail");
		Check(order[0] == &fixture.slots[3] && order[1] == &fixture.slots[1], "short drawOrder keeps its prefix");
		Check(order[2] == &fixture.slots[2] && order[3] == &fixture.slots[3], "short drawOrder resets the tail");

		// orderSlots 为 0 但帧里带了排列时，按 slotsCount 处理
		drawOrder.orderSlots = 0;
		timeline::RepairDrawOrder(fixture.skeleton, drawOrder, 2.0f);
		for (int i = 0; i < 4; ++i) {
			Check(order[i] == &fixture.slots[permuted[i]], "zero orderSlots falls back to slotsCount");
		}

		// 帧里没有排列（null）时回到初始顺序
		const std::int32_t* emptyOrders[] = { nullptr };
		drawOrder.orders = emptyOrders;
		Check(timeline::RepairDrawOrder(fixture.skeleton, drawOrder, 2.0f), "null order restores identity");
		for (int i = 0; i < 4; ++i) {
			Check(order[i] == &fixture.slots[i], "null order gives the identity draw order");
		}
	}

	void TestRepairSprite()
	{
		Fixture fixture(2);
		auto    context = FakeContext();

		const char* names[] = { "body" };
		float       attachmentFrames[] = { 0.0f };
		spine::AttachmentTimeline attachment{};
		attachment.type = spine::TimelineTypeAttachment;
		attachment.frameCount = 1;
		attachment.frames = attachmentFrames;
		attachment.slotIndex = 0;
		attachment.names = names;

		const std::int32_t permuted[] = { 1, 0 };
		const std::int32_t* orders[] = { permuted };
		float               orderFrames[] = { 0.0f };
		spine::DrawOrderTimeline drawOrder{};
		drawOrder.type = spine::TimelineTypeDrawOrder;
		drawOrder.frameCount = 1;
		drawOrder.frames = orderFrames;
		drawOrder.orders = orders;
		drawOrder.orderSlots = 2;

		void* timelines[] = { &attachment, &drawOrder };
		spine::Animation animation{};
		animation.name = "idle";
		animation.timelinesCount = 2;
		animation.timelines = timelines;

		spine::TrackEntry track{};
		track.animation = &animation;
		track.time = 1.0f;
		spine::TrackEntry* tracks[] = { &track };
		spine::AnimationState state{};
		state.tracksCount = 1;
		state.tracks = tracks;

		spine::SpriteEntry entry{};
		entry.skeleton = &fixture.skeleton;
		entry.state = &state;

		spine::Sprite sprite{};
		sprite.index = 0;
		sprite.entries = &entry;
		sprite.entriesEnd = &entry + 1;

		const auto stats = timeline::RepairSprite(sprite, context);
		CheckEqual(stats.attachments, 1, "RepairSprite counted one attachment fix");
		CheckEqual(stats.drawOrders, 1, "RepairSprite counted one drawOrder fix");
		Check(fixture.slots[0].attachment == &g_body, "RepairSprite applied the attachment");
		Check(fixture.drawOrder[0] == &fixture.slots[1], "RepairSprite applied the draw order");

		// 第二条轨道只有骨骼时间线时要跳过，且不能崩
		spine::Animation empty{};
		empty.name = "empty";
		spine::TrackEntry upper{};
		upper.animation = &empty;
		upper.time = 0.5f;
		spine::TrackEntry* twoTracks[] = { &track, &upper };
		state.tracksCount = 2;
		state.tracks = twoTracks;
		const auto second = timeline::RepairSprite(sprite, context);
		CheckEqual(second.attachments + second.drawOrders, 0, "second pass changes nothing");
	}

	void TestLoopWrap()
	{
		Fixture     fixture(1);
		auto        context = FakeContext();
		const char* names[] = { "body", "weapon" };
		float       frames[] = { 0.0f, 1.0f };

		spine::AttachmentTimeline attachment{};
		attachment.type = spine::TimelineTypeAttachment;
		attachment.frameCount = 2;
		attachment.frames = frames;
		attachment.slotIndex = 0;
		attachment.names = names;

		void* timelines[] = { &attachment };
		spine::Animation animation{};
		animation.name = "walk";
		animation.duration = 1.0f;
		animation.timelinesCount = 1;
		animation.timelines = timelines;

		spine::TrackEntry track{};
		track.animation = &animation;
		track.time = 2.25f;  // 已经播过两遍
		track.loop = 1;
		spine::TrackEntry* tracks[] = { &track };
		spine::AnimationState state{};
		state.tracksCount = 1;
		state.tracks = tracks;

		spine::SpriteEntry entry{};
		entry.skeleton = &fixture.skeleton;
		entry.state = &state;

		spine::Sprite sprite{};
		sprite.entries = &entry;
		sprite.entriesEnd = &entry + 1;

		// 先把它放到末帧，再让回卷后的时间把它拉回第一帧
		fixture.slots[0].attachment = &g_weapon;
		timeline::RepairSprite(sprite, context);
		Check(fixture.slots[0].attachment == &g_body, "looped animation keeps cycling frames");

		// loop=0 时游戏把时间夹在末帧，修补也不应回卷
		track.loop = 0;
		fixture.slots[0].attachment = &g_body;
		timeline::RepairSprite(sprite, context);
		Check(fixture.slots[0].attachment == &g_weapon, "non-looped animation stays on the last frame");
	}

	void SetBlend(Fixture& a_fixture, int a_index, std::uint32_t a_blend)
	{
		a_fixture.slotData[static_cast<std::size_t>(a_index)].blend = a_blend;
	}

	void ShowAll(Fixture& a_fixture)
	{
		for (auto& slot : a_fixture.slots) {
			slot.attachment = &g_body;
		}
	}

	void TestBuildRuns()
	{
		Fixture fixture(4);
		ShowAll(fixture);
		SetBlend(fixture, 0, 0);
		SetBlend(fixture, 1, 0);
		SetBlend(fixture, 2, 2);  // 正片叠底
		SetBlend(fixture, 3, 0);

		spine::SpriteEntry entry{};
		entry.skeleton = &fixture.skeleton;
		spine::Sprite sprite{};
		sprite.entries = &entry;
		sprite.entriesEnd = &entry + 1;

		Check(render::BlendOfSlot(fixture.slots[2]) == render::Blend::Multiply, "blend byte 2 is multiply");
		Check(render::BlendOfSlot(fixture.slots[0]) == render::Blend::Normal, "blend byte 0 is normal");

		auto runs = render::BuildRuns(sprite, 8, true);
		CheckEqual<long long>(static_cast<long long>(runs.size()), 3, "three runs for normal/multiply/normal");
		Check(runs[0].blend == render::Blend::Normal && runs[0].begin == 0 && runs[0].end == 2, "first run covers 0..1");
		Check(runs[1].blend == render::Blend::Multiply && runs[1].begin == 2 && runs[1].end == 3,
			"multiply run covers slot 2");
		Check(runs[2].blend == render::Blend::Normal && runs[2].begin == 3 && runs[2].end == 4, "third run is slot 3");

		// 隐藏的切片不参与分段
		fixture.slots[2].attachment = nullptr;
		runs = render::BuildRuns(sprite, 8, true);
		CheckEqual<long long>(static_cast<long long>(runs.size()), 1, "hidden slots do not split runs");
		Check(runs[0].begin == 0 && runs[0].end == 4, "single normal run covers the visible slots");

		// additive 可以按配置当正常混合
		ShowAll(fixture);
		SetBlend(fixture, 2, 1);
		runs = render::BuildRuns(sprite, 8, false);
		CheckEqual<long long>(static_cast<long long>(runs.size()), 1, "additive ignored when disabled");
		runs = render::BuildRuns(sprite, 8, true);
		CheckEqual<long long>(static_cast<long long>(runs.size()), 3, "additive honored when enabled");

		// 段数上限：只允许一段
		runs = render::BuildRuns(sprite, 1, true);
		CheckEqual<long long>(static_cast<long long>(runs.size()), 1, "run limit merges everything into one pass");
		Check(runs[0].begin == 0 && runs[0].end == 4, "merged run covers the whole sprite");

		// 未知的混合值按正常处理
		SetBlend(fixture, 0, 200);
		Check(render::BlendOfSlot(fixture.slots[0]) == render::Blend::Normal, "unknown blend byte falls back to normal");
	}
}

int main()
{
	TestFindFrame();
	TestAttachment();
	TestDrawOrder();
	TestRepairSprite();
	TestLoopWrap();
	TestBuildRuns();

	std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
