#include "hooks.h"
#include "shield.h"

// 脱离游戏验证穿透结算和 shielded 规则的 CaveHook
using namespace dds;

namespace {

int g_failed = 0;

void Check(const char* name, bool ok) {
    printf("%-44s %s\n", name, ok ? "ok" : "FAILED");
    if (!ok) ++g_failed;
}

bool Near(float a, float b) { return fabs(a - b) < 0.001f; }

void Effect(void* target, const char* name, void* performer) {
    char buf[64] = {};
    strcpy_s(buf, name);
    ShieldOnEffectApply(target, buf, performer);
}

// 模拟规则判定函数：入口保存非易失寄存器，rdi 是角色，r15 是 Buff，sil 是规则结果
struct FakeRule : Xbyak::CodeGenerator {
    const std::uint8_t* site = nullptr;
    const std::uint8_t* tail = nullptr;

    FakeRule() {
        Xbyak::Label end;
        push(rdi);
        push(rsi);
        push(r15);
        mov(rdi, rcx);
        mov(r15, rdx);
        xor_(esi, esi);

        site = getCurr();
        mov(eax, dword[r15 + 0xF4]);
        // 未接管时走这里：结果带上被偷取的指令读到的值，用来确认它被还原执行了
        add(eax, 1000);
        jmp(end, T_NEAR);

        tail = getCurr();
        movzx(eax, sil);

        L(end);
        pop(r15);
        pop(rsi);
        pop(rdi);
        ret();
    }
};

void TestPierce() {
    alignas(16) static char actor[0x4000]{};
    char attackerA = 0, attackerB = 0;
    float credit = 0.0f;

    Check("no shield means no status", ShieldStatus(actor) == 0);

    Effect(actor, "shield_gain_20", &attackerB);
    Check("shield_gain_20 grants status", ShieldStatus(actor) == 1 && Near(FindShield(actor)->cur, 20.0f));

    t_damageAttacker = &attackerA;
    Check("plain hit is fully absorbed", ShieldAbsorbFinal(actor, 5.0f, credit) == AbsorbKind::Full && Near(FindShield(actor)->cur, 15.0f));

    Effect(actor, "shield_pierce_50", &attackerA);
    t_damageAttacker = &attackerB;
    Check("other attacker ignores pending pierce", ShieldAbsorbFinal(actor, 4.0f, credit) == AbsorbKind::Full && Near(FindShield(actor)->cur, 11.0f));
    ShieldEndHit(actor, &attackerB);

    t_damageAttacker = &attackerA;
    auto kind = ShieldAbsorbFinal(actor, 10.0f, credit);
    Check("pierce 50 splits the hit", kind == AbsorbKind::Partial && Near(credit, 5.0f) && Near(FindShield(actor)->cur, 6.0f));
    ShieldEndHit(actor, &attackerA);

    Check("pierce is consumed by the hit", ShieldAbsorbFinal(actor, 2.0f, credit) == AbsorbKind::Full && Near(FindShield(actor)->cur, 4.0f));

    Effect(actor, "shield_pierce", &attackerA);
    kind = ShieldAbsorbFinal(actor, 3.0f, credit);
    Check("bare shield_pierce ignores the shield", kind == AbsorbKind::Partial && Near(credit, 0.0f) && Near(FindShield(actor)->cur, 4.0f));
    ShieldEndHit(actor, &attackerA);

    Effect(actor, "shield_pierce_100", &attackerA);
    ShieldOnRound();
    Check("round start clears unused pierce", ShieldAbsorbFinal(actor, 1.0f, credit) == AbsorbKind::Full && Near(FindShield(actor)->cur, 3.0f));
    t_damageAttacker = nullptr;

    Effect(actor, "shield_break", &attackerA);
    Check("broken shield loses status", ShieldStatus(actor) == 0);
}

void TestRuleCave() {
    alignas(16) static char shielded[0x4000]{};
    alignas(16) static char bare[0x4000]{};
    char performer = 0;
    Effect(shielded, "shield_gain_5", &performer);

    FakeRule code;
    Check("stolen instruction is 7 bytes", code.tail - code.site > 7 && code.site[0] == 0x41 && code.site[1] == 0x8B && code.site[2] == 0x87);

    dku::Hook::Trampoline::GetTrampoline().PageAlloc(1 << 8, AsAddress(code.getCode()));
    InstallBuffStatusCave(AsAddress(code.site), 0, 7, AsAddress(code.tail));

    const auto rule = reinterpret_cast<int (*)(void*, const void*)>(const_cast<std::uint8_t*>(code.getCode()));

    char buff[0x100] = {};
    *reinterpret_cast<std::uint32_t*>(buff + 0xF4) = ShieldedStatusHash();
    Check("shielded actor passes the rule", rule(shielded, buff) == 1);
    Check("bare actor fails the rule", rule(bare, buff) == 0);

    *reinterpret_cast<std::uint32_t*>(buff + 0xF4) = 1234;
    Check("other status returns to the game", rule(shielded, buff) == 1234 + 1000);
}

} // namespace

int main() {
    TestPierce();
    TestRuleCave();
    printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
