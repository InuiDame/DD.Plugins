#include "loc.h"

static void Dump(const char* label, const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], n, nullptr, nullptr);
    printf("%s utf8-bytes:", label);
    for (unsigned char c : u) printf(" %02X", c);
    printf("  (len=%d)\n", (int)u.size());
}

static std::wstring W(const char* p) {
    std::wstring r;
    for (; *p; ++p) r += (wchar_t)*p;
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    return r;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::wstring dllDir = W(argv[1]);
    std::wstring gameRoot = W(argv[2]);
    dds::loc::Init(dllDir, gameRoot);
    Dump("name", dds::loc::Get("str_shield_name", L"<fallback>"));
    return 0;
}
