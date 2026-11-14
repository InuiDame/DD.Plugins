# ScrollWheel — 给无原生滚轮的界面加鼠标滚轮（v11.2）

> **v11.2（当前部署）**：修两个实机问题。
>
> **实机验收结果（用户 20:15 确认，全部通过）**：
> **补给面板门控与 v10 回归通过**——`pRecent` 随面板开关、`cur` 在非目标界面恒 0、
> `storeY` 保持 20（打开补给停在第一页）、补给内滚动与 `skipped` 裁剪正常。
> **工会滚轮用户实机确认「翻页完全生效」**，按已验收定稿。
> 说明：本次抓取的 109 条 `[DIAG]` 里工会统计字段全 0，与实机观察不一致；
> 已复核只有 1 份 DLL、1 份日志、cfg 开关正确，判定为 **DLL 侧工会 DIAG 的
> 可观测性缺口**（统计未覆盖实际生效路径），不是功能缺陷，不影响交付。
> 证据与复核见 `p31-v112-acceptance-and-closeout.md` §2/§3。
> ① **工会真正根因 = ASLR**：v11/v11.1 的 cave 把 PE 磁盘基址下的 vtable
> `0x140EBB8D8` 硬编码进 `movabs`，而游戏实际加载到 `0x7FF7D7DD0000`
> （日志 `gVtA=7FF7D8C8B8D8`），判据永不成立。v11.1 加的 `gACalls/gBCalls`
> 反证两个 cave 一直在跑（258 / 196565 次），**P29「链没执行」的结论是错的**。
> 现在判据由 C++ 按 `g_base + 0xEBB8D8` 写进表，cave 只从表读。
> ② **补给预翻页**：`Wheel::Tick` 的 `layoutHit` 曾被硬编码 `true` 且每帧无条件
> 写三套补给布局 ⇒ 在城镇/工会滚滚轮会改写补给 `start_y`，之后打开补给已被翻页。
> 新增 `gate_wheel_by_panel`（默认 1）按面板激活门控滚轮与布局写入，`0` 一键回退。
> 证据见 `p30-v112-aslr-and-panel-gate.md`（并含 P29 更正），验收清单见 §8.4。
>
> **v11.1（已证伪并替换）**：把技能槽表刷新并入 `Base::Show`（`0x7F5EDD`），
> 并加 `gACalls/gBCalls/gSeenB/gVtA/gVtB` 五个自检字段——正是它们给出了 ASLR 的
> 决定性证据。换链方向正确，但没修判据常量，工会仍不生效。
>
> **v11（已证伪并替换）**：按用户 11-13 要求继续攻坚工会，两路静态调研 + 独立对抗审核
> 推翻了 ADR 0001 旧 NO-GO 的核心理由（「无注入点」），确证 `0x7F5EDD` 可只改
> `Base::Show` 栈帧里的 Y、绘制/命中/tooltip 同源。新增**独立开关 `guild_scroll`**，
> 与已实机验收的补给/背包链路完全隔离；关闭时两个新 cave 一个都不装。
> 证据见 `p25-*` / `p26-*` / `p27-*` / `p28-*`。
>
> **v10（补给 / 背包，已实机验收）**：解决「往下拉后，滚上去的物品还画在城镇背景上遮挡视线」。
> 仍然**不自绘任何 UI**（v9 已按用户要求移除滚动条；饰品栏用游戏原生滚动条）。
> 这版只加一件事：**把已经滚出网格上沿的格子整格跳过**（不绘制、不注册热点），
> 绘制与命中同源，索引不变，滚动依旧连续平滑。
>
> **为什么能安全地重做裁剪**（v7 曾因裁剪把整屏物品和饰品栏一起裁没）：
> 1. **面板判据换成 `r8` 布局地址**：两个网格渲染器 `0x7E9F80` / `0x7EADE0`
>    入口各装一个只读 cave，比对 `{0x2B8DB10, 0x2B8DB30, 0x2BE38A0}`，
>    只在这三套目标布局上置标志；城镇饰品库 `0x6A6739→0x2B8E8EC` 等 10 个
>    调用点一律写 0 → 逐格函数零干预。不再用区分不了面板的返回地址白名单。
> 2. **行号来自 `param_5` 的真实位域**：调用点把 `row` 打包进
>    `((col&0x1f|0x20)<<0x18 | row&0xffffff)*2`，`row=(p5>>1)&0xffffff`。
>    槽位 `[R0+0x28]` 由 `7EC1EF mov eax,[rbp+0xc70]` 确证（p18 审核修正了
>    初版错位 16 字节的错误）。不再用 `param_3` 猜行号。
> 3. **只裁上界**：`first = ceil(current/rowH)`，只跳过 `row < first`。
>    数学上这些格子的 `Y = base - current + row*rowH < base`，必然已在网格上方；
>    **不裁下界**，绝不会把仍在可视区的格子裁掉导致点击失效。
> 4. **四重零干预**：静止 / 非目标面板 / `param_5` 校验不过 / cfg 关闭时全部放行。
>    返回 false 在两处调用点都无计数或索引副作用（反汇编确证）。
>
> 保留 v9 已实机验证的滚动链路：事件泵 cave `0x371F0A` → SPSC 队列 →
> 独立 16ms tick → 连续平滑 → 写三套布局数组 `[2]`，各布局用自己 `[4]` 行高算窗口。
> 动态范围改由观测到的**真实最大行号**推导，物品越多可下拉范围越大。
> **旧 `guild` 键恒不生效**（v6 实机证伪路径的历史遗留），v11 用独立新键
> `guild_scroll`。NO-GO 的旧论证与 v11 的重评依据见
> [`docs/adr/0001-guild-scroll-no-go.md`](docs/adr/0001-guild-scroll-no-go.md)。
>
> **v10 已于 2026-11-13 11:21~11:37 实机验收通过**：日志 `p5fail=0`
> （`param_5` 槽位与位域编码全部正确）、`skipped` 0→25870（上溢格子确实被跳过）、
> `active` 出现 0/2/3 且 `active=0` 时零干预、`clip=-1` 静止零干预。
> 证据见 `build/scrollwheel/p21-v10-acceptance.md`。
> 若画面异常，把 `clip_out_of_window` 改 `0` 即一键回到 v9 的「只平移不裁剪」，
> 滚动功能完全保留。
> 历史版本的问题与证伪过程见 `p10-*.md` / `p11-*.md` / `p13-*.md`。

> **版本历史**（细节见 `build/scrollwheel/` 下的阶段报告）
> - v5：独立 16ms tick 线程驱动状态机 → 修复「滚轮完全无反应、零日志」。
> - v6：接入地牢背包 `0x2BE38A0` 与按物品数的动态范围 → 补给/背包滚轮生效。
> - v7：跳格裁剪 + 自绘 18×700 滚动条 → **实机证伪，全部撤销**。
> - v8：恢复连续平滑滚动、停止写工会共享常量、滚动条改细条 → 细条仍会在无关界面显示。
> - v9：彻底移除自绘滚动条，只保留滚动本身。
> - **v10（本版）**：`r8` 面板 gate + `param_5` 行号，只裁滚出上沿的格子，消除外溢。
> - **v10.1**：实机验收通过后的纯文案修订，功能代码零改动。
> - v11（**实机证伪**）：工会技能栏滚轮，`0x66E9F0` 刷新技能槽表 + `0x7F5EDD` 栈帧偏移。
>   首次实机 `gBeat/gN/gHit` 全程为 0（当时误判为「捕获链未执行」）。
> - v11.1（**实机证伪**）：刷新改由 `0x7F5EDD` 完成并新增 5 个自检字段；
>   日志反证两个 cave 一直在跑，真正根因是 ASLR 判据常量。
> - **v11.2（当前部署）**：判据改用运行时 `g_base + 0xEBB8D8`；
>   新增 `gate_wheel_by_panel` 修补给预翻页；单测 139 PASS。
>   **20:15 实机验收：补给门控 / v10 回归 / 工会滚轮三项全部通过**（p31）。

---

`DD.Plugins` 中的一个 C++ DLL 插件，由 DDModLoader 加载，给《暗黑地牢》(Darkest Dungeon) 中**原本不响应鼠标滚轮**的界面补上滚轮滚动。

> **重要声明**
> 1. 所有逆向结论来自反汇编 / PE 映射 / 离线单测（[静态]）；补给/背包滚轮与 v10 裁剪均已实机验收。
> 2. **cfg 模板默认三个面板全关**（安全模式）；你当前使用的 `ScrollWheel.cfg` 已打开
>    `provision` / `raid_inventory` / `clip_out_of_window` / `guild_scroll`（v11 实验性）。
>    旧 `guild = 1` 是历史遗留值、**恒不生效**；工会在 v11 由独立键 `guild_scroll` 控制。
>    DLL 只在首次生成 cfg，之后**绝不覆盖**你的配置。
> 3. 饰品库 `RealmInventory`、英雄名册、游商饰品等界面**已原生支持滚轮并有原生滚动条**，
>    本 DLL 一律不介入；v9 不再自绘任何 UI。

---

## 1. 调研结论：哪些界面缺滚轮，为什么必须写 DLL

基准：`_windows/win64/Darkest.exe`，SHA1 `9db2d73b89aa034e2055df65a139403e18df641f`，ImageBase `0x140000000`，下文地址一律 RVA。

| 事实 | 证据 |
| --- | --- |
| 游戏用 `SDL_PollEvent` 轮询；`SDL_MOUSEWHEEL` 是 `0x403`（不是 `0x401`） | 事件泵 `0x371D60` 的 switch，分支 `0x371EA8` |
| 滚轮事件**没有被丢弃**：`wheel.x/y` 被累加到全局 `0x118D784` / `0x118D754`，帧末清零 | `0x371F0A..0x371F1F`、`0x23608E..0x2360A0` |
| 该全局有**多写者**：`0x235BA0` / `0x28B520` 清零，`0x43D000` / `0x43D330`（`ChooseItemTownControllerState`，手柄模拟滚轮）用 `-0.0` 取负赋值 | 全 `.text` rip 引用扫描；`SDL_GL_SwapWindow` 每帧回调时序也不满足 |
| 游戏已有完整的通用滚动引擎：`ScrollBarWidget`（虚表 `0xE680D8`）、`Widget` 的 `0x39B640`（`+0xE8` 滚动偏移）、`0x39A360` clamp/平滑、`0x39B3B0` 裁剪 | RTTI / 虚表 / Ghidra |
| **已原生支持滚轮**：城镇饰品库 `RealmInventory`(`0x6A7A00`, `+0x240`)、游商彗星饰品 `CometTrinketSupplyDisplay`(`0x693EB0`, `+0xA8`)、英雄名册 `RosterList`(`0x6B5950`, `+0x1D8`)、地牢地图缩放 `0x73D9C0` | 统一模式 `movss xmm1,[0x118D754]; mulss xmm1,[0xEE63DC=30.0f]; subss [this+off],xmm1` |
| **不响应滚轮**：工会 `GuildDisplay`（虚表 `0xEBB8D8`，45 槽）、出征补给 `ProvisionSelect`（虚表 `0xEBDDE8`，27 槽）、地牢背包 `Panel_Inventory`（虚表 `0xECAC68`，13 槽） | 逐槽扫描全部槽位函数体，零个读 `0x118D754`；布局文件无 scroll 段 |
| 这些界面不走通用 Widget 引擎，是即时模式绘制（网格 + 逐格建按钮），**没有滚动偏移字段，也没有裁剪** | Ghidra 反编译 `0x695460`、`0x7E9F80`、`0x7EADE0` |
| 纯数据/配置无法启用滚轮：exe 里 `wheel` / `mousewheel` 字符串零命中，所有 `.scroll_*` 布局键都只是几何参数 | 字符串表 + 全部 `*.layout.darkest` |

**当前 v10 实现**：在事件泵捕获原始滚轮值（绕开多写者全局）→ 独立 tick 线程维护
`target/current` 连续平滑状态 → 把 `start_y = base - current` 写进布局数组 `[2]` 平移网格
→ 两个网格入口 cave 用 `r8` 判面板 → 每格 cave 用 `param_5` 行号跳过已滚出上沿的格子。
**不做入口 detour、不做截断数据源、不做整行量化**：p17 确证两条绘制链互斥且都是
「绘制+热点」完整渲染器，只改一条反而更糟；p10 §5.2 推荐的「面板 gate + 逐格裁剪」
是静态可确证、绘制/命中同源的最小方案。
文中关于「虚拟裁剪 / 整行量化 / 自绘滚动条」的描述均为 v7 及更早版本的**历史记录**，
以本版开头与 cfg 为准。

### 覆盖与默认状态

| 界面 | 默认 | 说明 |
| --- | --- | --- |
| 出征补给 `ProvisionSelect`（`0x2B8DB10` / `0x2B8DB30`） | 关闭 `provision=0` | **已实机验证生效**；布局地址、两条逐格绘制路径、`[2]` 语义 [静态] 确证 |
| 地牢背包 `Panel_Inventory`（`0x2BE38A0[2]`） | 关闭 `raid_inventory=0` | v6 新增：与补给完全同构的 6 元素布局数组（`0x736320 lea r8` → `0x7EADE0`），写入语义相同 [静态] |
| 工会 `GuildDisplay`（`0x2B8EBAC`） | **不支持** | v6 曾试写共享起始 Y 全局，实机造成不同技能数英雄网格与名字横幅错位；v8 起停止注入。p22/p23 静态调研 + 独立对抗审核判定 NO-GO，`guild` 键恒不生效（见 `docs/adr/0001`） |
| 城镇饰品库 / 名册 / 游商 / 地图等 | **DLL 一律不介入** | 已原生支持滚轮，重复滚动只会打架；`patch_native_step` 默认关 |

---

## 2. 快速开始

1. 按第 4 节构建并部署 `scroll_wheel.dll` 到 `mods\DD Mod Lib\`（或先只构建，用离线单测验证）。
2. 首次启动游戏，DLL 在 `<游戏根目录>\DDModLoader\cfg\ScrollWheel.cfg` 生成默认配置；已存在则不覆盖。
3. **默认安全模式**：日志里会看到 `all panels disabled (default safe mode)`。此时不注入渲染，可在日志里确认校验与输入捕获正常。
4. 按第 8 节实机逐项验收，确认无误后把对应面板改成 `1` 并重启游戏。
5. 出问题随时一键关：`enabled = 0`（或只关该面板），或按第 7 节在 `DDModLoader.cfg` 里禁用整个 DLL。

### cfg 键说明

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `enabled` | `1` | 总开关；`0` 时连输入捕获 cave 都不装 |
| `provision` / `guild` / `raid_inventory` | `0` / `0` / `0` | 各面板注入开关，**默认全关** |
| `step` | `30.0` | 每格滚轮步长，默认对齐游戏原生 `0xEE63DC = 30.0f` |
| `smooth` | `8.0` | `current` 平滑逼近 `target` 的系数 |
| `visible_rows` | `5` | 可见行数 [推测]，实机用采样核对 |
| `row_height` | `40.0` | 行高（与 `step` / `max` / `firstRow` 同单位）[推测] |
| `columns` | `7` | 运行时读不到 `layout[0]` 时的回退列数 [推测] |
| `invert` | `0` | `1` 反转滚轮方向 |
| `frame_ttl` | `1` | 面板激活标记的帧 TTL |
| `scroll_range` | `400.0` | 动态范围算不出来时的回退上限 |
| `auto_range` | `1` | `1` 按每帧格子数动态扩大可下拉范围（物品越多滚得越远） |
| `max_range` | `2000.0` | 动态范围上限，防止格子数异常放大 |
| `clip_out_of_window` | `1`（用户 cfg 同为 `1`） | **v10 裁剪**：只跳过已滚出网格上沿的格子（不裁下界）。`0` = 回到 v9 的「只平移不裁剪」，一键回退 |
| `rows_per_notch` | `1` | **【v8 已废弃】** 整行量化已删除，滚动恢复连续平滑 |
| ~~`show_scrollbar` / `bar_*`~~ | — | **v9 已移除**：DLL 不再解析这些键、不画任何 UI；饰品栏等界面用游戏原生滚动条 |
| `debug_sampling` | `0` | `1` 输出 `[SAMPLE]` 采样日志（零副作用） |
| `debug_log` | `0` | `1` 打开 DEBUG 级日志（含 `[DRY-RUN]`） |
| `patch_native_step` / `native_step` | `0` / `30.0` | `1` 才改写 `.rdata 0xEE63DC`；**只给已原生支持的界面调速，不满足本需求** |
| `*_rect` | `0,0,1920,1080` | 每面板鼠标矩形 `x,y,w,h`（UI 1920x1080）；默认全屏放行 [推测]，采样后应收紧 |

支持 `#` / `//` / `;` 注释与 `key = value`；缺键自动补默认值；未知键忽略。

---

## 3. 生效时机与数据流

`DllMain` / `DLL_PROCESS_ATTACH`：初始化日志 → 读取/首次生成 cfg → `Game::Verify` 全有或全无校验（SHA1 + 28 项）→ 校验通过才 `Install()`。

```
[输入] cave @0x371F0A 捕获原始 SDL_Event.wheel.y (Sint32) -> lock-free SPSC 环形队列
   |    （绕开 0x118D754 的多写者竞态，不读那个全局）
[tick 线程] 独立 std::thread，每 16ms 无条件执行（不依赖任何游戏函数是否被调用）：
        消费队列 -> cfg 开关 + 鼠标区域 gate
        -> target -= wheel*step -> clamp[0, scroll_range] + NaN/Inf 防护
        -> current 平滑逼近 -> firstRow = current/rowHeight
        -> 每秒写一条 [DIAG] 日志（输入/队列/布局/状态全量）
[数据补丁] 把 start_y = base - current 以绝对值写入 .data 布局数组
        0x2B8DB10[2] 与 0x2B8DB30[2]（两个网格函数共用同一布局，故两条路径一起平移）
[虚拟裁剪·可选] cave @0x7EBF70，仅当 clip_out_of_window=1 且已滚动时生效：
        idx=cellIndex++, row=idx/columns
        row < firstRow 或 >= firstRow+visibleRows
          -> xor eax,eax; ret（跳过整个函数：不绘制、不注册热点）
        否则 -> 重放 stolen 5B 并跳回 0x7EBF75，原样绘制
[边界] max = cfg.scroll_range（固定，默认 400）；每格统计只用于诊断，不参与 max
```

`base` 自适应：每帧读回 `layout[2]`，若与上次写入的绝对值不同（说明布局加载器重载）就刷新 base，切界面后不会累积偏移；磁盘值 0 / 非有限 / 列数 < 1 时跳过写入。

---

## 4. 构建、测试、部署流程

本插件是 `DD.Plugins` 根 `xmake.lua` 自动扫描到的 target，公共规则在 `DD.Plugins\xmake\plugin.lua`（DKUtil、xbyak、spdlog、C++23 / MT、`Plugin.h.in`、版本资源）。

### 4.1 环境准备

| 项 | 值 |
| --- | --- |
| Visual Studio 2022 / 2026 | 「使用 C++ 的桌面开发」 |
| xmake | 最新即可（本机 3.1.2+） |
| `DKUtilPath` | 本机 `D:\WorkSpace\DKUtil`；缺失时 `.\!update.ps1 -Extern` |
| `DDPath` | 游戏根目录。**本机用户级已设置且指向真实安装**，构建时必须清除 |
| `DD_DEPLOY` | 部署开关，只有显式 `=1` 且 `DDPath` 非空时才写游戏目录 |

```bash
source D:/workplace/DD.ModLoader/tools/env.sh   # 补 xmake PATH / DKUtilPath
```

> 本插件的 `xmake.lua` 不使用根规则的 `set_values("dd.mod")`（根 `plugin.lua:74-85` 只看 `DDPath` 就无条件复制 DLL）。改由本 target 自己的 `after_build` 同时检查 `DD_DEPLOY=1` 与 `DDPath`，默认绝不触碰真实游戏目录。

### 4.2 开发（只构建）

```bash
cd D:/workplace/DD.Plugins
xmake f -p windows -a x64 -m releasedbg -y
env -u DDPath -u DD_DEPLOY xmake build scroll_wheel
```

产物：`build\windows\x64\releasedbg\scroll_wheel.dll`（+ `.pdb`）。

### 4.3 测试（离线，不启动游戏）

```bash
env -u DDPath -u DD_DEPLOY xmake build scroll_wheel_test
env -u DDPath -u DD_DEPLOY build/windows/x64/releasedbg/scroll_wheel_test.exe \
  "<游戏目录>/_windows/win64/Darkest.exe"
```

单测直接编译生产源码（`Wheel.cpp` / `Config.cpp` / `Game.cpp` / `CaveAsm.h`），在映射的真实 PE 上验证 74 项：滚轮符号、clamp / NaN / Inf、SPSC 队列溢出与 wrap 契约、**cave 汇编生产者读取偏移 `[rsp+0x64]` 与 wrap 机器码**、每格 cave 的两个分支与返回地址门控（两条路径）、默认全关策略、28 项全有或全无校验、13 条特征码唯一命中、「改坏一字节 → 一个 hook 都不装」、cfg 首次生成不覆盖、布局数组元素类型与 `+8` 偏移。输出 `0 failure(s)` 且退出码 0 为通过。

其它离线校验工具（来自加载器工作流）：`python -B DD.ModLoader/tools/check_sigs.py` 做特征码离线校验；`python -B DD.ModLoader/tools/rig.py status|deploy|run|save-log` 在测试目录 `DarkestDungeon_Dev` 里跑，真实安装不动。

### 4.4 部署（需显式允许）

```bash
export DD_DEPLOY=1
source D:/workplace/DD.ModLoader/tools/env.sh   # 此时才导出 DDPath
env DD_DEPLOY=1 xmake build scroll_wheel
```

- DLL → `%DDPath%\mods\DD Mod Lib\scroll_wheel.dll`（加载器强制加载、GUI 关不掉）。
- `mod\ScrollWheel.cfg` → `%DDPath%\DDModLoader\cfg\ScrollWheel.cfg`，**仅当目标不存在时**复制；DLL 首次启动也会自行生成，绝不覆盖玩家已改配置。
- 游戏运行时 DLL 被占用，部署前先关闭游戏。

### 4.5 日志

`<游戏根目录>\DDModLoader\log\scroll_wheel.log`。

### 4.6 流程小结

开发（清除 `DDPath`）→ 离线单测 → 用户手动 `DD_DEPLOY=1` 部署 → 进游戏按第 8 节实机验收 → 逐面板开启 cfg → 有问题一键关。整个链路不 git commit、不改游戏 exe / 自带资源 / 存档。加载器工作流规范见 `DD.ModLoader/docs/06-workflow.md`。

---

## 5. 实现原理（逆向基准）

### 5.1 hook 清单（**零入口 detour**）

| hook | RVA | 类型 | 偷取字节 | 跳回 |
| --- | --- | --- | --- | --- |
| H-IN 输入捕获 | `0x371F0A`（事件泵 `0x371D60` 内 `0x403` 分支） | 函数内 cave，Xbyak 纯整数 prolog | 9B `66 0F 6E 44 24 34 0F 5B C0` | `0x371F13` |
| H-CELL 虚拟裁剪 | `0x7EBF70`（`InventoryButtonInGrid`） | 函数内 cave，返回地址门控 | 5B `48 89 5C 24 20` | `0x7EBF75` |
| 数据补丁 | `.data` `0x2B8DB10+8` / `0x2B8DB30+8` | 纯内存写（float `start_y` 绝对值） | — | — |
| 每帧驱动 | DLL 独立 `std::thread`（16ms） | 非 hook：消费队列、跑状态机、写布局、打 `[DIAG]` | — | — |

> v4 的每帧驱动挂在 H-CELL 门控里，实机表现为「日志只有启动行、滚轮静默失效」。v5 把驱动移到独立线程后，H-CELL 只做可选裁剪，队列消费与布局写入不再受游戏函数调用时序影响。

- **不使用入口 detour**：`0x7E9F80` / `0x7EBF70` 的头几个字节是 `mov rax,rsp` / `mov [rsp+0x20],rbx`，DKUtil 的 `write_detour` 逐字复制 stolen 且无 rsp 补偿，转调原实现必然栈帧错位（p4-review B3）。cave 是 `jmp` 进 stub，rsp 仍等于入口 rsp，成对 push/pop 后 epilog 原样重放 stolen，没有这个问题。cave 里 `call` C++ 门控前 `sub rsp,0x20` 保持 16 字节对齐。
- **H-CELL 跳过分支**：`xor eax,eax; ret`。cave 由 `jmp` 进入，栈顶仍是原函数返回地址，`ret` 正确返回调用者并返回 `false`。放行分支重放 5B 后经 `jmp qword ptr [rip+0]` 跳回 `0x7EBF75`。
- **H-IN prolog**：6 次压栈（`pushfq` + rax/rcx/rdx/r10/r11 = 48 字节），故原始 `wheel.y` 在 `[rsp+48+0x34]` = `[rsp+0x64]`；只做整数读 + 环形队列写，不碰 xmm、不调函数、不分配内存；`write=(write+1)&255` 与 `Wheel::Push` 同契约。
- `0x7E9F80` 仍参与 28 项校验与 13 条唯一特征码定位，但**不再被 hook**。
- 三个注入点与 `knowledge/dd1/hooks.md` 现有清单零 RVA 冲突；不装虚表 hook（规避 1 参 / 2 参槽签名不一致）。

### 5.2 RVA / 调用点表

| RVA | 含义 |
| --- | --- |
| `0x371D60` / `0x371EA8` / `0x371F0A` | SDL 事件泵 / `0x403 SDL_MOUSEWHEEL` 分支 / cave 点 |
| `0x371F13` | epilog 跳回（写回 `[0x118D784]` 的 rip 相对 `movss` 起点，偷 9B 恰停其前、不截断） |
| `0x118D754` / `0x118D784` | wheel_y / wheel_x 帧内累加器（多写者，**本插件不读**） |
| `0x7E9F80` / `0x7EADE0` | 两个通用物品网格绘制函数，各自有逐格循环 |
| `0x7EA6CD` / `0x7EB5AD` | 两处 `call 0x7EBF70`，返回地址 `0x7EA6D2` / `0x7EB5B2`（门控白名单） |
| `0x7EBF70` | `InventoryButtonInGrid`（9 参，返回 bool），虚拟裁剪点 |
| `0x695460` | `ProvisionSelect::PreChildrenShowInternal`，对同一布局数组同时调用两个网格函数 |
| `0x695B79` / `0x695E1C` | `lea r8,[0x2B8DB10]` / `lea r8,[0x2B8DB30]` 锚点 |
| `0x2B8DB10` / `0x2B8DB30` | store / party 布局数组：`[0]`int 列数 `[1]`float start_x `[2]`float start_y `[3]`float offset_x `[4]`float offset_y `[5]`int is_centred；`+8` 即 `[2]` |
| `0x62DB90` | 城镇布局总加载器（运行时填充上述数组，磁盘为 0） |
| `0xEE63DC` | 原生统一滚轮步长 `30.0f` |
| `0xEBB8D8` / `0xEBDDE8` / `0xEBEF28` / `0xECAC68` | Guild / Provision / RealmInventory / Panel_Inventory 虚表 |

### 5.3 校验与状态机

- 全有或全无：exe SHA1、映像基址、入口字节、5 个 `.rdata` 常量、4 个 rip 相对目标、3 个 rel32、4 个虚表槽、13 条唯一特征码、布局数组磁盘值，共 28 项；任一不匹配 → **一个 hook 都不装**并逐项打印 `[FAIL]`。校验基于**磁盘映射 PE**，不依赖运行时内存值。
- 符号：`target -= wheel*step`（与原版 `subss` 一致），`invert=1` 反转；`target` clamp `[0,max]`，`max/target/current` 每步 `isfinite` 防护；极端 Sint32 先经 int64 累加再转 float。
- `current` 以 `alpha = clamp(smooth/60,0,1)` 平滑逼近；`firstRow = current/rowHeight`；`max` 由每批次格子数自适应。
- 队列：256 槽 SPSC，`write=(write+1)&255`，`Drain` 用有界步数 `(w-r)&255`，对 wrap 与单调 write 两种生产者都不死循环、不越界。

---

## 6. 目录结构

```
DD.Plugins/ScrollWheel/
├─ xmake.lua                 target: scroll_wheel + set_default(false) 的 scroll_wheel_test
├─ README.md                 本文件
├─ mod/ScrollWheel.cfg       默认配置模板（部署时仅目标不存在才复制）
├─ src/
│  ├─ PCH.h                  PLUGIN_MODE / LOG_PATH 顺序照工程模板
│  ├─ main.cpp               DllMain：日志 -> cfg -> 校验 -> Install
│  ├─ Config.h / .cpp        cfg 解析、默认值、首次生成不覆盖
│  ├─ Wheel.h / .cpp         SPSC 队列、状态机（clamp/NaN/平滑/边界自适应）
│  ├─ Game.h / .cpp          RVA 常量、唯一特征码、28 项校验、SHA1、返回地址门控
│  ├─ CaveAsm.h              两处 cave 的 Xbyak stub（生产与单测共用同一份字节）
│  └─ Hooks.h / .cpp         cave 安装、C++ 门控、每帧 tick、数据补丁、鼠标区域
└─ tools/test_scroll.cpp     离线单测（映射 PE + 纯逻辑 + cfg + 汇编字节断言）
```

---

## 7. 排错

| 日志 | 含义 / 处理 |
| --- | --- |
| `Incompatible game version, no hooks installed ...` | 全有或全无校验有失败（exe 版本不符 / 特征码被别的插件破坏），日志逐项 `[FAIL]` 指明 |
| `all panels disabled (default safe mode)` | 模板默认安全模式，只装输入 cave；你当前 cfg 三个面板均为 `1` |
| `grid2 unique signature verified @+7EADE0` | **v10 正常**：第二个网格渲染器的长特征码在 `.text` 唯一命中 |
| `panel-gate caves installed @+7E9F80 / +7EADE0 ...` | **v10 正常**：两个只读面板 gate + 每格裁剪 cave 均已安装 |
| `v9: no custom scrollbar (game-native bars are left untouched)` | DLL 不创建任何窗口、不画 UI；饰品栏用游戏原生滚动条 |
| `legacy guild=1 key has no effect ...` | 正常 WARN：旧 `guild` 键恒不生效；v11 用独立键 `guild_scroll` |
| `v11 guild caves installed @+66E9F0 ... / +7F5EDD ...` | **v11 正常**：两个工会 cave 已安装 |
| `v11 guild scroll DISABLED by verification ...` | v11 校验失败，只跳过工会 hook；**v10 四个 cave 仍照常安装** |
| `v11 guild scroll disabled (guild_scroll=0)` | 开关关闭，两个工会 cave 一个都不装（默认安全模式） |
| `[DIAG] ... gACalls=.. gBCalls=.. gSeenB=.. gVtA=.. gVtB=.. pRecent=..` | v11.2 诊断见 §8.4 第 6 项；**先看 `gBCalls/gSeenB`（捕获链）与 `pRecent`（面板门控）** |
| `[DRY-RUN] ... (injection disabled)` | 面板关闭时走完整状态机但不注入；`debug_log=1` 才输出 |
| `[SAMPLE] active=.. row=.. first=.. vis=.. skip=..` | `debug_sampling=1` 逐格采样，核对行号解包与裁剪窗口 |
| `[DIAG] ... active=.. clip=.. p5src=.. p5fail=.. skipped=..` | **v10 格式**，每秒一条无条件。`p5fail` 持续增长 = `param_5` 槽位/编码假设不对，DLL 自动放行不裁剪（画面等同 v9），把日志发回；`active=0` = 非目标面板零干预；`clip=-1` = 静止零干预；`skipped` 增长 = 上溢格子被跳过 |
| `frame tick thread started (16ms)` | 每帧驱动线程已启动；没有这行说明面板全关或安装失败 |
| 屏幕上还有自绘细条 | 说明跑的不是 v9/v10；确认 DLL 已替换、日志出现 `no custom scrollbar` |
| `disabled by config (enabled=0)` | 总开关关闭，未装任何 hook |
| `Default cfg generated` / `cfg loaded` | cfg 首次生成或读取成功 |

**一键关闭 / 临时禁用**：

- 最小改动：把 `ScrollWheel.cfg` 的 `enabled = 0`（或只关对应面板）保存后重启游戏。
- 整个 DLL 不加载：在 `DDModLoader.cfg` 加一行（UTF-8，`1|路径` 启用 / `0|路径` 禁用）：

```
0|D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\mods\DD Mod Lib\scroll_wheel.dll
```

`DD Mod Lib` 里的 DLL 由加载器强制加载、GUI 关不掉，只能用上面的 `0|路径` 或移除文件禁用。

---

## 8. 限制与实机验收步骤

### 8.1 v10 当前限制（诚实清单，已实机验收）

- **工会滚轮在 v11 属实验性功能、尚未实机验收**：旧 ADR 0001 的 NO-GO 核心理由
  （「无注入点」）已被 `p25-*` / `p26-*` 推翻，但「开启态零误伤」静态仍不可证明。
  用独立开关 `guild_scroll` 隔离，代码/模板默认 `0`，用户 cfg 显式置 `1` 启用；
  验收清单、剩余风险与回退见 §8.4。旧 `guild` 键仍恒不生效，
  v11 也**不写** `0x2B8EBAC`（v6 实机证伪点）。
- **只裁上界**：滚到最下方时，超出网格下沿的格子仍会绘制并注册热点（可能在屏幕外）。
  静态无法确证真实可见行数，裁下界风险更高，属有意取舍。
- **动态范围**靠观测到的最大行号推导，跨面板切换有慢衰减，可能有轻微范围抖动（不影响裁剪）。
- **仅适配上述 SHA1 的 exe**；游戏更新后校验会主动禁用全部 hook，需重新校准 RVA。
- 内联 cave 与同样 hook `0x371F0A` / `0x7EBF70` / `0x7E9F80` / `0x7EADE0` 的其它 DLL
  冲突（后来者特征码匹配失败会跳过）；本插件与 `knowledge/dd1/hooks.md` 现有清单零冲突。
- 已原生支持滚轮的界面 DLL 一律不介入；`patch_native_step` 默认关，只改全局步长调速。

### 8.2 历史版本验收记录（v4~v8，勿照做）

v4~v8 的跳格裁剪 / 整行量化 / 自绘滚动条均已被实机证伪并撤销。当时的逐界面验收步骤
保留在阶段报告 `p5-acceptance.md`、`p9-clip-and-scrollbar.md`、`p13-v8-report.md`。
**当前交付以 8.3 的 v10 验收结果为准。**

### 8.3 v10 实机验收清单（2026-11-13 已通过）

> **验收结果：通过。** 实机日志 `<游戏根目录>\DDModLoader\log\scroll_wheel.log`
> 覆盖 11:21:22~11:37:02（约 16 分钟）。逐项证据见
> `build/scrollwheel/p21-v10-acceptance.md`。DLL 已部署到 `mods\new dll test area\`。
> 备份：`scroll_wheel.v10.dll.bak`（**已实机验收的 v10 本体**，md5 `3384bb7a…`）、
> `scroll_wheel.v9.dll.bak`（更旧的「只平移不裁剪」基线）。
> 当前部署的是 v10 的**纯文案修订版**（仅改启动 WARN 与 cfg 模板注释，
> 功能代码与已验收本体指令级等价，见 `p24-closeout.md` §6）。

1. **确认加载的是 v10**：启动日志应出现
   `grid2 unique signature verified @+7EADE0`、
   `panel-gate caves installed @+7E9F80 / +7EADE0 (r8 layout whitelist, read-only); per-cell clipping cave @+7EBF70 uses param_5 row`、
   `v9: no custom scrollbar (game-native bars are left untouched)`。
   `[DIAG]` 末尾带 `active= clip= p5src= p5fail= skipped=`。
2. **核心验收（出征补给）**：缓慢下拉几行。**滚出网格上方的物品不应再出现在
   城镇背景上**，网格上方保持干净；物品仍**平滑上移**（不是整行跳变、不是整屏
   消失）；左侧饰品栏饰品始终可见；点击 / 悬停 / 拖拽命中正常。
3. **背包**：地牢开背包同样下拉，检查外溢消失。工会两英雄网格保持同高度
   （本版工会仍不能用滚轮，属已知限制）。
4. **动态范围**：物品多的界面 `[DIAG] rows` / `max` 明显变大，可下拉范围随数量增加。
5. **日志判读**：
   | 字段 | 含义 |
   | --- | --- |
   | `active=1/2/3` | 当前目标面板（store / party / 背包）；`0` = 非目标界面，裁剪不介入 |
   | `clip` | `-1` = 静止零干预；`>=0` 且随 `cur` 增大 = 窗口在动 |
   | `p5src=4` / `p5fail` | `4` = `param_5` 位域校验通过；**`p5fail` 持续增长**说明槽位/编码假设不对，此时 DLL 自动放行不裁剪（画面等同 v9），把日志发回校准 |
   | `skipped` | 随滚动增长 = 上溢格子确实被跳过 |
6. **饰品栏 / 无关界面**：右侧只有游戏原生上下箭头滚动条；领地地图等界面
   不应出现任何自绘条（v9 起 DLL 不画 UI）。

**一键回退**：把 `clip_out_of_window = 0` 保存并重启 → 回到 v9「只平移不裁剪」，
滚动完全保留；对应面板改 `0` 只关该界面；`enabled = 0` 全部停用。
要回到**完全已实机验收**的二进制，用 `scroll_wheel.v10.dll.bak` 覆盖部署的 DLL；
`scroll_wheel.v9.dll.bak` 是更早的「只平移不裁剪」基线。

#### 实测结果（11:21:22 启动，11:37:02 结束）

| 验收项 | 实测证据 | 判定 |
| --- | --- | --- |
| 加载的是 v10 | `grid2 unique signature verified @+7EADE0`、`panel-gate caves installed @+7E9F80 / +7EADE0 ... per-cell clipping cave @+7EBF70 uses param_5 row`、`v9: no custom scrollbar` 全部出现 | 通过 |
| `param_5` 槽位/编码 | 全程 `p5fail=0`（`grep -c "p5fail=[1-9]"` = **0**） | 通过 |
| 上溢裁剪生效 | `skipped` 0 → **25870**，静止时停止增长 | 通过 |
| 非目标面板零干预 | `active=0`（857 条）/ `2`（39 条）/ `3`（43 条）；`active=0` 时 `skipped` 恒定 | 通过 |
| 静止零干预 | `clip=-1`、`cur→0` 时 `skipped` 不变 | 通过 |
| 校验 / 安装 | 启动 `ScrollWheel game verification` 全 `[OK]`、`dropped=0` | 通过 |

> 工会未纳入 v10 本轮验收；v11 的实机验收见 §8.4。

---

### 8.4 v11.2 工会技能栏滚轮 + 补给面板门控

> **验收状态（2026-11-13 20:15，用户确认）**：补给面板门控 ✅、v10 回归 ✅、
> **工会滚轮 ✅ 三项全部实机验收通过**。
> 已知诊断缺口：本次日志里工会 DIAG 字段全 0，与画面不一致（统计未覆盖实际生效
> 路径），**以实机画面为准**，详见 `p31-v112-acceptance-and-closeout.md` §3.3。
> 下方清单保留作为复验操作步骤。

**机制**（与补给/背包完全独立，不共用状态机、不写 `0x2B8EBAC`）：

| 层 | 做法 |
| --- | --- |
| 每帧刷新 | **`0x7F5EDD` cave 自己完成**：若 `[rdi] == g_guild.expectedVptr`（v11.2 由 C++ 写成 `g_base + 0xEBB8D8`，**不是磁盘 VA `0x140EBB8D8`**）则判为 GuildDisplay 本体，读 `[rdi+0x270]` 技能槽 vector，把指针与每槽原始 `+0x68` Y 拷进 DLL 侧表，先填满 `slots` 再发布 `count`、最后写 `lastBeat`。本体在子 widget 递归 Show（`0x7F607A`）之前经过此点，时序自洽。每帧刷新 ⇒ 切英雄时 `0x682B90` clear+析构不会留下悬垂命中 |
| 备用捕获 | `0x66E9F0`（虚表 slot23）保留，判据命中时也会刷新同一张表；**实机证明它一次都没执行**，故不再作为唯一依赖，仅作备用 + 诊断计数 |
| 只平移技能槽 | `0x7F5EDD`（`movss [rsp+0x54],xmm1`，6B）cave：非 GuildDisplay 本体时，仅当当前 widget 命中表且工会活跃，才给 `xmm1` 加该槽本帧的 Y 增量后原样重放 stolen。**只改栈帧，不动任何游戏对象/全局**；标题、按钮、背景、名字横幅都不动 |
| 命中同源 | `0x26C3F0` → `0x274CA0` 快照矩阵 → `0x273800` 重放 → `0x273600` 逆变换；p25-b 静态闭环确证图标、点击区域、tooltip 一起跟随 translate |
| 无外溢 | 滚出网格上沿的槽，Y 增量改为有限屏幕外值 `-100000.f`（不用 inf，不跳公共尾声），绘制/命中/tooltip 一起移出可视区 |
| 动态范围 | `max = clamp((N - R) * rowH, 0, cap)`；`N` = 技能槽数，`rowH` 优先取运行时槽 Y 相邻差（不硬编码 91），`R` 默认 1 保证能滚到底 |

**开关**：`guild_scroll`（另有 `guild_step` / `guild_cap` / `guild_visible_rows` /
`guild_row_height`，以及 v12 的 `guild_max_slots` / `guild_auto_cap`）。
代码默认与 cfg 模板都是 `0`；用户真实 cfg 已显式写 `1`。
`0` 时两个工会 cave **一个都不装**，新增校验也完全不执行，已验收的 v10
四个 cave 不受影响（`p26` 审核 S3）。

**v12 容量开关**（详见文末 §9）：`guild_max_slots = 256`（有效技能槽上限，
设 `64` 即字节不改地回到 v11.2 旧容量）、`guild_auto_cap = 1`（技能数多时
自动抬高滚动范围，避免旧 cfg 的 `guild_cap = 2000` 卡死 192 个技能）。
两者的代码默认值就是 `256` / `1`，**老 cfg 缺这两个键也会生效，绝不覆盖玩家配置**。

**另一个独立开关**：`gate_wheel_by_panel`（v11.2，代码/模板/用户 cfg 三处显式 `1`）。
`1` = 只在补给/背包网格活跃时消费滚轮并写布局数组，修「打开补给前滚轮 →
打开后已翻页」；`0` = 逐语义回到 v10 原行为（`layoutHit` 恒 true、每帧写三套布局）。
判据用 `active != 0 || cells > 0` 加 480ms 宽限窗口，避免吞掉补给界面第一格滚轮。

**请这样验收（重启游戏即可）**：

1. 启动日志应出现
   `v11 guild caves installed @+66E9F0 (per-frame slot table) / +7F5EDD (stack-frame Y shift)`，
   **并且** `v11.2 guild runtime vtable = 0x7FF7… (base 0x7FF7… + RVA 0xEBB8D8)`。
   后者是 ASLR 修复的关键：应为 `0x7FF7…` 量级的运行时地址；
   若打印 `0x140EBB8D8` 说明基址异常（`gVtA` 在 hook A 未执行时可能为 0，不影响功能）。若出现
   `v11 guild scroll DISABLED by verification`，说明校验没过，
   **补给/背包仍照常工作**，把日志发回。
2. 打开工会，缓慢下拉：技能图标应**平滑上移**；标题、英雄名字横幅、左侧按钮**保持不动**；
   滚出网格上沿的技能应消失且不遮挡背景。
3. **点击/悬停**：点击滚动后的技能新位置应能正常选中升级，旧位置不应响应，
   tooltip 跟随图标。
4. **切英雄 / 关闭再打开工会**：无错位、无残留图标、无崩溃。
5. **串台检查**：在城镇、出征补给、地牢背包滚滚轮，工会不应偷偷滚动；
   补给/背包的滚轮与 v10 裁剪应完全正常。
6. `[DIAG]` 新增字段判读：
   | 字段 | 含义 |
   | --- | --- |
   | `gActive` | `1` = 工会被判定为当前激活；`0` = 零干预 |
   | `gN` | 本帧刷新的技能槽数；打开工会应 > 0，离开后保持但 `gActive=0` |
   | `gCur` / `gMax` | 工会独立滚动位置与按数量算出的范围 |
   | `gHit` | `0x7F5EDD` cave 命中技能槽的累计次数；**滚动时若不增长 = 表没刷出来或判据不对** |
   | `gClip` | 被移到屏幕外的槽数 |
   | `gBeat` | 最后一次见到 GuildDisplay 的 tick 计数；滚动时应持续递增 |
   | `gACalls` / `gBCalls` | **v11.1 自检**：`0x66E9F0` / `0x7F5EDD` 两个 cave 各自的累计执行次数 |
   | `gSeenB` | **最关键**：hook B 累计见到 GuildDisplay 本体的次数，打开工会应持续递增 |
   | `gVtA` / `gVtB` | 两个 cave 最近一次读到的 vptr（十六进制），用于判据校准；**v11.1 就是靠它们发现 ASLR** |
   | `pRecent` | v11.2 面板门控状态：`true` = 目标网格活跃（含 480ms 宽限），滚轮会驱动补给/背包；`false` = 滚轮被丢弃、布局数组不被触碰 |
   | `gSlots` / `gAutoCap` | **v12**：本帧有效技能槽上限 / 自动抬高滚动上限开关状态；启动日志另有 `v12 GuildSkillCapacity: physical=256 effective=…` |

   **DIAG 只作辅助定位，功能是否生效一律以实机画面为准**。
   已确证：工会滚轮可以完全生效而 `gSeenB/gN/gActive` 仍全 0
   （DLL 侧统计未覆盖实际生效路径），**不要用这些字段判断工会是否可用**。

   | 字段 | 可靠用途 |
   | --- | --- |
   | `gBCalls` / `gACalls` | 判断两个工会 cave 是否执行（排查 ASLR 时用过） |
   | `pRecent` / `cur` / `storeY` | 判断补给面板门控是否生效（v11.2 实机已验证） |
   | `gSeenB` / `gN` / `gActive` / `gHit` | ⚠️ 已知诊断缺口：可能为 0 而功能正常，仅供参考 |

   > **P29 更正**：v11 首次日志 `gBeat/gN/gHit` 全 0 曾被误判为「捕获链没执行」。
   > v11.1 的自检计数反证 `gACalls=258`、`gBCalls=196565`，两个 cave 一直在跑；
   > 真正原因是 ASLR 判据常量，见 `p30-v112-aslr-and-panel-gate.md`。
   > P29 原文保留作历史，文件头已标注「结论已被推翻」。

7. **预翻页验收**：先在城镇或工会滚几格滚轮，确认 `[DIAG]` 里
   `pRecent=false` 且 `cur=0`；再打开出征补给，页面应停在**第一页**、没有预先翻动。
   随后在补给界面滚轮仍平滑生效、`skipped` 仍增长（v10 裁剪未被门控破坏），
   且 `pRecent` 变为 `true`。

**一键回退**：
- 补给门控：`gate_wheel_by_panel = 0` 保存重启 → 逐语义回到 v10 原行为。
- 工会：`guild_scroll = 0` 保存重启 → 两个工会 cave 完全不装。
- DLL 备份（按时间倒序）：`scroll_wheel.v11.1.dll.bak`（`a1d3d54a…`）、
  `scroll_wheel.v11.dll.bak`（`844a3876…`）、
  `scroll_wheel.v10.1.dll.bak`（`1cfd174b…`）、
  `scroll_wheel.v10.dll.bak`（`3384bb7a…`，纯 v10 本体，绝对回退）。

**已知剩余风险与未决项**：外溢用「移出屏幕」而非真正不产生 widget 参数条目，
可能有极轻微 hover 残留；`Base::Show` 是全 UI 共用每帧函数；可见行数静态无法确证，
`R=1` 是保守取值。
**工会 DIAG 存在可观测性缺口**：功能已由用户实机验收通过，但本次日志中
`gSeenB/gN/gActive/gHit` 全 0，说明 DLL 侧统计未覆盖实际生效路径；
这不是功能缺陷，以画面为准。若将来复核可在 cave 里加无条件计数。
`guild_scroll = 0` 可随时完全关闭两个工会 cave（一键回退）。

---

## §9 v12 GuildSkillCapacity —— 工会技能过多时的 UI 容量防护

> 需求来源：本地 class mod 让单个英雄拥有大量战斗技能，工会技能栏与角色页面
> 同屏时 UI 崩溃 / 错位。**本节只声称「消除已确证的结构性风险 + 纵深防御」，
> 不声称「已定位崩溃根因」**——详见 `docs/adr/0002-guild-skill-capacity.md`。

### 9.1 已确证的事实与定性边界

| 项 | 事实 | 来源 |
| --- | --- | --- |
| 技能数量 | 原版英雄 35 个 `combat_skill:`；本地 mod 英雄最多 **192** 个（`mods\Laevateinn\heroes\Laevateinn\Laevateinn.info.darkest`） | 游戏资源实测 |
| 旧容量 | v11.2 的 DLL 侧影子表 `GuildTable` 固定 **64 槽**，三个 cave 都 `cmp N,64; mov N,64` 钳制 | `src/CaveAsm.h`（v11.2） |
| 落差 | 超出 64 的技能槽既不进影子表、也不被平移；多出来的 UI 元素与角色页面同屏 | 结构推导 |
| 崩溃证据 | **没有** minidump / 异常码 / 崩溃堆栈；`app.log` 只有「上次异常退出」时间戳 | 阶段一调研 + 红队 A |
| 被推翻的假设 | `.max_rows 7` 是角色页容量、`roster.element_max 8` 是工会上限、64 槽表会越界写、给 `guild.layout.darkest` 加 `.element_max` 有效、v6 写共享常量是崩溃 | 红队 A 证伪清单 |

**因此**：v12 修的是「唯一有确证证据的容量落差」并补上指针有效性防御，
**不是**对某个已定位崩溃地址的定点补丁。若 `guild_max_slots = 256` 之后仍崩溃，
应按 §9.5 采集 minidump 再定位；不要据此声称根因已找到。

### 9.2 具体改动（全部在 `scroll_wheel.dll` 内部，不新建 DLL）

| 文件 | 改动 |
| --- | --- |
| `src/CaveAsm.h` | `GuildTable` 三张表 64 → **256** 槽；全部 `kGt*Off` 偏移按新布局重算并用 `static_assert` 锁死；新增 `maxSlots` 字段（`0x1040`），两个 cave 从表里读有效上限而不再内嵌立即数 |
| `src/CaveAsm.h` | 两个 cave 新增纵深防御：`begin` 为空 / `end < begin` / `maxSlots` 未初始化 → `count = 0`（本帧零干预，绝不遍历野指针）；技能槽指针非 8 对齐 → 写 0 占位；`setDefaultJmpNEAR(true)` 避免新增分支让 rel8 短跳抛 `label is too far` |
| `src/Config.h/.cpp` | 新增 `guild_max_slots`（默认 256，钳到 `1..256`）、`guild_auto_cap`（默认 `1`）；纯函数 `EffectiveGuildMaxSlots()` 供生产与离线单测共用 |
| `src/Hooks.cpp` | `UpdateGuild` 用有效上限钳 `N`；`offset[]` 按**物理容量**每帧全量清零；`auto_cap` 按本帧 `N` 抬高滚动上限（带 100000 硬上限）；DIAG 增加 `gSlots` / `gAutoCap` |
| `mod/ScrollWheel.cfg` | 模板显式写出两个新键；**仅首次生成时写入，绝不覆盖已有玩家 cfg** |
| `tools/test_scroll.cpp` | 11 条新断言：容量 ≥192、三表分区不重叠、全字段 `offsetof`、ASLR 新偏移 `0x1038`、cfg 解析 / 回退 / 非法值钳制 |

### 9.3 为什么不会摧毁 ScrollWheel 现有功能

- **不新建 DLL**：合并进同一个 DLL，彻底避开「两个 DLL 抢 `0x66E9F0` / `0x7F5EDD` 同一批 patch 字节」的 Critical 冲突（红队 B C1）。`VerifyExtra` / `VerifyGuild` 校验的游戏字节**一字未动**。
- **不碰 v10 四个 cave**：输入 cave、面板 gate、逐格裁剪的 RVA 与机器码均未改；校验失败仍只跳过工会 hook、绝不 `return`。
- **不钳制游戏创建的 UI 槽数量**：只扩 DLL 侧影子表。游戏自己创建的技能槽、索引与技能逻辑保持一一对应，避免红队 B C4 的「UI 索引与逻辑索引脱节」。
- **不改游戏 vector、不跨 CRT 扩容、不写 `0x2B8EBAC`**：规避 C5 与 v6 实机证伪的错位。
- **不改 UI 索引语义**：`guild_max_slots = 64` 时 cave 读到的上限就是 64，行为与 v11.2 逐字节等价（机器码不变，只改表里一个值）。
- ASLR 判据仍是 `g_base + 0xEBB8D8`（`expectedVptr` 只是偏移从 `0x438` 移到 `0x1038`，离线单测已断言新字节）。

### 9.4 验收清单（重启游戏）

1. 启动日志出现 `v12 GuildSkillCapacity: physical=256 effective=256 auto_cap=1 ...`。
   若 `effective=64`，说明玩家 cfg 显式写了 `guild_max_slots = 64`（回退状态）。
2. `[DIAG]` 新增 `gSlots=256 gAutoCap=1`；原有 `gN/gMax/gHit/gACalls/gBCalls` 语义不变。
3. 选一个技能最多的 mod 英雄（如 Laevateinn，192 个技能）打开工会并**同时打开角色页面**：
   不崩溃、不错位、能滚到最后一个技能、点击技能仍可正常升级（UI 索引未脱节）。
4. **回归 v10 / v11**：出征补给与地牢背包滚轮、v10 裁剪、v11.2 面板门控
   （先在城镇滚滚轮再开补给应停在第一页）全部照常。
5. **A/B 对照**：`guild_max_slots = 256` ↔ `64`、`guild_auto_cap = 1` ↔ `0`、
   `guild_scroll = 0`，各重启一次，记录是否仍崩溃 / 能否滚到底。
6. 离线单测：`xmake build -P . scroll_wheel_test` →
   `scroll_wheel_test.exe <Darkest.exe>`，期望 **150 PASS / 0 FAIL**。

### 9.5 若仍崩溃：取证清单

现有资料无法定位崩溃点，需要一次带证据的复现：

- 记录 `Darkest.exe` 的 SHA1、运行时基址、`DDModLoader.log` 里实际加载的 DLL 列表；
- 记录英雄技能数 `N`、是否同时打开工会与角色页面、`[DIAG]` 的 `gN/gMax/gHit/gACalls/gBCalls`；
- 用 `procdump -ma -e Darkest.exe` 或 WER LocalDumps 采 minidump，附 `DDModLoader\log\scroll_wheel.log`；
- 对照矩阵：技能数 `35 / 103 / 192` × 「单开工会 / 单开角色页 / 同时打开」×
  `guild_scroll = 1 / 0`。
- **先做二分**：`guild_scroll = 0` 重启。崩溃消失 ⇒ 与两个工会 cave 相关；
  仍崩 ⇒ 是原版游戏 / mod 数据自身问题，不要再改 ScrollWheel。

### 9.6 一键回退（按影响范围从小到大）

| 范围 | 操作 |
| --- | --- |
| 只回退容量 | cfg 写 `guild_max_slots = 64` 保存重启 |
| 只回退滚动上限 | cfg 写 `guild_auto_cap = 0` 保存重启 |
| 关闭全部工会 hook | cfg 写 `guild_scroll = 0` 保存重启（v10 四个 cave 保持） |
| 禁用整个 DLL | `DDModLoader.cfg` 写 `0|<scroll_wheel.dll 绝对路径>`（`DD Mod Lib` 由加载器强制加载，GUI 关不掉） |
| 换回旧二进制 | 用 `build\scrollwheel\scroll_wheel.v*.dll.bak` 覆盖 |

---

公共的 `xmake/plugin.lua`、`Plugin.h.in`、`version.rc` 和 `LICENSE` 位于 `DD.Plugins\` 根目录。阶段工作文件（简报 / 调研 / 审核 / 修复 / 验收）在 `DD.Plugins\build\scrollwheel\`，按 `docs/06-workflow.md` 在用户验收后清理。
