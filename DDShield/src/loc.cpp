#include "loc.h"
#include "json.h"

namespace dds {
namespace loc {

namespace {

std::unordered_map<std::string, std::wstring> g_strings;
std::string g_language = "english";

std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
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

const JsonValue* FindKeyRecursive(const JsonValue& v, const std::string& key) {
    if (v.type == JsonValue::Object) {
        for (auto& kv : v.obj) {
            if (kv.first == key) return &kv.second;
            if (const JsonValue* r = FindKeyRecursive(kv.second, key)) return r;
        }
    } else if (v.type == JsonValue::Array) {
        for (auto& item : v.arr)
            if (const JsonValue* r = FindKeyRecursive(item, key)) return r;
    }
    return nullptr;
}

bool KnownLanguage(const std::string& s) {
    static const char* kIds[] = {
        "english", "schinese", "french", "german", "italian", "japanese",
        "koreana", "polish", "russian", "spanish", "czech", "brazilian"
    };
    for (auto id : kIds) if (s == id) return true;
    return false;
}

std::wstring DocumentsDir() {
    wchar_t buf[MAX_PATH] = {};
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD size = sizeof(buf);
        DWORD type = 0;
        RegQueryValueExW(key, L"Personal", nullptr, &type, (LPBYTE)buf, &size);
        RegCloseKey(key);
    }
    if (!buf[0]) {
        GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        wcscat_s(buf, L"\\Documents");
    }
    wchar_t expanded[MAX_PATH] = {};
    ExpandEnvironmentStringsW(buf, expanded, MAX_PATH);
    std::wstring s(expanded);
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}

std::string DetectLanguage() {
    // Darkest Dungeon stores the in-game language in
    // <Documents>\Darkest\persist.options.json  ->  "language": "schinese"
    std::wstring path = DocumentsDir() + L"Darkest\\persist.options.json";
    std::string text;
    if (ReadFileUtf8(path, text)) {
        JsonValue root;
        if (JsonParseFile(text, root)) {
            if (const JsonValue* v = FindKeyRecursive(root, "language")) {
                std::string lang = v->Str();
                if (KnownLanguage(lang)) {
                    INFO("game language: {}", lang);
                    return lang;
                }
            }
        }
    }
    // Fallback: OS UI language.
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_CHINESE:    return "schinese";
        case LANG_FRENCH:     return "french";
        case LANG_GERMAN:     return "german";
        case LANG_ITALIAN:    return "italian";
        case LANG_JAPANESE:   return "japanese";
        case LANG_KOREAN:     return "koreana";
        case LANG_POLISH:     return "polish";
        case LANG_RUSSIAN:    return "russian";
        case LANG_SPANISH:    return "spanish";
        case LANG_CZECH:      return "czech";
        case LANG_PORTUGUESE: return "brazilian";
        default:              return "english";
    }
}

void ParseSection(const std::string& text, const std::string& lang) {
    std::string open = "<language id=\"" + lang + "\">";
    size_t start = text.find(open);
    if (start == std::string::npos) return;
    start += open.size();
    size_t end = text.find("</language>", start);
    if (end == std::string::npos) return;
    std::string body = text.substr(start, end - start);

    size_t pos = 0;
    while (true) {
        size_t e = body.find("<entry id=\"", pos);
        if (e == std::string::npos) break;
        size_t idStart = e + 11;
        size_t idEnd = body.find('"', idStart);
        if (idEnd == std::string::npos) break;
        std::string id = body.substr(idStart, idEnd - idStart);
        size_t cdata = body.find("<![CDATA[", idEnd);
        size_t nextEntry = body.find("<entry id=\"", idEnd);
        if (cdata == std::string::npos || (nextEntry != std::string::npos && cdata > nextEntry)) {
            pos = idEnd;
            continue;
        }
        size_t valueStart = cdata + 9;
        size_t valueEnd = body.find("]]>", valueStart);
        if (valueEnd == std::string::npos) break;
        // Keep the primary language; English is only a fallback for missing keys.
        if (!g_strings.count(id))
            g_strings[id] = Utf8ToUtf16(body.substr(valueStart, valueEnd - valueStart));
        pos = valueEnd;
    }
}

void LoadFile(const std::wstring& path) {
    std::string text;
    if (!ReadFileUtf8(path, text)) {
        WARN("localization file not found: {}", ToUtf8(path));
        return;
    }
    ParseSection(text, g_language);
    if (g_language != "english") ParseSection(text, "english");   // fill gaps
    INFO("loaded localization: {}", ToUtf8(path));
}

// ---- file system helpers ---------------------------------------------------
bool FileExistsW(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ParentDir(std::wstring p) {
    if (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return p.substr(0, slash + 1);
}

std::vector<std::wstring> ListSubdirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L"*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        out.push_back(dir + (dir.back() == L'\\' ? L"" : L"\\") + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::wstring SteamPathFromRegistry() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    std::wstring s(buf);
    for (auto& c : s) if (c == L'/') c = L'\\';
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}

// Steam library folders from <steam>\config\libraryfolders.vdf ("path" "...")
void AddLibraryFolders(std::vector<std::wstring>& out, const std::wstring& steam) {
    std::string text;
    if (!ReadFileUtf8(steam + L"config\\libraryfolders.vdf", text)) return;
    size_t pos = 0;
    while (true) {
        pos = text.find("\"path\"", pos);
        if (pos == std::string::npos) break;
        size_t q1 = text.find('"', pos + 6);
        size_t q2 = (q1 == std::string::npos) ? std::string::npos : text.find('"', q1 + 1);
        if (q1 == std::string::npos || q2 == std::string::npos) break;
        std::string val = text.substr(q1 + 1, q2 - q1 - 1);
        std::wstring w;
        for (char c : val) {
            if (c == '\\' && !w.empty()) { /* keep */ }
            w += (wchar_t)(unsigned char)c;
        }
        if (!w.empty() && w.back() != L'\\') w += L'\\';
        out.push_back(w);
        pos = q2;
    }
}

void AddWorkshopCandidates(std::vector<std::wstring>& out, const std::wstring& steamapps) {
    std::wstring content = steamapps + L"workshop\\content\\262060\\";
    for (auto& idDir : ListSubdirs(content)) {
        std::wstring file = idDir + L"\\localization\\shield.string_table.xml";
        if (FileExistsW(file)) out.push_back(file);
    }
}

} // namespace

void Init(const std::wstring& dllDir, const std::wstring& gameRoot) {
    g_strings.clear();
    g_language = DetectLanguage();

    std::vector<std::wstring> files;

    // 1) The DLL may ship inside the mod folder (workshop or local).
    if (!dllDir.empty()) {
        std::wstring f = dllDir + L"localization\\shield.string_table.xml";
        if (FileExistsW(f)) files.push_back(f);
    }
    // 2) Regular local mod install: <root>\mods\<any>\localization\...
    std::wstring modsDir = gameRoot + L"mods\\";
    for (auto& modDir : ListSubdirs(modsDir)) {
        std::wstring f = modDir + L"\\localization\\shield.string_table.xml";
        if (FileExistsW(f)) files.push_back(f);
    }
    // 3) Steam Workshop: <steamapps>/workshop/content/262060/<id>/localization/...
    std::wstring common = ParentDir(gameRoot);         // <steamapps>/common/
    std::wstring steamapps = ParentDir(common);        // <steamapps>/
    if (!steamapps.empty()) AddWorkshopCandidates(files, steamapps);

    std::wstring steam = SteamPathFromRegistry();
    if (!steam.empty()) {
        AddWorkshopCandidates(files, steam + L"steamapps\\");
        std::vector<std::wstring> libs;
        AddLibraryFolders(libs, steam);
        for (auto& lib : libs) AddWorkshopCandidates(files, lib + L"steamapps\\");
    }
    // 4) Loose file in the install root (development convenience).
    {
        std::wstring f = gameRoot + L"localization\\shield.string_table.xml";
        if (FileExistsW(f)) files.push_back(f);
    }

    if (files.empty())
        WARN("no shield string table found (looked in mods/ and workshop/262060)");

    for (auto& f : files) LoadFile(f);

    INFO("localization strings loaded: {} (language {})", g_strings.size(), g_language);
}

std::wstring Get(const char* key, const wchar_t* fallback) {
    auto it = g_strings.find(key);
    if (it != g_strings.end()) return it->second;
    return fallback ? fallback : L"";
}

} // namespace loc
} // namespace dds
