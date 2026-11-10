# Darkest Dungeon Shield Core

[中文](#中文) | [English](#english)

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

---

## 中文

为《暗黑地牢》提供一套**独立于血条的护盾核心系统**的插件。玩家角色与怪物均可拥有自己的护盾条，护盾可享受护体、格挡等机制带来的福利，并通过 `effect`、角色 `info` 与 `buff` 参数进行丰富的自定义配置。

本项目使用 **GPL V3** 开源协议。

### 依赖说明

**本插件依赖于 DDModLoader 运行。如果你没有安装 DDModLoader，请前往：**

https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader

下载并安装。

> 本插件无法脱离 DDModLoader 单独运行。

### 功能特性

- 新增一套**护盾核心系统**，护盾条独立于血条显示与结算。
- 玩家角色与怪物均可配置护盾。
- 护盾同样能享受护体、格挡等机制带来的福利。
- 支持通过 `effect` 命名快速添加护盾效果。
- 支持在角色 `info` 中配置护盾上限与初始值。
- 支持在 `buff` 中配置护盾上限、护盾获取、每回合再生/衰减等参数。
- 支持固定值与百分比两种规格，便于制作多样化技能、饰品、怪癖与怪物机制。
- 支持按“自身有护盾”“对方有护盾”生效的 buff 规则，以及穿透护盾的 effect。

### 安装

1. 安装并配置 [DDModLoader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader)。
2. 下载本插件，并按 DDModLoader 的插件安装说明放入对应目录。
3. 在 DDModLoader 中启用本插件。
4. 启动游戏并验证护盾系统是否生效。

### 快速开始

#### 1. 在 `effect` 中添加护盾效果

只需要在 `effect` 里增加以下命名的 effect：

| Effect 命名 | 作用 |
|---|---|
| `shield_gain_37` | 加 37 护盾 |
| `shield_self_gain_12` | 给自己加 12 护盾 |
| `shield_heal_10` / `shield_heal_pct_25` | 护盾治疗：固定值 / 百分比 |
| `shield_max_12` / `shield_max_pct_50` | 护盾上限：固定值 / 百分比 |
| `shield_damage_8` / `shield_damage_pct_25` | 护盾伤害：固定值 / 百分比 |
| `shield_regen_3_4` | 每回合 +3，持续 4 回合 |
| `shield_decay_2_3` | 每回合 -2，持续 3 回合 |
| `shield_full` / `shield_break` | 满盾 / 破盾 |
| `shield_pierce_50` / `shield_pierce` | 本次命中 50% / 全部伤害无视护盾，直接打血 |

> 命名中的数字可按需替换。例如 `shield_gain_37` 表示增加 37 点护盾。
>
> 穿透 effect 要作用在被攻击的目标上（`.target "target"`，`.on_miss false`），只影响本次技能对该目标的这一次命中。

#### 2. 在角色 `info` 中配置护盾

只需在角色的 `info` 下添加以下参数：

```ini
shield: .max 25 .start 25
```

含义：

- `.max 25`：护盾最大值 25。
- `.start 25`：护盾初始值 25。

#### 3. 在 `buff` 中配置护盾参数

`buff` 拥有以下护盾相关参数：

| 参数 | 说明 |
|---|---|
| `shield_max` / `shield_max_pct` | buff 存在期间提高护盾上限（固定值 / 百分比） |
| `shield` / `shield_gain` | buff 首次生效时给予的护盾点数 |
| `shield_regen` / `shield_decay` | 每回合开始时的护盾增减 |

#### 4. 按护盾状态生效的 buff 规则

沿用游戏的状态规则，把 `rule_data.string` 写成 `shielded`：

| `rule_type` | 条件 |
|---|---|
| `is_actor_status` | 自身有护盾时 |
| `actorStatus` | 对方有护盾时 |

`is_false_rule` 设为 `true` 表示没有护盾时生效。护盾值大于 0 才算有护盾，和其他状态规则一样只在战斗中判定。

```json
{
  "id": "bonus_vs_shielded",
  "stat_type": "combat_stat_multiply",
  "stat_sub_type": "damage_low",
  "amount": 0.33,
  "remove_if_not_active": false,
  "rule_type": "actorStatus",
  "is_false_rule": false,
  "rule_data": { "float": 0, "string": "shielded" }
}
```

提示文字来自 `localization/shield.string_table.xml` 里的 `buff_rule_data_tooltip_shielded`，游戏读取的是生成后的 `.loc2`，改动后需要重新生成或重新上传 mod。

### Buff 示例

```json
{
  "id": "HHSK2_Heal5",
  "stat_type": "hp_dot_heal",
  "stat_sub_type": "",
  "amount": 13,
  "duration_type": "after_round",
  "duration": 1,
  "remove_on_battle_complete": true,
  "remove_if_not_active": true,
  "is_clear_debuff_valid": false,
  "has_description": true,
  "rule_type": "hpabove",
  "is_false_rule": false,
  "rule_data": {
    "float": 0.5,
    "string": ""
  },
  "shield_max": 10,
  "shield": 10,
  "shield_decay": 1
}
```

该 buff 在拥有 13 点 HOT 的同时，还会额外提供 1 回合 10 点最大护盾值和 10 点护盾值，且回合结束时护盾值掉 1 点。

### 适用场景

- 为角色或怪物制作独立护盾机制。
- 制作护盾类技能、饰品、怪癖、疾病或战斗效果。
- 制作具有护盾再生、护盾衰减、破盾、满盾等机制的敌人。
- 在不修改血条逻辑的前提下扩展战斗资源体系。

### 从源码构建

本插件属于 DD.Plugins 工程，在工程根目录用 xmake 构建，环境准备见[工程 README](../README.md)：

```powershell
xmake build DDShield                  # 构建，设置了 DDPath 时部署到 mods\DDShield
xmake install -o build\dist DDShield  # 生成完整的 mod 文件夹 build\dist\DDShield
xmake build ddshield_test_config      # 可选的测试工具，另有 ddshield_test_loc、ddshield_test_classes、ddshield_test_swap、ddshield_test_shield
```

部署时 dll 总是更新，其他文件只在缺失时复制，不会覆盖游戏目录里调整过的 `DDShield.ini` 和带创意工坊 ID 的 `project.xml`。修改 `DDShield.effects.json` 等数据文件后，需要手动同步到游戏目录。

### 反馈与贡献

- 遇到问题或有功能建议，欢迎通过 GitHub Issues 反馈。
- 欢迎提交 Pull Request。
- 提交贡献即表示你同意相关内容以本项目的 GPLv3 协议开源。

### 开源协议

本项目采用 **GNU General Public License v3.0（GPLv3）** 开源协议。

你可以自由使用、修改和分发本项目，但分发衍生作品时需遵守 GPLv3 条款，包括提供相应的源代码。完整协议条款请参见本目录下的 [LICENSE](LICENSE) 文件。

### 致谢

- 感谢《暗黑地牢》模组社区对护盾机制与自定义玩法的探索。

---

## English

A core shield system plugin for **Darkest Dungeon**. It adds shield bars independent of HP for both heroes and monsters. Shields can benefit from mechanics such as guard and block, and can be extensively customized through `effect`, character `info`, and `buff` parameters.

This project is licensed under **GPL V3**.

### Dependency

**This plugin depends on DDModLoader to run. If you have not installed DDModLoader, please go to:**

https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader

Download and install it.

> This plugin cannot run without DDModLoader.

### Features

- Adds a **core shield system** with shield bars independent of HP display and calculation.
- Supports shields for both heroes and monsters.
- Shields can benefit from mechanics such as guard and block.
- Supports quickly adding shield effects through named `effect` entries.
- Supports configuring shield cap and initial value in character `info`.
- Supports configuring shield cap, shield gain, and per-turn regeneration/decay in `buff`.
- Supports both fixed values and percentages for flexible skills, trinkets, quirks, and monster mechanics.
- Supports buff rules that depend on your own or the opponent's shield, and effects that pierce shields.

### Installation

1. Install and configure [DDModLoader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader).
2. Download this plugin and place it in the appropriate directory according to DDModLoader's plugin installation instructions.
3. Enable this plugin in DDModLoader.
4. Launch the game and verify that the shield system works.

### Quick Start

#### 1. Add shield effects in `effect`

You only need to add the following named effects in `effect`:

| Effect Name | Description |
|---|---|
| `shield_gain_37` | Gain 37 shield |
| `shield_self_gain_12` | Gain 12 shield for self |
| `shield_heal_10` / `shield_heal_pct_25` | Shield heal: fixed value / percentage |
| `shield_max_12` / `shield_max_pct_50` | Shield cap: fixed value / percentage |
| `shield_damage_8` / `shield_damage_pct_25` | Shield damage: fixed value / percentage |
| `shield_regen_3_4` | +3 per turn, lasts 4 turns |
| `shield_decay_2_3` | -2 per turn, lasts 3 turns |
| `shield_full` / `shield_break` | Full shield / break shield |
| `shield_pierce_50` / `shield_pierce` | 50% / all of this hit ignores the shield and goes straight to HP |

> The number in the effect name can be replaced as needed. For example, `shield_gain_37` grants 37 shield.
>
> A pierce effect must apply to the enemy being hit (`.target "target"`, `.on_miss false`). It only affects that skill's hit on that target.

#### 2. Configure shields in character `info`

You only need to add the following parameters under the character's `info`:

```ini
shield: .max 25 .start 25
```

Meaning:

- `.max 25`: maximum shield value 25.
- `.start 25`: initial shield value 25.

#### 3. Configure shield parameters in `buff`

A `buff` has the following shield-related parameters:

| Parameter | Description |
|---|---|
| `shield_max` / `shield_max_pct` | Increases the shield cap while the buff is active (fixed value / percentage) |
| `shield` / `shield_gain` | The shield points granted when the buff first takes effect |
| `shield_regen` / `shield_decay` | Shield increase/decrease at the start of each turn |

#### 4. Buff rules based on shields

Use the game's status rules with `shielded` as `rule_data.string`:

| `rule_type` | Condition |
|---|---|
| `is_actor_status` | You have a shield |
| `actorStatus` | The opponent has a shield |

Set `is_false_rule` to `true` to apply when there is no shield. A shield counts when its value is above 0, and like other status rules it is only checked in combat.

```json
{
  "id": "bonus_vs_shielded",
  "stat_type": "combat_stat_multiply",
  "stat_sub_type": "damage_low",
  "amount": 0.33,
  "remove_if_not_active": false,
  "rule_type": "actorStatus",
  "is_false_rule": false,
  "rule_data": { "float": 0, "string": "shielded" }
}
```

The tooltip text is `buff_rule_data_tooltip_shielded` in `localization/shield.string_table.xml`. The game reads the generated `.loc2` files, so regenerate them or re-upload the mod after changing it.

### Buff Example

```json
{
  "id": "HHSK2_Heal5",
  "stat_type": "hp_dot_heal",
  "stat_sub_type": "",
  "amount": 13,
  "duration_type": "after_round",
  "duration": 1,
  "remove_on_battle_complete": true,
  "remove_if_not_active": true,
  "is_clear_debuff_valid": false,
  "has_description": true,
  "rule_type": "hpabove",
  "is_false_rule": false,
  "rule_data": {
    "float": 0.5,
    "string": ""
  },
  "shield_max": 10,
  "shield": 10,
  "shield_decay": 1
}
```

While this buff has 13 points of HOT, it also additionally provides 10 max shield value and 10 shield value for 1 turn, and at the end of the turn the shield value drops by 1 point.

### Use Cases

- Create independent shield mechanics for heroes or monsters.
- Create shield-related skills, trinkets, quirks, diseases, or combat effects.
- Create enemies with shield regeneration, shield decay, shield break, full shield, and other mechanics.
- Extend the combat resource system without modifying HP bar logic.

### Building from Source

This plugin is part of the DD.Plugins project. Build it with xmake from the project root; see the [project README](../README.md) for setup:

```powershell
xmake build DDShield                  # build, and deploy to mods\DDShield when DDPath is set
xmake install -o build\dist DDShield  # create the complete mod folder build\dist\DDShield
xmake build ddshield_test_config      # optional test tools, also ddshield_test_loc, ddshield_test_classes, ddshield_test_swap and ddshield_test_shield
```

Deployment always updates the DLL. Other files are copied only when missing, so a tuned `DDShield.ini` or a `project.xml` holding the Workshop ID in the game folder is never overwritten. After changing data files such as `DDShield.effects.json`, sync them to the game folder by hand.

### Feedback and Contributions

- For issues or feature suggestions, please use GitHub Issues.
- Pull Requests are welcome.
- By submitting a contribution, you agree that it will be licensed under this project's GPLv3 license.

### License

This project is licensed under the **GNU General Public License v3.0 (GPLv3)**.

You are free to use, modify, and distribute this project, but derivative works must comply with the GPLv3 terms, including providing the corresponding source code. See the [LICENSE](LICENSE) file in this directory for the full license text.

### Acknowledgements

- Thanks to the Darkest Dungeon modding community for exploring shield mechanics and custom gameplay.