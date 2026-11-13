#include "Rules.h"

namespace Rules
{
	namespace fs = std::filesystem;
	using json = nlohmann::json;

	namespace
	{
		enum class Kind
		{
			Buff,
			Trinket
		};

		// 只识别带 "_type_" 标记的字段，天然避开原版 melee/ranged/attack/move 等字段名
		constexpr std::array<std::string_view, 2> FieldSuffixes{
			"_type_additional_effect_ids",  // buff 风格，用户要求的写法
			"_type_additional_effects"      // trinket 风格
		};

		constexpr std::array<std::string_view, 4> DefaultBlockedTypes{
			"teleport", "melee", "ranged", "move"
		};

		// 与 DeathBlowCap.cfg / ProtectionCap.cfg 等 MOD 一致，配置统一放在 DDModLoader\cfg
		constexpr std::string_view CfgFileName{ "CustomTypeEffects.cfg" };
		constexpr std::string_view DefaultCfg{
			"# CustomTypeEffects config\n"
			"# Types that must NEVER trigger custom <type>_type_additional_effect_ids.\n"
			"# Comma or whitespace separated; repeated blocked_types lines are merged.\n"
			"# Remove this key (or delete the file) to use the built-in defaults.\n"
			"# teleport      : skill teleports the hero out of combat\n"
			"# melee/ranged : already handled by vanilla triggers, avoid double firing\n"
			"# move         : vanilla movement skill\n"
			"blocked_types = teleport, melee, ranged, move\n"
		};

		struct OwnerRules
		{
			// 透明比较器，运行时可以用 string_view 查询而不分配内存
			std::map<std::string, EffectIds, std::less<>> byType;
		};

		using RuleMap = std::unordered_map<std::int32_t, OwnerRules>;

		RuleMap                             BuffRules;
		RuleMap                             TrinketRules;
		std::set<std::string, std::less<>>  KnownTypes;
		std::set<std::string, std::less<>>  BlockedTypes;

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

		// 低优先级 -> 高优先级；同 (id, type) 时后者覆盖前者
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

		// 与 DeathBlowCap / ProtectionCap 等 MOD 一致：# 或 // 注释，key = value
		std::string_view Trim(std::string_view a_text) noexcept
		{
			constexpr std::string_view whitespace{ " \t\r\n" };
			const auto begin = a_text.find_first_not_of(whitespace);
			if (begin == std::string_view::npos) {
				return {};
			}
			const auto end = a_text.find_last_not_of(whitespace);
			return a_text.substr(begin, end - begin + 1);
		}

		fs::path CfgPath(const fs::path& a_gameRoot)
		{
			return a_gameRoot / "DDModLoader" / "cfg" / std::string{ CfgFileName };
		}

		// 配置文件不存在时自动生成一份带注释的默认配置（和其它 MOD 一样）
		void WriteDefaultCfg(const fs::path& a_file)
		{
			std::error_code ec;
			fs::create_directories(a_file.parent_path(), ec);
			if (ec) {
				WARN("Cannot create config dir {}: {}", ToUtf8(a_file.parent_path()), ec.message());
				return;
			}

			std::ofstream out(a_file, std::ios::binary);
			if (!out.is_open()) {
				WARN("Cannot write default cfg: {}", ToUtf8(a_file));
				return;
			}
			out.write(DefaultCfg.data(), static_cast<std::streamsize>(DefaultCfg.size()));
			INFO("Default cfg generated: {}", ToUtf8(a_file));
		}

		void LoadConfig(const fs::path& a_gameRoot)
		{
			for (const auto type : DefaultBlockedTypes) {
				BlockedTypes.emplace(std::string{ type });
			}

			const auto file = CfgPath(a_gameRoot);
			std::error_code ec;
			if (!fs::is_regular_file(file, ec)) {
				WriteDefaultCfg(file);
				return;
			}

			std::ifstream in(file, std::ios::binary);
			if (!in.is_open()) {
				WARN("Cannot open cfg {}, using built-in defaults", ToUtf8(file));
				return;
			}

			bool        overridden = false;
			std::string line;
			std::size_t lineNo = 0;
			while (std::getline(in, line)) {
				++lineNo;
				if (const auto hash = line.find('#'); hash != std::string::npos) {
					line.erase(hash);
				}
				if (const auto slash = line.find("//"); slash != std::string::npos) {
					line.erase(slash);
				}

				const auto content = Trim(line);
				if (content.empty()) {
					continue;
				}

				const auto eq = content.find('=');
				if (eq == std::string_view::npos) {
					WARN("cfg line {} has no '=': {}", lineNo, content);
					continue;
				}

				const auto key = Lowercase(std::string{ Trim(content.substr(0, eq)) });
				const auto value = Trim(content.substr(eq + 1));
				if (key != "blocked_types") {
					WARN("cfg line {} unknown key: {}", lineNo, key);
					continue;
				}

				// 出现该键即覆盖内置默认值；多行 / 逗号 / 空白分隔的值会合并
				if (!overridden) {
					BlockedTypes.clear();
					overridden = true;
				}

				std::string token;
				const auto  addToken = [&]() {
					auto type = NormalizeType(token);
					if (!type.empty()) {
						BlockedTypes.insert(std::move(type));
					}
					token.clear();
				};
				for (const char c : value) {
					if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
						addToken();
					} else {
						token.push_back(c);
					}
				}
				addToken();
			}

			INFO("cfg loaded: {} ({} blocked type(s){})", ToUtf8(file), BlockedTypes.size(),
				overridden ? ", overridden" : ", built-in defaults");
			for (const auto& type : BlockedTypes) {
				INFO("  blocked .type \"{}\"", type);
			}
		}

		std::optional<std::string> ExtractType(const std::string& a_key)
		{
			for (const auto suffix : FieldSuffixes) {
				if (a_key.size() > suffix.size() && a_key.ends_with(suffix)) {
					return a_key.substr(0, a_key.size() - suffix.size());
				}
			}
			return std::nullopt;
		}

		void Register(Kind a_kind, const std::string& a_id, const std::string& a_type,
			const json& a_value, const fs::path& a_file)
		{
			EffectIds ids;
			if (a_value.is_string()) {
				ids.emplace_back(a_value.get<std::string>());
			} else if (a_value.is_array()) {
				for (const auto& value : a_value) {
					if (value.is_string()) {
						ids.emplace_back(value.get<std::string>());
					} else {
						WARN("Non-string effect id in '{}' of '{}' ({})",
							a_type, a_id, ToUtf8(a_file));
					}
				}
			} else {
				WARN("Field for .type '{}' of '{}' must be a string or an array ({})",
					a_type, a_id, ToUtf8(a_file));
				return;
			}

			// 同一个字段里重复写的效果名只触发一次
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			if (ids.empty()) {
				return;
			}

			auto& map = a_kind == Kind::Buff ? BuffRules : TrinketRules;
			auto& byType = map[Hash(a_id)].byType;

			KnownTypes.insert(a_type);
			INFO("[RULE] {} '{}' .type \"{}\" -> {} effect(s)  [{}]",
				a_kind == Kind::Buff ? "buff   " : "trinket", a_id, a_type, ids.size(), ToUtf8(a_file));
			for (const auto& id : ids) {
				INFO("        effect \"{}\"", id);
			}

			byType[a_type] = std::move(ids);
		}

		void ParseFile(const fs::path& a_file, Kind a_kind)
		{
			std::ifstream in(a_file, std::ios::binary);
			if (!in.is_open()) {
				return;
			}

			const std::string content{ std::istreambuf_iterator<char>(in), {} };
			if (content.find("_type_additional_") == std::string::npos) {
				return;
			}

			// 游戏自己的 JSON 允许注释、不允许尾随逗号
			const auto doc = json::parse(content, nullptr, false, true);
			if (doc.is_discarded()) {
				WARN("JSON parse failed in {}", ToUtf8(a_file));
				return;
			}

			const json* list = nullptr;
			if (doc.is_array()) {
				list = std::addressof(doc);
			} else if (doc.is_object()) {
				const auto it = doc.find(a_kind == Kind::Buff ? "buffs" : "entries");
				if (it != doc.end() && it->is_array()) {
					list = std::addressof(*it);
				}
			}
			if (!list) {
				return;
			}

			for (const auto& entry : *list) {
				if (!entry.is_object()) {
					continue;
				}

				const auto idIt = entry.find("id");
				if (idIt == entry.end() || !idIt->is_string()) {
					continue;
				}
				const auto id = idIt->get<std::string>();

				for (auto field = entry.begin(); field != entry.end(); ++field) {
					const auto rawType = ExtractType(field.key());
					if (!rawType) {
						continue;
					}

					const auto type = NormalizeType(*rawType);
					if (type.empty()) {
						continue;
					}
					if (IsBlockedType(type)) {
						WARN("Ignoring '{}' on '{}': .type \"{}\" is blocked", field.key(), id, type);
						continue;
					}

					Register(a_kind, id, type, field.value(), a_file);
				}
			}
		}

		void ScanDirectory(const fs::path& a_dir)
		{
			std::error_code ec;
			if (!fs::is_directory(a_dir, ec)) {
				return;
			}

			INFO("Scanning: {}", ToUtf8(a_dir));
			try {
				for (const auto& entry : fs::recursive_directory_iterator(
						 a_dir, fs::directory_options::skip_permission_denied)) {
					if (!entry.is_regular_file(ec)) {
						continue;
					}

					const auto name = Lowercase(ToUtf8(entry.path().filename()));
					if (name.ends_with(".buffs.json")) {
						ParseFile(entry.path(), Kind::Buff);
					} else if (name.ends_with(".entries.trinkets.json")) {
						ParseFile(entry.path(), Kind::Trinket);
					}
				}
			} catch (const std::exception& e) {
				WARN("Error while scanning {}: {}", ToUtf8(a_dir), e.what());
			}
		}
	}

	void Load()
	{
		const auto gameRoot = GetGameRoot();
		LoadConfig(gameRoot);

		for (const auto& root : CollectRoots(gameRoot)) {
			ScanDirectory(root);
		}

		INFO("Loaded custom type triggers: {} buff(s), {} trinket(s), {} distinct type(s)",
			BuffRules.size(), TrinketRules.size(), KnownTypes.size());
		for (const auto& type : KnownTypes) {
			INFO("  .type \"{}\"", type);
		}
		if (Empty()) {
			WARN("No custom type triggers found. Add \"<type>_type_additional_effect_ids\" to a buffs/trinkets JSON file.");
		}
	}

	bool Empty() noexcept
	{
		return KnownTypes.empty();
	}

	bool HasType(std::string_view a_type) noexcept
	{
		return KnownTypes.contains(a_type);
	}

	bool IsBlockedType(std::string_view a_type) noexcept
	{
		return BlockedTypes.contains(a_type);
	}

	std::string NormalizeType(std::string_view a_text)
	{
		constexpr std::string_view whitespace{ " \t\r\n" };
		const auto begin = a_text.find_first_not_of(whitespace);
		if (begin == std::string_view::npos) {
			return {};
		}
		const auto end = a_text.find_last_not_of(whitespace);
		return Lowercase(std::string{ a_text.substr(begin, end - begin + 1) });
	}

	const EffectIds* FindBuff(std::int32_t a_idHash, std::string_view a_type) noexcept
	{
		const auto owner = BuffRules.find(a_idHash);
		if (owner == BuffRules.end()) {
			return nullptr;
		}
		const auto rule = owner->second.byType.find(a_type);
		return rule != owner->second.byType.end() ? std::addressof(rule->second) : nullptr;
	}

	const EffectIds* FindTrinket(std::int32_t a_idHash, std::string_view a_type) noexcept
	{
		const auto owner = TrinketRules.find(a_idHash);
		if (owner == TrinketRules.end()) {
			return nullptr;
		}
		const auto rule = owner->second.byType.find(a_type);
		return rule != owner->second.byType.end() ? std::addressof(rule->second) : nullptr;
	}

	std::int32_t Hash(std::string_view a_text) noexcept
	{
		std::uint32_t hash = 0;
		for (const auto c : a_text) {
			hash = hash * 53 + static_cast<unsigned char>(c);
		}
		return static_cast<std::int32_t>(hash);
	}

	std::size_t BuffCount() noexcept { return BuffRules.size(); }
	std::size_t TrinketCount() noexcept { return TrinketRules.size(); }
	std::size_t TypeCount() noexcept { return KnownTypes.size(); }
}
