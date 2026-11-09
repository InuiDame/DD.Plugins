#include "BuffIcons.h"

namespace BuffIcons
{
	namespace fs = std::filesystem;
	using json = nlohmann::json;

	namespace
	{
		// ===================== 全局变量与定义 =====================
		std::vector<Icon>                             Icons;
		std::unordered_map<std::int32_t, std::size_t> IconByKey;
		std::unordered_map<std::int32_t, std::size_t> IconByBuff;

		// 小写的 overlays 相对路径 -> 文件，仅在 Load 期间使用
		std::unordered_map<std::string, fs::path> OverlayFiles;

		std::string ToUtf8(const fs::path& a_path)
		{
			const auto u8 = a_path.u8string();
			return { reinterpret_cast<const char*>(u8.data()), u8.size() };
		}

		fs::path FromUtf8(std::string_view a_text)
		{
			return std::u8string_view{ reinterpret_cast<const char8_t*>(a_text.data()), a_text.size() };
		}

		// 只处理 ASCII，UTF-8 多字节序列保持原样
		std::string Normalize(std::string a_text)
		{
			for (auto& c : a_text) {
				if (c == '\\') {
					c = '/';
				} else if (c >= 'A' && c <= 'Z') {
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

		// ===================== 目录收集 =====================
		void AddChildDirectories(std::vector<fs::path>& a_roots, const fs::path& a_dir)
		{
			std::error_code ec;
			if (!fs::is_directory(a_dir, ec)) {
				return;
			}

			INFO("Scanning mod directory: {}", ToUtf8(a_dir));

			try {
				for (const auto& entry : fs::directory_iterator(a_dir, fs::directory_options::skip_permission_denied)) {
					if (entry.is_directory(ec)) {
						a_roots.push_back(entry.path());
					}
				}
			} catch (const std::exception& e) {
				WARN("Error while scanning {}: {}", ToUtf8(a_dir), e.what());
			}
		}

		// 按图标查找优先级排序：本地 Mod、创意工坊 Mod、游戏本体
		std::vector<fs::path> CollectRoots()
		{
			const auto exe = GetExecutablePath();
			const auto gameRoot = exe.parent_path().parent_path().parent_path();

			std::vector<fs::path> roots;
			AddChildDirectories(roots, gameRoot / "mods");

			fs::path steamLibRoot;
			bool     isSteam = false;
			for (const auto& part : exe) {
				if (::_wcsicmp(part.c_str(), L"steamapps") == 0) {
					isSteam = true;
					break;
				}
				steamLibRoot /= part;
			}

			if (isSteam) {
				AddChildDirectories(roots, steamLibRoot / "steamapps" / "workshop" / "content" / "262060");
			} else {
				WARN("Could not find 'steamapps' in game path, workshop mods are skipped. Current path: {}", ToUtf8(exe));
			}

			roots.push_back(gameRoot);
			return roots;
		}

		void IndexOverlays(const std::vector<fs::path>& a_roots)
		{
			for (const auto& root : a_roots) {
				const auto      dir = root / "overlays";
				std::error_code ec;
				if (!fs::is_directory(dir, ec)) {
					continue;
				}

				try {
					for (const auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
						if (entry.is_regular_file(ec) && Normalize(ToUtf8(entry.path().extension())) == ".png") {
							// 先到先得，保持 CollectRoots 的优先级
							OverlayFiles.emplace(Normalize(ToUtf8(entry.path().lexically_relative(dir))), entry.path());
						}
					}
				} catch (const std::exception& e) {
					WARN("Error while indexing {}: {}", ToUtf8(dir), e.what());
				}
			}
		}

		// ===================== 图标解析 =====================
		std::optional<fs::path> ResolveIconFile(std::string_view a_name, const fs::path& a_owner)
		{
			auto base = Normalize(std::string{ a_name });
			if (base.ends_with(".png")) {
				base.resize(base.size() - 4);
			}

			const std::array candidates{ "tray_" + base + ".png", base + ".png" };

			// 定义该 buff 的 Mod 自带的图标优先
			std::error_code ec;
			for (const auto& candidate : candidates) {
				auto file = a_owner / "overlays" / FromUtf8(candidate);
				if (fs::is_regular_file(file, ec)) {
					return file;
				}
			}

			for (const auto& candidate : candidates) {
				if (const auto it = OverlayFiles.find(candidate); it != OverlayFiles.end()) {
					return it->second;
				}
			}

			return std::nullopt;
		}

		void Register(const std::string& a_buffId, const std::string& a_iconName, const fs::path& a_owner)
		{
			if (a_iconName.empty()) {
				return;
			}

			const auto file = ResolveIconFile(a_iconName, a_owner);
			if (!file) {
				WARN("Custom icon '{}' of buff '{}' not found (defined in {})", a_iconName, a_buffId, ToUtf8(a_owner));
				return;
			}

			auto path = ToUtf8(*file);
			std::ranges::replace(path, '\\', '/');

			const auto key = Hash("buff_icon:" + Normalize(path));

			auto [it, inserted] = IconByKey.try_emplace(key, Icons.size());
			if (inserted) {
				Icons.push_back({ key, path });
			}

			IconByBuff[Hash(a_buffId)] = it->second;
			INFO("[MAP] {} -> {}", a_buffId, path);
		}

		// ===================== JSON 加载 =====================
		void ParseBuffFile(const fs::path& a_file, const fs::path& a_owner)
		{
			std::ifstream file(a_file, std::ios::binary);
			if (!file.is_open()) {
				return;
			}

			const std::string content{ std::istreambuf_iterator<char>(file), {} };
			if (content.find("buff_icon") == std::string::npos) {
				return;
			}

			const auto doc = json::parse(content, nullptr, false, true);
			if (doc.is_discarded()) {
				WARN("JSON parse failed in {}", ToUtf8(a_file));
				return;
			}

			const auto buffs = doc.find("buffs");
			if (buffs == doc.end() || !buffs->is_array()) {
				return;
			}

			for (const auto& buff : *buffs) {
				if (!buff.is_object()) {
					continue;
				}

				const auto id = buff.find("id");
				const auto icon = buff.find("buff_icon");
				if (id != buff.end() && id->is_string() && icon != buff.end() && icon->is_string()) {
					Register(id->get<std::string>(), icon->get<std::string>(), a_owner);
				}
			}
		}

		void ScanBuffFiles(const fs::path& a_root)
		{
			const auto      dir = a_root / "shared" / "buffs";
			std::error_code ec;
			if (!fs::is_directory(dir, ec)) {
				return;
			}

			try {
				for (const auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
					if (entry.is_regular_file(ec) && Normalize(ToUtf8(entry.path().filename())).ends_with(".buffs.json")) {
						ParseBuffFile(entry.path(), a_root);
					}
				}
			} catch (const std::exception& e) {
				WARN("Error while scanning {}: {}", ToUtf8(dir), e.what());
			}
		}
	}

	void Load()
	{
		const auto roots = CollectRoots();
		IndexOverlays(roots);

		// 逆序处理，同 id 时优先级高的 Mod 最后写入
		for (const auto& root : std::views::reverse(roots)) {
			ScanBuffFiles(root);
		}

		OverlayFiles.clear();
		INFO("Total custom buff icons mapped: {} buff(s), {} icon(s)", IconByBuff.size(), Icons.size());
	}

	std::int32_t Hash(std::string_view a_text) noexcept
	{
		std::uint32_t hash = 0;
		for (const auto c : a_text) {
			hash = hash * 53 + static_cast<unsigned char>(c);
		}
		return static_cast<std::int32_t>(hash);
	}

	std::span<const Icon> All() noexcept
	{
		return Icons;
	}

	const Icon* FindByBuff(std::int32_t a_buffHash) noexcept
	{
		const auto it = IconByBuff.find(a_buffHash);
		return it != IconByBuff.end() ? std::addressof(Icons[it->second]) : nullptr;
	}

	const Icon* FindByKey(std::int32_t a_key) noexcept
	{
		const auto it = IconByKey.find(a_key);
		return it != IconByKey.end() ? std::addressof(Icons[it->second]) : nullptr;
	}
}
