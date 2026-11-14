#pragma once

// 两个「函数内 cave」的 Xbyak stub 机器码生成器。
// 只依赖 Xbyak + <cstdint>，不含 DKUtil / Win32 / 其它源码依赖，
// 因此生产 Hooks.cpp 与离线单测(test_scroll.cpp)可共用同一份字节，
// 单测用「字节断言 + 外部 capstone 反汇编」验证 B1 偏移与 B2 wrap 契约。
//
// 全部 RVA / 字节 [静态] 复核自基准 exe（SHA1 9db2d73b...，见 p4-fix.md）：
//  - 0x371F0A 9B = movd xmm0,[rsp+34] + cvtdq2ps xmm0,xmm0
//  - 0x371F13    = rip 相对 movss（跳回点，不截断）
//  - 0x7EBF70 5B = mov [rsp+20],rbx
//  - 0x7EBF75    = 跳回点
//  - 0x7EA6CD / 0x7EB5AD 两条 E8 call 的返回地址 = 0x7EA6D2 / 0x7EB5B2

#include <xbyak/xbyak.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ScrollAsm
{
	constexpr std::uint32_t kInputStolen = 9;
	constexpr std::uint32_t kInputResume = 0x371F13;
	constexpr std::uint8_t  kInputBytes[9]{
		0x66, 0x0F, 0x6E, 0x44, 0x24, 0x34, // movd xmm0,[rsp+0x34]
		0x0F, 0x5B, 0xC0                    // cvtdq2ps xmm0,xmm0
	};

	constexpr std::uint32_t kCellStolen = 5;
	constexpr std::uint32_t kCellResume = 0x7EBF75;
	constexpr std::uint8_t  kCellBytes[5]{ 0x48, 0x89, 0x5C, 0x24, 0x20 }; // mov [rsp+20],rbx

	constexpr std::uint32_t kRetPathA = 0x7EA6D2; // 0x7EA6CD (in 0x7E9F80) 之后
	constexpr std::uint32_t kRetPathB = 0x7EB5B2; // 0x7EB5AD (in 0x7EADE0) 之后

	// v10：param_5 在 0x7EBF70 入口栈上的确切位置（p18-v10-review B1 修正）。
	// prolog: mov [rsp+20],rbx(5B) + 7 push(0x38) + lea rbp,[rsp-0xc10]
	//   => rbp = R0-0xc48；铁证 7EC1EF `mov eax,[rbp+0xc70]` = [R0+0x28]。
	// 标准 x64 第 5 参位置一致。stub 的 rsp = R0-88(0x58)，
	// 故 param_5 = [rsp + 0x58 + 0x28] = [rsp+0x80] = 128。
	// 上一版误写成 112/116/120（整体错位 16B），param_5 从未被读到。
	constexpr std::uint32_t kStubFrameBytes = 7 * 8 + 0x20; // 88
	constexpr std::uint32_t kCellParam5Disp = kStubFrameBytes + 0x28; // 128

	// 布局数组元素（[静态] 由 0x7E9F80 汇编 + 0x628A90 布局描述表确证）：
	// [0]+0x00 int number_of_columns / [1]+0x04 float start_x
	// [2]+0x08 float start_y  <-- 数据补丁写这里 / [3]+0x0C float offset_x
	// [4]+0x10 float offset_y / [5]+0x14 int is_centred
	constexpr std::uint32_t kLayoutColumnsOff = 0x00;
	constexpr std::uint32_t kLayoutStartXOff = 0x04;
	constexpr std::uint32_t kLayoutStartYOff = 0x08;
	constexpr std::uint32_t kLayoutOffsetXOff = 0x0C;
	constexpr std::uint32_t kLayoutOffsetYOff = 0x10;
	constexpr std::uint32_t kLayoutCentredOff = 0x14;

	// SpscQueue 字段偏移由构造参数传入（与 Wheel.h 的 offsetof 一致），
	// 避免本头文件依赖 Wheel.h，便于单测独立生成。
	struct InputParams
	{
		std::uint64_t queueAddr;   // &Wheel::g_queue（运行时求值，ASLR 安全）
		std::uint64_t resumeAddr;  // g_base + kInputResume
		std::size_t   offWrite;
		std::size_t   offRead;
		std::size_t   offRing;
		std::size_t   offDropped;
		std::uint32_t mask;
		std::uint32_t capacity;
	};

	// 输入 cave stub：纯整数汇编，捕获 [rsp+0x64] = 原始 wheel.y (Sint32)，
	// 以 wrap 契约 (write=(write+1)&255) 写入 SPSC 队列。
	// 6 次压栈（pushfq + rax/rcx/rdx/r10/r11）= 48 字节 →
	//   wheel.y 在 RSP0+0x34 = (rsp+48)+0x34 = [rsp+0x64]  ← B1 自证
	// 成对 push/pop；epilog 原样重放 9B 后用 jmp qword ptr [rip+0] 跳回。
	// 不碰 xmm（此处 xmm1/xmm2 有活值）、不调函数、不分配内存。
	struct InputCaveAsm : Xbyak::CodeGenerator
	{
		explicit InputCaveAsm(const InputParams& a_p)
		{
			using namespace Xbyak::util;
			Xbyak::Label full, done;
			pushfq();
			push(rax);
			push(rcx);
			push(rdx);
			push(r10);
			push(r11); // 6 次压栈 = 48 字节
			mov(eax, dword[rsp + 48 + 0x34]); // 8B 44 24 64  ← 原始 wheel.y
			mov(r11, a_p.queueAddr);          // movabs r11, imm64
			mov(ecx, dword[r11 + a_p.offWrite]); // w (wrap 0..255)
			mov(r10d, dword[r11 + a_p.offRead]); // r
			mov(edx, ecx);
			add(edx, 1);
			and_(edx, a_p.mask);              // nw = (w+1)&255  ← B2 与 Wheel::Push 一致
			cmp(edx, r10d);
			je(full);                          // 满：nw == r 直接丢弃
			and_(ecx, a_p.mask);              // slot = w & 255
			mov(dword[r11 + a_p.offRing + rcx * 4], eax);
			mov(dword[r11 + a_p.offWrite], edx); // release 发布 nw
			jmp(done);
			L(full);
			db(0xF0); // LOCK 前缀：dropped++
			inc(dword[r11 + a_p.offDropped]);
			L(done);
			pop(r11);
			pop(r10);
			pop(rdx);
			pop(rcx);
			pop(rax);
			popfq();
			db(kInputBytes, sizeof(kInputBytes)); // 原样重放偷取的 9B
			static constexpr std::uint8_t absJmp[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
			db(absJmp, sizeof(absJmp));
			dq(a_p.resumeAddr);
		}
	};

	// v10：两个网格渲染器入口的「只读」cave。
	// 跳进来时 rsp = R0（call 的返回地址在 [R0]），r8 仍是调用点用
	// `lea r8,[rip+..]` 传入的布局数组地址（p16 复核 15/15 调用点）。
	// stub 只做：保存 flags/rax -> 用 r8 比对 3 个目标布局绝对地址 ->
	// 把一个 uint32 面板标志写成 0(非目标) / 1(store) / 2(party) / 3(raid)
	// -> 恢复 flags/rax（rsp 回到 R0）-> 原样重放 stolen -> FF25 跳回。
	// 不改任何参数寄存器、不调用 C++、不分配内存、不碰 xmm。
	// 非目标面板（含城镇饰品库 0x6A6739 -> 0x2B8E8EC）写 0，
	// 使随后进入 0x7EBF70 的格子一律放行，从根上消除 v7 的饰品栏误伤。
	struct GridEntryParams
	{
		std::uint64_t flagAddr;     // &Hooks::g_activeLayout (std::atomic<uint32_t>)
		std::uint64_t resumeAddr;   // g_base + kGridResume / kGrid2Resume
		std::uint64_t target1;      // g_base + 0x2B8DB10 (store)
		std::uint64_t target2;      // g_base + 0x2B8DB30 (party)
		std::uint64_t target3;      // g_base + 0x2BE38A0 (raid)
		const std::uint8_t* stolen; // kGridBytes(7B) / kGrid2Bytes(5B)
		std::uint32_t stolenLen;
	};

	struct GridEntryAsm : Xbyak::CodeGenerator
	{
		explicit GridEntryAsm(const GridEntryParams& a_p)
		{
			using namespace Xbyak::util;
			Xbyak::Label hit1, hit2, hit3, done;
			pushfq();
			push(rax); // rsp = R0-16，不触碰 [R0+0x20..] 的栈参
			mov(rax, a_p.target1);
			cmp(r8, rax);
			je(hit1);
			mov(rax, a_p.target2);
			cmp(r8, rax);
			je(hit2);
			mov(rax, a_p.target3);
			cmp(r8, rax);
			je(hit3);
			mov(rax, a_p.flagAddr);
			mov(dword[rax], 0u);
			jmp(done);
			L(hit1);
			mov(rax, a_p.flagAddr);
			mov(dword[rax], 1u);
			jmp(done);
			L(hit2);
			mov(rax, a_p.flagAddr);
			mov(dword[rax], 2u);
			jmp(done);
			L(hit3);
			mov(rax, a_p.flagAddr);
			mov(dword[rax], 3u);
			L(done);
			pop(rax);
			popfq(); // rsp == R0：stolen 里的 [rsp+0x20] / [rax+0x10] 语义不变
			db(a_p.stolen, a_p.stolenLen);
			static constexpr std::uint8_t absJmp[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
			db(absJmp, sizeof(absJmp));
			dq(a_p.resumeAddr);
		}
	};

	struct CellParams
	{
		std::uint64_t gateFn;     // &Hooks::CellGate, bool(retAddr, param5)
		std::uint64_t resumeAddr; // g_base + kCellResume
	};

	// 每格裁剪 cave stub（v10 改用 param_5 的真实行号）：
	//  - cave 是 0x7EBF70 入口 5B 处的 E9 jmp，跳进来时 rsp = R0，
	//    [R0] 仍是原函数返回地址（jmp 不压栈）。
	//  - 7 push + sub rsp,0x20 = 88 字节，call C++ 前 rsp 16 字节对齐 + shadow。
	//  - param_5 的确切栈槽是 [R0+0x28]（见 kCellParam5Disp 的铁证），
	//    stub 里即 [rsp+128]。编码（p10 §2.1 [静态]）：
	//      param_5 = (((col&0x1f)|0x20)<<0x18 | (row&0xffffff)) * 2
	//      => row = (param_5 >> 1) & 0xffffff
	//    C++ 门控用位域特征 (bit30=1 且 bit0=0) 做 sanity check，
	//    不匹配就放行（零破坏降级）。**不再在多个候选里猜**，
	//    避免 p18 审核指出的「栈残留/指针被误判成 param_5」。
	//  - 门控返回 true(窗口外)：pop 干净后 rsp==R0 → xor eax,eax; ret
	//    （al=0 返回 false；调用点 A 只做 `test al,al; cmovne r15,rbx`，
	//      调用点 B 只做 `test al,al; je 汇合点`，跳过无计数/索引副作用 [静态]）。
	//  - 门控返回 false：重放 stolen 5B 并 jmp 回 0x7EBF75。
	//  - 9 个参数全是整数/指针，无 xmm 参数；xmm0-5 是易失寄存器，不保存。
	struct CellCaveAsm : Xbyak::CodeGenerator
	{
		explicit CellCaveAsm(const CellParams& a_p)
		{
			using namespace Xbyak::util;
			Xbyak::Label skip;
			// 入口 rsp=RSP0 (≡8 mod 16)；7 push=56 → rsp≡0 mod 16
			push(rax);
			push(rcx);
			push(rdx);
			push(r8);
			push(r9);
			push(r10);
			push(r11);
			sub(rsp, 0x20); // 0x20 shadow，保持 16 对齐
			// 此时 rsp = RSP0-88，返回地址在 [rsp+88] = [rsp+0x58]
			mov(rcx, qword[rsp + kStubFrameBytes]); // 第 1 参 = 返回地址（R0）
			mov(edx, dword[rsp + kCellParam5Disp]); // 第 2 参 = param_5 [R0+0x28]
			mov(rax, a_p.gateFn);                   // movabs rax, imm64
			call(rax);                              // bool CellGate(ret, param5) -> al
			                                        // r8/r9 不再被覆盖
			add(rsp, 0x20);
			test(al, al);
			jnz(skip);

			// 放行：恢复寄存器（rsp==RSP0）→ epilog
			pop(r11); pop(r10); pop(r9); pop(r8); pop(rdx); pop(rcx); pop(rax);
			db(kCellBytes, sizeof(kCellBytes)); // mov [rsp+0x20],rbx
			static constexpr std::uint8_t absJmp[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
			db(absJmp, sizeof(absJmp));
			dq(a_p.resumeAddr); // FF 25 已经把控制流带走，下面的字节不会被执行

			// 跳过：恢复寄存器（rsp==RSP0）→ xor eax,eax; ret
			L(skip);
			pop(r11); pop(r10); pop(r9); pop(r8); pop(rdx); pop(rcx); pop(rax);
			xor_(eax, eax); // Xbyak: `xor` 是 MSVC 的 C++ 替代标记(== ^)，必须用 xor_
			ret();
		}
	};

	// ==================== v11：工会技能栏滚轮（实验性，独立开关）====================
	// 两个 cave 全是纯整数/SSE 指令，**绝不 call C++**（p26 审核 S1：call 会破坏
	// hook B 依赖的 xmm1 与 hook A 的 rcx=this）。共享一张 DLL 侧表，布局固定，
	// CaveAsm.h / Hooks.cpp / 离线单测三方共用同一份 offsetof 断言。
	//
	// v12 容量防护：DLL 侧影子表 64 -> 256 槽。实测本地 mod 英雄最多 192 个
	// combat_skill（Laevateinn.info.darkest），旧表把 N 硬钳到 64，多出来的
	// 技能槽既不进表也不被平移，与角色页面同屏时形成容量落差。
	// ★ 只扩 DLL 侧影子表；绝不碰游戏自己的 vector，也绝不钳制游戏创建的
	//   UI 槽数量（那会让 UI 索引与技能逻辑索引脱节，p26 对抗审核 C4）。
	constexpr std::uint32_t kGuildCapacity = 256;
	constexpr std::uint32_t kGuildMaxSlots = kGuildCapacity; // 兼容旧名 = 物理容量
	struct GuildTable
	{
		std::uint64_t slots[kGuildCapacity];      // 0x000 技能槽 widget*
		std::uint32_t baseY[kGuildCapacity];      // 0x800 原始 +0x68 的 float 位模式
		float         offset[kGuildCapacity];     // 0xC00 本帧给该槽 Y 加的增量
		std::atomic<std::uint32_t> count;         // 0x1000
		std::atomic<std::uint32_t> lastBeat;      // 0x1004 hook A 最近见 Guild 的 tick 计数
		std::atomic<std::uint32_t> active;        // 0x1008 tick 侧算出的活跃标志
		std::atomic<std::uint32_t> beatTick;      // 0x100C tick 侧自增计数
		std::atomic<std::uint32_t> hits;          // 0x1010 hook B 命中次数 [DIAG]
		std::atomic<std::uint32_t> clipped;       // 0x1014 被移到屏幕外的槽数 [DIAG]
		// v11.1 诊断：实机日志证明 hook A(0x66E9F0) 的 gBeat 恒 0，必须能区分
		// 「cave 没执行」与「判据没命中」。四个计数器都在游戏主线程递增，
		// tick 线程只做 relaxed 读取（仅诊断，撕裂不影响功能）。
		std::atomic<std::uint32_t> callsA;        // 0x1018 hook A 执行次数
		std::atomic<std::uint32_t> callsB;        // 0x101C hook B 执行次数
		std::atomic<std::uint64_t> lastVtA;       // 0x1020 hook A 最近一次 [rcx] vptr
		std::atomic<std::uint64_t> lastVtB;       // 0x1028 hook B 最近一次 [rdi] vptr
		std::atomic<std::uint32_t> seenB;         // 0x1030 hook B 见到 GuildDisplay 本体次数
		// v11.2：★ ASLR 修复 ★ 期望 vptr 必须由 C++ 写成 g_base + 0xEBB8D8。
		// v11/v11.1 在 cave 里硬编码 PE 磁盘基址下的 0x140EBB8D8，而游戏实机被
		// 重定位到 0x7FF7D7DD0000（日志 gVtA=7FF7D8C8B8D8 为证），判据永不成立。
		std::uint64_t expectedVptr;               // 0x1038 运行时 GuildDisplay vtable 地址
		// v12：运行时「有效上限」，由 Hooks::Install 按 cfg 写一次（游戏主线程
		// 只读，tick 线程不写，无竞态）。cave 从这里取上限而不是内嵌立即数，
		// 因此 guild_max_slots=64 无需改任何机器码即可回到 v11.2 旧容量。
		std::uint32_t maxSlots{ kGuildCapacity }; // 0x1040
	};
	constexpr std::uint32_t kGtSlotsOff       = 0x000;
	constexpr std::uint32_t kGtBaseYOff       = 0x800;
	constexpr std::uint32_t kGtOffsetOff      = 0xC00;
	constexpr std::uint32_t kGtCountOff       = 0x1000;
	constexpr std::uint32_t kGtBeatOff        = 0x1004;
	constexpr std::uint32_t kGtActiveOff      = 0x1008;
	constexpr std::uint32_t kGtTickOff        = 0x100C;
	constexpr std::uint32_t kGtHitsOff        = 0x1010;
	constexpr std::uint32_t kGtClipOff        = 0x1014;
	constexpr std::uint32_t kGtCallsAOff      = 0x1018;
	constexpr std::uint32_t kGtCallsBOff      = 0x101C;
	constexpr std::uint32_t kGtVtAOff         = 0x1020;
	constexpr std::uint32_t kGtVtBOff         = 0x1028;
	constexpr std::uint32_t kGtSeenBOff       = 0x1030;
	constexpr std::uint32_t kGtVtExpectedOff  = 0x1038;
	constexpr std::uint32_t kGtMaxSlotsOff    = 0x1040;
	// ★ ASLR 铁律：GuildDisplay vtable 的「磁盘 VA」只可用于离线断言，
	// 绝不能出现在 cave 的运行时比较里。实机日志证明游戏基址被重定位到
	// 0x7FF7D7DD0000（gVtA=7FF7D8C8B8D8），与 0x140EBB8D8 永不相等。
	// 运行时判据一律读 GuildTable.expectedVptr = g_base + kGuildVtableRva。
	constexpr std::uint32_t kGuildVtableRva   = 0xEBB8D8u;
	constexpr std::uint64_t kGuildVtable      = 0x140000000ULL + kGuildVtableRva; // 磁盘 VA，仅断言
	constexpr std::uint32_t kGuildVtSlot23    = kGuildVtableRva + 23u * 8u; // = 0xEBB990
	// cave 用「表基址 + 常量偏移」访问；布局一旦变化必须立刻编译失败。
	static_assert(offsetof(GuildTable, slots)    == kGtSlotsOff);
	static_assert(offsetof(GuildTable, baseY)    == kGtBaseYOff);
	static_assert(offsetof(GuildTable, offset)   == kGtOffsetOff);
	static_assert(offsetof(GuildTable, count)    == kGtCountOff);
	static_assert(offsetof(GuildTable, lastBeat) == kGtBeatOff);
	static_assert(offsetof(GuildTable, active)   == kGtActiveOff);
	static_assert(offsetof(GuildTable, beatTick) == kGtTickOff);
	static_assert(offsetof(GuildTable, hits)     == kGtHitsOff);
	static_assert(offsetof(GuildTable, clipped)  == kGtClipOff);
	static_assert(offsetof(GuildTable, callsA)   == kGtCallsAOff);
	static_assert(offsetof(GuildTable, callsB)   == kGtCallsBOff);
	static_assert(offsetof(GuildTable, lastVtA)  == kGtVtAOff);
	static_assert(offsetof(GuildTable, lastVtB)  == kGtVtBOff);
	static_assert(offsetof(GuildTable, seenB)    == kGtSeenBOff);
	static_assert(offsetof(GuildTable, expectedVptr) == kGtVtExpectedOff);
	static_assert(offsetof(GuildTable, maxSlots) == kGtMaxSlotsOff);
	// v12：三张表按偏移连续分区，最大索引处不得互相重叠 / 越过 count 字段。
	static_assert(kGtSlotsOff  + kGuildCapacity * 8u <= kGtBaseYOff);
	static_assert(kGtBaseYOff  + kGuildCapacity * 4u <= kGtOffsetOff);
	static_assert(kGtOffsetOff + kGuildCapacity * 4u <= kGtCountOff);

	// hook A：0x66E9F0 BuildingDisplay::PreChildrenShowInternal 入口（slot23，每帧）。
	// 只有 [rcx] == GuildDisplay vtable 才刷新技能槽指针表；其它建筑零干预。
	constexpr std::uint32_t kGuildRefreshRva    = 0x66E9F0;
	constexpr std::uint32_t kGuildRefreshResume = 0x66E9F5; // 5B stolen
	constexpr std::uint8_t  kGuildRefreshBytes[5]{ 0x48, 0x8B, 0xC4, 0x55, 0x53 };

	// hook B：0x7F5EDD `movss [rsp+0x54],xmm1`（6B）。改 xmm1 后原样重放 stolen，
	// 只动栈帧，不动任何对象/全局；rsp 回到 R0 后 [rsp+0x54] 语义不变（p26 S2）。
	// 0x7F5EE3 之后 xmm1 每次都先重新加载再使用（capstone 确证），改它安全。
	constexpr std::uint32_t kGuildShiftRva    = 0x7F5EDD;
	constexpr std::uint32_t kGuildShiftResume = 0x7F5EE3;
	constexpr std::uint8_t  kGuildShiftBytes[6]{ 0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x54 };

	struct GuildParams
	{
		std::uint64_t tableAddr;
		std::uint64_t resumeAddr;
	};

	struct GuildRefreshAsm : Xbyak::CodeGenerator
	{
		explicit GuildRefreshAsm(const GuildParams& a_p)
		{
			using namespace Xbyak::util;
			// v12：新增纵深防御分支后，前向短跳(rel8)距离超过 ±127 会让
			// Xbyak 抛 "label is too far"。统一用 rel32，代价仅是每条跳转多 4B。
			setDefaultJmpNEAR(true);
			Xbyak::Label done, countOk, loop, hit, next, notGuild, badTable;
			pushfq();
			push(rax); push(rdx); push(r8); push(r9); push(r10); push(r11);
			// rcx = this 必须原样保留；rsp = R0-56
			mov(r11, a_p.tableAddr);
			inc(dword[r11 + kGtCallsAOff]);      // v11.1 诊断：本 cave 到底有没有执行
			mov(rax, qword[rcx]);                // 记录真实 vptr，供判据校准
			mov(qword[r11 + kGtVtAOff], rax);
			mov(rax, qword[r11 + kGtVtExpectedOff]); // ★ 运行时 vtable，非磁盘 VA
			cmp(qword[rcx], rax);
			jne(notGuild);                       // 非 GuildDisplay：零干预
			mov(rdx, qword[rcx + 0x270]);       // vector begin
			mov(r8,  qword[rcx + 0x278]);       // vector end
			sub(r8, rdx);
			sar(r8, 3);                         // N = (end-begin)>>3
			// v12 纵深防御：半初始化 / 被污染的 vector 绝不遍历。
			// begin=null 会让 [rdx+r9*8] 空指针读；end<begin 会得到巨大的
			// 无符号 N，旧代码虽钳到 64 但仍会读 64 个野指针。
			test(rdx, rdx);
			jz(badTable);
			test(r8, r8);
			js(badTable);
			// v12：上限来自表（cfg 可配、C++ 写一次），不再内嵌立即数。
			mov(eax, dword[r11 + kGtMaxSlotsOff]);
			test(eax, eax);
			jz(badTable);
			cmp(r8, rax);
			jbe(countOk);
			mov(r8, rax);
			L(countOk);
			// p28 应修 2：先填满 slots/baseY，**最后**才发布 count 与 lastBeat。
			// 否则 tick 线程/hook B 可能在「新 count + 旧 slots」的窗口里命中悬垂指针。
			xor_(r9d, r9d);                     // i = 0
			L(loop);
			cmp(r9, r8);
			jae(done);
			mov(r10, qword[rdx + r9 * 8]);      // slot*
			test(r10, r10);
			jz(hit);                            // 空指针：写 0 占位，避免悬垂命中
			test(r10, 7);                       // v12：非 8 对齐 = 野指针，同样写 0
			jnz(hit);
			mov(qword[r11 + kGtSlotsOff + r9 * 8], r10);
			mov(eax, dword[r10 + 0x68]);        // 原始 Y（游戏自己写的 baseline）
			mov(dword[r11 + kGtBaseYOff + r9 * 4], eax);
			jmp(next);
			L(hit);
			mov(qword[r11 + kGtSlotsOff + r9 * 8], 0u);
			mov(dword[r11 + kGtBaseYOff + r9 * 4], 0u);
			L(next);
			inc(r9);
			jmp(loop);
			L(done);
			mov(dword[r11 + kGtCountOff], r8d);  // 表已完整，才发布 N
			mov(eax, dword[r11 + kGtTickOff]);   // 纯 mov 读 tick 计数，绝不 call
			mov(dword[r11 + kGtBeatOff], eax);
			jmp(notGuild);
			L(badTable);
			// v12：vector 不可信时宁可让工会滚轮本帧不生效（count=0 →
			// tick 侧 active=false → offset 全 0 → 零干预），绝不带着野指针刷新。
			mov(dword[r11 + kGtCountOff], 0);
			L(notGuild);
			pop(r11); pop(r10); pop(r9); pop(r8); pop(rdx); pop(rax); popfq();
			db(kGuildRefreshBytes, sizeof(kGuildRefreshBytes));
			static constexpr std::uint8_t absJmp[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
			db(absJmp, sizeof(absJmp));
			dq(a_p.resumeAddr);
		}
	};

	struct GuildShiftAsm : Xbyak::CodeGenerator
	{
		explicit GuildShiftAsm(const GuildParams& a_p)
		{
			using namespace Xbyak::util;
			// 与 hook A 同理：v12 分支变多，前向 jcc 必须是 rel32。
			setDefaultJmpNEAR(true);
			Xbyak::Label done, countOk, loop, hit, notSelf;
			Xbyak::Label rCountOk, rLoop, rDone, rZero, rNext, rBadTable;
			pushfq();
			push(rax); push(rcx); push(rdx); push(r8); push(r9); push(r10); push(r11);
			// rdi = 当前 widget，绝不改动；xmm1 是本 cave 唯一有意修改的寄存器
			mov(r11, a_p.tableAddr);
			inc(dword[r11 + kGtCallsBOff]);      // v11.1 诊断：本 cave 到底有没有执行
			mov(rax, qword[rdi]);
			mov(qword[r11 + kGtVtBOff], rax);    // 最近一次 widget 的 vptr（判据校准用）

			// ---- v11.1 主修复：不再依赖 0x66E9F0（实机证明它没执行）----
			// GuildDisplay 本体作为 widget 每帧必定经过 Base::Show 的 0x7F5EDD，
			// 且时序上早于 0x7F607A 的子 widget 递归 Show。在这里直接刷新技能槽表，
			// 与 hook A 同构（同一主线程、同样「先填 slots 后发布 count」）。
			mov(rax, qword[r11 + kGtVtExpectedOff]); // ★ 运行时 vtable，非磁盘 VA
			cmp(qword[rdi], rax);
			jne(notSelf);
			inc(dword[r11 + kGtSeenBOff]);
			mov(rdx, qword[rdi + 0x270]);       // vector begin
			mov(r8,  qword[rdi + 0x278]);       // vector end
			sub(r8, rdx);
			sar(r8, 3);                         // N = (end-begin)>>3
			// v12 纵深防御（与 hook A 同构）：begin 空 / end<begin / 上限未初始化
			// 时一律不刷新表，把 count 发布为 0，避免 [rdx+r9*8] 野指针读。
			test(rdx, rdx);
			jz(rBadTable);
			test(r8, r8);
			js(rBadTable);
			mov(eax, dword[r11 + kGtMaxSlotsOff]); // v12 运行时有效上限
			test(eax, eax);
			jz(rBadTable);
			cmp(r8, rax);
			jbe(rCountOk);
			mov(r8, rax);
			L(rCountOk);
			xor_(r9d, r9d);
			L(rLoop);
			cmp(r9, r8);
			jae(rDone);
			mov(r10, qword[rdx + r9 * 8]);
			test(r10, r10);
			jz(rZero);
			test(r10, 7);                       // v12：非 8 对齐 = 野指针，写 0 占位
			jnz(rZero);
			mov(qword[r11 + kGtSlotsOff + r9 * 8], r10);
			mov(eax, dword[r10 + 0x68]);
			mov(dword[r11 + kGtBaseYOff + r9 * 4], eax);
			jmp(rNext);
			L(rZero);
			mov(qword[r11 + kGtSlotsOff + r9 * 8], 0u);
			mov(dword[r11 + kGtBaseYOff + r9 * 4], 0u);
			L(rNext);
			inc(r9);
			jmp(rLoop);
			L(rDone);
			mov(dword[r11 + kGtCountOff], r8d);  // 表已完整，才发布 N
			mov(eax, dword[r11 + kGtTickOff]);
			mov(dword[r11 + kGtBeatOff], eax);
			jmp(done);                           // GuildDisplay 本体自身不需要平移

			L(rBadTable);
			// v12：vector 不可信 → 本帧零干预（count=0），绝不带着野指针刷新。
			mov(dword[r11 + kGtCountOff], 0);
			jmp(done);

			L(notSelf);
			// ---- 原有：命中技能槽表才平移 ----
			mov(eax, dword[r11 + kGtActiveOff]);
			test(eax, eax);
			jz(done);                            // 工会未激活：零干预
			mov(eax, dword[r11 + kGtMaxSlotsOff]); // v12 有效上限
			test(eax, eax);
			jz(done);
			mov(ecx, dword[r11 + kGtCountOff]);
			cmp(ecx, eax);
			jbe(countOk);
			mov(ecx, eax);
			L(countOk);
			xor_(edx, edx);                     // i = 0
			L(loop);
			cmp(edx, ecx);
			jae(done);
			mov(r8, qword[r11 + kGtSlotsOff + rdx * 8]);
			cmp(r8, rdi);
			je(hit);
			inc(edx);
			jmp(loop);
			L(hit);
			addss(xmm1, dword[r11 + kGtOffsetOff + rdx * 4]);
			inc(dword[r11 + kGtHitsOff]);
			L(done);
			pop(r11); pop(r10); pop(r9); pop(r8); pop(rdx); pop(rcx); pop(rax); popfq();
			db(kGuildShiftBytes, sizeof(kGuildShiftBytes));
			static constexpr std::uint8_t absJmp[]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
			db(absJmp, sizeof(absJmp));
			dq(a_p.resumeAddr);
		}
	};
}
