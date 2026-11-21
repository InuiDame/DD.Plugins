#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HeroLevelCap {

// 原版所有英雄共用的 resolve level 上限是 6（阈值表最后一项 48）。
inline constexpr int kDefaultCap = 6;
inline constexpr int kMinCap = 1;
inline constexpr int kMaxCap = 20;

inline constexpr std::uint32_t kVanillaThresholds[] = { 0, 2, 8, 14, 24, 36, 48 };

using CapMap = std::unordered_map<std::string, int>;

struct InfoLineResult {
    bool matched = false;  // 行首是 level_cap:
    bool valid = false;    // 解析出了一个合法数字
    int cap = 0;
};

// 解析一行 `.info.darkest` 文本。支持：
//   level_cap: .max 10
//   level_cap: 10
// 大小写不敏感，token 之间可用空白或逗号分隔；`.max` 之外的 token 忽略。
InfoLineResult ParseInfoLine(std::string_view line);

// 读取一个职业文件；同一文件内多个合法行按先后顺序覆盖，最后一行生效。
void ParseInfoFile(const std::wstring& path, const std::string& classId, CapMap& caps);

// 递归扫描根目录下 heroes/<class>/<class>.info.darkest；根按调用顺序后者覆盖前者。
void ScanHeroesRoot(const std::wstring& root, CapMap& caps);

// 依次扫描 <game>/heroes、steam workshop、<game>/mods；返回是否找到任何规则。
void LoadRules(const std::wstring& gameRoot, CapMap& caps);

int CapForClass(const CapMap& caps, const std::string& classId, int fallback = kDefaultCap);

// 复制游戏内存里的阈值表，并按需要向后扩展到 maxCap + 1 项。
std::vector<std::uint32_t> BuildThresholds(const std::uint32_t* base, std::size_t count, int maxCap);

// 按阈值表把 XP 换算成 resolve level，并按职业上限截断（可返回小数，和原版一致）。
float ComputeLevel(std::uint32_t xp, const std::vector<std::uint32_t>& thresholds, int cap);

std::string Lower(std::string s);

// 从任意地址读一段内嵌的 id 字符串（ActorClass+8 是 char[0x40]，不是指针）。
bool ReadInlineId(const void* p, char out[64]);

}  // namespace HeroLevelCap
