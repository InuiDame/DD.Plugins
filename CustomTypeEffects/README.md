# Custom Type Additional Effects（自定义技能 type 触发器）

`DD.Plugins` 中的一个 C++ DLL 插件，随加载器预装于 `mods\DD Mod Lib`，为《暗黑地牢》的 **buff / trinket** 增加按技能 `.type` 匹配的附加效果触发器。面向 MOD 制作者的简短说明另见安装器附带的《预制功能说明 Built-in Features.txt》。

原版只有 `melee_attack_additional_effect_ids` / `ranged_attack_additional_effect_ids`（buff）和 `melee_attack_skill_additional_effects` / `ranged_attack_skill_additional_effects`（trinket），只有把英雄 `*.info.darkest` 里的 `.type` 写成 `"melee"` / `"ranged"` 才会生效。特殊 type（如 `teleport`）和自定义 type 吃不到这些效果。

本 MOD 补足这一块。

---

## 1. 新增的触发器字段

在 `shared/buffs/*.buffs.json` 的某个 buff 对象里，或在 `trinkets/*.entries.trinkets.json` 的某个 entry 对象里，添加：

```json
"<type>_type_additional_effect_ids" : [ "效果名A", "效果名B" ]
```

或（trinket 风格，两种后缀在两类文件里都认）：

```json
"<type>_type_additional_effects" : [ "效果名A" ]
```

`<type>` 就是技能 `combat_skill: ... .type "xxx"` 里的字符串，匹配时**不区分大小写**、忽略首尾空白。例如：

| 技能定义                  | 需要写的字段                                   |
| --------------------- | ---------------------------------------- |
| `.type "shadow_step"` | `shadow_step_type_additional_effect_ids` |
| `.type "beast_form"`  | `beast_form_type_additional_effect_ids`  |
| `.type "heal"`        | `heal_type_additional_effect_ids`        |

值可以是字符串数组，也可以是单个字符串；数组内的重复项会被去重。效果名必须能在 `*.effects.darkest` 里查到，否则只会打日志、不会生效。

### 默认屏蔽的 type

| type       | 原因                                 |
| ---------- | ---------------------------------- |
| `teleport` | **使用后会传送出战斗**，此时追加效果没有意义                 |
| `melee`    | 已有原版 `melee_attack_*` 触发器，避免重复触发   |
| `ranged`   | 已有原版 `ranged_attack_*` 触发器，避免重复触发  |
| `move`     | 原版位移技能，不参与攻击触发器                    |

为这些 type 写的字段会在加载时被忽略并在日志里给出警告。屏蔽列表由 `<游戏根目录>\DDModLoader\cfg\CustomTypeEffects.cfg` 控制，位置与格式都与 `DeathBlowCap.cfg` / `ProtectionCap.cfg` 等 MOD 一致：

```ini
# CustomTypeEffects config
# 逗号或空白分隔；重复写 blocked_types 行会合并
# 删掉这个键（或删掉整个文件）则使用内置默认值
blocked_types = teleport, melee, ranged, move
```

- 第一次启动时若文件不存在，DLL 会**自动生成**一份带注释的默认 cfg（`DDModLoader\cfg` 目录也会一并创建）。
- `blocked_types` 出现即**覆盖**内置默认值；写 `blocked_types =`（空值）表示不屏蔽任何 type。
- 支持 `#` 和 `//` 注释、`key = value` 格式，未知键与无 `=` 的行只会在日志里告警。
- 设置了 `DDPath` 时构建会把模板放到 `DDModLoader\cfg\`，但**不会覆盖**你已经改过的配置；DLL 首次启动也会自行生成。

> 字段必须带 `_type_` 标记。这样能天然避开 `attack_additional_effect_ids`、`melee_attack_additional_effect_ids` 等所有原版字段名，不会和原版逻辑撞车。



---

## 2. 快速开始

1. 加载器已预装本 DLL（`mods\DD Mod Lib\custom_type_effects.dll`），从源码构建的方法见第 4 节。
2. 在**你的数据 MOD**里定义效果（`*.effects.darkest`）。
3. 给 buff / trinket 加上 `<type>_type_additional_effect_ids`。
4. 把对应英雄技能的 `.type` 改成那个自定义 type。
5. 预制功能始终启用，只需在加载器配置工具里启用你的数据 MOD，进战斗验证。

`example/` 是一套可直接照抄的最小示例（type 为 `shadow_step`）：

```
example/
├─ effects/custom_type_example.effects.darkest            两个效果定义
├─ shared/buffs/custom_type_example.buffs.json           buff 触发器
├─ trinkets/custom_type_example.entries.trinkets.json    trinket 触发器
└─ heroes/example/custom_type_example.info.darkest       把 .type 改成 shadow_step
```

---

## 3. 生效时机

DLL 在游戏进程启动时（`DllMain` / `DLL_PROCESS_ATTACH`）扫描磁盘上的 JSON，建立规则表；战斗中 hook 游戏的「附加效果收集」函数，在原版收集完所有效果之后，把匹配当前 `.type` 的自定义效果追加进同一个输出列表。

因此自定义效果与原版效果走**完全相同**的后续应用流程（`ApplyHit` / 友方技能 / 其它路径），包括暴击、命中/未命中、队列等判定。

- buff 侧沿用游戏自己的 `479CD0` 谓词判断该 buff 当前是否生效，行为与原版触发器一致。
- trinket 侧按背包物品逐个匹配 trinket id，行为与 `484580` 一致。

---

## 4. 构建与部署

本插件是 `DD.Plugins` 工程的一个 target，由根目录 `xmake.lua` 自动包含，不需要单独配置：

```bash
cd D:\workplace\DD.Plugins
xmake f -p windows -a x64 -m releasedbg -y
xmake build custom_type_effects    # 或直接 xmake 构建全部插件
```

公共规则在 `DD.Plugins\xmake\plugin.lua`，依赖 `DKUtil`、`nlohmann_json`、`spdlog`、`xbyak`、`simpleini`；环境准备见[工程根 README](../README.md)。

产物：`DD.Plugins\build\windows\x64\releasedbg\custom_type_effects.dll`

部署遵循 DD.Plugins 的约定：设置了 `DDPath` 时（工作流里先 `source DD.ModLoader/tools/env.sh`，再 `export DD_DEPLOY=1`），公共规则把 DLL 复制到 `%DDPath%\mods\DD Mod Lib\`，本插件的 `after_build` 再把配置模板放到 `%DDPath%\DDModLoader\cfg\`：

```
%DDPath%\mods\DD Mod Lib\
    custom_type_effects.dll
%DDPath%\DDModLoader\cfg\
    CustomTypeEffects.cfg     # 仅当不存在时生成，不会覆盖已有配置
```

未设置 `DDPath` 时只生成 DLL，不改动游戏目录。DLL 首次启动时也会自行在 `DDModLoader\cfg\` 下生成默认 `CustomTypeEffects.cfg`。

游戏运行时 DLL 被占用，部署前先关闭游戏。`DD Mod Lib` 由加载器强制加载、始终启用，无需在配置工具里勾选。

日志：`<游戏根目录>\DDModLoader\log\custom_type_effects.log`

---

## 5. 实现原理（逆向基准）

基准文件：`_windows/win64/Darkest.exe`  
SHA1：`9db2d73b89aa034e2055df65a139403e18df641f`，ImageBase `0x140000000`，以下均为 RVA。

| RVA                 | 作用                                                                              |
| ------------------- | ------------------------------------------------------------------------------- |
| `479470`            | **hook 点**。统一收集技能附加效果：`(Actor*, Vector<Effect*>*, Skill*, Context*, bool crit)` |
| `479CD0`            | buff 生效谓词，自定义 buff 效果复用它                                                        |
| `484580`            | trinket 效果收集（由 `479470` 经虚表 `+0x1A8` 调用）                                        |
| `4781D0`            | trinket 按类别取 `Vector<Effect*>`                                                  |
| `4F3E50`            | 按效果名查 `Effect*`（字符串只走 `rdx`，`rcx` 未使用）                                          |
| `239AA0`            | `Vector<T*>` 扩容 / `push_back` helper                                            |
| `487D80`            | 技能 `.type` 解析到 `Skill+0x50`（`char[0x40]`）                                       |
| `4AA770` / `4FEB50` | buff / trinket JSON 解析（本 MOD 不 hook，只读取同一批文件）                                   |

关键内存布局：

| 偏移                       | 含义                                         |
| ------------------------ | ------------------------------------------ |
| `Actor+0x460`            | `Vector<BuffEntry>`，`BuffEntry` 步长 `0x418` |
| `BuffEntry+0x8`          | `Buff`                                     |
| `Buff+0xA4`              | buff id hash                               |
| `Skill+0x50`             | `.type` 字符串                                |
| `Actor+0x1288 / +0x1290` | 背包首 / 尾指针，物品步长 `0x5A8`                     |
| `Item+0x88`              | trinket id hash                            |

id hash 与游戏一致：`h = h * 53 + byte`（`uint32` 截断为 `int32`）。

所有函数入口都用唯一特征码定位，另外用 8 组指令字节校验结构偏移；任一不匹配就**拒绝安装 hook**，只在日志里报错，不会读坏内存。

扫描根目录（低优先级 → 高优先级，同一 `(id, type)` 后者覆盖前者）：

```
<game>/shared/buffs、<game>/trinkets、<game>/dlc
<steamlib>/steamapps/workshop/content/262060
<game>/mods
```

不同 root 里相同 id 的**不同 type** 会累加；同一 type 由优先级更高的 MOD 覆盖。

---

## 6. 目录结构

```
DD.Plugins/CustomTypeEffects/
├─ xmake.lua                     target: custom_type_effects（dd.mod = DD Mod Lib）
├─ README.md                     本文件
├─ mod/
│  ├─ CustomTypeEffects.cfg      默认配置模板，部署到 DDModLoader\cfg
│  └─ project.xml                早期独立发布时的 MOD 元数据，作为预制功能时不使用
├─ src/
│  ├─ PCH.h
│  ├─ Rules.h / Rules.cpp        扫描 JSON、id hash 映射、cfg 屏蔽列表
│  ├─ Hooks.h / Hooks.cpp        特征码定位、detour、效果追加
│  └─ main.cpp                   DllMain 初始化
└─ example/                      示例数据 MOD
```

公共的 `xmake/plugin.lua`、`Plugin.h.in`、`version.rc` 和 `LICENSE` 位于 `DD.Plugins\` 根目录。

---

## 7. 排错

| 日志                                              | 含义 / 处理                                              |
| ----------------------------------------------- | ---------------------------------------------------- |
| `Incompatible game version, hook NOT installed` | exe 版本与基准不符（或特征码被别的插件破坏）；需要重新校准 RVA                  |
| `No custom type triggers found`                 | DLL 正常，但没有任何数据 MOD 写了 `*_type_additional_effect*` 字段 |
| `Ignoring 'xxx' ... .type "yyy" is blocked`     | 该 type 在屏蔽列表里，字段被跳过                                  |
| `Undefined additional effect "..."`             | `*.effects.darkest` 里没有这个名字，或写错大小写                   |
| `cfg loaded: ...` / `Default cfg generated: ...` | 屏蔽列表的来源；可确认 cfg 位置和实际生效的 type 列表 |
| `[RULE] ...`                                    | 启动扫描时读到的每条规则，可用来确认 MOD 是否被扫到                         |
| `cfg line N unknown key` / `has no '='`          | cfg 里有拼错的键或格式错误，该行被跳过                                   |

**临时禁用 DLL**（例如实机验证前）：在 `<游戏根目录>\DDModLoader\cfg\DDModLoader.cfg` 里加一行（UTF-8，格式 `1|路径` 启用 / `0|路径` 禁用）：

```
0|D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\mods\DD Mod Lib\custom_type_effects.dll
```

---

## 8. 限制

- **仅适配上述 SHA1 的 exe**。游戏更新后特征码/布局校验会主动禁用 hook。
- DLL 在游戏启动时扫描磁盘上的全部 JSON，**不按存档启用列表过滤**（与 DD.ModLoader 的「DLL 全局生效」一致）。被禁用 MOD 引用的效果名若不存在于当前游戏数据，`EffectLookup` 会返回空并只打日志，不会崩；极端情况下若效果名与当前已加载的某个效果重名，仍可能被触发。
- 内联 hook 与同样 hook `479470` 的其它 DLL 冲突；DD.Plugins 的其它插件与 DDCore 的 hook 清单均未使用该函数。
- 自定义 type 的效果在该技能被调用时统一追加，**不区分** `+0x3AD`（友方技能）与 `+0x90` 等原版分类标记；如果只想在攻击类技能上触发，请把 `.type` 只用在攻击技能上。
- 已完成游戏内实机验收；游戏版本更新后建议再用 `example/` 里的示例 MOD 复测一遍。
