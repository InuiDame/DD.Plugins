#include "Config.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace Config
{
	namespace
	{
		// 默认全部面板关闭（B5 安全模式）：只装输入 cave + 全有或全无校验，
		// 不写布局、不装每格裁剪 cave、不改渲染。实机按 README §8.2 逐界面验收后，
		// 由用户手动把对应面板改成 1 才会启用渲染注入。
		// 矩形为 UI 1920x1080 基准坐标；默认值是全屏放行，须用 debug_sampling 实机收紧 [推测]。
	constexpr std::string_view kDefaultCfg{
		"# ScrollWheel.cfg - 给无原生滚轮的界面加滚轮（v10，已实机验收）\n"
		"# 首次启动生成，之后绝不覆盖本文件；所有键都显式写出，避免缺键时\n"
		"# 悄悄套用代码默认值（v7 的裁剪事故正是这样发生的）。\n"
		"# 支持 # // ; 注释，key = value。\n"
		"\n"
		"enabled = 1                    ; 总开关（关掉后连输入捕获 cave 都不装）\n"
		"\n"
		"; 每面板独立开关；默认全部关闭（安全模式）。guild 恒不生效，见下。\n"
		"provision = 0                  ; 出征补给 ProvisionSelect（layout 0x2B8DB10/0x2B8DB30）\n"
		"guild = 0                      ; 【已废弃/恒不生效】工会明确不支持，见 docs/adr/0001\n"
		"raid_inventory = 0             ; 地牢背包 Panel_Inventory（layout 0x2BE38A0）\n"
		"\n"
		"step = 30.0                    ; 每格滚轮步长 (= 游戏原生 0xEE63DC = 30.0f)\n"
		"smooth = 8.0                   ; current 平滑逼近系数（连续滚动，不整行跳变）\n"
		"visible_rows = 5               ; 可见行数（滑块长度用）\n"
		"row_height = 40.0              ; 行高回退值（优先用运行时布局 [4]）\n"
		"columns = 7                    ; 列数回退值（优先用运行时布局 [0]）\n"
		"invert = 0                     ; =1 反转滚轮方向\n"
		"frame_ttl = 1                  ; 面板激活标记的帧 TTL\n"
		"scroll_range = 400.0           ; 动态范围算不出来时的回退上限\n"
		"auto_range = 1                 ; =1 按每帧格子数动态扩大可下拉范围\n"
		"max_range = 2000.0             ; 动态范围上限\n"
		"clip_out_of_window = 1         ; v10 裁剪：面板 gate(r8) + param_5 行号，0 = 只平移不裁剪\n"
		"rows_per_notch = 1             ; 【已废弃】整行量化已删除，此键不再生效\n"
		"\n"
		"; === v11 工会技能栏滚轮（实验性，与旧的恒不生效 guild 键无关）===\n"
		"; 0 = 两个工会 cave 一个都不装，已验收的补给/背包行为字节级不变。\n"
		"; 全部键在代码默认 / 本模板 / 用户实际 cfg 三处显式写值，杜绝隐式开关。\n"
		"guild_scroll = 0               ; 工会滚轮总开关（实验性，默认关闭）\n"
		"guild_step = 30.0              ; 每格滚轮步长\n"
		"guild_cap = 2000.0             ; 可下拉范围上限\n"
		"guild_visible_rows = 1         ; max 公式保留行数 R（=1 保证不滚不到底）\n"
		"guild_row_height = 91.0        ; 行高回退；优先用运行时技能槽 Y 相邻差\n"
		"\n"
		"; === v12 工会技能容量防护（技能过多时防止 UI 崩溃）===\n"
		"; 实测本地 mod 英雄最多 192 个 combat_skill（Laevateinn.info.darkest），\n"
		"; v11.2 的 DLL 侧表只有 64 槽，超出部分被硬钳制，形成容量落差。\n"
		"guild_max_slots = 256          ; 有效上限 1..256；设 64 = 一键回退 v11.2 旧容量\n"
		"guild_auto_cap = 1             ; =1 技能多时自动抬高滚动上限，0 = 严格用 guild_cap\n"
		"\n"
		"; === v11.2 滚轮按面板激活门控（修「打开补给前滚轮导致预翻页」）===\n"
		"; 1 = 只在补给/背包网格活跃时消费滚轮并写布局（默认，推荐）\n"
		"; 0 = 回到 v10 原行为（任何界面滚轮都驱动补给状态机），一键回退\n"
		"gate_wheel_by_panel = 1\n"
		"\n"
		"; v9：自绘滚动条已移除，DLL 不创建任何窗口、不画 UI；\n"
		";      饰品库等界面的原生滚动条由游戏自己负责。\n"
		"\n"
		"debug_sampling = 0             ; =1 输出 [SAMPLE] 统计日志（只统计，不裁剪）\n"
		"debug_log = 0                  ; =1 打开 DEBUG 级日志\n"
		"\n"
		"patch_native_step = 0          ; =1 才改 .rdata 0xEE63DC（只调速，默认关）\n"
		"native_step = 30.0\n"
		"\n"
		"; 每面板鼠标矩形 x,y,w,h（UI 1920x1080）。默认全屏放行；\n"
		"; 写 0,0,0,0 也视为放行。\n"
		"provision_rect = 0,0,1920,1080\n"
		"guild_rect = 0,0,1920,1080\n"
		"raid_inventory_rect = 0,0,1920,1080\n"
	};

		std::string_view Trim(std::string_view a_text) noexcept
		{
			constexpr std::string_view ws{ " \t\r\n" };
			const auto begin = a_text.find_first_not_of(ws);
			if (begin == std::string_view::npos) {
				return {};
			}
			const auto end = a_text.find_last_not_of(ws);
			return a_text.substr(begin, end - begin + 1);
		}

		float ToFloat(std::string_view a_text, float a_fallback) noexcept
		{
			if (a_text.empty()) {
				return a_fallback;
			}
			std::string tmp{ a_text };
			char* end = nullptr;
			const float v = std::strtof(tmp.c_str(), &end);
			return end == tmp.c_str() ? a_fallback : v;
		}

		int ToInt(std::string_view a_text, int a_fallback) noexcept
		{
			if (a_text.empty()) {
				return a_fallback;
			}
			std::string tmp{ a_text };
			char* end = nullptr;
			const long v = std::strtol(tmp.c_str(), &end, 10);
			return end == tmp.c_str() ? a_fallback : static_cast<int>(v);
		}

		bool ToBool(std::string_view a_text, bool a_fallback) noexcept
		{
			const auto v = Trim(a_text);
			if (v.empty()) {
				return a_fallback;
			}
			if (v == "1" || v == "true" || v == "yes" || v == "on") {
				return true;
			}
			if (v == "0" || v == "false" || v == "no" || v == "off") {
				return false;
			}
			return ToInt(v, a_fallback ? 1 : 0) != 0;
		}

		Rect ToRect(std::string_view a_text, const Rect& a_fallback) noexcept
		{
			std::array<float, 4> v{ a_fallback.x, a_fallback.y, a_fallback.w, a_fallback.h };
			std::size_t i = 0;
			std::size_t pos = 0;
			while (i < 4 && pos <= a_text.size()) {
				const auto comma = a_text.find(',', pos);
				const auto token = Trim(a_text.substr(pos, (comma == std::string_view::npos ? a_text.size() : comma) - pos));
				v[i++] = ToFloat(token, v[i - 1]);
				if (comma == std::string_view::npos) {
					break;
				}
				pos = comma + 1;
			}
			return { v[0], v[1], v[2], v[3] };
		}

		std::string Lower(std::string_view a_text)
		{
			std::string s{ a_text };
			for (auto& c : s) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return s;
		}
	}

	std::string_view DefaultCfgText() noexcept
	{
		return kDefaultCfg;
	}

	void Parse(std::string_view a_text, Data& a_out)
	{
		// 先恢复到默认，再逐行覆盖（缺键自动用默认值补齐）。
		// B5：默认全部面板关闭，Data::PanelCfg::enabled 默认已是 false，不显式置 true。
		Data def;
		def.panels[Provision].rect = { 0.f, 0.f, 1920.f, 1080.f };
		def.panels[Guild].rect = { 0.f, 0.f, 1920.f, 1080.f };
		def.panels[RaidInventory].rect = { 0.f, 0.f, 1920.f, 1080.f };
		a_out = def;

		std::size_t pos = 0;
		while (pos <= a_text.size()) {
			const auto nl = a_text.find('\n', pos);
			auto line = a_text.substr(pos, (nl == std::string_view::npos ? a_text.size() : nl) - pos);
			pos = nl == std::string_view::npos ? a_text.size() + 1 : nl + 1;

			for (const char c : { '#', ';' }) {
				if (const auto k = line.find(c); k != std::string_view::npos) {
					line = line.substr(0, k);
				}
			}
			if (const auto k = line.find("//"); k != std::string_view::npos) {
				line = line.substr(0, k);
			}
			const auto content = Trim(line);
			if (content.empty()) {
				continue;
			}
			const auto eq = content.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const auto key = Lower(Trim(content.substr(0, eq)));
			const auto val = Trim(content.substr(eq + 1));

			if (key == "enabled") {
				a_out.enabled = ToBool(val, a_out.enabled);
			} else if (key == "provision") {
				a_out.panels[Provision].enabled = ToBool(val, a_out.panels[Provision].enabled);
			} else if (key == "guild") {
				a_out.panels[Guild].enabled = ToBool(val, a_out.panels[Guild].enabled);
			} else if (key == "raid_inventory") {
				a_out.panels[RaidInventory].enabled = ToBool(val, a_out.panels[RaidInventory].enabled);
			} else if (key == "step") {
				a_out.step = ToFloat(val, a_out.step);
			} else if (key == "smooth") {
				a_out.smooth = ToFloat(val, a_out.smooth);
			} else if (key == "visible_rows") {
				a_out.visibleRows = ToInt(val, a_out.visibleRows);
			} else if (key == "row_height") {
				a_out.rowHeight = ToFloat(val, a_out.rowHeight);
			} else if (key == "columns") {
				a_out.columns = ToInt(val, a_out.columns);
			} else if (key == "invert") {
				a_out.invert = ToBool(val, a_out.invert);
			} else if (key == "frame_ttl") {
				a_out.frameTtl = static_cast<std::uint32_t>(ToInt(val, static_cast<int>(a_out.frameTtl)));
			} else if (key == "scroll_range") {
				a_out.scrollRange = ToFloat(val, a_out.scrollRange);
			} else if (key == "auto_range") {
				a_out.autoRange = ToBool(val, a_out.autoRange);
			} else if (key == "max_range") {
				a_out.maxRange = ToFloat(val, a_out.maxRange);
			} else if (key == "rows_per_notch") {
				a_out.rowsPerNotch = ToInt(val, a_out.rowsPerNotch);
		} else if (key == "clip_out_of_window") {
			a_out.clipOutOfWindow = ToBool(val, a_out.clipOutOfWindow);
		} else if (key == "guild_scroll") {
			a_out.guildScroll = ToBool(val, a_out.guildScroll);
		} else if (key == "guild_step") {
			a_out.guildStep = ToFloat(val, a_out.guildStep);
		} else if (key == "guild_cap") {
			a_out.guildCap = ToFloat(val, a_out.guildCap);
		} else if (key == "guild_visible_rows") {
			a_out.guildVisibleRows = ToInt(val, a_out.guildVisibleRows);
		} else if (key == "guild_row_height") {
			a_out.guildRowHeight = ToFloat(val, a_out.guildRowHeight);
		} else if (key == "guild_max_slots") {
			a_out.guildMaxSlots = ToInt(val, a_out.guildMaxSlots);
		} else if (key == "guild_auto_cap") {
			a_out.guildAutoCap = ToBool(val, a_out.guildAutoCap);
		} else if (key == "gate_wheel_by_panel") {
			a_out.gateWheelByPanel = ToBool(val, a_out.gateWheelByPanel);
			} else if (key == "debug_sampling") {
				a_out.debugSampling = ToBool(val, a_out.debugSampling);
			} else if (key == "debug_log") {
				a_out.debugLog = ToBool(val, a_out.debugLog);
			} else if (key == "patch_native_step") {
				a_out.patchNativeStep = ToBool(val, a_out.patchNativeStep);
			} else if (key == "native_step") {
				a_out.nativeStep = ToFloat(val, a_out.nativeStep);
			} else if (key == "provision_rect") {
				a_out.panels[Provision].rect = ToRect(val, a_out.panels[Provision].rect);
			} else if (key == "guild_rect") {
				a_out.panels[Guild].rect = ToRect(val, a_out.panels[Guild].rect);
			} else if (key == "raid_inventory_rect") {
				a_out.panels[RaidInventory].rect = ToRect(val, a_out.panels[RaidInventory].rect);
			}
		}
	}

	bool Load(const std::filesystem::path& a_file, Data& a_out)
	{
		Parse({}, a_out); // 先填默认值

		std::error_code ec;
		if (!std::filesystem::is_regular_file(a_file, ec)) {
			std::filesystem::create_directories(a_file.parent_path(), ec);
			std::ofstream out(a_file, std::ios::binary);
			if (!out.is_open()) {
				return false;
			}
			out.write(kDefaultCfg.data(), static_cast<std::streamsize>(kDefaultCfg.size()));
			return true; // 首次生成，out 已是默认值
		}

		std::ifstream in(a_file, std::ios::binary);
		if (!in.is_open()) {
			return false;
		}
		const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		Parse(text, a_out); // 绝不写回用户文件
		return true;
	}
}
