# dd_immune（暗黑地牢：kill / control / capture 免疫）

`DD.Plugins` 中的一个 C++ DLL 插件，随加载器预装于 `mods\DD Mod Lib`，为《暗黑地牢》提供三个互相独立的免疫模块。目标只对**指定的 effect 字段**失效，同 effect 里的其它字段（如 stun、伤害 buff）照常执行。

- **免疫.kill**：目标不会被 effect 的 `.kill`（含 `.kill_enemy_types`）当场击杀。
- **免疫.control**：目标不会被 effect 的 `.control` 施加"控制"（siren 式，走 `Battle::UpdateControl`）。**事前阻断**，绝不动 `.uncontrol`。
- **免疫.capture**：目标不会被 effect 的 `.capture`（CaptureBosun / CaptureHag）捕获，同时屏蔽 `.capture_remove_from_party`，避免半捕获。

> **实机验收已通过（2026-11-17）**：十字军 info 加 `immune: .kill true .control true .capture true` 后，最终 BOSS 的 `.kill` 点名不再致死。日志中可见 `[DIAG] class id ok: +8='crusader' ... -> mask=0x7`，且不再出现 `class id read failed`。

---

## 1. 用法（两种参数形式，可同时使用）

目标最终的免疫 mask = **class 级（info） | buff 级（buff）**，两级按位 OR。三个位：`kill=1`、`control=2`、`capture=4`。

### 1.1 buff 形式（挂在目标身上的 buff）

在你自己的数据 MOD 里新增布尔字段，值 `true`/`false`，缺省 `false`。游戏会忽略这些未知字段，DLL 扫描 `*.buffs.json` 读取。

```json
{
  "buffs": [
    {
      "id": "immune_all",
      "stat_type": "status",
      "immune_kill": true,
      "immune_control": true,
      "immune_capture": true
    }
  ]
}
```

- 扫描根目录（低 → 高优先级，同 id 后者覆盖前者，含显式 `false` 覆盖低优先级 `true`）：
  `<game>/shared/buffs`、`<game>/trinkets`、`<game>/dlc`、`<steamlib>/steamapps/workshop/content/262060`、`<game>/mods`。
- 运行时只要该 buff 挂在目标身上（`Actor+0x460` 组内存在对应 id 哈希）即生效。**默认不调用 `479CD0`**，以"条目存在"为 active。
- 非布尔值写 WARN 并忽略；未知字段不影响。

完整的三个字段演示见 `example/example.buffs.json`。

### 1.2 info 形式（职业固有免疫）

**直接在职业文件顶层新增真实块 `immune:`，与 DDShield 的 `shield: .max 25 .start 25` 同款，不需要 `//` 注释。**
DLL 读**原始文本**；游戏解析器只在"标识符后面不是 `:`"时报错停止，而 `immune:` 是合法块（标识符 + 冒号），
游戏不认识但会忽略，**不会截断**后续的 `resistances:` / `combat_skill:` 等块。

```
immune: .kill true .control true .capture true
immune: kill, control
immune: .capture 0
resistances: ...
combat_skill: ...
```

- 扫描 `heroes/**/*.info.darkest`、`monsters/**/*.info.darkest`。
- 行首跳过空白后首个 token 为大小写不敏感的 `immune:` 即识别；冒号后按 `,`/空白切 token，
  支持 `.kill true` / `.control false` / `.capture 1` / `.capture 0`（`.` 前缀 + 布尔值）
  与裸 `kill` / `control` / `capture`（等价 true）；其它 token 写 WARN 忽略。
- 多行按文件顺序处理：显式 `false`/`0` 清除此前置位（与 buff 的"后者覆盖"一致）；
  之后的 `true` 可再置回。仅为向后兼容，仍识别旧写法 `// ... DD_IMMUNE:` / `// ... immune:`，
  但**不再要求**必须有 `//`。
- 职业名取最深层目录名（与文件名相同）。每个职业登记三种哈希变体：`Hash(name)`、`Hash("hero_class_name_"+name)`、`Hash("str_monstername_"+name)`；运行时对 `ActorClass+8` 与 `+0x48` 两个候选 id 串各求上述三哈希，命中任一即可（兼容 `MonsterClass+0x1598` 非零的怪物改用 `+0x48`，两串同时命中取 OR）。
- 优先级：buff 与 info 两级 OR；同名职业在多个根出现时按根优先级后者覆盖前者。

示例见 `example/example.info.darkest` 与 `example/README.md`。

### 1.3 可选 rule-gated 模式（v1 未启用）

buff 里写 `immune_require_active: true` 可声明"要求 buff 当前 active"。**v1 只解析并记录该字段，不接入 detour**（即仍按"条目存在"生效）。如需启用：调用 `479CD0(actor, inst, actor, target, nullptr)` 前必须过主线程门（DllMain 缓存的主线程 ID）、`rule_type(+0x5C)` 预筛（`always(0)` 直接 active、`55` 解析失败跳过、需要 `Raid` 的类型在 `Raid::Current==0` 时跳过）、整体 `__try/__except` 包裹与日志节流。`combat.md:256` 测得 `479CD0` 约 114 万次/22 分钟、其中 1340 次在加载线程，默认不调用是为规避这一风险。

`Rules::RuleGatePrecheck()` 已实现上述前置判定的纯谓词并有单测覆盖，供后续接入。

---

## 2. 三个模块的语义与边界

| 字段 | 偏移 | 清零后的效果 | 未触碰的字段 |
| --- | --- | --- | --- |
| `.kill` | `+0x1F0` | 整个当场击杀分支不进入，`MonsterDeathEvent`/死亡 FX 全部消失 | 同 effect 的 stun 等独立字段照常执行 |
| `.kill_enemy_types` | first `+0x1F8` / last `+0x200` / end `+0x208` | 副本里把 `last` 设为 `first`，消费者 `first!=last` 为假 → 整段跳过；**`end` 不动** | 不写游戏原 Effect 内存 |
| `.control` | `+0x070` | 值为 0 → `Battle__UpdateControl(...,1,0)` 从未被调用，目标不会被置为"被控制" | **`+0x074 .uncontrol` 原样透传** |
| `.capture` | `+0x320` | 整个捕获分支不进入，不构造/不派发 `CaptureEvent` | — |
| `.capture_remove_from_party` | `+0x324` | 同时清零，避免未来消费者变化导致半捕获 | — |

### `.kill` 的注意事项（最终 BOSS 的秒杀复读）

最终 BOSS 会**检测队伍人数**，对秒杀招数进行"复读"：当被点名目标没有按预期死亡时，它会再次施放该秒杀招，形成连续点名。因此：

- 只靠"免疫.kill"让目标不死，会被反复点名，压力/回合消耗仍然很大，甚至陷入僵持。
- **建议给配备"免疫.kill"的角色同时装上反伤或反击**（riposte / 反伤类 buff、饰品或技能），让 BOSS 的每次复读秒杀都反过来受伤，把点名变成输出窗口。
- 本插件只保证"不被当场击杀"，不改变 BOSS 的选目标与复读逻辑；反伤/反击需由你的 MOD 或原版手段另行提供。

### `.uncontrol` 与"免疫.control"的区别（重要）

- `.control(+0x70)` 是**施加控制**：命中后调用 `Battle::UpdateControl(...,1,0)`，把目标置为被控制。
- `.uncontrol(+0x74)` 是**解除控制**：命中后调用 `Battle::UpdateControl(...,0,1)`，把目标从控制中释放。
- 本插件的"免疫.control"是**事前阻断**：在 effect 生效前把副本的 `.control` 清零，让施加分支整体不执行。它**不是**"先让目标被控制、再调用 uncontrol 清除"，因此**绝不写 `.uncontrol`**。即使同一 effect 同时带 `.control` 和 `.uncontrol`，解除逻辑仍按原样执行。
- stun/unstun、daze/undaze、immobilize/unimmobilize 是各自独立的字段/分支，**不调用 `Battle::UpdateControl`**，不属于本工程"控制"的范畴，也不会绕过本免疫。

---

## 3. 逆向基准

基准：`_windows/win64/Darkest.exe`，SHA1 `9db2d73b89aa034e2055df65a139403e18df641f`，ImageBase `0x140000000`，以下均为 RVA。

| RVA | 作用 |
| --- | --- |
| `4C5050` | `EffectApplyInternal(prng&, performer, target, Effect const*, item, roll, crit, scale, source, sourcedata, out, bool, bool)`，13 参。入口已被 DDShield(DDCore) 占用，本插件**不打补丁**到这里 |
| `4CB4D0` | `ApplyEffect`：按 `.target` 收集目标，逐个调 `4C5050` |
| `4CB8C0` | **本插件 hook 点**：`4CB4D0` 内唯一 `call 4C5050`，实测 `E8 8B 97 FF FF` → 目标 `g_base+0x4C5050`，全 `.text` 唯一 |
| `479CD0` | `IsRuleActive`（可选 rule-gated 模式用，v1 默认不调） |
| `5B2680` | `Battle::UpdateControl(bool,bool)`（非 effect 路径残留风险，见 §5） |

Effect 结构 0x618 字节；关键偏移见 §2。id 哈希算法 `h = h*53 + byte`（`uint32` 截断为 `int32`，`Hash("stunned")==0xF9DA5CF5`）。buff 组布局：`Actor+0x460`、`BuffEntry` 步长 `0x418`、Instance `+0xA4`。职业类：虚表槽 6 → `ActorClass*`；`ActorClass+8` 与 `MonsterClass+0x48` 是**内嵌 `char[0x40]` 字符串本身**（不是 `char*` 指针），运行时直接按内嵌数组读取。

---

## 4. hook 方案与冲突规避（方案 B）

`4C5050` 入口已被 DDShield（MinHook 内联 `E9`）和 DDCore（P2 语义 hook）占用。若像方案 A 那样改入口，会与它们互相破坏特征码。本插件采用 **方案 B：改调用点**：

1. 把 `4CB8C0` 的 5 字节 `E8 rel32` 改成指向本模块 `Detour` 的 `call`（保持 call/ret 语义，返回地址仍是 `4CB8C5`）。
2. `Detour` 按 swap 算真 target、求两级 mask、`mask==0` 直接原样转发；否则在栈上复制 0x618 字节，按 mask 清零副本，再**直接 call `g_base+0x4C5050`**。
3. 传下去的 `performer`/`target` 不交换——target 的真正判定仍由原函数按 swap 标志完成，保证与游戏内部状态一致。
4. 若 Detour 与调用点相距超过 ±2GB，在调用点附近分配一页 `FF25 [abs64]` 中继再跳过来；中继分配失败则**跳过本 hook**。

**冲突规避**：不碰 `4C5050` 入口，DDShield/DDCore 的 hook 照常生效，并会看到我们传下去的副本（其它字段未改，不影响护盾逻辑）。`hooks.md` 的现有 hook 清单里没有 `4CB4D0`/`4CB8C0`。

**全有或全无 / 安全降级**：安装前逐项校验 V1..V8，写 `[OK]`/`[FAIL]` 日志。任一**硬校验**（V1 调用点目标与唯一性、V2 ApplyEffect 入口、V6 buff 布局、V8 中继）失败 → 只跳过本 hook，`Install()` 正常返回，绝不 `abort`/`__fastfail`，也不影响其它初始化。`Detour` 的 `__try/__except` **只包插件自身逻辑**（target 判定、mask 查询、`memcpy`、字段清零）：异常时回退为原 `Effect*`（fail-open）；对 `g_original` 的调用在受保护块之外且全函数**只有一处**，因此游戏函数自身抛出的 SEH 异常照常向上传播，绝不会被兜底吞掉后二次调用、造成 effect 副作用双施加。V3（`4C5050` 入口字节）为**信息项**：因该入口常被第三方改为 `E9/E8`，若设为硬门禁会使插件在真实共存环境静默失效，故只记录、不作为门禁。V4 已由对抗审核对真实 exe 反汇编 `4CB4D0` 独立复核（13 参 `[rsp+0x20..0x60]`，两个 float 在栈上而非 xmm）；V5/V7 为信息项。

---

## 5. 限制与已验收范围

- **实机验收已通过（2026-11-17）**：在真实安装上，十字军 info 配 `immune: .kill true .control true .capture true` 后，最终 BOSS 的 `.kill` 点名不再致死；`dd_immune.log` 显示 hook 校验全 `[OK]`、`[DIAG] class id ok: +8='crusader' -> mask=0x7`。`.control` / `.capture` 按同一机制实现，未逐项单独实机演示。
- **仅适配上述 SHA1 的 exe**。游戏更新后特征码/布局校验会主动禁用 hook（`static_assert sizeof==0x618` 也会在布局漂移时编译失败）。
- `.control` 免疫只覆盖 `EffectApplyInternal` 的 effect 路径。若非 effect 代码直接调用 `5B2680 Battle::UpdateControl(...,1,0)`（如某些怪物 AI/脚本），本 effect 级 hook 看不到，免疫不生效。该调用路径 `[未确证]`，v1 不覆盖；覆盖它需要新增 `5B2680` hook 并从 Battle 状态反推 target，列为后续。
- 内联 hook 与同样改 `4CB8C0`/`4CB4D0` 的其它 DLL 冲突（当前清单无此类）。
- `require_active` 在 v1 未强制（见 §1.3）。

---

## 6. 构建、单测与排错

```bash
cd D:/workplace/DD.Plugins
# 不要把 DDPath / DD_DEPLOY 带进来（不要部署、不碰真实游戏目录）
env -u DDPath -u DD_DEPLOY xmake f -P . -p windows -a x64 -m releasedbg -y
env -u DDPath -u DD_DEPLOY xmake build -P . dd_immune

# 离线单测（set_default(false)，需显式构建并运行）
env -u DDPath -u DD_DEPLOY xmake build -P . dd_immune_test
build/windows/x64/releasedbg/dd_immune_test.exe
```

产物：`build/windows/x64/releasedbg/dd_immune.dll`。日志：`<gameRoot>/DDModLoader/log/dd_immune.log`。

| 日志 | 含义 / 处理 |
| --- | --- |
| `[FAIL] V1 ...` | `4CB8C0` 不是预期的 `E8 rel32`、目标不是 `4C5050`、或 `.text` 内不唯一 → exe 版本不符或被别的插件改过 |
| `[FAIL] V2 ...` / `[FAIL] V6 ...` | `4CB4D0` 入口 / buff 组布局特征码不匹配 → 重新校准基准 |
| `[FAIL] V8 ...` | Detour 超出 rel32 范围且近页分配失败 → 跳过本 hook |
| `V3 ... first byte` | `4C5050` 入口首字节（信息项）。`0x40` 原版 / `E9`/`E8` 已被第三方 hook |
| `no rules found, hook NOT installed` | DLL 正常，但没扫到任何 `immune_*` buff 字段或 `immune:` 真实块规则 |
| `[BUFF-RULE]` / `[INFO-RULE]` | 启动扫描读到的每条规则，用来确认 MOD 是否被扫到 |
| `immune detour exception, forwarded original effect` | SEH 兜底触发，原 effect 已原样转发；检查数据是否触发了野指针 |
| `[DIAG] immune detour hit` | 命中记录（前 16 条），含 mask 与 swap，用来确认免疫确实作用于目标 |

---

## 7. 临时禁用

`DD Mod Lib` 由加载器强制加载、GUI 不列出，无法按存档勾选。禁用方法：

1. 在 `<gameRoot>/DDModLoader/cfg/DDModLoader.cfg` 加一行（UTF-8，`0|路径` 禁用 / `1|路径` 启用）：
   ```
   0|D:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon\mods\DD Mod Lib\dd_immune.dll
   ```
2. 或直接从 `mods\DD Mod Lib\` 移走/删除 `dd_immune.dll`。

---

## 8. 目录结构

```
DD.Plugins/dd_immune/
├─ xmake.lua                 target: dd_immune（dd.mod = DD Mod Lib）+ dd_immune_test
├─ README.md                 本文件
├─ src/
│  ├─ PCH.h                  PLUGIN_MODE / LOG_PATH / DKUtil / nlohmann/json
│  ├─ Rules.h/.cpp           mask 位、Effect 偏移、哈希、buff/info 解析、副本清零、规则门
│  ├─ Hooks.h/.cpp           特征码与布局校验、方案 B detour、fail-open、日志
│  └─ main.cpp               DllMain：Rules::Load() → Hooks::Install()
├─ tools/
│  ├─ TestPCH.h              单测最小 PCH（日志宏置空）
│  └─ test_immune.cpp        离线单测
└─ example/                  两种参数形式的最小示例 + 说明
```
