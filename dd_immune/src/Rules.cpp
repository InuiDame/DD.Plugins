#include "Rules.h"

namespace Rules
{
	namespace fs = std::filesystem;
	using json = nlohmann::json;

	namespace
	{
		// buff 组布局（与 CustomTypeEffects/src/Hooks.cpp:15-18、brief.md:49 一致）
		constexpr std::ptrdiff_t ACTOR_BUFFS_OFFSET = 0x460;
		constexpr std::ptrdiff_t BUFF_ENTRY_SIZE    = 0x418;
		constexpr std::ptrdiff_t BUFF_IN_ENTRY      = 0x8;
		constexpr std::ptrdiff_t BUFF_ID_HASH       = 0xA4;
		constexpr std::size_t    MAX_BUFFS          = 128;

		struct ClassEntry { std::uint8_t mask; };
		struct BuffEntryRule
		{
			std::uint8_t mask{ 0 };
			bool         requireActive{ false };  // v1 保留字段，不接入 detour（见 README）
		};

		// 键统一用 int32（Hash 的返回类型），三哈希变体都落进这张表。
		std::unordered_map<std::int32_t, ClassEntry> ClassRules;
		std::unordered_map<std::int32_t, BuffEntryRule> BuffRules;
		bool RequireActiveSeen = false;

		std::string ToUtf8(const fs::path& a_path)
		{
			const auto u8 = a_path.u8string();
			return { reinterpret_cast<const char*>(u8.data()), u8.size() };
		}

		std::string Lowercase(std::string a_text)
		{
			for (auto& c : a_text) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return a_text;
		}

		fs::path GetExecutablePath()
		{
			std::wstring buf(MAX_PATH, L'\0');
			while (true) {
				const auto len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
				if (len < buf.size()) {
					buf.resize(len);
					return buf;
				}
				buf.resize(buf.size() * 2);
			}
		}

		fs::path GetGameRoot()
		{
			// <gameRoot>\_windows\win64\Darkest.exe -> 上溯三级
			return GetExecutablePath().parent_path().parent_path().parent_path();
		}

		// 低优先级 -> 高优先级；同 id 时后者覆盖前者。
		std::vector<fs::path> CollectRoots(const fs::path& a_gameRoot)
		{
			const auto exe = GetExecutablePath();

			std::vector<fs::path> roots;
			roots.emplace_back(a_gameRoot / "shared" / "buffs");
			roots.emplace_back(a_gameRoot / "trinkets");
			roots.emplace_back(a_gameRoot / "dlc");

			fs::path libRoot;
			bool     isSteam = false;
			for (const auto& part : exe) {
				if (::_wcsicmp(part.c_str(), L"steamapps") == 0) {
					isSteam = true;
					break;
				}
				libRoot /= part;
			}

			if (isSteam) {
				roots.emplace_back(libRoot / "steamapps" / "workshop" / "content" / "262060");
			} else {
				WARN("Could not find 'steamapps' in game path, workshop mods are skipped: {}", ToUtf8(exe));
			}

			roots.emplace_back(a_gameRoot / "mods");
			return roots;
		}

		// ---- info 注释解析辅助 ----

		std::size_t FindIgnoreCase(std::string_view a_text, std::string_view a_lowerTag) noexcept
		{
			if (a_lowerTag.empty() || a_text.size() < a_lowerTag.size()) {
				return std::string_view::npos;
			}
			for (std::size_t i = 0; i + a_lowerTag.size() <= a_text.size(); ++i) {
				bool match = true;
				for (std::size_t j = 0; j < a_lowerTag.size(); ++j) {
					char c = a_text[i + j];
					if (c >= 'A' && c <= 'Z') {
						c = static_cast<char>(c - 'A' + 'a');
					}
					if (c != a_lowerTag[j]) {
						match = false;
						break;
					}
				}
				if (match) {
					return i;
				}
			}
			return std::string_view::npos;
		}

		// info token → 模块位：".kill"/"kill" 等（已小写）。0 表示未知。
		std::uint8_t BitForModule(std::string_view a_lowerToken) noexcept
		{
			if (a_lowerToken == ".kill" || a_lowerToken == "kill") {
				return IMM_KILL;
			}
			if (a_lowerToken == ".control" || a_lowerToken == "control") {
				return IMM_CONTROL;
			}
			if (a_lowerToken == ".capture" || a_lowerToken == "capture") {
				return IMM_CAPTURE;
			}
			return 0;
		}

		// ---- buff JSON 扫描 ----

		void ParseBuffFile(const fs::path& a_file)
		{
			std::ifstream in(a_file, std::ios::binary);
			if (!in.is_open()) {
				return;
			}
			const std::string content{ std::istreambuf_iterator<char>(in), {} };
			if (content.find("immune_") == std::string::npos) {
				return;
			}

			const json doc = json::parse(content, nullptr, false, true);  // 允许注释
			if (doc.is_discarded()) {
				WARN("JSON parse failed in {}", ToUtf8(a_file));
				return;
			}

			const json* list = nullptr;
			if (doc.is_array()) {
				list = std::addressof(doc);
			} else if (doc.is_object()) {
				const auto it = doc.find("buffs");
				if (it != doc.end() && it->is_array()) {
					list = std::addressof(*it);
				}
			}
			if (!list) {
				return;
			}

			for (const auto& entry : *list) {
				if (!entry.is_object() || !HasImmuneFields(entry)) {
					continue;
				}
				const auto idIt = entry.find("id");
				if (idIt == entry.end() || !idIt->is_string()) {
					continue;
				}

				const auto hash = Hash(idIt->get<std::string>());
				const auto mask = ParseBuffFields(entry);

				bool requireActive = false;
				const auto reqIt = entry.find("immune_require_active");
				if (reqIt != entry.end() && reqIt->is_boolean() && reqIt->get<bool>()) {
					requireActive = true;
					RequireActiveSeen = true;
				}

				// 高优先级文件覆盖低优先级（显式 false 也会被登记，从而覆盖掉低优先级的 true）。
				BuffRules[hash] = BuffEntryRule{ mask, requireActive };
				INFO("[BUFF-RULE] '{}' -> mask 0x{:X}{} [{}]",
					idIt->get<std::string>(), static_cast<unsigned>(mask),
					requireActive ? " (require-active: v1 未强制)" : "", ToUtf8(a_file));
			}
		}

		bool IsInfoClassPath(const fs::path& a_file) noexcept
		{
			bool inClassDir = false;
			for (const auto& part : a_file) {
				std::wstring w{ part.c_str() };
				for (auto& c : w) {
					if (c >= L'A' && c <= L'Z') {
						c = static_cast<wchar_t>(c - L'A' + L'a');
					}
				}
				if (w == L"heroes" || w == L"monsters") {
					inClassDir = true;
					break;
				}
			}
			return inClassDir;
		}

		void ParseInfoFile(const fs::path& a_file)
		{
			std::ifstream in(a_file, std::ios::binary);
			if (!in.is_open()) {
				return;
			}
			const std::string content{ std::istreambuf_iterator<char>(in), {} };
			const auto        mask = ParseInfoText(content);
			if (mask == 0) {
				return;
			}

			// 职业名取最深层目录名（与文件名相同）。
			const auto cls = ToUtf8(a_file.parent_path().filename());
			if (cls.empty()) {
				return;
			}

			const std::int32_t hashes[3]{
				Hash(cls),
				Hash("hero_class_name_" + cls),
				Hash("str_monstername_" + cls),
			};
			for (const auto h : hashes) {
				ClassRules[h] = ClassEntry{ mask };  // 后扫描（高优先级）覆盖先扫描
			}
			INFO("[INFO-RULE] class '{}' -> mask 0x{:X} [{}]",
				cls, static_cast<unsigned>(mask), ToUtf8(a_file));
		}

		template <class TFn>
		void ScanRecursive(const fs::path& a_dir, TFn a_accept)
		{
			std::error_code ec;
			if (!fs::is_directory(a_dir, ec)) {
				return;
			}
			try {
				for (const auto& e : fs::recursive_directory_iterator(
						 a_dir, fs::directory_options::skip_permission_denied)) {
					if (!e.is_regular_file(ec)) {
						continue;
					}
					a_accept(e.path());
				}
		} catch (const std::exception& ex) {
			(void)ex;  // 单测 PCH 下 WARN 为 no-op，避免未使用告警
			WARN("Error while scanning {}: {}", ToUtf8(a_dir), ex.what());
		}
		}

		void ScanBuffs(const fs::path& a_dir)
		{
			INFO("Scanning buffs: {}", ToUtf8(a_dir));
			ScanRecursive(a_dir, [](const fs::path& p) {
				const auto name = Lowercase(ToUtf8(p.filename()));
				if (name.ends_with(".buffs.json")) {
					ParseBuffFile(p);
				}
			});
		}

		void ScanInfo(const fs::path& a_dir)
		{
			INFO("Scanning info: {}", ToUtf8(a_dir));
			ScanRecursive(a_dir, [](const fs::path& p) {
				const auto name = Lowercase(ToUtf8(p.filename()));
				if (name.ends_with(".info.darkest") && IsInfoClassPath(p)) {
					ParseInfoFile(p);
				}
			});
		}

		// ---- SEH 包裹的原始内存读取：函数内不得出现需要 unwinding 的 C++ 局部对象 ----

		// 同时读 ActorClass+8 与 +0x48 两个 id 串（combat.md:40：MonsterClass+0x1598 非零时
		// 改用 +0x48）。分别用 __try 兜住，任一个读失败只让该串作废，另一串仍可用；
		// 两段合起来全失败才返回 false（fail-open）。函数内不得有需 unwinding 的 C++ 对象。
		bool TryReadClassIds(
			const void* a_actor, char* a_out8, char* a_out48, std::size_t a_cap) noexcept
		{
			a_out8[0] = '\0';
			a_out48[0] = '\0';

			const void* cls = nullptr;
			__try {
				auto* const* vt = *reinterpret_cast<void* const* const*>(a_actor);
				using GetClassFn = const void* (*)(const void*);
				const auto fn = reinterpret_cast<GetClassFn>(vt[6]);  // 虚表槽 6
				cls = fn(a_actor);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
			if (!cls) {
				return false;
			}

			// 关键（实机 2026-11-17 修复）：cls+8 / cls+0x48 是内嵌 char[0x40] 数组本身，
			// 统一走 ReadEmbeddedClassId（直接读字符串，非二次解引用；SEH 包裹）。
			const bool ok8 = ReadEmbeddedClassId(cls, 8, a_out8, a_cap);
			const bool ok48 = ReadEmbeddedClassId(cls, 0x48, a_out48, a_cap);

			return ok8 || ok48;
		}

		// ClassMaskFromIds 的查询适配器：把全局 ClassRules 表暴露成无捕获函数指针。
		std::uint8_t LookupClassRules(std::int32_t a_hash, const void* a_context) noexcept
		{
			(void)a_context;
			const auto it = ClassRules.find(a_hash);
			return it != ClassRules.end() ? it->second.mask : 0;
		}

		std::size_t TryReadBuffHashes(
			const void* a_actor, std::int32_t* a_out, std::size_t a_max) noexcept
		{
			__try {
				struct VectorLayout { std::byte* first; std::byte* last; std::byte* end; };
				const auto& buffs = *reinterpret_cast<const VectorLayout*>(
					static_cast<const std::byte*>(a_actor) + ACTOR_BUFFS_OFFSET);
				if (!buffs.first || !buffs.last || buffs.last <= buffs.first) {
					return 0;
				}
				std::size_t count = 0;
				for (auto* e = buffs.first; e < buffs.last && count < a_max; e += BUFF_ENTRY_SIZE) {
					const auto* buff = e + BUFF_IN_ENTRY;
					a_out[count++] = *reinterpret_cast<const std::int32_t*>(buff + BUFF_ID_HASH);
				}
				return count;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}
	}

	// ===================== 纯逻辑 =====================

	std::int32_t Hash(std::string_view a_text) noexcept
	{
		std::uint32_t hash = 0;
		for (const auto c : a_text) {
			hash = hash * 53 + static_cast<unsigned char>(c);
		}
		return static_cast<std::int32_t>(hash);
	}

	bool ReadEmbeddedClassId(
		const void* a_classObj, std::ptrdiff_t a_offset,
		char* a_out, std::size_t a_cap) noexcept
	{
		a_out[0] = '\0';
		// ActorClass+8 / MonsterClass+0x48 是内嵌的 char[0x40] NUL 结尾字符串，
		// 直接取其首地址逐字节拷贝。绝不写 *(const char**) —— 那是二次解引用。
		// 只读到 a_cap-1 字节；].darkest id 最长 0x40，a_cap=0x100 足够并留余量。
		__try {
			const char* id = reinterpret_cast<const char*>(
				static_cast<const std::byte*>(a_classObj) + a_offset);
			std::size_t i = 0;
			for (; i + 1 < a_cap && id[i] != '\0'; ++i) {
				a_out[i] = id[i];
			}
			a_out[i] = '\0';
			return a_out[0] != '\0';
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			a_out[0] = '\0';
			return false;
		}
	}

	InfoLineEffect ParseInfoLine(std::string_view a_line) noexcept
	{
		InfoLineEffect result;
		std::string_view rest;

		// 1) 真实块：行首（跳过空白）"immune:"，**不需要 //**。与 DDShield 的 shield: 同款。
		const auto first = a_line.find_first_not_of(" \t\r");
		if (first != std::string_view::npos) {
			const auto tail = a_line.substr(first);
			constexpr std::string_view realTag{ "immune:" };
			if (FindIgnoreCase(tail, realTag) == 0) {
				rest = tail.substr(realTag.size());
				result.matched = true;
			}
		}

		// 2) 向后兼容：// 注释里的 dd_immune: / immune:（旧示例不必失效）。
		if (!result.matched) {
			const auto slash = a_line.find("//");
			if (slash != std::string_view::npos) {
				const auto comment = a_line.substr(slash + 2);
				constexpr std::string_view compatTags[]{
					std::string_view{ "dd_immune:" }, std::string_view{ "immune:" }
				};
				for (const auto tag : compatTags) {
					const auto pos = FindIgnoreCase(comment, tag);
					if (pos != std::string_view::npos) {
						rest = comment.substr(pos + tag.size());
						result.matched = true;
						break;
					}
				}
			}
		}
		if (!result.matched) {
			return result;
		}

		std::uint8_t value = 0;      // 本行各模块最终值
		std::uint8_t mentioned = 0;  // 本行提到过的模块
		std::uint8_t pending = 0;    // ".kill" 等待布尔值

		const auto apply = [&](std::uint8_t a_bit, bool a_on) noexcept {
			value = a_on ? static_cast<std::uint8_t>(value | a_bit)
			             : static_cast<std::uint8_t>(value & ~a_bit);
			mentioned = static_cast<std::uint8_t>(mentioned | a_bit);
			pending = 0;
		};
		const auto dropPending = [&]() noexcept {
			if (pending) {
				WARN("immune: module token without true/false value, ignored");
				pending = 0;
			}
		};
		const auto handle = [&](std::string_view a_tok) noexcept {
			if (a_tok.empty()) {
				return;
			}
			const auto low = Lowercase(std::string{ a_tok });
			if (pending) {
				if (low == "true" || low == "1") {
					apply(pending, true);
					return;
				}
				if (low == "false" || low == "0") {
					apply(pending, false);
					return;
				}
				dropPending();  // 非法值：丢弃前面的 .xxx
			}
			const auto bit = BitForModule(low);
			if (bit == 0) {
				WARN("immune: unknown token '{}'", a_tok);
				return;
			}
			if (a_tok.front() == '.') {
				pending = bit;  // .kill/.control/.capture：等下一个 token 给布尔值
			} else {
				apply(bit, true);  // 裸 token kill/control/capture 等价于 true
			}
		};

		std::string token;
		for (const char c : rest) {
			if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
				handle(token);
				token.clear();
			} else {
				token.push_back(c);
			}
		}
		handle(token);
		dropPending();

		result.setMask = static_cast<std::uint8_t>(mentioned & value);
		result.clearMask = static_cast<std::uint8_t>(mentioned & ~value);
		return result;
	}

	std::uint8_t ParseInfoText(std::string_view a_text) noexcept
	{
		std::uint8_t mask = 0;
		std::size_t  start = 0;
		while (start <= a_text.size()) {
			auto end = a_text.find('\n', start);
			if (end == std::string_view::npos) {
				end = a_text.size();
			}
			const auto effect = ParseInfoLine(a_text.substr(start, end - start));
			if (effect.matched) {
				// 按文件顺序：先 OR 置位、再清除显式 false/0（后者覆盖此前置位）。
				mask = static_cast<std::uint8_t>((mask | effect.setMask) & ~effect.clearMask);
			}
			if (end == a_text.size()) {
				break;
			}
			start = end + 1;
		}
		return mask;
	}

	std::uint8_t ParseBuffFields(const json& a_entry) noexcept
	{
		std::uint8_t mask = 0;
		if (!a_entry.is_object()) {
			return 0;
		}
		struct Field { const char* key; std::uint8_t bit; };
		constexpr Field fields[]{
			{ "immune_kill",     IMM_KILL },
			{ "immune_control",  IMM_CONTROL },
			{ "immune_capture",  IMM_CAPTURE },
		};
		for (const auto& f : fields) {
			const auto it = a_entry.find(f.key);
			if (it == a_entry.end()) {
				continue;
			}
			if (it->is_boolean()) {
				if (it->get<bool>()) {
					mask = static_cast<std::uint8_t>(mask | f.bit);
				}
			} else {
				WARN("immune field '{}' must be a boolean, ignored", f.key);
			}
		}
		return mask;
	}

	bool HasImmuneFields(const json& a_entry) noexcept
	{
		if (!a_entry.is_object()) {
			return false;
		}
		return a_entry.contains("immune_kill") ||
		       a_entry.contains("immune_control") ||
		       a_entry.contains("immune_capture");
	}

	const void* ResolveTarget(
		const void* a_performer, const void* a_targetArg, const std::byte* a_effect) noexcept
	{
		const bool swap = *reinterpret_cast<const bool*>(a_effect + OFF_SWAP);
		return swap ? a_performer : a_targetArg;
	}

	void ApplyImmunity(std::byte* a_copy, std::uint8_t a_mask) noexcept
	{
		if (a_mask & IMM_KILL) {
			*reinterpret_cast<std::int32_t*>(a_copy + OFF_KILL) = 0;
			// kill_enemy_types：last 退回 first → 消费者 first!=last 为假，整段跳过。
			// end(+0x208) 不动；不改游戏原 Effect 内存。
			*reinterpret_cast<void**>(a_copy + OFF_KILL_TYPES_LAST) =
				*reinterpret_cast<void**>(a_copy + OFF_KILL_TYPES);
		}
		if (a_mask & IMM_CONTROL) {
			*reinterpret_cast<std::int32_t*>(a_copy + OFF_CONTROL) = 0;
			// +0x074 .uncontrol 永不写（事前阻断语义）。
		}
		if (a_mask & IMM_CAPTURE) {
			*reinterpret_cast<std::int32_t*>(a_copy + OFF_CAPTURE) = 0;
			*reinterpret_cast<std::int32_t*>(a_copy + OFF_CAPTURE_RMP) = 0;
		}
	}

	ForwardDecision DecideForwardEffect(
		const void* a_originalEffect, const void* a_copy,
		std::uint8_t a_mask, bool a_failed) noexcept
	{
		if (a_failed) {
			// 插件逻辑异常 → fail-open：强制原指针原样转发，mask 视为 0。
			return ForwardDecision{ a_originalEffect, 0, false, true };
		}
		if (a_mask == 0 || !a_copy) {
			return ForwardDecision{ a_originalEffect, 0, false, false };
		}
		return ForwardDecision{ a_copy, a_mask, true, false };
	}

	std::uint8_t ClassMaskFromIds(
		std::initializer_list<std::string_view> a_ids,
		ClassMaskLookup a_lookup, const void* a_context) noexcept
	{
		std::uint8_t mask = 0;
		for (const auto id : a_ids) {
			if (id.empty()) {
				continue;
			}
			for (const auto hashed : {
				     Hash(id),
				     Hash("hero_class_name_" + std::string{ id }),
				     Hash("str_monstername_" + std::string{ id }) }) {
				mask = static_cast<std::uint8_t>(mask | a_lookup(hashed, a_context));
			}
		}
		return mask;
	}

	bool RuleGatePrecheck(
		std::int32_t a_ruleType, bool a_isMainThread, bool a_raidPresent) noexcept
	{
		if (!a_isMainThread) {
			return false;
		}
		if (a_ruleType == 0) {   // always
			return true;
		}
		if (a_ruleType == 55) {  // 解析失败
			return false;
		}
		// 需要 Raid / 区域存在的类型（combat.md:232）
		constexpr std::array<std::int32_t, 10> needRaid{
			2, 3, 16, 18, 21, 22, 27, 35, 41, 42
		};
		for (const auto t : needRaid) {
			if (a_ruleType == t) {
				return a_raidPresent;
			}
		}
		return true;
	}

	// ===================== 装载与运行时查询 =====================

	void Load()
	{
		const auto gameRoot = GetGameRoot();

		for (const auto& root : CollectRoots(gameRoot)) {
			ScanBuffs(root);
		}

		// info：原版 heroes/ monsters/ 直接位于 gameRoot 下；mod / workshop 覆盖各自根目录递归。
		ScanInfo(gameRoot / "heroes");
		ScanInfo(gameRoot / "monsters");
		for (const auto& root : CollectRoots(gameRoot)) {
			ScanInfo(root);
		}

		INFO("dd_immune loaded: {} buff rule(s), {} class hash(es)", BuffRules.size(), ClassRules.size());
		if (RequireActiveSeen) {
			INFO("immune_require_active seen, but rule-gated mode is NOT enforced in v1 (see README)");
		}
		if (Empty()) {
			WARN("No immune rules found. Add immune_kill/control/capture to a *.buffs.json "
			     "or an 'immune: .kill true ...' block to a heroes/monsters *.info.darkest file.");
		}
	}

	bool Empty() noexcept
	{
		return ClassRules.empty() && BuffRules.empty();
	}

	std::size_t BuffCount() noexcept { return BuffRules.size(); }
	std::size_t ClassHashCount() noexcept { return ClassRules.size(); }

	// 诊断日志隔离到 noinline 函数：ClassMask 在 noexcept 热路径，不能在 __try 构造 fmt 对象。
	// 节流：成功读到 id 的前 8 次、失败的前 8 次各打一条，验收时可直接确认 class 级免疫是否命中。
	__declspec(noinline) void DiagClassId(bool a_ok, const char* a_id8, const char* a_id48,
	                                       std::uint8_t a_mask) noexcept
	{
		static std::atomic<std::uint32_t> okCount{ 0 };
		static std::atomic<std::uint32_t> failCount{ 0 };
		if (a_ok) {
			const auto n = okCount.fetch_add(1, std::memory_order_relaxed);
			if (n < 8) {
				INFO("[DIAG] class id ok: +8='{}' +0x48='{}' -> mask=0x{:X}",
				     a_id8, a_id48, static_cast<unsigned>(a_mask));
			}
		} else {
			const auto n = failCount.fetch_add(1, std::memory_order_relaxed);
			if (n < 8) {
				WARN("[DIAG] class id read failed (actor ptr may be non-Actor)");
			}
		}
	}

	std::uint8_t ClassMask(const void* a_actor) noexcept
	{
		if (!a_actor || ClassRules.empty()) {
			return 0;
		}
		char id8[0x100];
		char id48[0x100];
		if (!TryReadClassIds(a_actor, id8, id48, sizeof(id8))) {
			DiagClassId(false, "", "", 0);
			return 0;
		}
		// +8 与 +0x48 两个串各求三哈希；任一命中即计入（OR），避免依赖虚表槽 4 判断 Hero/Monster。
		const auto mask = ClassMaskFromIds({ id8, id48 }, &LookupClassRules, nullptr);
		DiagClassId(true, id8, id48, mask);
		return mask;
	}

	std::uint8_t BuffMask(const void* a_actor) noexcept
	{
		if (!a_actor || BuffRules.empty()) {
			return 0;
		}
		std::int32_t hashes[MAX_BUFFS];
		const auto   count = TryReadBuffHashes(a_actor, hashes, MAX_BUFFS);
		std::uint8_t mask = 0;
		for (std::size_t i = 0; i < count; ++i) {
			const auto it = BuffRules.find(hashes[i]);
			if (it != BuffRules.end()) {
				mask = static_cast<std::uint8_t>(mask | it->second.mask);
			}
		}
		// v1 默认不调用 479CD0：以"条目存在"为 active。
		return mask;
	}

	std::uint8_t Mask(const void* a_actor) noexcept
	{
		return CombineMask(ClassMask(a_actor), BuffMask(a_actor));
	}
}
