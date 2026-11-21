#include "Hooks.h"

#include "Config.h"
#include "Fix.h"
#include "Gl.h"
#include "SpineLayout.h"
#include "Timeline.h"

namespace Hooks
{
	using namespace DKUtil::Alias;
	using namespace SpineFix;

	namespace
	{
		namespace Asm = dku::Hook::Assembly;

		// ===================== 游戏接口 =====================
		// 基准 exe: Darkest.exe SHA1 9db2d73b89aa034e2055df65a139403e18df641f，地址都是 RVA
		constexpr std::uintptr_t RvaRender                 = 0x2585E0;  // SpineSprite::Render(this, tint)
		constexpr std::uintptr_t RvaGetAttachmentForSlot   = 0x853330;  // spSkeleton_getAttachmentForSlotIndex
		constexpr std::uintptr_t RvaSetAttachment          = 0x8559A0;  // spSlot_setAttachment

		using Render_t            = void (*)(void* a_sprite, std::uint64_t a_tint);
		using GetAttachmentForSlot_t = void* (*)(void* a_skeleton, std::int32_t a_slotIndex, const char* a_name);
		using SetAttachment_t     = void (*)(void* a_slot, void* a_attachment);

		Render_t               RenderOriginal{ nullptr };
		GetAttachmentForSlot_t GetAttachmentForSlot{ nullptr };
		SetAttachment_t        SetAttachment{ nullptr };

		// ===================== 日志节流 =====================
		// Render 每帧都会被调用，修补明细只在预算内记录，之后只累计。
		constexpr int RepairLogBudget = 24;

		int       g_repairLogsLeft = RepairLogBudget;
		long long g_attachmentFixes = 0;
		int       g_passLogsLeft = 8;
		long long g_drawOrderFixes = 0;

		std::uintptr_t ToRva(const void* a_address)
		{
			return AsAddress(a_address) - dku::Hook::Module::get().base();
		}

		timeline::RepairContext MakeRepairContext()
		{
			timeline::RepairContext context;
			context.resolve = reinterpret_cast<void* (*)(void*, std::int32_t, const char*)>(GetAttachmentForSlot);
			context.attach = reinterpret_cast<void (*)(void*, void*)>(SetAttachment);
			return context;
		}

		// 每种骨骼资源第一次绘制时打一份自检，用来确认 .skel 读进来没有、混合字节写对没有
		void ProbeOnce(spine::Sprite& a_sprite)
		{
			static std::unordered_set<const void*> seen;  // 只在渲染线程用

			auto* entry = spine::CurrentEntry(&a_sprite);
			if (!entry || !entry->skeleton) {
				return;
			}
			if (!seen.insert(entry->skeleton->data).second || seen.size() > 256) {
				return;
			}

			int normal = 0;
			int additive = 0;
			int multiply = 0;
			int screen = 0;
			int unknown = 0;

			const auto& skeleton = *entry->skeleton;
			for (std::int32_t i = 0; i < skeleton.slotsCount; ++i) {
				auto* slot = skeleton.slots ? skeleton.slots[i] : nullptr;
				if (!slot || !slot->data) {
					continue;
				}
				switch (static_cast<const spine::SlotData*>(slot->data)->blend) {
				case 0: ++normal; break;
				case 1: ++additive; break;
				case 2: ++multiply; break;
				case 3: ++screen; break;
				default: ++unknown; break;
				}
			}

			const char* animation = "-";
			int         attachments = 0;
			int         drawOrders = 0;
			int         colors = 0;
			int         timelines = 0;
			if (entry->state && entry->state->tracks && entry->state->tracksCount > 0) {
				if (auto* track = entry->state->tracks[0]; track && track->animation) {
					const auto& current = *track->animation;
					animation = current.name ? current.name : "-";
					timelines = current.timelinesCount;
					for (std::int32_t i = 0; i < current.timelinesCount; ++i) {
						auto* timeline = static_cast<spine::Timeline*>(current.timelines[i]);
						if (!timeline) {
							continue;
						}
						if (timeline->type == spine::TimelineTypeAttachment) {
							++attachments;
						} else if (timeline->type == spine::TimelineTypeDrawOrder) {
							++drawOrders;
						} else if (timeline->type == 4) {
							++colors;
						}
					}
				}
			}

			// 当前动画可能不是出问题的那个，所以顺手扫一遍整份骨骼的所有动画：
			// 只要文件里存在 drawOrder 时间线，这里就能看到，不需要正好播到那一段。
			int         allAnimations = 0;
			int         allAttachments = 0;
			int         allDrawOrders = 0;
			std::string drawOrderNames;
			const auto* data = static_cast<const spine::SkeletonData*>(skeleton.data);
			if (data && data->animationsCount > 0 && data->animationsCount <= 4096 && data->animations) {
				allAnimations = data->animationsCount;
				for (std::int32_t i = 0; i < data->animationsCount; ++i) {
					const auto* current = data->animations[i];
					if (!current || !current->timelines || current->timelinesCount <= 0 ||
						current->timelinesCount > 0x10000) {
						continue;
					}
					int attachment = 0;
					int drawOrder = 0;
					for (std::int32_t j = 0; j < current->timelinesCount; ++j) {
						const auto* timeline = static_cast<const spine::Timeline*>(current->timelines[j]);
						if (!timeline) {
							continue;
						}
						if (timeline->type == spine::TimelineTypeAttachment) {
							++attachment;
						} else if (timeline->type == spine::TimelineTypeDrawOrder) {
							++drawOrder;
						}
					}
					allAttachments += attachment;
					allDrawOrders += drawOrder;

					if (drawOrder > 0 && drawOrderNames.size() < 160) {
						if (!drawOrderNames.empty()) {
							drawOrderNames += ',';
						}
						const char* name = current->name ? current->name : "?";
						drawOrderNames.append(name, std::min<std::size_t>(std::strlen(name), 40));
						drawOrderNames += '(' + std::to_string(drawOrder) + ')';
					}
				}
			}

			INFO("probe: slots={} blend normal/add/mul/screen/other={}/{}/{}/{}/{} animation=\"{}\" timelines={} "
				 "attachment={} drawOrder={} color={} | all: anims={} attachment={} drawOrder={}{}{}",
				skeleton.slotsCount, normal, additive, multiply, screen, unknown, animation, timelines, attachments,
				drawOrders, colors, allAnimations, allAttachments, allDrawOrders,
				drawOrderNames.empty() ? "" : " names=", drawOrderNames);
		}

		// 记录 slot 附件与 drawOrder，供「只记录不修补」模式回滚
		class Snapshot
		{
		public:
			explicit Snapshot(const spine::Skeleton& a_skeleton)
			{
				const auto count = a_skeleton.slotsCount > 0 ? static_cast<std::size_t>(a_skeleton.slotsCount) : 0;
				if (a_skeleton.slots) {
					m_attachments.resize(count, nullptr);
					for (std::size_t i = 0; i < count; ++i) {
						if (a_skeleton.slots[i]) {
							m_attachments[i] = a_skeleton.slots[i]->attachment;
						}
					}
				}
				if (a_skeleton.drawOrder) {
					m_drawOrder.assign(a_skeleton.drawOrder, a_skeleton.drawOrder + count);
				}
			}

			void Restore(spine::Skeleton& a_skeleton) const
			{
				if (a_skeleton.slots && a_skeleton.slotsCount > 0) {
					for (std::size_t i = 0; i < m_attachments.size(); ++i) {
						if (a_skeleton.slots[i]) {
							a_skeleton.slots[i]->attachment = m_attachments[i];
						}
					}
				}
				if (a_skeleton.drawOrder && !m_drawOrder.empty()) {
					std::copy(m_drawOrder.begin(), m_drawOrder.end(), a_skeleton.drawOrder);
				}
			}

		private:
			std::vector<void*>     m_attachments;
			std::vector<spine::Slot*> m_drawOrder;
		};

		// 把时间线修补结果写进日志；每个动画只报一次，避免刷屏
		void ReportRepair(const spine::Sprite& a_sprite, const timeline::RepairStats& a_stats)
		{
			g_attachmentFixes += a_stats.attachments;
			g_drawOrderFixes += a_stats.drawOrders;

			if (!config::Verbose() && g_repairLogsLeft <= 0) {
				return;
			}

			auto* entry = spine::CurrentEntry(const_cast<spine::Sprite*>(&a_sprite));
			const char* animation = "<unknown>";
			if (entry && entry->state && entry->state->tracks && entry->state->tracksCount > 0) {
				if (auto* track = entry->state->tracks[0]; track && track->animation && track->animation->name) {
					animation = track->animation->name;
				}
			}

			--g_repairLogsLeft;
			INFO("repaired \"{}\": {} attachment, {} drawOrder (total {} / {})", animation, a_stats.attachments,
				a_stats.drawOrders, g_attachmentFixes, g_drawOrderFixes);
		}

		// 当前动画真的带 drawOrder 时间线时记一条（每个动画一次）。用来区分
		// 「那份动画根本没播到」和「播到了但修补算出来没有差异」。
		void ReportDrawOrderActive(const spine::Sprite& a_sprite)
		{
			if (!config::Verbose() && !config::Probe()) {
				return;
			}
			static std::unordered_set<const spine::Animation*> seen;  // 只在渲染线程用

			auto* entry = spine::CurrentEntry(const_cast<spine::Sprite*>(&a_sprite));
			if (!entry || !entry->state || !entry->state->tracks || entry->state->tracksCount <= 0) {
				return;
			}
			auto* track = entry->state->tracks[0];
			if (!track || !track->animation || !seen.insert(track->animation).second) {
				return;
			}

			const auto& animation = *track->animation;
			int drawOrders = 0;
			if (animation.timelines && animation.timelinesCount > 0) {
				for (std::int32_t i = 0; i < animation.timelinesCount; ++i) {
					const auto* timeline = static_cast<const spine::Timeline*>(animation.timelines[i]);
					if (timeline && timeline->type == spine::TimelineTypeDrawOrder) {
						++drawOrders;
					}
				}
			}
			if (drawOrders > 0) {
				INFO("drawOrder timeline active: \"{}\" ({})", animation.name ? animation.name : "-", drawOrders);
			}
		}

		// ===================== 分段绘制 =====================
		// 原版一次 Render 只用一个混合状态把整只 sprite 画完。这里按混合模式把 drawOrder
		// 切成连续的段，每段只保留该段的切片可见、调好混合状态后各调一次原函数，于是
		// 顶点、UV、贴图、颜色仍然由原版代码算，只有混合状态和可见集合在变。
		void DrawInPasses(void* a_spriteRaw, std::uint64_t a_tint, spine::Sprite& a_sprite,
			const std::vector<render::Run>& a_runs)
		{
			auto* entry = spine::CurrentEntry(&a_sprite);
			if (!entry || !entry->skeleton || !entry->skeleton->slots || !entry->skeleton->drawOrder) {
				RenderOriginal(a_spriteRaw, a_tint);
				return;
			}

			auto& skeleton = *entry->skeleton;
			const std::int32_t count = skeleton.slotsCount;
			if (count <= 0) {
				RenderOriginal(a_spriteRaw, a_tint);
				return;
			}

			const auto size = static_cast<std::size_t>(count);
			std::vector<void*> saved(size, nullptr);
			for (std::int32_t i = 0; i < count; ++i) {
				if (skeleton.slots[i]) {
					saved[static_cast<std::size_t>(i)] = skeleton.slots[i]->attachment;
				}
			}

			// drawOrder 的每个位置对应哪个槽位下标；drawOrder 是 slots 的排列，正常只有一次命中
			std::vector<std::int32_t> slotOf(size, -1);
			for (std::int32_t position = 0; position < count; ++position) {
				auto* slot = skeleton.drawOrder[position];
				for (std::int32_t i = 0; i < count; ++i) {
					if (skeleton.slots[i] == slot) {
						slotOf[static_cast<std::size_t>(position)] = i;
						break;
					}
				}
			}

			// drawOrder 不是 slots 的排列时不拆段（数据坏了的时候宁可回到原版行为）
			if (std::any_of(slotOf.begin(), slotOf.end(), [](std::int32_t a_index) { return a_index < 0; })) {
				RenderOriginal(a_spriteRaw, a_tint);
				return;
			}

			const auto blendFuncSeparate = gl::ResolveBlendFuncSeparate();
			const auto blendEquationSeparate = gl::ResolveBlendEquationSeparate();
			const auto state = gl::Save();

			for (const auto& run : a_runs) {
				for (std::int32_t i = 0; i < count; ++i) {
					if (skeleton.slots[i]) {
						skeleton.slots[i]->attachment = nullptr;
					}
				}
				for (std::int32_t position = run.begin; position < run.end; ++position) {
					const auto index = slotOf[static_cast<std::size_t>(position)];
					if (index >= 0 && skeleton.slots[index]) {
						skeleton.slots[index]->attachment = saved[static_cast<std::size_t>(index)];
					}
				}

				switch (run.blend) {
				case render::Blend::Normal:
					gl::SetEnabled(state.enabled);
					gl::SetEquation(blendEquationSeparate, static_cast<unsigned>(state.equationRgb),
						static_cast<unsigned>(state.equationAlpha));
					gl::SetFuncSeparate(blendFuncSeparate, static_cast<unsigned>(state.srcRgb),
						static_cast<unsigned>(state.dstRgb));
					break;
				case render::Blend::Additive:
					// 贴图是预乘的，加法就是 ONE / ONE
					gl::SetEnabled(true);
					gl::SetEquation(blendEquationSeparate, gl::FuncAdd, gl::FuncAdd);
					gl::SetFuncSeparate(blendFuncSeparate, gl::One, gl::One);
					break;
				case render::Blend::Multiply:
					// 正片叠底：dst * src + dst * (1 - srcA)
					gl::SetEnabled(true);
					gl::SetEquation(blendEquationSeparate, gl::FuncAdd, gl::FuncAdd);
					gl::SetFuncSeparate(blendFuncSeparate, gl::DstColor, gl::OneMinusSrcAlpha);
					break;
				case render::Blend::Screen:
					// 滤色：dst + src * (1 - dst)
					gl::SetEnabled(true);
					gl::SetEquation(blendEquationSeparate, gl::FuncAdd, gl::FuncAdd);
					gl::SetFuncSeparate(blendFuncSeparate, gl::One, gl::OneMinusSrcColor);
					break;
				}

				RenderOriginal(a_spriteRaw, a_tint);
			}

			for (std::int32_t i = 0; i < count; ++i) {
				if (skeleton.slots[i]) {
					skeleton.slots[i]->attachment = saved[static_cast<std::size_t>(i)];
				}
			}

			gl::SetEnabled(state.enabled);
			gl::SetEquation(blendEquationSeparate, static_cast<unsigned>(state.equationRgb),
				static_cast<unsigned>(state.equationAlpha));
			gl::SetFuncSeparate(blendFuncSeparate, static_cast<unsigned>(state.srcRgb),
				static_cast<unsigned>(state.dstRgb));
		}

		// ===================== detour =====================
		void RenderDetour(void* a_spriteRaw, std::uint64_t a_tint)
		{
			auto* sprite = static_cast<spine::Sprite*>(a_spriteRaw);
			auto* entry = spine::CurrentEntry(sprite);
			if (!entry || !entry->skeleton || !entry->state) {
				RenderOriginal(a_spriteRaw, a_tint);
				return;
			}

			if (config::Probe()) {
				ProbeOnce(*sprite);
			}

			const auto repairMode = config::RepairMode();
			if (repairMode == 1) {
				const auto stats = timeline::RepairSprite(*sprite, MakeRepairContext());
				if (stats.attachments || stats.drawOrders) {
					ReportRepair(*sprite, stats);
				}
			} else if (repairMode == 2) {
				// 只记录：借用同一套修补逻辑算出差异，再把状态原样放回去
				Snapshot snapshot(*entry->skeleton);
				const auto stats = timeline::RepairSprite(*sprite, MakeRepairContext());
				snapshot.Restore(*entry->skeleton);
				if (stats.attachments || stats.drawOrders) {
					ReportRepair(*sprite, stats);
				}
			}

			ReportDrawOrderActive(*sprite);

			if (!config::BlendModes()) {
				RenderOriginal(a_spriteRaw, a_tint);
				return;
			}

			const auto runs = render::BuildRuns(*sprite, static_cast<std::size_t>(config::MaxPasses()),
				config::Additive());
			std::vector<render::Run> passes(runs);
			if (config::DebugSplitPasses() && passes.size() == 1 && passes.front().blend == render::Blend::Normal &&
				passes.front().end - passes.front().begin > 1) {
				// 自检：把一个正常段拆成两段，画面必须和不拆时一致
				const auto middle = passes.front().begin + (passes.front().end - passes.front().begin) / 2;
				const auto end = passes.front().end;
				passes.front().end = middle;
				passes.push_back(render::Run{ render::Blend::Normal, middle, end });
			}

			// 没有特殊混合时一次都不拆，原版行为完全不变
			if (passes.size() <= 1 && (passes.empty() || passes.front().blend == render::Blend::Normal)) {
				RenderOriginal(a_spriteRaw, a_tint);
				return;
			}

			if (g_passLogsLeft > 0) {
				--g_passLogsLeft;
				INFO("multi-pass draw: {} pass(es)", passes.size());
			}

			DrawInPasses(a_spriteRaw, a_tint, *sprite, passes);
		}

	}

	// ===================== 初始化 =====================
	void Install()
	{
		auto* render = Asm::search_pattern<
			"48 89 54 24 10 "           // mov     [rsp+10h], rdx
			"53 55 "                    // push    rbx, rbp
			"48 81 EC D8 00 00 00 "     // sub     rsp, 0D8h
			"48 8B 05 ?? ?? ?? ?? "     // mov     rax, cs:__security_cookie
			"48 33 C4 "                 // xor     rax, rsp
			"48 89 44 24 28 "           // mov     [rsp+28h], rax
			"48 8B DA "                 // mov     rbx, rdx
			"48 8B E9 "                 // mov     rbp, rcx
			"48 8B 51 20">();           // mov     rdx, [rcx+20h]

		auto* getAttachment = Asm::search_pattern<
			"48 89 5C 24 08 "           // mov     [rsp+8], rbx
			"48 89 74 24 10 "           // mov     [rsp+10h], rsi
			"57 "                       // push    rdi
			"48 83 EC 20 "              // sub     rsp, 20h
			"49 8B F0 "                 // mov     rsi, r8
			"8B FA "                    // mov     edi, edx
			"48 8B D9 "                 // mov     rbx, rcx
			"83 FA FF "                 // cmp     edx, -1
			"74 38 "                    // je      short ...
			"48 8B 49 48 "              // mov     rcx, [rcx+48h]
			"48 85 C9 "                 // test    rcx, rcx
			"74 0A">();                 // je      short ...

		auto* setAttachment = Asm::search_pattern<
			"48 8B 41 08 "                    // mov     rax, [rcx+8]
			"48 89 51 20 "                    // mov     [rcx+20h], rdx
			"48 8B 50 08 "                    // mov     rdx, [rax+8]
			"8B 42 60 "                       // mov     eax, [rdx+60h]
			"89 41 38 "                       // mov     [rcx+38h], eax
			"C7 41 2C 00 00 00 00 "           // mov     dword ptr [rcx+2Ch], 0
			"C3">();                          // ret

		if (!render || !getAttachment || !setAttachment) {
			WARN("Incompatible game version, no hook installed (Render: {}, GetAttachment: {}, SetAttachment: {})",
				render != nullptr, getAttachment != nullptr, setAttachment != nullptr);
			return;
		}

		const auto renderRva = ToRva(render);
		const auto getAttachmentRva = ToRva(getAttachment);
		const auto setAttachmentRva = ToRva(setAttachment);
		if (renderRva != RvaRender || getAttachmentRva != RvaGetAttachmentForSlot ||
			setAttachmentRva != RvaSetAttachment) {
			WARN("Offline build or unexpected layout: Render +{:X} (want +{:X}), GetAttachment +{:X} (want +{:X}), "
				 "SetAttachment +{:X} (want +{:X}); hooks are still installed, structure offsets may differ",
				renderRva, RvaRender, getAttachmentRva, RvaGetAttachmentForSlot, setAttachmentRva, RvaSetAttachment);
		} else {
			INFO("Located Render +{:X}, GetAttachment +{:X}, SetAttachment +{:X}", renderRva, getAttachmentRva,
				setAttachmentRva);
		}

		GetAttachmentForSlot = std::bit_cast<GetAttachmentForSlot_t>(getAttachment);
		SetAttachment = std::bit_cast<SetAttachment_t>(setAttachment);

		dku::Hook::Trampoline::AllocTrampoline(1 << 10);
		RenderOriginal = dku::Hook::write_detour<5>(AsAddress(render), RenderDetour);

		if (!RenderOriginal) {
			WARN("Failed to install the Render detour, nothing is patched");
			return;
		}

		INFO("SpineSprite::Render hook installed");
	}
}
