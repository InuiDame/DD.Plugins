// HeroLevelCap 离线单测：只覆盖纯逻辑（info 解析、阈值扩展、等级计算）。
// 离线 PASS 不代表运行时生效，见 README 的验证层级说明。

#include "Rules.h"

#include <cstdio>
#include <filesystem>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool cond, const char* expr, const char* file, int line)
{
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", file, line, expr);
    }
}

#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

void Section(const char* title)
{
    std::printf("\n== %s ==\n", title);
}

}  // namespace

int main()
{
    using namespace HeroLevelCap;

    Section("1. info line parse");
    {
        const auto r = ParseInfoLine("level_cap: .max 10");
        CHECK(r.matched && r.valid && r.cap == 10);
    }
    {
        const auto r = ParseInfoLine("  LEVEL_CAP: 8  ");
        CHECK(r.matched && r.valid && r.cap == 8);
    }
    {
        const auto r = ParseInfoLine("level_cap: .cap 12, .max 9");
        CHECK(r.matched && r.valid && r.cap == 9);  // 后者覆盖前者
    }
    {
        const auto r = ParseInfoLine("resistances: .stun 40%");
        CHECK(!r.matched && !r.valid);
    }
    {
        const auto r = ParseInfoLine("level_cap: .max 99");
        CHECK(r.matched && !r.valid);
    }
    {
        const auto r = ParseInfoLine("level_cap: .max xxx");
        CHECK(r.matched && !r.valid);
    }
    {
        // 未知 token 只告警忽略；后面出现的合法数字仍然按“最后生效”处理。
        const auto r = ParseInfoLine("level_cap: .max 10 .whatever 4");
        CHECK(r.matched && r.valid && r.cap == 4);
    }
    {
        const auto r = ParseInfoLine("level_cap: .max 0");
        CHECK(r.matched && !r.valid);
    }

    Section("2. info file parse (last line wins)");
    {
        const auto path = std::filesystem::temp_directory_path() / "hero_level_cap_test.info.darkest";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "resistances: .stun 40%\n"
                << "level_cap: .max 10\n"
                << "combat_skill: .id \"smite\"\n"
                << "level_cap: .max 7\n";
        }
        CapMap caps;
        ParseInfoFile(path.wstring(), "Crusader", caps);
        CHECK(caps.size() == 1);
        CHECK(CapForClass(caps, "crusader") == 7);
        CHECK(CapForClass(caps, "CRUSADER") == 7);
        CHECK(CapForClass(caps, "vestal") == kDefaultCap);
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    Section("3. threshold extension");
    {
        const auto t = BuildThresholds(kVanillaThresholds, 7, 10);
        CHECK(t.size() == 11);
        CHECK(t[6] == 48);
        CHECK(t[7] == 60);  // 沿用最后一段步长 12
        CHECK(t[10] == 96);
    }
    {
        const auto t = BuildThresholds(nullptr, 0, 6);
        CHECK(t.size() == 7);
        CHECK(t == std::vector<std::uint32_t>(std::begin(kVanillaThresholds), std::end(kVanillaThresholds)));
    }
    {
        const std::uint32_t base[] = { 0, 2, 8 };
        const auto t = BuildThresholds(base, 3, 8);
        CHECK(t.size() == 9);
        CHECK(t[3] == 14);
    }

    Section("4. level computation matches vanilla and caps");
    {
        const auto t = BuildThresholds(kVanillaThresholds, 7, 10);
        CHECK(ComputeLevel(0, t, 6) == 0.0f);
        CHECK(ComputeLevel(1, t, 6) == 0.5f);
        CHECK(ComputeLevel(2, t, 6) == 1.0f);
        CHECK(ComputeLevel(48, t, 6) == 6.0f);
        CHECK(ComputeLevel(100, t, 6) == 6.0f);  // 默认上限
        CHECK(ComputeLevel(48, t, 10) == 6.0f);
        CHECK(ComputeLevel(60, t, 10) == 7.0f);
        CHECK(ComputeLevel(1000, t, 10) == 10.0f);
        CHECK(ComputeLevel(60, t, 4) == 4.0f);   // 低于原版上限的职业
    }
    {
        const auto t = BuildThresholds(kVanillaThresholds, 7, 6);
        CHECK(ComputeLevel(24, t, 6) == 4.0f);
        CHECK(ComputeLevel(30, t, 6) == 4.5f);
    }

    Section("5. inline class id read");
    {
        char source[64] = {};
        std::memcpy(source, "highwayman", 11);
        char out[64] = {};
        CHECK(ReadInlineId(source, out));
        CHECK(std::string(out) == "highwayman");
    }
    {
        char source[64] = {};
        std::memcpy(source, "bad id!", 8);
        char out[64] = {};
        CHECK(!ReadInlineId(source, out));
    }
    {
        char out[64] = {};
        CHECK(!ReadInlineId(nullptr, out));
    }

    std::printf("\n== ALL %d PASS, %d FAIL ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
