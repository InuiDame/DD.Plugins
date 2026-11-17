// dd_immune 离线单测
// 覆盖 design.md §5.1 全部 8 类断言：偏移常量/哈希/info 真实块解析/buff 解析/两级 OR/
// target 解析/副本清零（含 .uncontrol 保留、原 buffer 零改动）/可选规则门/
// 转发决策（异常时只调用一次原函数）/class 双 id（+8 与 +0x48）合并。
// 注意：离线 PASS ≠ 实机通过（见 README）。

#include "Rules.h"

using json = nlohmann::json;

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void Check(bool a_cond, const char* a_name, const char* a_file, int a_line)
	{
		if (a_cond) {
			++g_pass;
		} else {
			++g_fail;
			std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", a_file, a_line, a_name);
		}
	}

#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

	void Section(const char* a_title)
	{
		std::printf("\n== %s ==\n", a_title);
	}

	Rules::EffectMirror MakeSentinel()
	{
		Rules::EffectMirror m{};
		std::memset(&m, 0xAB, sizeof(m));
		m.control = 0x11111111;
		m.uncontrol = 0x22222222;
		m.kill = 0x33333333;
		m.killFirst = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x12345000));
		m.killLast = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x12345678));
		m.killEnd = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x12345ABC));
		m.capture = 0x44444444;
		m.captureRemoveFromParty = 0x55555555;
		m.swap = true;
		return m;
	}
}

int main()
{
	// ---------- 1. 偏移常量 / 大小（编译期 static_assert 在 Rules.h 已焊死，这里额外运行时断言） ----------
	Section("1. offsets & size");
	CHECK(Rules::kEffectSize == 0x618);
	CHECK(Rules::OFF_CONTROL == 0x070);
	CHECK(Rules::OFF_UNCONTROL == 0x074);
	CHECK(Rules::OFF_KILL == 0x1F0);
	CHECK(Rules::OFF_KILL_TYPES == 0x1F8);
	CHECK(Rules::OFF_KILL_TYPES_LAST == 0x200);
	CHECK(Rules::OFF_KILL_TYPES_END == 0x208);
	CHECK(Rules::OFF_CAPTURE == 0x320);
	CHECK(Rules::OFF_CAPTURE_RMP == 0x324);
	CHECK(Rules::OFF_SWAP == 0x3E0);
	CHECK(sizeof(Rules::EffectMirror) == 0x618);

	// ---------- 2. 哈希 ----------
	Section("2. hash");
	CHECK(Rules::Hash("stunned") == static_cast<std::int32_t>(0xF9DA5CF5));
	{
		const auto direct = Rules::Hash("hero_class_name_crusader");
		const auto built = Rules::Hash(std::string("hero_class_name_") + "crusader");
		CHECK(direct == built);
	}

	// ---------- 3. info 真实块解析（核心回归点：行首 immune:，无 //） ----------
	Section("3. info real block parse");
	{
		// 核心回归：真实块，行首 immune:，无 //
		const auto e = Rules::ParseInfoLine("immune: .kill true .control true .capture true");
		CHECK(e.matched && e.setMask == 0x7 && e.clearMask == 0);
	}
	{
		// 行首空白 + 大小写 + 裸 token + 逗号分隔
		const auto e = Rules::ParseInfoLine("  IMMUNE: kill, control ,CAPTURE");
		CHECK(e.matched && e.setMask == 0x7 && e.clearMask == 0);
	}
	{
		const auto e = Rules::ParseInfoLine("immune: .capture 1");
		CHECK(e.matched && e.setMask == Rules::IMM_CAPTURE && e.clearMask == 0);
	}
	{
		// 显式 false / 0 → clearMask
		const auto e = Rules::ParseInfoLine("immune: .control false");
		CHECK(e.matched && e.setMask == 0 && e.clearMask == Rules::IMM_CONTROL);
	}
	{
		const auto e = Rules::ParseInfoLine("immune: .capture 0");
		CHECK(e.matched && e.setMask == 0 && e.clearMask == Rules::IMM_CAPTURE);
	}
	{
		// 同一行内后者覆盖前者
		const auto e = Rules::ParseInfoLine("immune: .kill true .kill false");
		CHECK(e.matched && e.setMask == 0 && e.clearMask == Rules::IMM_KILL);
	}
	{
		// 未知 token / 缺布尔值 → WARN 忽略
		CHECK(Rules::ParseInfoLine("immune: bogus").setMask == 0);
		CHECK(Rules::ParseInfoLine("immune: .kill").setMask == 0);
	}
	{
		// 普通块 / 普通注释 / 非 immune 旧标签都不识别
		CHECK(!Rules::ParseInfoLine("resistances: stun 0.3").matched);
		CHECK(!Rules::ParseInfoLine("// just a normal comment").matched);
		CHECK(!Rules::ParseInfoLine("DD_IMMUNE: kill").matched);
	}
	{
		// 向后兼容：// 注释里的旧写法仍可用
		const auto e = Rules::ParseInfoLine("// DD_IMMUNE: kill control capture");
		CHECK(e.matched && e.setMask == 0x7 && e.clearMask == 0);
	}
	{
		const auto e = Rules::ParseInfoLine("resistances: x // trailing DD_IMMUNE: kill capture");
		CHECK(e.matched && e.setMask == (Rules::IMM_KILL | Rules::IMM_CAPTURE));
	}
	{
		// 整文件：真实块与 resistances / combat_skill 并列、不截断；显式 false 清除此前置位
		const auto m = Rules::ParseInfoText(
			"immune: .kill true .control true .capture true\n"
			"resistances:\n"
			"   stun: 0\n"
			"immune: .control false\n"
			"combat_skill: demo\n");
		CHECK(m == (Rules::IMM_KILL | Rules::IMM_CAPTURE));
	}
	{
		// 多行覆盖语义：false 清除前置位，后续 true 再置回
		const auto m = Rules::ParseInfoText(
			"immune: kill control\n"
			"immune: .control false\n"
			"immune: .control true\n");
		CHECK(m == (Rules::IMM_KILL | Rules::IMM_CONTROL));
	}
	{
		// 旧写法多行仍按覆盖语义
		const auto m = Rules::ParseInfoText(
			"// DD_IMMUNE: kill\n"
			"resistances: blah\n"
			"// DD_IMMUNE: control\n");
		CHECK(m == (Rules::IMM_KILL | Rules::IMM_CONTROL));
	}

	// ---------- 4. buff 字段解析 ----------
	Section("4. buff field parse");
	{
		const json j = json::parse(R"({"id":"x","immune_kill":true,"immune_control":true,"immune_capture":true})");
		CHECK(Rules::ParseBuffFields(j) == 0x7);
		CHECK(Rules::HasImmuneFields(j));
	}
	{
		const json j = json::parse(R"({"id":"x","immune_kill":true,"immune_capture":false})");
		CHECK(Rules::ParseBuffFields(j) == Rules::IMM_KILL);
	}
	{
		const json j = json::parse(R"({"id":"x","immune_kill":false})");
		CHECK(Rules::ParseBuffFields(j) == 0);
		CHECK(Rules::HasImmuneFields(j));  // 显式 false 也要登记以覆盖低优先级
	}
	{
		const json j = json::parse(R"({"id":"x"})");
		CHECK(Rules::ParseBuffFields(j) == 0);
		CHECK(!Rules::HasImmuneFields(j));
	}
	{
		const json j = json::parse(R"({"id":"x","immune_kill":"yes","immune_control":true})");
		CHECK(Rules::ParseBuffFields(j) == Rules::IMM_CONTROL);  // 非布尔忽略并 WARN
	}
	{
		const json j = json::parse(R"({"id":"x","immune_kill":true,"unknown_field":123})");
		CHECK(Rules::ParseBuffFields(j) == Rules::IMM_KILL);  // 未知字段不影响
	}

	// ---------- 5. 两级 OR ----------
	Section("5. two-level OR");
	CHECK(Rules::CombineMask(Rules::IMM_KILL, Rules::IMM_CONTROL) ==
	      (Rules::IMM_KILL | Rules::IMM_CONTROL));
	CHECK(Rules::CombineMask(Rules::IMM_KILL, 0) == Rules::IMM_KILL);
	CHECK(Rules::CombineMask(0, Rules::IMM_CAPTURE) == Rules::IMM_CAPTURE);
	CHECK(Rules::CombineMask(0, 0) == 0);
	CHECK(Rules::CombineMask(0x7, 0x5) == 0x7);

	// ---------- 6. target 解析（swap 决定取第 2 / 第 3 参） ----------
	Section("6. target resolution");
	{
		int performer = 0, targetArg = 0;
		Rules::EffectMirror eff{};
		eff.swap = false;
		CHECK(Rules::ResolveTarget(&performer, &targetArg,
		          reinterpret_cast<const std::byte*>(&eff)) == &targetArg);
		eff.swap = true;
		CHECK(Rules::ResolveTarget(&performer, &targetArg,
		          reinterpret_cast<const std::byte*>(&eff)) == &performer);
	}

	// ---------- 7. 副本清零（核心） ----------
	Section("7. copy clearing");
	{
		// KILL：kill=0 且 kill_types.last=first，end 不变；其他字段全部原值；原 buffer 零改动。
		const auto src = MakeSentinel();
		const auto before = src;
		alignas(8) std::byte copy[Rules::kEffectSize];
		std::memcpy(copy, &src, Rules::kEffectSize);
		Rules::ApplyImmunity(copy, Rules::IMM_KILL);
		const auto* out = reinterpret_cast<const Rules::EffectMirror*>(copy);
		CHECK(out->kill == 0);
		CHECK(out->killLast == out->killFirst);
		CHECK(out->killEnd == src.killEnd);
		CHECK(out->control == src.control);
		CHECK(out->uncontrol == src.uncontrol);
		CHECK(out->capture == src.capture);
		CHECK(out->captureRemoveFromParty == src.captureRemoveFromParty);
		CHECK(std::memcmp(&src, &before, Rules::kEffectSize) == 0);  // 游戏原 Effect 零改动
	}
	{
		// CONTROL：只有 +0x70 变 0；+0x74 .uncontrol 必须保持哨兵值（硬要求）。
		const auto src = MakeSentinel();
		const auto before = src;
		alignas(8) std::byte copy[Rules::kEffectSize];
		std::memcpy(copy, &src, Rules::kEffectSize);
		Rules::ApplyImmunity(copy, Rules::IMM_CONTROL);
		const auto* out = reinterpret_cast<const Rules::EffectMirror*>(copy);
		CHECK(out->control == 0);
		CHECK(out->uncontrol == src.uncontrol);  // 硬断言：绝不动 .uncontrol
		CHECK(out->kill == src.kill);
		CHECK(out->killFirst == src.killFirst);
		CHECK(out->killLast == src.killLast);
		CHECK(out->killEnd == src.killEnd);
		CHECK(out->capture == src.capture);
		CHECK(out->captureRemoveFromParty == src.captureRemoveFromParty);
		CHECK(std::memcmp(&src, &before, Rules::kEffectSize) == 0);
	}
	{
		// CAPTURE：+0x320 与 +0x324 都清零，其他原值。
		const auto src = MakeSentinel();
		const auto before = src;
		alignas(8) std::byte copy[Rules::kEffectSize];
		std::memcpy(copy, &src, Rules::kEffectSize);
		Rules::ApplyImmunity(copy, Rules::IMM_CAPTURE);
		const auto* out = reinterpret_cast<const Rules::EffectMirror*>(copy);
		CHECK(out->capture == 0);
		CHECK(out->captureRemoveFromParty == 0);
		CHECK(out->control == src.control);
		CHECK(out->uncontrol == src.uncontrol);
		CHECK(out->kill == src.kill);
		CHECK(out->killLast == src.killLast);
		CHECK(out->killEnd == src.killEnd);
		CHECK(std::memcmp(&src, &before, Rules::kEffectSize) == 0);
	}
	{
		// 组合 mask：KILL|CONTROL|CAPTURE 三个模块都生效，但 .uncontrol 仍原值。
		const auto src = MakeSentinel();
		alignas(8) std::byte copy[Rules::kEffectSize];
		std::memcpy(copy, &src, Rules::kEffectSize);
		Rules::ApplyImmunity(copy, 0x7);
		const auto* out = reinterpret_cast<const Rules::EffectMirror*>(copy);
		CHECK(out->kill == 0);
		CHECK(out->control == 0);
		CHECK(out->capture == 0);
		CHECK(out->captureRemoveFromParty == 0);
		CHECK(out->uncontrol == src.uncontrol);
	}
	{
		// mask==0：不复制、不清零——直接转发原指针，ApplyImmunity 不改任何字节。
		const auto src = MakeSentinel();
		const auto before = src;
		alignas(8) std::byte copy[Rules::kEffectSize];
		std::memcpy(copy, &src, Rules::kEffectSize);
		Rules::ApplyImmunity(copy, 0);
		CHECK(std::memcmp(copy, &before, Rules::kEffectSize) == 0);
	}

	// ---------- 8. 可选规则门（rule-gated 前置判定） ----------
	Section("8. rule gate (optional mode)");
	CHECK(Rules::RuleGatePrecheck(0, true, false));      // always → active
	CHECK(!Rules::RuleGatePrecheck(55, true, true));     // 解析失败 → skip
	CHECK(!Rules::RuleGatePrecheck(0, false, true));     // 非主线程 → skip
	CHECK(!Rules::RuleGatePrecheck(42, true, false));    // 需 Raid 且无 Raid → skip
	CHECK(Rules::RuleGatePrecheck(42, true, true));      // 需 Raid 且有 Raid → proceed
	CHECK(Rules::RuleGatePrecheck(4, true, false));      // hpbelow 不需要 Raid → proceed

	// ---------- 9. 转发决策：异常时只转发一次（应修 1） ----------
	Section("9. forward decision (single call)");
	{
		int original = 0;
		alignas(8) std::byte copy[Rules::kEffectSize]{};
		const void* orig = &original;
		const void* cp = copy;

		auto d = Rules::DecideForwardEffect(orig, cp, 0, false);
		CHECK(d.effect == orig && d.mask == 0 && !d.usedCopy && !d.failed);

		d = Rules::DecideForwardEffect(orig, cp, Rules::IMM_KILL, false);
		CHECK(d.effect == cp && d.mask == Rules::IMM_KILL && d.usedCopy && !d.failed);

		// 插件逻辑异常 → 强制回退原指针（fail-open）
		d = Rules::DecideForwardEffect(orig, cp, Rules::IMM_KILL, true);
		CHECK(d.effect == orig && d.mask == 0 && !d.usedCopy && d.failed);

		// 异常且无副本 → 仍是原指针
		d = Rules::DecideForwardEffect(orig, nullptr, Rules::IMM_CAPTURE, true);
		CHECK(d.effect == orig && d.mask == 0 && d.failed);

		// 原指针本身为空 + 异常 → 不凭空造指针
		d = Rules::DecideForwardEffect(nullptr, cp, 0, true);
		CHECK(d.effect == nullptr && d.failed);
	}
	{
		// 调用计数语义：任何路径（含游戏函数自身抛异常）转发恰好一次。
		struct ForwardProbe
		{
			int calls = 0;
			const void* last = nullptr;
			void forward(const void* e, bool a_throw)
			{
				++calls;
				last = e;
				if (a_throw) {
					throw 42;  // 模拟 g_original 自身抛异常：不应被插件 SEH 兜底重试
				}
			}
		};
		int original = 0;
		alignas(8) std::byte copy[Rules::kEffectSize]{};
		const void* orig = &original;
		const void* cp = copy;

		{
			ForwardProbe p;
			const auto d = Rules::DecideForwardEffect(orig, cp, Rules::IMM_CONTROL, false);
			p.forward(d.effect, false);
			CHECK(p.calls == 1 && p.last == cp);
		}
		{
			ForwardProbe p;
			const auto d = Rules::DecideForwardEffect(orig, cp, 0, false);
			p.forward(d.effect, false);
			CHECK(p.calls == 1 && p.last == orig);
		}
		{
			ForwardProbe p;
			const auto d = Rules::DecideForwardEffect(orig, cp, Rules::IMM_KILL, true);
			p.forward(d.effect, false);
			CHECK(p.calls == 1 && p.last == orig);  // 插件异常路径只转发原指针一次
		}
		{
			ForwardProbe p;
			const auto d = Rules::DecideForwardEffect(orig, cp, Rules::IMM_KILL, false);
			bool caught = false;
			try {
				p.forward(d.effect, true);
			} catch (int) {
				caught = true;
			}
			CHECK(caught && p.calls == 1);  // 游戏自身异常向外传播，且只调用一次
		}
	}

	// ---------- 10. class 级双 id 合并（+8 与 +0x48 同时命中，应修 2） ----------
	Section("10. class mask from two ids (+8 / +0x48)");
	{
		std::unordered_map<std::int32_t, std::uint8_t> table;
		table[Rules::Hash("hero_class_name_alpha")] = Rules::IMM_KILL;
		table[Rules::Hash("str_monstername_beta")] = Rules::IMM_CAPTURE;
		table[Rules::Hash("gamma")] = Rules::IMM_CONTROL;  // 裸名变体

		const auto lookup = [](std::int32_t h, const void* ctx) noexcept -> std::uint8_t {
			const auto* t = static_cast<const std::unordered_map<std::int32_t, std::uint8_t>*>(ctx);
			const auto it = t->find(h);
			return it == t->end() ? 0 : it->second;
		};
		const Rules::ClassMaskLookup fn = lookup;

		CHECK(Rules::ClassMaskFromIds({ "alpha", "beta" }, fn, &table) ==
		      (Rules::IMM_KILL | Rules::IMM_CAPTURE));  // 两串同时命中 → OR
		CHECK(Rules::ClassMaskFromIds({ "alpha", "" }, fn, &table) == Rules::IMM_KILL);     // 仅 +8
		CHECK(Rules::ClassMaskFromIds({ "", "beta" }, fn, &table) == Rules::IMM_CAPTURE);   // 仅 +0x48
		CHECK(Rules::ClassMaskFromIds({ "gamma" }, fn, &table) == Rules::IMM_CONTROL);      // 裸名变体
		CHECK(Rules::ClassMaskFromIds({ "delta", "epsilon" }, fn, &table) == 0);            // 都不命中
		CHECK(Rules::ClassMaskFromIds({}, fn, &table) == 0);                                // 空列表
	}

	// ---------- 11. 内嵌 class id 读取（实机 bug 回归：不可二次解引用）----------
	// 反编译 4D0140/4DCB90：ActorClass+8 / MonsterClass+0x48 是内嵌 char[0x40] 字符串本身。
	// 旧实现 *(const char**)(cls+off) 会读 "crusader" 的 ASCII 当指针 → 崩 → 免疫全失效。
	Section("11. ReadEmbeddedClassId (inline char array, real bug regression)");
	{
		// 模拟一个 0x100 字节的 class 对象，在 +8 与 +0x48 处放内嵌字符串。
		std::byte fakeClass[0x100];
		std::memset(fakeClass, 0, sizeof(fakeClass));
		{
			const char id8[] = "crusader";
			std::memcpy(fakeClass + 8, id8, sizeof(id8));       // 含 NUL
		}
		{
			const char id48[] = "monster_boss";
			std::memcpy(fakeClass + 0x48, id48, sizeof(id48));
		}

		char out8[0x100];
		char out48[0x100];
		CHECK(Rules::ReadEmbeddedClassId(fakeClass, 8, out8, sizeof(out8)));
		CHECK(std::string(out8) == "crusader");                 // 核心：内嵌数组被直接读出
		CHECK(Rules::ReadEmbeddedClassId(fakeClass, 0x48, out48, sizeof(out48)));
		CHECK(std::string(out48) == "monster_boss");

		// 空内嵌数组（首字节即 NUL）→ false（fail-open）
		std::byte emptyCls[0x100];
		std::memset(emptyCls, 0, sizeof(emptyCls));
		CHECK(!Rules::ReadEmbeddedClassId(emptyCls, 8, out8, sizeof(out8)));

		// 无 NUL 的满缓冲：只读 cap-1 字节，保证 NUL 结尾、不越界。
		std::byte fullCls[0x100];
		std::memset(fullCls, 'a', sizeof(fullCls));
		CHECK(Rules::ReadEmbeddedClassId(fullCls, 8, out8, 16));
		CHECK(std::strlen(out8) == 15);                         // cap-1

		// 端到端：用假 class 的 id 串走哈希查询，断言十字军 mask 命中。
		// 注意 out8 已被上面的"满缓冲"用例改写，这里从干净的 fakeClass 重读。
		CHECK(Rules::ReadEmbeddedClassId(fakeClass, 8, out8, sizeof(out8)));
		CHECK(std::string(out8) == "crusader");
		std::unordered_map<std::int32_t, std::uint8_t> table;		table[Rules::Hash("hero_class_name_crusader")] = Rules::IMM_KILL | Rules::IMM_CONTROL | Rules::IMM_CAPTURE;
		const auto lookup = [](std::int32_t h, const void* ctx) noexcept -> std::uint8_t {
			const auto* t = static_cast<const std::unordered_map<std::int32_t, std::uint8_t>*>(ctx);
			const auto it = t->find(h);
			return it == t->end() ? 0 : it->second;
		};
		CHECK(Rules::ClassMaskFromIds({ out8 }, lookup, &table) == 0x7);
	}

	std::printf("\n==== RESULT: %d PASS, %d FAIL ====\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
