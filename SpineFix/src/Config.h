#pragma once

namespace SpineFix::config
{
	// 读 <gameRoot>\DDModLoader\cfg\SpineFix.cfg；文件不存在时写一份默认配置。
	// 只读一次，在 DllMain 里调用。
	void Load();

	// 按时间线补 attachment（切片显示/隐藏）与 drawOrder（层级）关键帧。
	// 0 = 不动，1 = 修补，2 = 只记录差异不修改（用来判断原版到底补没补）。
	[[nodiscard]] int RepairMode() noexcept;

	// 支持 slot 的混合模式（加法、正片叠底、滤色）
	[[nodiscard]] bool BlendModes() noexcept;

	// 原版 2.1 的 additive 是否也算特殊混合；关掉后只认 2/3
	[[nodiscard]] bool Additive() noexcept;

	// 一个 sprite 最多拆成几段绘制，超出的段并进最后一段
	[[nodiscard]] int MaxPasses() noexcept;

	// 每次修补都记一条日志（默认只在第一次遇到新动画时记）
	[[nodiscard]] bool Verbose() noexcept;

	// 每种骨骼资源第一次绘制时打印一份自检信息（槽位数、混合字节分布、当前动画的时间线）
	[[nodiscard]] bool Probe() noexcept;

	// 自检用：把每个 sprite 强行拆成两段（两段都是正常混合）画，画面必须和不拆时完全一样
	[[nodiscard]] bool DebugSplitPasses() noexcept;
}
