#include "shield.h"

static std::wstring W(const char* p) {
    std::wstring r;
    for (; *p; ++p) r += (wchar_t)*p;
    if (!r.empty() && r.back() != L'\\') r += L'\\';
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    dds::ShieldLoadClasses(W(argv[1]));
    dds::ShieldLoadBuffs(W(argv[1]));
    return 0;
}
