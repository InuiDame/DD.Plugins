#include "config.h"

namespace dds {

Config g_cfg;

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool ParseFloat3(const std::string& v, float& r, float& g, float& b, float& a) {
    float vals[4] = { r, g, b, a };
    int n = 0;
    size_t pos = 0;
    while (n < 4 && pos < v.size()) {
        size_t comma = v.find(',', pos);
        std::string part = Trim(v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (!part.empty()) vals[n++] = (float)atof(part.c_str());
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (n < 3) return false;
    r = vals[0]; g = vals[1]; b = vals[2];
    if (n >= 4) a = vals[3];
    return true;
}

bool Config::Load(const std::wstring& path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string text;
    text.resize((size_t)len);
    if (len > 0) fread(&text[0], 1, (size_t)len, f);
    fclose(f);

    std::string section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = Trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        bool on = (val == "1" || val == "true" || val == "yes" || val == "on");

        if (section == "general") {
            if (key == "enabled") enabled = on;
            else if (key == "debugLog") debugLog = on;
        } else if (section == "bar") {
            if (key == "show") barShow = on;
            else if (key == "height") barHeight = (float)atof(val.c_str());
            else if (key == "gap") barGap = (float)atof(val.c_str());
            else if (key == "yOffset") barYOffset = (float)atof(val.c_str());
            else if (key == "xOffset") barXOffset = (float)atof(val.c_str());
            else if (key == "widthScale") barWidthScale = (float)atof(val.c_str());
            else if (key == "alwaysShow" || key == "showWhenFull") barAlwaysShow = on;
            else if (key == "border") barBorder = on;
            else if (key == "autoScale") autoScale = on;
            else if (key == "stretch") barStretch = on;
            else if (key == "uiWidth") uiWidth = (float)atof(val.c_str());
            else if (key == "uiHeight") uiHeight = (float)atof(val.c_str());
            else if (key == "scaleMul") barScaleMul = (float)atof(val.c_str());
            else if (key == "useLayoutOffset") barUseLayoutOffset = on;
            else if (key == "tooltip") tooltip = on;
            else if (key == "calibrate") calibrate = on;
            else if (key == "fillColor") ParseFloat3(val, fillR, fillG, fillB, fillA);
            else if (key == "backColor") ParseFloat3(val, backR, backG, backB, backA);
            else if (key == "borderColor") ParseFloat3(val, borderR, borderG, borderB, borderA);
        } else if (section == "shield") {
            if (key == "defaultMax") defaultMax = (float)atof(val.c_str());
            else if (key == "startFlat") startFlat = (float)atof(val.c_str());
            else if (key == "startHpRatio") startHpRatio = (float)atof(val.c_str());
            else if (key == "maxHpRatio") maxHpRatio = (float)atof(val.c_str());
            else if (key == "mirrorHeal") mirrorHeal = (float)atof(val.c_str());
            else if (key == "classIdOffset") classIdOffset = atoi(val.c_str());
            else if (key == "actionOffset") actionOffset = (int)strtol(val.c_str(), nullptr, 0);
            else if (key == "barHoldMs") barHoldMs = atoi(val.c_str());
            else if (key == "hideWhenCursor") hideWhenCursor = val == "1" || _stricmp(val.c_str(), "true") == 0;
            else if (key == "hideInPanels") hideInPanels = val == "1" || _stricmp(val.c_str(), "true") == 0;
            else if (key == "classShield") classShield = val == "1" || _stricmp(val.c_str(), "true") == 0;
            else if (key == "hideActing") hideActing = val == "1" || _stricmp(val.c_str(), "true") == 0;
            else if (key == "actingValues") {
                actingValueCount = 0;
                const char* p = val.c_str();
                while (*p && actingValueCount < 32) {
                    char* endp = nullptr;
                    long v = strtol(p, &endp, 0);
                    if (endp == p) break;
                    actingValues[actingValueCount++] = (int)v;
                    p = endp;
                    while (*p == ',' || *p == ' ') ++p;
                }
            }
            else if (key == "actionAllow") {
                actionAllowCount = 0;
                const char* p = val.c_str();
                while (*p && actionAllowCount < 32) {
                    char* endp = nullptr;
                    long v = strtol(p, &endp, 0);
                    if (endp == p) break;
                    actionAllow[actionAllowCount++] = (int)v;
                    p = endp;
                    while (*p == ',' || *p == ' ') ++p;
                }
            }
            else if (key == "regenPerRound") regenPerRound = (float)atof(val.c_str());
            else if (key == "decayPerRound") decayPerRound = (float)atof(val.c_str());
        }
    }
    return true;
}

bool Config::Save(const std::wstring& path) const {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return false;
    fprintf(f, "; DDShield configuration (auto-saved by F10)\n");
    fprintf(f, "[general]\nenabled = %d\ndebugLog = %d\n\n", (int)enabled, (int)debugLog);
    fprintf(f, "[bar]\nshow = %d\nheight = %.1f\ngap = %.1f\nxOffset = %.1f\nyOffset = %.1f\n",
            (int)barShow, barHeight, barGap, barXOffset, barYOffset);
    fprintf(f, "widthScale = %.3f\nalwaysShow = %d\nborder = %d\n",
            barWidthScale, (int)barAlwaysShow, (int)barBorder);
    fprintf(f, "autoScale = %d\nuiWidth = %.0f\nuiHeight = %.0f\nscaleMul = %.3f\n",
            (int)autoScale, uiWidth, uiHeight, barScaleMul);
    fprintf(f, "tooltip = %d\ncalibrate = %d\n", (int)tooltip, (int)calibrate);
    fprintf(f, "fillColor = %.2f, %.2f, %.2f, %.2f\n", fillR, fillG, fillB, fillA);
    fprintf(f, "backColor = %.2f, %.2f, %.2f, %.2f\n", backR, backG, backB, backA);
    fprintf(f, "borderColor = %.2f, %.2f, %.2f, %.2f\n\n", borderR, borderG, borderB, borderA);
    fprintf(f, "[shield]\ndefaultMax = %.1f\nstartFlat = %.1f\nstartHpRatio = %.3f\n",
            defaultMax, startFlat, startHpRatio);
    fprintf(f, "maxHpRatio = %.3f\nmirrorHeal = %.3f\nregenPerRound = %.1f\ndecayPerRound = %.1f\n",
            maxHpRatio, mirrorHeal, regenPerRound, decayPerRound);
    fclose(f);
    return true;
}

} // namespace dds

