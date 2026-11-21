#pragma once

// 离线单测最小 PCH：不依赖 DKUtil / Plugin.h，日志宏置空。
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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

#include <charconv>
#include <system_error>

#undef min
#undef max

using namespace std::literals;

#define INFO(...)  ((void)0)
#define DEBUG(...) ((void)0)
#define WARN(...)  ((void)0)
