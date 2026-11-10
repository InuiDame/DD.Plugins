#pragma once
#include "common.h"

namespace dds {
namespace render {

bool Init();
void OnSwap();          // called from the SDL_GL_SwapWindow detour
bool Ready();

// Where F10 writes the tuned values.
void SetConfigPath(const std::wstring& path);

} // namespace render
} // namespace dds
