# ADR 0002：工会技能过多时的 UI 容量防护（v12 GuildSkillCapacity）

- **状态**：已实现，离线单测通过（150 PASS / 0 FAIL），**尚未实机验收**
- **日期**：2026-11-14
- **影响范围**：`ScrollWheel`（`scroll_wheel.dll`）单插件；不新建 DLL，不改 `DD.ModLoader`
- **前置**：`0001-guild-scroll-no-go.md`（NO-GO 已被 v11 重评取代，滚轮已实机验收）
- **关联**：`README.md §9`、`DD.Plugins/docs/ARCHITECTURE.md`、`DD.Plugins/docs/DEV-TEST-DEPLOY.md`

---

## 1. 背景与问题陈述

用户报障：「工会技能过多，同时打开角色页面导致 UI 崩溃」。本地装了大量 class mod，
单个英雄的 `combat_skill:` 条数远超原版。

三个阶段的事实收集：

1. **阶段一（三路并行调研）**：游戏侧 / `DD.ModLoader` 侧 / `DD.Plugins` 侧。
2. **阶段二（对抗审核）**：两支独立红队分别证伪根因假设与工程方案。
3. **规格确认**：用户明确要求「把防止崩溃（扩大 UI 最大容纳数量）的功能与
   ScrollWheel **合并**，前提是不摧毁 ScrollWheel 的功能」。

### 1.1 已确证的事实

| 事实 | 证据 |
| --- | --- |
| 原版英雄 35 个 `combat_skill:`；本地 mod 英雄最多 **192** 个 | `mods\Laevateinn\heroes\Laevateinn\Laevateinn.info.darkest` 等实测计数 |
| v11.2 的 DLL 侧影子表 `GuildTable` 固定 **64 槽**，三个 cave 都把 `N` 钳到 64 | `src/CaveAsm.h`（v11.2）`kGuildMaxSlots = 64` |
| 超出 64 的技能槽不进影子表、不被平移 | 同上，钳制语义 |
| `guild.layout.darkest` 只解析 `skill_pos` / `skill_spacing`，无 `.element_max` / clip / 行数 | `p25-guild-slots-c.md`、`p10-count-research.md` |
| `ScrollWheel` 的 `VerifyExtra` / `VerifyGuild` 会在运行时逐字节校验 `0x6837C0`、`0x66E9F0`、`0x7F5EDD` 等地址 | `src/Hooks.cpp:65-138` |
| `DD.Plugins` 是旧式 DLL（`DllMain`）体系，与 `DD.ModLoader` 新 SDK 的 `DDPlugin_Query/Load` 互不兼容 | `DD.ModLoader/docs/adr/0017` |
| **无** minidump、异常码、崩溃堆栈；`app.log` 只有 `Post-crash check saved timestamp` | 阶段一 A 组、红队 A |

### 1.2 被对抗审核推翻的假设（不得再作为设计依据）

| 假设 | 推翻证据 |
| --- | --- |
| `character.layout.darkest` 的 `.max_rows 7` 是崩溃容量 | 它是角色页技能网格显示参数；`.combat_skills_size` 在 `:52` 不属网格节 |
| `roster.layout.darkest` 的 `.element_max 8` 是工会技能上限 | 全局唯一命中，语义是「名册每页可见行数」，10 处引用全在 `0x6Bxxxx` 名册函数 |
| 64 槽影子表会越界写导致崩溃 | 三处循环都是 `jbe/mov` **钳制**，索引 ≤63；后果只可能是功能截断 |
| v6 每 16ms 写共享常量 `0x2B8EBAC` 是崩溃 | 实机证伪为「不同技能数英雄网格与名字横幅**错位**」，v8 起已停止写入 |
| 给 `guild.layout.darkest` 加 `.element_max` 能解决 | 游戏只解析 4 个 float，新增键不会被读取 |
| ADR 0001 的 NO-GO 表示工会不可改 | 该 NO-GO 对象是「工会**滚轮**」，已被 v11 重评取代并实机验收 |
| DIAG `gActive=false gN=0 gMax=0` 是链路失效 | 同行 `gHit/gSeenB/gBeat/gACalls/gBCalls` 非零；是已知可观测性缺口 |

**结论**：崩溃点无法从现有资料定位；唯一有确证证据的结构性落差是
「影子表固定 64 槽 vs 最多 192 个技能」。

---

## 2. 决策

### D1 — 合并进 ScrollWheel，不新建独立 DLL

用户明确指定。工程上同时消除了红队 B 的 **C1/C3**：两个 DLL 若都 patch
`0x66E9F0` / `0x7F5EDD`，会因 `DD Mod Lib` 加载顺序不确定而互相使对方的
运行时 stolen 字节校验失败，连带禁用已实机验收的 v10 四个 cave。
合并后只有一个 DLL 写这些字节，校验链保持原样。

### D2 — 只扩 DLL 侧影子表，不钳制游戏创建的 UI 槽数

`GuildTable` 的 `slots/baseY/offset` 从 64 扩到 **256**（覆盖实测 192 + 余量）。
**绝不**改游戏自己的 `+0x270` vector、**绝不**减少游戏创建的技能槽数量。
理由：红队 B **C4** —— 只砍 UI 生成数量会让点击第 k 个可见槽映射到错误的技能，
比原崩溃更严重；**C5** —— 跨 CRT 用插件堆扩容游戏容器会导致堆损坏。

### D3 — 有效上限搬进表里，cfg 可一键回退

新增 `GuildTable.maxSlots`（偏移 `0x1040`），由 `Hooks::Install` 按 cfg 写一次；
两个 cave 从表里读上限，不再内嵌立即数。
`guild_max_slots = 64` ⇒ **机器码完全不变**地回到 v11.2 旧容量行为。
纯函数 `Config::EffectiveGuildMaxSlots()` 把玩家输入钳到 `[1, 256]`，
生产与离线单测共用一份逻辑。

### D4 — 纵深防御而非「定点修复」

两个 cave 增加：

- `begin == nullptr` / `end < begin`（半初始化或被污染的 vector）→ `count = 0`，
  本帧零干预，**绝不**遍历 `[begin + i*8]`；
- `maxSlots` 未初始化（0）→ 同样 `count = 0`；
- 技能槽指针非 8 对齐 → 写 0 占位，不做 `[slot+0x68]` 解引用；
- `setDefaultJmpNEAR(true)`：新增分支使前向 `jcc` 的 rel8 短跳超过 ±127，
  Xbyak 会抛 `label is too far`，统一改用 rel32（每条跳转多 4B）。

`count = 0` 会被 `UpdateGuild` 判为 `active=false` → `offset[]` 全 0 → 零干预，
即「表不可信时宁可不生效，也不带着野指针刷新」。

### D5 — 配套抬高滚动范围，但不覆盖玩家 cfg

`guild_auto_cap = 1`（代码默认）：当 `rawMax = (N - R) * rowH` 超过玩家 cfg 的
`guild_cap`（默认 2000）时，按 `rawMax * 1.05 + rowH` 抬高上限，并设 100000 绝对硬上限。
这样**老 cfg 一个字都不用改**，192 个技能也能滚到底；
`guild_auto_cap = 0` 严格回到旧行为。cfg 模板仍只在文件不存在时生成。

### D6 — 验收定性

**只声称「消除已确证的结构性风险 + 纵深防御 + 可诊断可回退」，
不声称「已定位崩溃根因」或「已修复该崩溃」。**
离线单测 PASS 只证明表布局、cfg 逻辑、cave 机器码与 ASLR 判据正确，
**不能**证明游戏内不再崩溃（红队 B **C6**、ADR 0001 的 v11 实机证伪教训）。

---

## 3. 被否决的方案

| 路线 | 否决理由 |
| --- | --- |
| 新建独立 DLL 去 hook `0x682D40` / `0x6837C0` 钳制技能数 | 红队 A/B 双重否定：这两个函数的语义被误判（`0x682D40` 是动作开始回调，`0x6837C0` 是布局重算）；且会与 ScrollWheel 的运行时字节校验冲突、连带禁用 v10 |
| 只改 `.darkest` 布局 / 配置 | `guild.layout.darkest` 加 `.element_max` 无效；数据层改 `*.info.darkest` 属「牺牲 mod 内容」的规避而非修复 |
| 扩大游戏 UI 容器（借游戏自己的 `ReallocIcons`） | 仅当根因确为容量不足才成立，且 `p4-progress.md` 记载「表内指针扩容/删除会失效」；无 dump 佐证 |
| 用 `DD.ModLoader` 新 SDK 的 `DDPlugin_Query/Load` | ADR 0017 明确不兼容现有 DD 插件，等于换工程 + 换部署 |
| 禁用 / 回退 `guild_scroll` | 只是 A/B 对照与缓解手段，不是修复；但保留为取证第一步 |

---

## 4. 不破坏既有功能的保证

- v10 四个 cave（输入 cave `0x371F0A`、面板 gate `0x7E9F80`/`0x7EADE0`、
  逐格裁剪 `0x7EBF70`）的 RVA 与机器码**一字未改**；
- `VerifyExtra` / `VerifyGuild` 校验的游戏字节未动；校验失败仍只跳过工会 hook、绝不 `return`；
- 不写共享布局常量 `0x2B8EBAC` / `0x2B8EBB4`（只读）；
- ASLR 判据仍是 `g_base + 0xEBB8D8`（`expectedVptr` 偏移 `0x438 → 0x1038`，单测已断言新编码）；
- 每个 cave 的无条件执行计数 `gACalls/gBCalls` 保留；
- DIAG 每秒一条的节流保留，只新增 `gSlots` / `gAutoCap` 两个字段；
- cfg 首次生成、绝不写回玩家文件。

---

## 5. 后果与风险

**正面**：影子表容量与实测最大技能数匹配；vector 异常时不再可能带着野指针遍历；
旧 cfg 无需改动；一键回退路径完整。

**代价 / 风险**：

- `GuildTable` 从 `0x440` 增至 `0x1048`（约 4KB 静态内存）；`offset[]` 每帧全量
  清零 256 个 float（约 1KB 写，可忽略）。
- cave 改用 rel32 后机器码变长，但仍远小于 `AllocTrampoline(1<<12)` 预算；
  离线单测已验证两个 cave 能生成、`FF25` 跳回、stolen 字节原样重放。
- 影子表扩容**只解决 DLL 侧容量落差**；若崩溃根因在游戏自身或 mod 数据，
  本改动不保证消除崩溃 —— 需按 `README.md §9.5` 采 minidump 再定位。
- 所有 RVA / SHA1 仍绑定单一 exe 版本（`9db2d73b…`）；游戏更新后必须重跑
  `check_sigs.py` 与离线单测。

---

## 6. 验证记录

| 项 | 结果 |
| --- | --- |
| 改动前基线 | `scroll_wheel_test.exe` **139 PASS / 0 FAIL** |
| TDD RED | 新增 2 条断言后 **139 PASS / 2 FAIL**（容量 ≥192、cfg 暴露新键） |
| TDD GREEN | 实现后 **150 PASS / 0 FAIL**，exit 0 |
| 生产 DLL | `xmake build -P . scroll_wheel` 成功；`build\windows\x64\releasedbg\scroll_wheel.dll` 671,744 B |
| 部署 | `scroll_wheel.dll` **未部署**（`DD_DEPLOY` 未设 + `ScrollWheel/xmake.lua:18-40` 双门控，游戏目录仍是 v11.2 的 664,576 B 旧版）。但一次 `xmake build -P .` 全量构建触发了 `xmake/plugin.lua:74-85` 的 `after_build`，把另外 5 个插件 DLL 复制进 `mods\`；实测覆盖前后 SHA256 **完全一致**（同一份源码产物，仅刷新时间戳）。已写入 `docs/DEV-TEST-DEPLOY.md` 的环境坑 |
| 实机验收 | **未进行**，按 `README.md §9.4` 由用户在测试目录完成 |

### 调试过程中修复的问题（保留为经验）

1. `xmake` 配置缓存被父目录 `D:\workplace\xmake.lua` 污染成 `projectdir = D:\workplace`，
   表现为 `scroll_wheel_test is not a valid target`。正确构建姿势：
   **`xmake build -P . <target>`**（action 在前，`-P .` 随后），必要时先
   `xmake f -P . -p windows -a x64 -m releasedbg -y`。
2. 新增分支让 Xbyak 抛 `label is too far`（前向 `jcc` 默认 rel8 ±127）→
   用 `setDefaultJmpNEAR(true)` 统一 rel32。
3. 测试输出块缓冲会掩盖崩溃点 → `wmain` 加 `setvbuf(stdout, nullptr, _IONBF, 0)`，
   并给 `TestV11Guild()` 包 `try/catch` 打印 `Xbyak::Error`。

---

## 7. 后续工作（未决）

- 实机验收 `README.md §9.4`，特别是「工会 + 角色页面同屏 + 192 技能」复现矩阵；
- 若仍崩溃：按 `§9.5` 采 minidump、做 `guild_scroll = 0` 二分，再决定是否做定点补丁；
- 复核 DIAG 可观测性缺口（v11.2 遗留，功能生效但 `gN/gSeenB` 可能为 0）；
- 游戏更新后重校 AOB / SHA1。
