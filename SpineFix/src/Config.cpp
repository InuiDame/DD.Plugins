#include "Config.h"

#include <filesystem>
#include <fstream>

namespace SpineFix::config
{
	namespace
	{
		namespace fs = std::filesystem;

		struct Values
		{
			int  repairMode{ 1 };
			bool blendModes{ true };
			bool additive{ true };
			int  maxPasses{ 8 };
			bool verbose{ false };
			bool probe{ false };
			bool debugSplit{ false };
		};

		Values g_values;

		constexpr std::string_view DefaultFile = R"(# SpineFix 配置
# 1 = 开，0 = 关。改完重启游戏生效。

# 按时间线补「切片显示/隐藏」（slot attachment）和「层级调整」（drawOrder）关键帧。
#   0 = 不动，1 = 修补，2 = 只记录差异不修改
# 原版这两条时间线在 spine-c 里是有的，但不一定在绘制前生效，插件在每次绘制前补一次。
# 想确认原版到底补没补，把它设成 2 看日志。
repair_timelines = 1

# 支持切片混合模式。混合模式取自 .skel 里 slot 的 additive 字节：
#   0 = 正常，1 = 加法（spine 2.1 的 additive），2 = 正片叠底，3 = 滤色
# 用 tools/set_blend.py 改 .skel 里的这个字节。
blend_modes = 1

# additive 字节为 1 时是否按「加法」画。原版资源里可能有依赖加法混合的特效，
# 关掉它可以让这些资源保持原版的画法，只对 2/3 生效。
additive = 1

# 一个 sprite 最多拆成几段绘制（不同混合模式的连续切片各一段）。
max_passes = 8

# 每次修补都写日志；默认只记每个动画的第一次。
verbose = 0

# 每种骨骼资源第一次绘制时打一份自检（槽位数、混合字节分布、当前动画的时间线类型），
# 用来确认 .skel 有没有被读进来、混合字节写对没有。
probe = 0

# 自检用：把每个 sprite 强行拆成两段画（两段都是正常混合）。用来验证分段绘制本身没画错，
# 画面应该和不拆时一模一样。平时保持 0。
debug_split_passes = 0
)";

		fs::path ExecutablePath()
		{
			std::wstring buffer(MAX_PATH, L'\0');
			while (true) {
				const auto length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length < buffer.size()) {
					buffer.resize(length);
					return buffer;
				}
				buffer.resize(buffer.size() * 2);
			}
		}

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

		void Apply(std::string_view a_key, std::string_view a_value)
		{
			const auto flag = [&]() noexcept {
				return a_value == "1" || a_value == "true" || a_value == "on" || a_value == "yes";
			};

			if (a_key == "repair_timelines") {
				const auto parsed = (!a_value.empty() && a_value[0] >= '0' && a_value[0] <= '9') ? a_value[0] - '0' : 1;
				g_values.repairMode = std::clamp(parsed, 0, 2);
			} else if (a_key == "blend_modes") {
				g_values.blendModes = flag();
			} else if (a_key == "additive") {
				g_values.additive = flag();
			} else if (a_key == "verbose") {
				g_values.verbose = flag();
			} else if (a_key == "probe") {
				g_values.probe = flag();
			} else if (a_key == "debug_split_passes") {
				g_values.debugSplit = flag();
			} else if (a_key == "max_passes") {
				int parsed = 0;
				for (const char c : a_value) {
					if (c < '0' || c > '9') {
						return;
					}
					parsed = parsed * 10 + (c - '0');
				}
				g_values.maxPasses = std::clamp(parsed, 1, 32);
			} else {
				WARN("cfg: unknown key \"{}\"", a_key);
			}
		}
	}

	void Load()
	{
		// <gameRoot>\_windows\win64\Darkest.exe -> <gameRoot>
		const auto root = ExecutablePath().parent_path().parent_path().parent_path();
		const auto dir = root / "DDModLoader" / "cfg";
		const auto file = dir / "SpineFix.cfg";

		std::error_code ec;
		if (!fs::is_regular_file(file, ec)) {
			fs::create_directories(dir, ec);
			if (ec) {
				WARN("cfg: cannot create {}, using defaults", dir.string());
				return;
			}
			std::ofstream out(file, std::ios::binary);
			if (out.is_open()) {
				out.write(DefaultFile.data(), static_cast<std::streamsize>(DefaultFile.size()));
				INFO("cfg: default written to {}", file.string());
			} else {
				WARN("cfg: cannot write {}, using defaults", file.string());
			}
			return;
		}

		std::ifstream in(file, std::ios::binary);
		if (!in.is_open()) {
			WARN("cfg: cannot open {}, using defaults", file.string());
			return;
		}

		std::string line;
		while (std::getline(in, line)) {
			if (const auto hash = line.find('#'); hash != std::string::npos) {
				line.erase(hash);
			}
			const auto content = Trim(line);
			if (content.empty()) {
				continue;
			}
			const auto eq = content.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			Apply(Trim(content.substr(0, eq)), Trim(content.substr(eq + 1)));
		}

		INFO("cfg: repaired-timelines={} blend-modes={} additive={} max-passes={} verbose={}", g_values.repairMode,
			g_values.blendModes, g_values.additive, g_values.maxPasses, g_values.verbose);
	}

	int RepairMode() noexcept
	{
		return g_values.repairMode;
	}

	bool BlendModes() noexcept
	{
		return g_values.blendModes;
	}

	bool Additive() noexcept
	{
		return g_values.additive;
	}

	int MaxPasses() noexcept
	{
		return g_values.maxPasses;
	}

	bool Verbose() noexcept
	{
		return g_values.verbose;
	}

	bool Probe() noexcept
	{
		return g_values.probe;
	}

	bool DebugSplitPasses() noexcept
	{
		return g_values.debugSplit;
	}
}
