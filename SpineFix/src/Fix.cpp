#include "Fix.h"

namespace SpineFix::render
{
	Blend BlendOfSlot(const spine::Slot& a_slot) noexcept
	{
		if (!a_slot.data) {
			return Blend::Normal;
		}

		// spSlotData 是自研结构，color 在 +0x18，additive 字节被当成 uint 存在 +0x28，
		// 见 2525A0（ReadSlots）和 8558C0（spSlot_create 从 data+0x18 取 color）。
		const auto value = static_cast<const spine::SlotData*>(a_slot.data)->blend;
		switch (value) {
		case 1:
			return Blend::Additive;
		case 2:
			return Blend::Multiply;
		case 3:
			return Blend::Screen;
		default:
			return Blend::Normal;
		}
	}

	std::vector<Run> BuildRuns(const spine::Sprite& a_sprite, std::size_t a_maxRuns, bool a_honorAdditive)
	{
		std::vector<Run> runs;

		auto* entry = spine::CurrentEntry(const_cast<spine::Sprite*>(&a_sprite));
		if (!entry || !entry->skeleton) {
			return runs;
		}

		const auto& skeleton = *entry->skeleton;
		if (!skeleton.drawOrder || !skeleton.slots || skeleton.slotsCount <= 0) {
			return runs;
		}

		if (a_maxRuns == 0) {
			a_maxRuns = 1;
		}

		for (std::int32_t i = 0; i < skeleton.slotsCount; ++i) {
			auto* slot = skeleton.drawOrder[i];
			if (!slot || !slot->attachment) {
				continue;
			}

			auto blend = BlendOfSlot(*slot);
			if (blend == Blend::Additive && !a_honorAdditive) {
				blend = Blend::Normal;
			}

			if (!runs.empty() && runs.back().blend == blend) {
				runs.back().end = i + 1;
				continue;
			}
			// 段数到上限后不再开新段，后面的并进最后一段（只会发生在极端资源上）
			if (runs.size() >= a_maxRuns) {
				runs.back().end = i + 1;
				continue;
			}

			runs.push_back(Run{ blend, i, i + 1 });
		}

		return runs;
	}
}
