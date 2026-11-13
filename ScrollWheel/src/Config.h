#pragma once

// cfg 解析：纯标准库，不依赖 DKUtil / Win32，可被离线单测直接编译运行。
// 路径计算(GetGameRoot)放在 main.cpp，本模块只处理「给定路径的文件」。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Config
{
	enum Panel : int
	{
		Provision = 0,
		Guild = 1,
		RaidInventory = 2,
		Count = 3
	};

	struct Rect
	{
		float x{ 0.f };
		float y{ 0.f };
		float w{ 0.f };
		float h{ 0.f };
	};

	struct PanelCfg
	{
		bool enabled{ false };
		Rect rect{};
	};

	struct Data
	{
		bool enabled{ true };                 // 总开关
		PanelCfg panels[Count]{};              // 默认安全模式：三个面板全部关闭（实机验收后手动开启）
		float step{ 30.f };
		float smooth{ 8.f };
		int visibleRows{ 5 };
		float rowHeight{ 40.f };
		bool invert{ false };
		bool debugSampling{ false };
		bool debugLog{ false };
		bool patchNativeStep{ false };
		float nativeStep{ 30.f };
		int columns{ 7 };                     // layout[0] 未加载时的回退列数
		float scrollRange{ 400.f };           // 动态范围不可用时的回退上限（v5）
		float maxRange{ 2000.f };             // 动态范围上限（v6）
		bool autoRange{ true };               // =1 按每帧格子数动态扩大可下拉范围（仿饰品仓库）
		int rowsPerNotch{ 1 };                // 保留兼容；v8 不再整行跳变
		// v10 重新启用裁剪，但机制与 v7 完全不同（见 p10 §5.2 / p17）：
		//  * 面板判据 = 网格渲染器入口 cave 用 r8 布局地址写的标志，零误伤；
		//  * 行号 = param_5 位域 (v>>1)&0xffffff，不再用 param_3 猜；
		//  * 静止时零干预，滚动时按 ceil(current/rowH) 开窗口、连续平滑；
		//  * 跳过 0x7EBF70 会同时跳过绘制与热点注册（两调用点返回值无副作用）。
		// 代码默认 = true，且用户 cfg 已显式写入 1（不再有隐式开关）。
		bool clipOutOfWindow{ true };

		// v11.2：修「在非补给界面滚滚轮 → 之后打开补给商店已被翻页」。
		// v10 把 Wheel::Tick 的 layoutHit 硬编码成 true 且每帧无条件写三套布局，
		// 导致任何界面的滚轮都改写补给 start_y。=1 时只在目标网格活跃（含
		// 480ms 宽限窗口）时消费滚轮并写布局；=0 一键回到 v10 原行为。
		bool gateWheelByPanel{ true };

		// ---- v11 工会技能栏滚轮（实验性，独立开关，默认全关）----
		// 与旧 `guild` 键完全无关：旧键恒不生效（ADR 0001），这里用新键。
		// 0 时两个新增 cave 一个都不装，已验收的 v10 四个 cave 字节级不变。
		bool  guildScroll{ false };
		float guildStep{ 30.f };        // 每格滚轮步长
		float guildCap{ 2000.f };       // 可下拉范围上限
		int   guildVisibleRows{ 1 };    // max 公式里的保留行数 R（默认 1，保证能滚到底）
		float guildRowHeight{ 91.f };   // 行高回退；优先用运行时 baseY 相邻差

		// 可见滚动条：游戏窗口的 owned popup、前台才显示、可拖动/点击/滚轮。
		// v9：自绘滚动条已彻底移除。饰品库等界面已有游戏原生滚动条，
		// DLL 不创建窗口、不处理鼠标消息、不画任何 UI。
		std::uint32_t frameTtl{ 1 };
	};

	// 是否任一面板被显式开启。纯函数，供生产 Install() 与离线单测 ⑤ 共用：
	// 默认全部关闭 → 不写布局、不装每格裁剪 cave、不改渲染（B5 安全模式）。
	inline bool AnyPanelEnabled(const Data& a_d) noexcept
	{
		for (const auto& p : a_d.panels) {
			if (p.enabled) {
				return true;
			}
		}
		return false;
	}

	// 默认配置文本（与 mod/ScrollWheel.cfg 一致）。
	std::string_view DefaultCfgText() noexcept;

	// 纯文本解析，供单测与 Load 复用；未知键/格式错误由调用方记录日志。
	void Parse(std::string_view a_text, Data& a_out);

	// 文件不存在时生成默认 cfg（绝不覆盖已有文件）；存在则只读取解析。
	// 返回 true=成功加载或生成；false=读写失败。out 始终填好默认值。
	bool Load(const std::filesystem::path& a_file, Data& a_out);
}
