# HeroLevelCap（按职业设置英雄等级上限）

`DD.Plugins` 中的 C++ DLL 插件，通过加载器注入游戏，在英雄职业的 `*.info.darkest` 里新增一行
`level_cap:` 块来设置该职业的 resolve level 上限。**不写这一行时保持原版 6 级上限。**

---

## 1. 用法

在你自己的数据 MOD 里，给需要改上限的职业文件加一行真实块（和 DDShield 的 `shield:`、
dd_immune 的 `immune:` 同款，不需要 `//` 注释）：

```
level_cap: .max 10
resistances: ...
combat_skill: ...
```

- 文件位置：`<你的MOD>\heroes\<职业名>\<职业名>.info.darkest`
- 职业名取文件所在目录名（与文件名一致），大小写不敏感。
- 也支持裸数字写法：`level_cap: 10`；`.cap 10` 等价于 `.max 10`。
- 取值范围 1–20，超出范围或非法值会写 WARN 并忽略该行。
- 同一文件里多行按先后顺序覆盖，最后一行生效；同一职业在多个根出现时按
  **游戏本体 < 创意工坊 < 本地 MOD** 的优先级覆盖。
- 游戏解析器只要求「标识符后面是冒号」，`level_cap:` 是合法块，游戏自身不认识会忽略，
  **不会截断**后面的 `resistances:` / `combat_skill:` 等块。
- 没有写 `level_cap:` 的职业一律按 6 级上限处理。
- 完整示例见 [`example/example.info.darkest`](example/example.info.darkest)。

改动 `.info.darkest` 或 DLL 后需要重启游戏。

---

## 2. 等级图标注意事项（重要）

提高上限后，**游戏原本只为 0–6 级准备了 resolve level 等级图标**。如果你允许英雄升到 7 级以上，
需要在下面的目录放置对应等级的图标，否则等级条会显示缺图/错误图标：

```
%Steam%\steamapps\common\DarkestDungeon\shared\resolve_level_bar\
```

| 文件 | 用途 |
| --- | --- |
| `resolve_level_bar_number_background_lvl<N>.png` | 等级数字底板（原版 lvl0–lvl6，64×64） |
| `resolve_level_bar_number_background_glow_lvl<N>.png` | 底板高亮（原版 lvl0–lvl6，64×64） |

`<N>` 是**阿拉伯数字的等级**，例如 7 级就是 `resolve_level_bar_number_background_lvl7.png`
与 `resolve_level_bar_number_background_glow_lvl7.png`。原版还有一张通用的
`resolve_level_bar_number_afflicted_background.png`（77×77），它不是按等级分的，不用复制多份。

`mod/game/shared/resolve_level_bar/` 里已经带了一套 **7–10 级** 图，由
`tools/gen_level_icons.py` 按原版规律生成：以原版 `lvl6` 为模板，只替换中心宝石的色相
（金色外框原样保留），延续原版「橙→红→紫」之后的顺序：

| 等级 | 颜色 | 宝石 HSV |
| --- | --- | --- |
| 7 | 紫 | hue 285 / S 1.00 / L 0.30 |
| 8 | 青 | hue 190 / S 1.00 / L 0.30 |
| 9 | 品红 | hue 325 / S 1.00 / L 0.30 |
| 10 | 冷白 | hue 210 / S 0.12 / L 0.90 |

要更高等级就改脚本里的 `RAMP` 再跑一次：

```
python tools/gen_level_icons.py --game "<游戏根目录>" --out mod/game/shared/resolve_level_bar --from 7 --to 12
```

glow 图沿用原版 4/5/6 级的做法（全透明 64×64），高段不需要额外高亮图。

> **已处理 [静态+代码]**：`UI::CampaignAssets::Load`（RVA `7BB9B0`）加载这两组图标的循环
> 原本是 `inc ebx; cmp ebx,7`（RVA `7BDF6C`），只会加载 `lvl0`–`lvl6`。**仅仅把 `lvl7.png`
> 放进目录不会让游戏加载它**。本插件会把这个边界改成 21（对应 `kMaxCap=20`），
> 从而加载 `lvl0`–`lvl20`；缺失的 PNG 会被游戏当作无效句柄跳过，不会崩溃。
> 需要为实际开放的等级补齐上面两张图。日志出现
> `icon loop +7BDF6C (bound=21, writeback=true)` 才说明这个补丁装上了。

把 PNG 放进目录后需要重启游戏才会重新加载资源。

---

## 3. 构建、部署与离线测试

在 `DD.Plugins` 根目录（需要 `DKUtilPath` 指向 DKUtil）：

```bash
export DKUtilPath="D:/WorkSpace/DKUtil"
cd D:/workplace/DD.Plugins

xmake f -p windows -a x64 -m releasedbg -y
xmake build hero_level_cap
xmake build hero_level_cap_test
build/windows/x64/releasedbg/hero_level_cap_test.exe
```

产物：`build/windows/x64/releasedbg/hero_level_cap.dll`。

部署（游戏必须已关闭；三个条件同时成立才会复制 DLL 和 `project.xml`）：

```bash
export DDPath="D:/Program Files (x86)/Steam/steamapps/common/DarkestDungeon"
export DD_DEPLOY=1
xmake build hero_level_cap
```

目标：`<游戏根目录>\mods\HeroLevelCap\`，包含 `hero_level_cap.dll` 与 `project.xml`。
`project.xml` 只在不存在时生成，不会覆盖玩家改过的内容。装好后需要在加载器配置工具里
勾选 `Hero Level Cap` 这个 MOD，并重启游戏。

日志：`<游戏根目录>\DDModLoader\log\hero_level_cap.log`。

---

## 4. 逆向基准与实现

基准：`_windows/win64/Darkest.exe`，SHA1 `9db2d73b89aa034e2055df65a139403e18df641f`，
ImageBase `0x140000000`。以下为 RVA，**离线版不同，靠特征码定位并校验**。

| RVA | 作用 |
| --- | --- |
| `46E920` | 读取 `campaign/roster/roster.variables.json` 的 `resolve_level_thresholds` |
| `118E0B8` | 指针 → `std::vector<uint32>` 阈值表（原版 `[0,2,8,14,24,36,48]`）|
| `5D8B70` | `Actor::GetResolveLevel()`，英雄虚表槽 `0x168`；XP 在 `Actor+0x1328`（`GetResolveLevel` 的 self 已是 `Hero+8`） |
| `587764` | `Roster::System::OnRaidFinish` 里把 XP 截断到阈值表最后一项（48）的 `cmp` + `cmovb` |
| `587784` | 同一函数里 `cmp edi,esi; je 58792C`，按整数等级是否变化决定是否刷新技能/等级条；本插件把 `je` 改成 NOP |
| `5D4950` | `RefreshSkills`，按 `GetResolveLevel()` 解锁技能；升级刷新时调用，幂等 |
| `6AF7F0` | **名册卡片**。`6AFC7B` 起自己读阈值表算等级：`count=(end-begin)/4`，`level=max{i | xp>=thr[i]}`，`i<count` |
| `6AFCE4` | 名册底图下标：`lea edx,[rbx-1]; shl rdx,4; add rdx,[2B8DC60]`，按 `level-1` 取 `rosterelement_res%d` |
| `62DB90` | 名册卡片资产构建；`62EFD3` 拼 `campaign/town/roster/rosterelement_res%d.png` |
| `62F01B` | 该资产的加载循环 `inc ebx; cmp ebx,7; jl`，本插件改成 `cmp ebx,21` |
| `7BB9B0` | `UI::CampaignAssets::Load`，加载等级徽章贴图 |
| `7BDF6C` | 图标加载循环的 `inc ebx; cmp ebx,7`，本插件改成 `cmp ebx,21` |

注：`Actor+0x137C` 不是等级字段（实测自定义职业该值为 37），不要据此找缓存等级；
存档里也没有独立的 `resolveLevel`，等级完全由 XP 实时算出。

做法：

1. 插件启动扫描 `.info.darkest`，建立「职业 → 上限」表；没有任何高于 6 的值时不碰游戏。
2. 用唯一特征码定位 `5D8B70`（入口偷 6 字节，正好两条完整指令）与 XP 截断指令，校验偷取字节、截断字节、
   `Hero` 虚表槽 0x168 指向同一函数、运行时阈值表可读——**任一项不符就一个字节都不写**。
3. 在 `GetResolveLevel` 入口打 detour：按 `Hero+0x1320` → `ActorClass+8`（内嵌 `char[0x40]`）
   取职业 id，查表得到上限；阈值表按最后一段步长（原版 12）向后扩展，用同样的插值公式算等级，
   超过该职业上限就截断。
4. 把 `OnRaidFinish` 的 XP 截断改成直通（`mov edx,eax` + NOP），否则高等级需要的 XP 会被 48 卡住。
   原版上限 6 的英雄即使 XP 超过 48，`GetResolveLevel` 仍返回 6，行为不变。
5. 把 `OnRaidFinish` 的等级变化判断 `je`（`587786`）改成 NOP。否则 XP 早已超过新门槛的英雄
   在加经验前后都返回同一上限值，游戏误判"等级没变"，跳过技能解锁与等级条刷新（详见第 5 节）。
6. 用 MOD 数据覆盖 `campaign/roster/roster.variables.json`，把 `resolve_level_thresholds`
   从 7 项改成 8 项（末尾补 `60`）。**这是名册能显示 7 级的唯一正解**，原因见下一节。
7. 把名册底图加载循环（`62F01B`）的边界也从 7 放宽到 21。名册底图是**按 `level-1` 下标**
   从 20 项容器里取用的（`6AFCE4`），容器只有 6 项时 7 级会越界读，必须同步放宽；
   `campaign/town/roster/rosterelement_res7..20.png` 由 `res6.png` 复制补齐（缺文件只会报占位图）。
8. detour 里把超额 XP 收回该职业上限对应的阈值（写前校验 `[self]+0x168` 指向同一函数），
   让满级后经验不再增长，并保证名册自算逻辑与职业上限一致。

### 4.1 为什么必须改数据表，而不是改函数

名册卡片（`6AF7F0`）在 `6AFC7B` 处**自己遍历阈值表**算等级：

```
6afc7b  mov rax, [118E0B8]      ; 阈值 vector 对象
6afc82  mov r8,  [rax]          ; begin
6afc89  sub rdx, r8
6afc8c  sar rdx, 2              ; count = 项数（原版 7）
6afca8  cmp r9d, [r8+rax*4]     ; xp vs thresholds[i]
6afcb7  jb  6afca6              ; i < count 就继续
```

循环上界就是**表的项数**，`level` 最大只能是 `count-1 = 6`。所以只 detour `GetResolveLevel`
永远改不到名册数字——这就是前面几轮「函数返回 7、面板显示 6」的根本原因。
把表本身变成 8 项后，所有读表路径（名册、面板、技能）自然都会算到 7。

数据文件放在 `mod/<mod>/campaign/roster/roster.variables.json`，部署时由构建钩子复制到
`mods/HeroLevelCap/campaign/roster/`，并把 `project.xml` 的 `ModDataPath` 改写成实际部署目录
（游戏会解析这个字段，留空可能导致数据/本地化不被加载）；MOD 需在游戏内启用。

### 4.2 上一版闪退的原因（勿重犯）

曾经尝试过「直接改游戏内存里的阈值 vector」和「在 detour 里回写 `Hero+0x1328` 的 XP」，
两者都会让游戏**闪退**（日志停在 `live threshold vector extended` 之后十几秒，
`app.log` 没有正常退出记录，属于堆损坏型的硬崩）：

- 直接写 `end[0]` 并前移 `end`，依赖「`vec+0x10` 是容量末尾」这个假设；假设不成立就是越界写堆。
- `GetResolveLevel` 会被一个通用 thunk（`4849C0`：`mov rax,[rcx]; call [rax+0x168]`）调用，
  传入的 `this` 不保证是 Hero。在 detour 里往 `this+0x1328` 写 4 字节，命中非 Hero 对象就是越界写。

结论：**插件运行时不得写游戏对象或容器**。改数据表要交给游戏自己的加载器（MOD 数据覆盖），
改代码才用 `WriteData` 在安装阶段打补丁。

计数器与日志：detour 每次进入都 `+1`，前若干条命中写 `DIAG`；被截断的命中单独计数。

### 4.3 上限如何落地：XP 收回职业上限

阈值表本身是全局一张，但上限仍按职业区分，靠两步配合：

1. `GetResolveLevel` 的 detour 按职业上限截断返回的等级；
2. detour 里把**超额 XP 收回**到该职业上限对应的阈值（`xp capped at class limit: ... 748 -> 60`）。
   名册那套自算逻辑只读 XP 和表，所以 XP 停在 threshold[cap] 后，名册、面板、技能三者一致。

写 XP 之前会校验对象类型：`[self] + 0x168` 必须指向同一个 `GetResolveLevel`。
`GetResolveLevel` 会被通用 thunk（`4849C0`）调用，`this` 不保证是 Hero；没有这道校验就写，
就是上次闪退的原因之一。校验不过就一个字节都不写。

**满级后经验不再增长**，与原版一致（原版封顶 48，这里封顶 threshold[该职业上限]）。

遗留的显示差异：工具提示里的经验上限显示的是**表的最后一项**，所以 6 级职业满级会显示
`48/60` 而不是 `48/48`。要支持更高上限就把 `mod/campaign/roster/roster.variables.json`
继续补项（每级 +12），表长决定 UI 能算到的最大等级。

---

## 5. 排错：拿到经验但没升级

先看日志 `<游戏根目录>\DDModLoader\log\hero_level_cap.log`：

- 出现 `installed: ... (writeback=true)` 才说明 XP 截断补丁真的写进去了；`writeback=false` 表示写入失败。
- `level cap from info: class=xxx cap=7` 表示 info 块被读到了；没有这行说明文件位置/职业名不对。
- `XP above vanilla: class=xxx xp=50 ... intLevel=6` 是关键证据：XP 已经超过原版 48，补丁生效。

**等级是浮点数，但游戏用整数部分判断是否升级**（`cvttss2si`）。按原版最后一段步长 12 外推，
阈值表变成 `[0,2,8,14,24,36,48,60,72,...]`，所以：

| XP | 计算出的等级 | 是否升到 7 级 |
| --- | --- | --- |
| 48 | 6.00 | 否 |
| 50–59 | 6.17–6.92 | 否（整数部分仍是 6） |
| 60 | 7.00 | 是 |

也就是说，从满级 6 的 48 XP 开始，**一次任务只加几点 XP 不会立刻升级**，需要累计到 60 才跳 7 级。
这是阈值外推的直接结果，不是补丁失效。插件已经通过 MOD 数据覆盖把表补到 8 项（末尾 60），
想再放宽就把 `mod/campaign/roster/roster.variables.json` 继续补项后重新部署。

另外，加载器是在 `DllMain` 之后才解析阈值表，所以插件第一次调用时才读取运行中的阈值表；
日志里的 `live thresholds loaded: 8 entries, last=60` 表示读到了 MOD 覆盖后的 8 项表
（若仍是 `7 entries, last=48`，说明 `mods/HeroLevelCap/campaign/roster/roster.variables.json`
没有被游戏加载，检查 MOD 是否启用、路径是否被别的 MOD 覆盖）。

**如果 `GetResolveLevel` 已经返回 7，但英雄面板仍显示 6**：先确认阈值表项数。
名册数字是**文本元素**（`resolve_level_bar.layout.darkest` 里 `.number_offset 30 31`），
由 `6AF7F0` 自己查表算出，不经过 `GetResolveLevel`；徽章底色才是贴图
（`resolve_level_bar_number_background_lvl<N>.png`，原版只加载 `lvl0`–`lvl6`，
需要插件的图标循环补丁）。两项都要到位：表要够长（见 4.1）、贴图循环要放宽（见第 2 节）。
`Hero+0x137C` 不是等级字段（实测自定义职业该值为 37），不要在那里找等级。

**XP 早已超过上限却一直不升级（真正的根因）**：`Roster::System::OnRaidFinish`（`586730`）
在加经验前后各调用一次 `GetResolveLevel`（`587730` / `587779`），用
`587784 cmp edi,esi; je 58792C` 比较**整数等级是否变化**；只有变化时才执行升级刷新
（`5D4950` 技能解锁、`HeroLogItem`、等级条更新）。如果英雄在装上插件时 XP 就已经超过
7 级门槛（外推值 60），两次调用都返回 `7`、整数相同，游戏会判定"等级没变"而跳过整个
刷新流程——于是技能不解锁、等级条不更新，看起来完全没生效，即使 detour 明明返回 7。

插件现在把 `587786` 的 `je` 改成 6 个 NOP，让升级刷新**每次任务结束都执行**；
`RefreshSkills` 与日志写入是幂等的，重复执行没有副作用。日志里会出现
`level-change +587786 (writeback=true)` 表示这条补丁也装上了。这是 XP 已经溢出后
"补触发"升级所必需的；从 1 级正常练上来的英雄没有这个问题，但多执行一次刷新也无害。

**经验溢出**：游戏内部用 `int32` 做 XP 加法，异常存档里的超大 XP 可能回绕成负数。
插件把 `xp > 1,000,000` 视为异常值，直接按职业上限返回，不再参与插值。
正常游玩不会碰到这个分支。升级判断点还做了保护：即使 XP 异常，刷新流程也只会
重复写同样的等级，不会因回绕产生负数等级。

**名册卡片的等级数字不跟着变（最关键的一条）**：名册卡片、部分 UI 显示等级时
**不调用 `Actor::GetResolveLevel`**，而是直接读 `resolve_level_thresholds` 这张
`std::vector<uint32>` 自己算。实测证据：日志里的调用者清单完全没有名册渲染相关的地址，
而 Hero 对象的内存扫描里也找不到值为 6 的等级字段（唯一像等级的是 `hero+0x1318 = 7`，
那是已经算好的等级缓存，但它由别的路径写入）。

所以插件在首次调用时会把**游戏运行时的阈值 vector 原地加长**（`TryExtendThresholdVector`）：
在现有 capacity 内追加 `60, 72, …` 直到 `职业上限 + 1` 项，再把 vector 的 end 指针前移。
这样所有直接读表的代码都能算出 7 级以上。日志 `live threshold vector extended: 7 -> 8 entries, last=60`
表示成功；出现 `threshold vector capacity too small` 表示该版本无法原地扩容（此时只会退回 detour 方案）。

因为表被加长后「XP 封顶在 48」对默认 6 级职业不再成立，detour 里另外按职业上限
把超额 XP 回写（日志 `xp clamped to cap: class=xxx 648 -> 48`），保持「不写 `level_cap:`
就是原版 6 级」的语义。

**7 级以上的称号文本**：游戏只有 `str_resolve_0..6`，7 级会显示成 `[<str_resolve_7>]`。
插件在 `mods/HeroLevelCap/localization/` 放了一份 `str_resolve_7..26` 的补充表（中英各一份）。
**注意**：这要求 `HeroLevelCap` 在游戏内被当作已启用 MOD 加载；如果只在加载器里注入 DLL
而没有在存档的 MOD 列表里启用它，这份本地化不会被读，称号仍显示占位符（不影响等级本身）。

---

## 6. 验证层级与限制

- 本次完成的是**离线单测**（info 解析、阈值扩展、等级计算），以及 AOB 特征码在原版 exe 上的唯一性校验。
- **尚未做加载自检与实机验收**：没有在游戏进程里验证 detour 命中、职业 id 读取、
  升级到 7 级以上、以及其他插件共存。不要把它当成已实机验证的功能。
- 仅适配上述 SHA1 的 Steam 版 exe；游戏更新后校验会主动拒绝安装。
- 等级图标循环已 patch 到加载 `lvl0`–`lvl20`，但只在你补齐对应 PNG 时才有图；缺图的等级仍会显示异常。
- 覆盖的是 `Roster::System::OnRaidFinish` 的 XP 路径；其他直接改写 XP 的途径（存档编辑器等）不受本插件限制。
- 与同样 hook `5D8B70` 或改 `587764` 附近的插件冲突；当前 `knowledge/dd1/hooks.md` 清单中没有。

## 7. 回退

删除数据 MOD 里的 `level_cap:` 行并重启游戏即可恢复 6 级上限；不需要删 DLL。
临时禁用 DLL：在 `<游戏根目录>\DDModLoader\cfg\DDModLoader.cfg` 中把对应 DLL 行改成 `0|路径`。
要完全卸载，删除 `<游戏根目录>\mods\HeroLevelCap\` 整个目录。

**本体数据文件也被改过（部署时写入，均带 `.bak` 备份）**：

| 文件 | 改动 | 备份 |
| --- | --- | --- |
| `campaign\roster\roster.variables.json` | `resolve_level_thresholds` 补一项 `60`（7→8 项） | `roster.variables.json.vanilla.bak` |
| `localization\miscellaneous.string_table.xml` | english / schinese 块内补 `str_resolve_7..20` | `miscellaneous.string_table.xml.hlc.bak` |
| `campaign\town\roster\rosterelement_res7..20.png` | 由 `res6.png` 复制（名册底图，按等级取用） | 删除即可，原版只到 `res6` |

改本体而不只靠 MOD 数据覆盖，是因为**本地 MOD 的数据只有在该 MOD 于游戏内启用后才会挂载**：
实测未启用时日志一直是 `live thresholds loaded: 7 entries`、tooltip 显示 `748/48`、称号仍是
`{str_resolve_7}` 占位符。把 `mods\HeroLevelCap` 在游戏 Mod 列表里勾选启用也能达到同样效果，
此时本体那两个文件可以用 `.bak` 还原（两边内容一致，谁生效都行）。

