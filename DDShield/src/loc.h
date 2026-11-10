#pragma once
#include "common.h"

namespace dds {
namespace loc {

// Locates the mod's string table (local mods, the DLL's own folder, or a
// Steam Workshop copy) and picks the section for the in-game language.
void Init(const std::wstring& dllDir, const std::wstring& gameRoot);

// Localized string for `key`, or `fallback` when the table has no entry.
std::wstring Get(const char* key, const wchar_t* fallback);

} // namespace loc
} // namespace dds
