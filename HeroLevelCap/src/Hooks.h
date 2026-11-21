#pragma once

#include "Rules.h"

namespace HeroLevelCap::Hooks
{

// 返回 true 表示已安装（等级上限补丁生效）。
bool Install(const CapMap& caps);

}  // namespace HeroLevelCap::Hooks
