#pragma once

// 离线单测的最小 PCH：不需要 DKUtil / Plugin.h，只要 Windows 类型、标准库、json，
// 以及把实现里用到的日志宏变成 no-op。Rules.cpp 在本 target 下用这份 PCH 编译。
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

using namespace std::literals;

#define INFO(...)  ((void)0)
#define DEBUG(...) ((void)0)
#define WARN(...)  ((void)0)
#define ERROR(...) ((void)0)
