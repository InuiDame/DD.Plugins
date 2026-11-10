#pragma once
#include "common.h"

namespace dds {

bool HooksInstall();

// SDL_GL_SwapWindow 的长跳 Hook，HooksInstall 会调用
bool HookSwapWindow();

// buff 规则 shielded 状态的 CaveHook，from~to 是 site 处的 mov eax, [r15+0xF4]，tail 是规则结果的结算入口
void InstallBuffStatusCave(std::uintptr_t site, std::ptrdiff_t from, std::ptrdiff_t to, std::uintptr_t tail);

// rule_data.string 为 shielded 时游戏算出的哈希
std::uint32_t ShieldedStatusHash();

} // namespace dds
