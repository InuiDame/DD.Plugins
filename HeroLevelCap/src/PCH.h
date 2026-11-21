#pragma once

// c
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cfenv>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <clocale>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cuchar>
#include <cwchar>
#include <cwctype>

// cxx
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// winnt
#include <Windows.h>
#include <ShlObj_core.h>

#undef min
#undef max

using namespace std::literals;

#define DLLEXPORT extern "C" [[maybe_unused]] __declspec(dllexport)

// Plugin（由 xmake/plugin.lua 的 Plugin.h.in 生成）
#include "Plugin.h"

// DKUtil
#define PLUGIN_MODE
#define LOG_PATH "DDModLoader\\log\\"
#include "DKUtil/Hook.hpp"
#include "DKUtil/Logger.hpp"
