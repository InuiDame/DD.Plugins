#include "Rules.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>

namespace HeroLevelCap {
namespace {

std::string_view TrimView(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

bool EqualsNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && EqualsNoCase(s.substr(0, prefix.size()), prefix);
}

std::vector<std::string_view> SplitTokens(std::string_view s)
{
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ',' || s[i] == '\r' || s[i] == '\n')) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != ',' && s[i] != '\r' && s[i] != '\n') {
            ++i;
        }
        if (i > begin) out.push_back(s.substr(begin, i - begin));
    }
    return out;
}

bool ParseInt(std::string_view s, int& out)
{
    if (s.empty()) return false;
    int value = 0;
    const char* begin = s.data();
    const char* end = begin + s.size();
    const auto result = std::from_chars(begin, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    out = value;
    return true;
}

bool ReadTextFile(const std::wstring& path, std::string& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0) {
        out.resize(static_cast<std::size_t>(len));
        if (fread(out.data(), 1, out.size(), f) != out.size()) {
            out.clear();
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

bool HasHeroesComponent(const std::filesystem::path& p)
{
    for (const auto& part : p) {
        if (EqualsNoCase(part.string(), "heroes")) return true;
    }
    return false;
}

}  // namespace

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

InfoLineResult ParseInfoLine(std::string_view line)
{
    InfoLineResult result;
    const std::string_view text = TrimView(line);
    constexpr std::string_view kKey = "level_cap:";
    if (!StartsWithNoCase(text, kKey)) return result;
    result.matched = true;

    const auto tokens = SplitTokens(text.substr(kKey.size()));
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        int value = 0;
        if (EqualsNoCase(tokens[i], ".max") || EqualsNoCase(tokens[i], ".cap")) {
            if (i + 1 >= tokens.size() || !ParseInt(tokens[i + 1], value)) {
                WARN("level_cap: .max needs an integer, ignoring line");
                return result;
            }
            ++i;
        } else if (!ParseInt(tokens[i], value)) {
            WARN("level_cap: unknown token '{}', ignoring", std::string(tokens[i]));
            continue;
        }
        if (value < kMinCap || value > kMaxCap) {
            WARN("level_cap: {} out of range [{}, {}], ignoring", value, kMinCap, kMaxCap);
            return result;
        }
        result.cap = value;
        result.valid = true;
    }
    return result;
}

void ParseInfoFile(const std::wstring& path, const std::string& classId, CapMap& caps)
{
    std::string text;
    if (!ReadTextFile(path, text)) return;

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line(text.data() + pos,
            nl == std::string::npos ? text.size() - pos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        const auto parsed = ParseInfoLine(line);
        if (parsed.matched && parsed.valid) {
            const std::string id = Lower(classId);
            caps[id] = parsed.cap;
            INFO("level cap from info: class={} cap={} path={}", id, parsed.cap,
                std::filesystem::path(path).string());
        }
    }
}

void ScanHeroesRoot(const std::wstring& root, CapMap& caps)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& path = it->path();
        if (!it->is_regular_file(ec)) continue;
        const std::string name = path.filename().string();
        if (name.size() < 13 || name.compare(name.size() - 13, 13, ".info.darkest") != 0) continue;
        if (!HasHeroesComponent(path)) continue;

        // 职业名取文件所在目录名（ heroes/<class>/<class>.info.darkest ），和游戏一致。
        std::string classId = path.parent_path().filename().string();
        if (classId.empty()) classId = path.stem().string();
        ParseInfoFile(path.wstring(), classId, caps);
    }
}

void LoadRules(const std::wstring& gameRoot, CapMap& caps)
{
    const std::filesystem::path root(gameRoot);

    // 优先级低 -> 高：游戏本体、创意工坊、本地 MOD。
    ScanHeroesRoot((root / "heroes").wstring(), caps);

    std::filesystem::path workshop;
    if (root.has_parent_path() && root.parent_path().has_parent_path()) {
        workshop = root.parent_path().parent_path() / "workshop" / "content" / "262060";
    }
    if (!workshop.empty()) ScanHeroesRoot(workshop.wstring(), caps);

    ScanHeroesRoot((root / "mods").wstring(), caps);
}

int CapForClass(const CapMap& caps, const std::string& classId, int fallback)
{
    const auto it = caps.find(Lower(classId));
    if (it == caps.end()) return fallback;
    return it->second;
}

std::vector<std::uint32_t> BuildThresholds(const std::uint32_t* base, std::size_t count, int maxCap)
{
    std::vector<std::uint32_t> out;
    if (base && count > 0 && count <= 64) {
        out.assign(base, base + count);
    } else {
        out.assign(std::begin(kVanillaThresholds), std::end(kVanillaThresholds));
    }

    std::uint32_t step = 12;
    if (out.size() >= 2 && out.back() > out[out.size() - 2]) {
        step = out.back() - out[out.size() - 2];
    }

    const std::size_t wanted = static_cast<std::size_t>(std::max(maxCap, kDefaultCap)) + 1;
    while (out.size() < wanted) {
        const std::uint32_t next = out.back() + step;
        if (next <= out.back()) break;  // 溢出保护
        out.push_back(next);
    }
    return out;
}

float ComputeLevel(std::uint32_t xp, const std::vector<std::uint32_t>& thresholds, int cap)
{
    if (thresholds.empty()) return static_cast<float>(kDefaultCap);
    cap = std::clamp(cap, kMinCap, kMaxCap);

    std::size_t index = 0;
    while (index + 1 < thresholds.size() && xp >= thresholds[index + 1]) {
        ++index;
    }
    if (index > static_cast<std::size_t>(cap)) {
        return static_cast<float>(cap);
    }
    if (index == static_cast<std::size_t>(cap)) {
        return static_cast<float>(cap);
    }

    const float lo = static_cast<float>(thresholds[index]);
    const float hi = static_cast<float>(thresholds[index + 1]);
    if (hi <= lo) return static_cast<float>(cap);
    const float frac = std::clamp((static_cast<float>(xp) - lo) / (hi - lo), 0.0f, 1.0f);
    return static_cast<float>(index) + frac;
}

bool ReadInlineId(const void* p, char out[64])
{
    out[0] = 0;
    if (!p) return false;
    __try {
        const char* c = static_cast<const char*>(p);
        for (int i = 0; i < 63; ++i) {
            const char ch = c[i];
            if (ch == 0) {
                out[i] = 0;
                return i > 0;
            }
            const unsigned char u = static_cast<unsigned char>(ch);
            if (!std::isalnum(u) && ch != '_' && ch != '-' && ch != '.') return false;
            out[i] = ch;
        }
        out[63] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

}  // namespace HeroLevelCap
