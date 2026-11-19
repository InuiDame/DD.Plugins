#pragma once

namespace Hooks
{
	// 特征码定位并安装 SpineSprite::Render 的 detour；找不到时一条 hook 都不装。
	void Install();
}
