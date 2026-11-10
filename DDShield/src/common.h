// DDShield - common definitions
#pragma once

namespace dds {

// ---- basic helpers --------------------------------------------------------
inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Vec2 { float x, y; };

inline std::string ToUtf8(std::wstring_view w) {
    return dku::string::utf16_to_utf8(w).value_or(std::string{});
}

} // namespace dds
