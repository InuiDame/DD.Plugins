#include "shield.h"
#include "config.h"
#include "game.h"
#include "json.h"

namespace dds {

thread_local int t_inDamage = 0;
thread_local void* t_damageAttacker = nullptr;
bool g_menuOpen = false;
bool g_panelOpen = false;
namespace { DWORD g_actingUntil = 0; }

void ShieldRecomputeBuffs(void* actor);

namespace {

ShieldMap g_shields;

struct BuffShieldDef {
    float maxAdd = 0.0f;
    float maxPct = 0.0f;
    float start = 0.0f;
    float gain = 0.0f;
    float regen = 0.0f;
    float decay = 0.0f;
    bool any() const { return maxAdd != 0.0f || maxPct != 0.0f || start != 0.0f || gain != 0.0f || regen != 0.0f || decay != 0.0f; }
};
std::unordered_map<std::string, BuffShieldDef> g_buffDefs;
std::unordered_map<std::string, ShieldEffectSpec> g_specs;
std::unordered_map<const void*, ShieldEffectSpec*> g_specByPtr;
std::vector<std::string> g_pendingNames;
DWORD g_lastResolve = 0;

void* SafeLookup(const char* name) {
    void* p = nullptr;
    __try {
        p = g_game.effectLookup(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

ShieldState& GetState(void* actor) {
    auto it = g_shields.find(actor);
    if (it == g_shields.end()) {
        ShieldState s;
        g_shields.emplace(actor, s);
        return g_shields[actor];
    }
    return it->second;
}

// ---- per-class shield from heroes/<class>/<class>.info.darkest -------------
struct ClassShieldDef {
    bool hasMax = false;
    bool hasStart = false;
    float max = 0.0f;
    float start = 0.0f;
};
std::unordered_map<std::string, ClassShieldDef> g_classDefs;

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)len);
    if (len > 0) fread(&out[0], 1, (size_t)len, f);
    fclose(f);
    return true;
}

// `shield: .max 25 .start 25` (a number without a key sets both).
void ParseInfoShield(const std::wstring& path, const std::string& classId) {
    std::string text;
    if (!ReadTextFile(path, text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        if (line.compare(a, 7, "shield:") != 0) continue;

        ClassShieldDef def;
        std::string rest = line.substr(a + 7);
        bool any = false;
        size_t i = 0;
        while (i < rest.size()) {
            if (rest.compare(i, 4, ".max") == 0) {
                def.max = (float)atof(rest.c_str() + i + 4);
                def.hasMax = true; any = true; i += 4;
            } else if (rest.compare(i, 6, ".start") == 0) {
                def.start = (float)atof(rest.c_str() + i + 6);
                def.hasStart = true; any = true; i += 6;
            } else if (rest.compare(i, 6, ".value") == 0) {
                float v = (float)atof(rest.c_str() + i + 6);
                def.max = v; def.hasMax = true;
                def.start = v; def.hasStart = true;
                any = true; i += 6;
            } else {
                ++i;
            }
        }
        if (!any) {
            // bare value: "shield: 25"
            float v = (float)atof(rest.c_str());
            if (v > 0.0f) {
                def.max = v; def.hasMax = true;
                def.start = v; def.hasStart = true;
                any = true;
            }
        }
        if (!any) continue;
        def.max = Clamp(def.max, 0.0f, 100000.0f);
        def.start = Clamp(def.start, 0.0f, 100000.0f);
        g_classDefs[Lower(classId)] = def;
        INFO("class shield from {}: id={} max={:.1f} start={:.1f}",
            ToUtf8(path), classId, def.max, def.start);
        return;
    }
}

void ScanHeroesRoot(const std::wstring& heroesDir, const std::wstring& sourceTag) {
    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = heroesDir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring classDir = heroesDir + L"\\" + fd.cFileName;
        std::string classId = ToUtf8(fd.cFileName);

        WIN32_FIND_DATAW fd2 = {};
        std::wstring pattern2 = classDir + L"\\*.info.darkest";
        HANDLE h2 = FindFirstFileW(pattern2.c_str(), &fd2);
        if (h2 != INVALID_HANDLE_VALUE) {
            do {
                ParseInfoShield(classDir + L"\\" + fd2.cFileName, classId);
            } while (FindNextFileW(h2, &fd2));
            FindClose(h2);
        }
        (void)sourceTag;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool IdCharsOk(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
}

// Reads a NUL-terminated id at an arbitrary address, validating its charset.
bool ReadIdInline(const void* p, char out[64]) {
    out[0] = 0;
    __try {
        const char* c = (const char*)p;
        for (int i = 0; i < 63; ++i) {
            char ch = c[i];
            if (ch == 0) { out[i] = 0; return i > 0; }
            if (!IdCharsOk(ch)) return false;
            out[i] = ch;
        }
        out[63] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MatchClassId(const char* id) {
    if (!id[0]) return false;
    for (auto& kv : g_classDefs) {
        if (_stricmp(kv.first.c_str(), id) == 0) return true;
    }
    return false;
}

bool PlausiblePtr(uintptr_t p) {
    return p > 0x10000 && p < 0x00007FFFFFFFFFFFull;
}

// Scans a memory region for a string that equals one of the loaded class ids,
// either stored inline (`deref == false`) or behind a pointer (`deref == true`).
bool ScanRegion(const void* base, size_t size, int step, bool deref,
                char out[64], int* foundOff) {
    const uint8_t* b = (const uint8_t*)base;
    for (size_t off = 0; off + 8 <= size; off += (size_t)step) {
        if (deref) {
            uintptr_t p = 0;
            __try {
                p = *(const uintptr_t*)(b + off);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (!PlausiblePtr(p)) continue;
            if (ReadIdInline((const void*)p, out) && MatchClassId(out)) {
                if (foundOff) *foundOff = (int)off;
                return true;
            }
        } else {
            if (ReadIdInline(b + off, out) && MatchClassId(out)) {
                if (foundOff) *foundOff = (int)off;
                return true;
            }
        }
    }
    return false;
}

// The class id is somewhere reachable from the Actor / its ActorClass object.
// Try the likely places in order and log which one matched.
// Only heroes carry a HeroClass, and only they can have a class shield block.
// Monsters/corpses would otherwise match a neighbouring string in memory.
bool IsHeroActor(void* actor) {
    __try {
        uintptr_t vt = *(const uintptr_t*)actor;
        return vt - ModuleBase() == 0xE8A288;   // .?AVHero@@ vtable
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Skill ids start with the class id (e.g. "huohuo1"), so a real match must be
// backed by at least one such string inside the actor. This rejects neighbours
// whose memory merely happens to point at some class object.
bool ActorHasSkillPrefix(void* actor, const char* id) {
    const size_t n = strlen(id);
    __try {
        const char* p = (const char*)actor;
        for (int off = 0; off < 0x4000; ++off) {
            if (memcmp(p + off, id, n) != 0) continue;
            char c = p[off + n];
            if ((c >= '0' && c <= '9') || c == '_') return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// Hero+0x1320 is the HeroClass pointer: the Hero copy constructor copies it
// (0x1404a1281) and other code dereferences `[hero+0x1320] + 0x1228/0xd4`.
// The class id is the char buffer at +8 of that object.
bool ReadClassId(void* actor, char out[64], int* usedOffset) {
    if (usedOffset) *usedOffset = 0x1320;
    if (g_classDefs.empty()) return false;
    if (!IsHeroActor(actor)) return false;

    void* cls = nullptr;
    __try {
        cls = *(void**)((char*)actor + 0x1320);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!cls) return false;

    __try {
        const char* p = (const char*)cls + 8;
        for (int i = 0; i < 63; ++i) {
            char c = p[i];
            if (c == 0) { out[i] = 0; return i > 0; }
            if (!IdCharsOk(c)) return false;
            out[i] = c;
        }
        out[63] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EnsureInit(void* actor, ShieldState& s) {
    if (s.initialized) return;
    s.initialized = true;
    const bool needMaxHp = (g_cfg.maxHpRatio > 0.0f || g_cfg.startHpRatio > 0.0f);
    float maxHp = needMaxHp ? ActorMaxHealth(actor) : 0.0f;
    float cap = g_cfg.defaultMax;
    if (g_cfg.maxHpRatio > 0.0f && maxHp > 0.0f)
        cap = g_cfg.maxHpRatio * maxHp;
    s.max = cap;
    float start = g_cfg.startFlat;
    if (g_cfg.startHpRatio > 0.0f && maxHp > 0.0f)
        start += g_cfg.startHpRatio * maxHp;

    // Per-class shield authored in heroes/<class>/<class>.info.darkest.
    char classId[64];
    int usedOff = -1;
    bool haveClass = g_cfg.classShield && ReadClassId(actor, classId, &usedOff);
    if (haveClass) {
        auto it = g_classDefs.find(Lower(classId));
        if (it != g_classDefs.end()) {
            const ClassShieldDef& def = it->second;
            if (def.hasMax) cap = def.max;
            if (def.hasStart) start = def.start;
            DEBUG("class shield: id={} (offset {:#x}) max={:.1f} start={:.1f}", classId, usedOff,
                def.hasMax ? def.max : -1.0f, def.hasStart ? def.start : -1.0f);
        } else {
            DEBUG("class id={} (no shield block)", classId);
        }
    }

    s.baseMax = cap;
    s.max = cap;
    s.cur = Clamp(start, 0.0f, s.max > 0.0f ? s.max : start);
    ShieldRecomputeBuffs(actor);           // may raise max / grant from active buffs
    if (s.max <= 0.0f) s.max = s.cur;
    DEBUG("shield init actor={} maxHp={:.1f} base={:.1f} max={:.1f} cur={:.1f}", actor, maxHp, s.baseMax, s.max, s.cur);
}

int ActorAction(void* actor) {
    __try {
        return *(int*)((char*)actor + g_cfg.actionOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool ActionAllowed(int a) {
    if (g_cfg.actionAllowCount <= 0) return true;
    for (int i = 0; i < g_cfg.actionAllowCount; ++i)
        if (g_cfg.actionAllow[i] == a) return true;
    return false;
}

// Names such as shield_gain_37 / shield_self_max_12 / shield_regen_3_4 are
// understood without a JSON entry, so new numbers need no rebuild and no
// extra config: only the .darkest effect definition.
bool ParseNameSpec(const char* name, ShieldEffectSpec& out) {
    const char* p = name;
    if (strncmp(p, "shield_", 7) != 0) return false;
    p += 7;
    if (strncmp(p, "self_", 5) == 0) p += 5;

    auto num = [](const char* s) { return (float)atoi(s); };
    if (strncmp(p, "gain_pct_", 9) == 0)   { out.op = "addpct";    out.value = num(p + 9);  return true; }
    if (strncmp(p, "heal_pct_", 9) == 0)   { out.op = "addpct";    out.value = num(p + 9);  return true; }
    if (strncmp(p, "max_pct_", 8) == 0)    { out.op = "maxpct";    out.value = num(p + 8);  return true; }
    if (strncmp(p, "damage_pct_", 11) == 0){ out.op = "damagepct"; out.value = num(p + 11); return true; }
    if (strncmp(p, "gain_", 5) == 0)       { out.op = "add";       out.value = num(p + 5);  return true; }
    if (strncmp(p, "heal_", 5) == 0)       { out.op = "add";       out.value = num(p + 5);  return true; }
    if (strncmp(p, "max_", 4) == 0)        { out.op = "max";       out.value = num(p + 4);  return true; }
    if (strncmp(p, "damage_", 7) == 0)     { out.op = "damage";    out.value = num(p + 7);  return true; }
    if (strncmp(p, "regen_", 6) == 0) {
        out.op = "regen";
        out.value = (float)atoi(p + 6);
        const char* us = strchr(p + 6, '_');
        out.duration = us ? atoi(us + 1) : 1;
        return true;
    }
    if (strncmp(p, "decay_", 6) == 0) {
        out.op = "decay";
        out.value = (float)atoi(p + 6);
        const char* us = strchr(p + 6, '_');
        out.duration = us ? atoi(us + 1) : 1;
        return true;
    }
    if (strcmp(p, "pierce") == 0)          { out.op = "pierce";    out.value = 100.0f;      return true; }
    if (strncmp(p, "pierce_", 7) == 0)     { out.op = "pierce";    out.value = num(p + 7);  return true; }
    if (strcmp(p, "full") == 0)  { out.op = "full";  return true; }
    if (strcmp(p, "break") == 0) { out.op = "break"; return true; }
    return false;
}

// Total cap = (class/config + effect bonuses) scaled by buff percentage, plus
// flat buff bonuses. Keeping the layers separate lets buffs expire without
// throwing away cap that a skill effect granted.
void RecalcMax(ShieldState& s) {
    float m = (s.baseMax + s.effectMax) * (1.0f + s.buffMaxPct / 100.0f) + s.buffMaxAdd;
    if (m < 0.0f) m = 0.0f;
    s.max = m;
    if (s.cur > s.max) s.cur = s.max;
    if (s.cur < 0.0f) s.cur = 0.0f;
}

void ApplySpec(void* target, const ShieldEffectSpec& spec, void* performer) {
    if (!target) return;
    ShieldState& s = GetState(target);
    EnsureInit(target, s);
    const std::string& op = spec.op;
    if (op == "add" || op == "heal") {
        s.cur += spec.value;
    } else if (op == "addpct") {
        s.cur += s.max * spec.value / 100.0f;
    } else if (op == "max") {
        s.effectMax += spec.value;
        if (s.effectMax < 0.0f) s.effectMax = 0.0f;
        RecalcMax(s);
    } else if (op == "maxpct") {
        s.effectMax += (s.baseMax + s.effectMax) * spec.value / 100.0f;
        if (s.effectMax < 0.0f) s.effectMax = 0.0f;
        RecalcMax(s);
    } else if (op == "set") {
        s.cur = spec.value;
    } else if (op == "full") {
        s.cur = s.max;
    } else if (op == "break" || op == "clear") {
        s.cur = 0.0f;
    } else if (op == "damage") {
        s.cur -= spec.value;
    } else if (op == "damagepct") {
        s.cur -= s.max * spec.value / 100.0f;
    } else if (op == "pierce") {
        // effect 先于伤害结算，这里记下，由紧接着的这次命中消耗
        s.piercePct = Clamp(spec.value, 0.0f, 100.0f);
        s.pierceAttacker = performer;
    } else if (op == "regen") {
        s.regen += spec.value;
        s.regenTurns = spec.duration > 0 ? spec.duration : 1;
    } else if (op == "decay") {
        s.decay += spec.value;
        s.decayTurns = spec.duration > 0 ? spec.duration : 1;
    }
    if (s.cur > s.max && s.max > 0.0f) s.cur = s.max;
    if (s.cur < 0.0f) s.cur = 0.0f;
    if (s.max <= 0.0f && s.cur > 0.0f) s.max = s.cur;   // first grant defines the cap
    DEBUG("shield effect op={} value={:.1f} -> cur={:.1f}/{:.1f}", op, spec.value, s.cur, s.max);
}

} // namespace

ShieldMap& Shields() { return g_shields; }

void ShieldLoadEffects(const std::wstring& jsonPath) {
    FILE* f = nullptr;
    _wfopen_s(&f, jsonPath.c_str(), L"rb");
    if (!f) {
        WARN("no shield effect file: {}", ToUtf8(jsonPath));
        return;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string text;
    text.resize((size_t)len);
    if (len > 0) fread(&text[0], 1, (size_t)len, f);
    fclose(f);

    JsonValue root;
    if (!JsonParseFile(text, root)) {
        WARN("shield effect file parse error");
        return;
    }
    const JsonValue* effects = root.Find("effects");
    if (!effects || effects->type != JsonValue::Object) {
        WARN("shield effect file: missing 'effects' object");
        return;
    }
    for (auto& kv : effects->obj) {
        ShieldEffectSpec spec;
        if (kv.second.type == JsonValue::Object) {
            if (const JsonValue* op = kv.second.Find("op")) spec.op = op->Str();
            if (const JsonValue* v = kv.second.Find("value")) spec.value = (float)v->Num();
            if (const JsonValue* d = kv.second.Find("duration")) spec.duration = d->Int();
        } else if (kv.second.type == JsonValue::Number) {
            spec.op = "add";
            spec.value = (float)kv.second.Num();
        }
        if (spec.op.empty()) continue;
        g_specs[kv.first] = spec;
    }
    INFO("loaded {} shield effect definitions", g_specs.size());
}

bool ShieldHasEffects() { return !g_specs.empty(); }

namespace {
std::wstring ParentOf(std::wstring p) {
    if (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return p.substr(0, slash + 1);
}
} // namespace

void ShieldLoadClasses(const std::wstring& gameRoot) {
    g_classDefs.clear();

    // 1) base game
    ScanHeroesRoot(gameRoot + L"heroes", L"base");

    // 2) local mods
    WIN32_FIND_DATAW fd = {};
    std::wstring modsPattern = gameRoot + L"mods\\*";
    HANDLE h = FindFirstFileW(modsPattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            ScanHeroesRoot(gameRoot + L"mods\\" + fd.cFileName + L"\\heroes", L"mod");
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 3) Steam Workshop
    std::wstring steamapps = ParentOf(ParentOf(gameRoot));   // <steamapps>/
    if (!steamapps.empty()) {
        std::wstring wsPattern = steamapps + L"workshop\\content\\262060\\*";
        WIN32_FIND_DATAW fd2 = {};
        HANDLE h2 = FindFirstFileW(wsPattern.c_str(), &fd2);
        if (h2 != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd2.cFileName, L".") == 0 || wcscmp(fd2.cFileName, L"..") == 0) continue;
                ScanHeroesRoot(steamapps + L"workshop\\content\\262060\\" + fd2.cFileName + L"\\heroes", L"workshop");
            } while (FindNextFileW(h2, &fd2));
            FindClose(h2);
        }
    }

    INFO("class shield definitions: {}", g_classDefs.size());
}

void ShieldTick() {
    ShieldPrune(GetTickCount());
    // Buff effects (shield_max/shield_regen/...) are re-evaluated on a timer so
    // expiry is picked up even when the game does not call ResetStats.
    if (!g_buffDefs.empty()) {
        for (auto& kv : g_shields) ShieldRecomputeBuffs(kv.first);
    }
}

void ShieldOnActorBar(void* actor, Vec2 pos, int rank, bool shared) {
    if (!actor) return;
    ShieldState& s = GetState(actor);
    EnsureInit(actor, s);

    s.geo.x = pos.x;
    s.geo.y = pos.y;
    s.geo.rank = rank;
    s.geo.shared = shared;
    s.geo.width = BarWidth(rank, shared);
    s.geo.valid = true;

    // Optional action filter: only draw while the actor is in one of the
    // whitelisted action states (idle / combat idle / walk by default empty).
    s.action = ActorAction(actor);
    // 0/1/2 are the idle / walk / combat-idle states observed in game; any other
    // value means this actor is mid-animation. While any actor is, hide every
    // bar so the overlay matches the game's own HUD behaviour.
    for (int i = 0; i < g_cfg.actingValueCount; ++i) {
        if (s.action == g_cfg.actingValues[i]) {
            g_actingUntil = GetTickCount() + 400;
            break;
        }
    }
    if (g_cfg.debugLog && s.action != s.loggedAction) {
        static DWORD lastChange = 0;
        DWORD nowMs = GetTickCount();
        if (nowMs - lastChange > 120) {
            lastChange = nowMs;
            s.loggedAction = s.action;
            DEBUG("action change actor={} action={}", actor, s.action);
        }
    }
    if (!ActionAllowed(s.action)) s.geo.valid = false;

    if (g_cfg.debugLog) {
        static DWORD lastLog = 0;
        DWORD now = GetTickCount();
        if (now - lastLog > 1000) {
            lastLog = now;
            float* layout = (float*)(ModuleBase() + 0x10AA768);
            DEBUG("bar actor={} pos=({:.1f},{:.1f}) rank={} action={} width={:.1f} hp={:.1f} layoutOff=({:.1f},{:.1f})",
                actor, pos.x, pos.y, rank, s.action, s.geo.width, ActorHealth(actor), layout[8], layout[9]);
        }
    }

    s.lastSeen = GetTickCount();

    float hp = ActorHealth(actor);
    if (s.lastHealth < 0.0f) {
        s.lastHealth = hp;
    } else {
        // Ignore the health we refunded ourselves during a partial absorb.
        float delta = hp - s.lastHealth - s.pendingHealthCredit;
        if (delta > 0.01f && g_cfg.mirrorHeal > 0.0f)
            s.cur = Clamp(s.cur + delta * g_cfg.mirrorHeal, 0.0f, s.max);
    }
    s.pendingHealthCredit = 0.0f;
    s.lastHealth = hp;
    if (hp <= 0.0f) {
        s.cur = 0.0f;
    }
}

ShieldState* FindShield(void* actor) {
    auto it = g_shields.find(actor);
    return it == g_shields.end() ? nullptr : &it->second;
}

AbsorbKind ShieldAbsorbFinal(void* actor, float damage, float& creditOut) {
    creditOut = 0.0f;
    if (!actor || damage <= 0.0f) return AbsorbKind::None;
    ShieldState* sp = FindShield(actor);
    if (!sp || sp->cur <= 0.0f) return AbsorbKind::None;

    ShieldState& s = *sp;
    // 穿透的部分直接打血，护盾只吸收剩下的部分
    const float pierce = s.pierceAttacker && s.pierceAttacker == t_damageAttacker ? s.piercePct : 0.0f;
    float absorb = std::min(s.cur, damage * (1.0f - pierce / 100.0f));
    s.cur -= absorb;
    DEBUG("absorb actor={} final={:.1f} pierce={:.0f} absorbed={:.1f} left={:.1f}/{:.1f}", actor, damage, pierce, absorb, s.cur, s.max);
    if (absorb >= damage - 0.001f) return AbsorbKind::Full;
    // The engine is about to subtract the full `damage` from health.  Adding
    // the absorbed part back first makes the net loss exactly (damage - absorb).
    creditOut = absorb;
    return AbsorbKind::Partial;
}

void ShieldPrune(DWORD now) {
    // An actor whose bar stops being drawn (battle end, retreat, death) is
    // dropped after a short grace period so no stale bar is left on screen.
    for (auto it = g_shields.begin(); it != g_shields.end();) {
        ShieldState& s = it->second;
        if (s.lastSeen != 0 && now - s.lastSeen > 2500) {
            DEBUG("shield state expired actor={}", it->first);
            it = g_shields.erase(it);
        } else {
            ++it;
        }
    }
}

void ShieldOnRound() {
    for (auto& kv : g_shields) {
        ShieldState& s = kv.second;
        if (!s.initialized) continue;
        // 没被命中消耗的穿透（技能未造成伤害）不留到下一回合
        s.piercePct = 0.0f;
        s.pierceAttacker = nullptr;
        if (g_cfg.regenPerRound != 0.0f) s.cur += g_cfg.regenPerRound;
        if (g_cfg.decayPerRound != 0.0f) s.cur -= g_cfg.decayPerRound;
        if (s.buffRegen != 0.0f) s.cur += s.buffRegen;
        if (s.buffDecay != 0.0f) s.cur -= s.buffDecay;
        if (s.regenTurns > 0) {
            s.cur += s.regen;
            if (--s.regenTurns == 0) s.regen = 0.0f;
        }
        if (s.decayTurns > 0) {
            s.cur -= s.decay;
            if (--s.decayTurns == 0) s.decay = 0.0f;
        }
        s.cur = Clamp(s.cur, 0.0f, s.max > 0.0f ? s.max : s.cur);
    }
}

void ShieldEndHit(void* actor, void* attacker) {
    ShieldState* s = FindShield(actor);
    if (s && s->pierceAttacker == attacker) {
        s->piercePct = 0.0f;
        s->pierceAttacker = nullptr;
    }
}

int ShieldStatus(void* actor) {
    const ShieldState* s = FindShield(actor);
    return s && s->cur > 0.0f ? 1 : 0;
}

void ShieldOnEffectApply(void* target, const void* effect, void* performer) {
    if (!effect) return;
    // Effect::name is a 64-byte char array at offset 0 (the effect parser
    // copies the ".name" string there; the name hash follows at +0x40).
    char name[65];
    memcpy(name, effect, 64);
    name[64] = 0;
    if (name[0]) {
        if (g_cfg.debugLog) {
            static std::unordered_map<std::string, int> allSeen;
            if (allSeen.size() < 400 && allSeen.emplace(name, 1).second)
                DEBUG("effect seen: {}", name);
        }
        auto it = g_specs.find(name);
        ShieldEffectSpec parsed;
        const bool found = it != g_specs.end();
        const bool parsedOk = !found && ParseNameSpec(name, parsed);
        if (found || parsedOk) {
            if (g_cfg.debugLog) {
                static std::unordered_map<std::string, int> seen;
                if (seen[name]++ == 0)
                    DEBUG("effect matched: {} (json={})", name, found);
            }
            ApplySpec(target, found ? it->second : parsed, performer);
            return;
        }
    }
    // Fallback: pointer identity, in case the name layout ever changes.
    auto pit = g_specByPtr.find(effect);
    if (pit != g_specByPtr.end()) ApplySpec(target, *pit->second, performer);
}

// ---- buff-provided shield stats -------------------------------------------
namespace {

bool RegionHasString(const uint8_t* p, size_t n, const char* s) {
    const size_t len = strlen(s);
    if (len == 0 || n < len) return false;
    const uint8_t first = (uint8_t)s[0];
    for (size_t i = 0; i + len <= n; ++i) {
        if (p[i] != first) continue;
        if (memcmp(p + i, s, len) == 0) return true;
    }
    return false;
}

bool ReadBuffVector(void* actor, const uint8_t** begin, const uint8_t** end) {
    __try {
        *begin = *(const uint8_t**)((const char*)actor + 0x460);
        *end = *(const uint8_t**)((const char*)actor + 0x468);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ParseBuffFile(const std::wstring& path) {    std::string text;
    if (!ReadTextFile(path, text)) return;
    if (text.size() > 16 * 1024 * 1024) return;
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF)
        text.erase(0, 3);                                     // UTF-8 BOM
    if (text.find("\"shield") == std::string::npos) return;   // cheap pre-filter
    JsonValue root;
    JsonParser parser(text);
    if (!parser.Parse(root)) return;
    const JsonValue* buffs = root.Find("buffs");
    if (!buffs || buffs->type != JsonValue::Array) return;
    for (auto& b : buffs->arr) {
        if (b.type != JsonValue::Object) continue;
        const JsonValue* idv = b.Find("id");
        if (!idv || idv->type != JsonValue::String) continue;
        const std::string id = idv->str;
        if (id.empty()) continue;
        BuffShieldDef def;
        if (auto* v = b.Find("shield_max")) def.maxAdd = (float)v->Num();
        if (auto* v = b.Find("shield_max_pct")) def.maxPct = (float)v->Num();
        if (auto* v = b.Find("shield_start")) def.start = (float)v->Num();
        if (auto* v = b.Find("shield_regen")) def.regen = (float)v->Num();
        if (auto* v = b.Find("shield_decay")) def.decay = (float)v->Num();
        // `shield` / `shield_gain` only grants current shield; max stays separate.
        if (auto* v = b.Find("shield")) def.gain = (float)v->Num();
        if (auto* v = b.Find("shield_gain")) def.gain = (float)v->Num();
        if (!def.any()) continue;
        g_buffDefs[id] = def;
        INFO("buff shield def: {} max+{:.1f} pct+{:.1f} gain+{:.1f} regen{:.1f} decay{:.1f}",
            id, def.maxAdd, def.maxPct, def.gain, def.regen, def.decay);
    }
}

void ScanBuffDir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = dir + L"\\*.buffs.json";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (++n > 250) break;
        ParseBuffFile(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

void ShieldLoadBuffs(const std::wstring& gameRoot) {
    g_buffDefs.clear();
    ScanBuffDir(gameRoot + L"shared\\buffs");

    WIN32_FIND_DATAW fd = {};
    std::wstring modsPattern = gameRoot + L"mods\\*";
    HANDLE h = FindFirstFileW(modsPattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            ScanBuffDir(gameRoot + L"mods\\" + fd.cFileName + L"\\shared\\buffs");
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::wstring steamapps = ParentOf(ParentOf(gameRoot));
    if (!steamapps.empty()) {
        std::wstring wsPattern = steamapps + L"workshop\\content\\262060\\*";
        WIN32_FIND_DATAW fd2 = {};
        HANDLE h2 = FindFirstFileW(wsPattern.c_str(), &fd2);
        if (h2 != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd2.cFileName, L".") == 0 || wcscmp(fd2.cFileName, L"..") == 0) continue;
                ScanBuffDir(steamapps + L"workshop\\content\\262060\\" + fd2.cFileName + L"\\shared\\buffs");
            } while (FindNextFileW(h2, &fd2));
            FindClose(h2);
        }
    }
    INFO("buff shield definitions: {}", g_buffDefs.size());
}

void ShieldRecomputeBuffs(void* actor) {
    auto it = g_shields.find(actor);
    if (it == g_shields.end()) return;
    ShieldState& s = it->second;
    if (!s.initialized || g_buffDefs.empty()) return;

    const DWORD now = GetTickCount();
    if (now - s.lastBuffScan < 150) return;
    s.lastBuffScan = now;

    const uint8_t* begin = nullptr;
    const uint8_t* end = nullptr;
    if (!ReadBuffVector(actor, &begin, &end)) return;
    if (!begin || !end || end <= begin) return;
    size_t bytes = (size_t)(end - begin);
    if (bytes > 0x40000) bytes = 0x40000;

    float add = 0.0f, pct = 0.0f, gain = 0.0f, regen = 0.0f, decay = 0.0f;
    for (auto& kv : g_buffDefs) {
        if (!RegionHasString(begin, bytes, kv.first.c_str())) continue;
        add += kv.second.maxAdd;
        pct += kv.second.maxPct;
        gain += kv.second.gain;
        regen += kv.second.regen;
        decay += kv.second.decay;
        if (g_cfg.debugLog) {
            static std::unordered_map<std::string, int> seen;
            if (seen[kv.first]++ == 0) DEBUG("buff active: {}", kv.first);
        }
    }
    const float prevGain = s.buffGain;
    s.buffMaxAdd = add;
    s.buffMaxPct = pct;
    s.buffGain = gain;
    s.buffRegen = regen;
    s.buffDecay = decay;

    // `shield` / `shield_gain` grant current shield once when the buff appears;
    // `shield_max` only moves the cap.
    if (gain > prevGain + 0.001f) s.cur += (gain - prevGain);

    const float oldMax = s.max;
    RecalcMax(s);
    if (s.max != oldMax)
        DEBUG("buff shield max {:.1f} -> {:.1f} (base={:.1f} effect={:.1f} add={:.1f} pct={:.1f} actor={})",
            oldMax, s.max, s.baseMax, s.effectMax, add, pct, actor);
}

bool ShieldActingNow() {
    return GetTickCount() < g_actingUntil;
}

void ShieldOnBlockPopup(int type) {
    // The engine presents a shield block as popup type 0x31; its colour id sits
    // in a runtime-initialised table (stride 0xDC) that the popup function
    // loads with `lea rXX, [rip + disp]`. Locate that table from the function
    // itself so the DRM-free build (different layout) is handled too.
    if (type != 0x31) return;
    static int attempts = 0;
    static bool patched = false;
    if (patched || attempts >= 5) return;
    ++attempts;

    void* fn = (void*)g_game.blockPopup;
    if (!fn) return;

    uint8_t* table = nullptr;
    __try {
        const uint8_t* p = (const uint8_t*)fn;
        const uint8_t* imgBase = (const uint8_t*)ModuleBase();
        const uint8_t* imgEnd = imgBase + ModuleSize();
        for (int i = 0; i + 7 <= 0x100; ++i) {
            if (p[i] == 0x4C && p[i + 1] == 0x8D && p[i + 2] == 0x3D) {
                int32_t disp = *(const int32_t*)(p + i + 3);
                uint8_t* cand = (uint8_t*)(p + i + 7 + disp);
                if (cand >= imgBase && cand < imgEnd) {
                    table = cand;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!table) {
        WARN("block popup: table reference not found");
        return;
    }

    uint8_t* entry = table + (size_t)type * 0xDC;
    __try {
        for (size_t i = 0; i + 10 < 0xDC; ++i) {
            if (memcmp(entry + i, "pop_text_", 9) != 0) continue;
            char buf[64] = {};
            size_t n = 0;
            while (n < 60 && entry[i + n] >= 32 && entry[i + n] < 127) {
                buf[n] = (char)entry[i + n];
                ++n;
            }
            INFO("block popup colour id: {}", buf);
            const char* repl = "pop_text_guard";   // light cyan (#79f8ff)
            size_t rl = strlen(repl);
            if (n >= rl) {
                memset(entry + i, 0, n);
                memcpy(entry + i, repl, rl);
                INFO("block popup recoloured to {}", repl);
                patched = true;
            }
            return;
        }
        char name[32] = {};
        for (int k = 0; k < 31 && entry[k] >= 32 && entry[k] < 127; ++k) name[k] = (char)entry[k];
        INFO("block popup type name: {}", name);
        for (int style = 0; style < 2; ++style) {
            const uint8_t* st = entry + 0x54 + style * 0x40;
            char hex[3 * 64 + 1] = {};
            for (int k = 0; k < 64; ++k) sprintf_s(hex + k * 3, 4, "%02x ", st[k]);
            INFO("block popup style{}: {}", style, hex);
            __try {
                const float* f = (const float*)st;
                INFO("block popup style{} floats: {:.3f} {:.3f} {:.3f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f}",
                    style, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WARN("block popup recolour faulted");
    }
}

void ShieldOnStatsReset(void* actor) {
    ShieldRecomputeBuffs(actor);
}

} // namespace dds





