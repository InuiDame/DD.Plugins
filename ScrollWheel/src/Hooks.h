#pragma once

#include "Config.h"

namespace Hooks
{
	// DllMain 在 DLL_PROCESS_ATTACH 里调用；内部完成全有或全无校验后安装：
	//  - 输入 cave @0x371F0A（始终装，只要 enabled=1）
	//  - 每格裁剪 cave @0x7EBF70（仅当任一面板显式开启才装；默认全关不装）
	//  - 纯数据补丁（每帧写布局数组 [2] 绝对值，随每格 cave 驱动，默认不写）
	void Install(const Config::Data& a_cfg);
}
