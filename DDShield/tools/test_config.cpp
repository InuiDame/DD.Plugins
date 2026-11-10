#include <cstdio>
#include <string>
#include "config.h"
int main(int argc, char** argv) {
    std::wstring p;
    for (char* c = argv[1]; *c; ++c) p += (wchar_t)*c;
    dds::g_cfg.Load(p);
    printf("actionAllowCount=%d values=", dds::g_cfg.actionAllowCount);
    for (int i = 0; i < dds::g_cfg.actionAllowCount; ++i) printf("%d,", dds::g_cfg.actionAllow[i]);
    printf(" barHoldMs=%d mirrorHeal=%.2f\n", dds::g_cfg.barHoldMs, dds::g_cfg.mirrorHeal);
    return 0;
}
